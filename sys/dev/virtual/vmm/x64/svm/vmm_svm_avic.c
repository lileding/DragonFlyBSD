/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD Advanced Virtual Interrupt Controller support.
 *
 * This is an SVM-internal interrupt implementation.  It owns AVIC backing
 * pages and target tables, while vmm_svm.c remains the sole owner of VMCB
 * layout and VMRUN.
 */
#include <sys/errno.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_page.h>

#include <machine/clock.h>

#include "../../vmm.h"
#include "../../vmm_machine.h"
#include "../../vmm_vcpu.h"
#include "../vmm_x64_pic.h"
#include "../vmm_x64_pit.h"
#include "vmm_svm.h"
#include "vmm_svm_avic.h"
#include "vmm_svm_os.h"
#include "vmm_svm_x86defs.h"

#define VMM_SVM_AVIC_APIC_BASE		0xfee00000ULL
#define VMM_SVM_AVIC_PHYS_VALID	__BIT(63)
#define VMM_SVM_AVIC_PHYS_RUNNING	__BIT(62)
#define VMM_SVM_AVIC_HOST_APIC_ID_MASK	0xfffU
#define VMM_SVM_AVIC_MAX_PHYS_ID	0xfeU
#define VMM_SVM_AVIC_LOGICAL_VALID	__BIT(31)
#define VMM_SVM_AVIC_LOGICAL_APIC_ID	__BITS(7, 0)
#define VMM_SVM_AVIC_DOORBELL_MSR	0xc001011bU

static bool vmm_svm_avic_ipi_enabled;

#define VMM_SVM_MSR_APICBASE			0x01bU
#define VMM_SVM_APICBASE_BSP			0x00000100ULL
#define VMM_SVM_APICBASE_ENABLED		0x00000800ULL
#define VMM_SVM_APICBASE_ADDRESS		0xfffff000ULL
#define VMM_SVM_AVIC_APICBASE_VALID	(VMM_SVM_APICBASE_BSP | \
	VMM_SVM_APICBASE_ENABLED | VMM_SVM_APICBASE_ADDRESS)

#define VMM_SVM_IOAPIC_BASE		0xfec00000ULL
#define VMM_SVM_IOAPIC_PINS		24U
#define VMM_SVM_IOAPIC_REG_ID		0x00U
#define VMM_SVM_IOAPIC_REG_VERSION	0x01U
#define VMM_SVM_IOAPIC_REG_ARB		0x02U
#define VMM_SVM_IOAPIC_REDIR_BASE	0x10U
#define VMM_SVM_IOAPIC_VERSION		(((VMM_SVM_IOAPIC_PINS - 1U) << 16) | 0x11U)
#define VMM_SVM_IOAPIC_REDIR_MASKED	__BIT(16)
#define VMM_SVM_IOAPIC_REDIR_FIXED	0x00000000U
#define VMM_SVM_IOAPIC_REDIR_DELIVERY_MASK	0x00000700U
#define VMM_SVM_IOAPIC_REDIR_DEST_LOGICAL	__BIT(11)
#define VMM_SVM_IOAPIC_REDIR_REMOTE_IRR	__BIT(14)
#define VMM_SVM_IOAPIC_REDIR_LEVEL	__BIT(15)

#define VMM_SVM_APIC_ID			0x020U
#define VMM_SVM_APIC_VERSION		0x030U
#define VMM_SVM_APIC_TPR			0x080U
#define VMM_SVM_APIC_EOI			0x0b0U
#define VMM_SVM_APIC_LDR			0x0d0U
#define VMM_SVM_APIC_DFR			0x0e0U
#define VMM_SVM_APIC_SVR			0x0f0U
#define VMM_SVM_APIC_IRR_BASE		0x200U
#define VMM_SVM_APIC_ESR			0x280U
#define VMM_SVM_APIC_ICR_LOW		0x300U
#define VMM_SVM_APIC_ICR_HIGH		0x310U
#define VMM_SVM_APIC_LVTT			0x320U
#define VMM_SVM_APIC_LVT_THERMAL		0x330U
#define VMM_SVM_APIC_LVT_PERF			0x340U
#define VMM_SVM_APIC_LVT0			0x350U
#define VMM_SVM_APIC_LVT1			0x360U
#define VMM_SVM_APIC_LVT_ERROR		0x370U
#define VMM_SVM_APIC_TMICT			0x380U
#define VMM_SVM_APIC_TMCCT			0x390U
#define VMM_SVM_APIC_TDCR			0x3e0U
#define VMM_SVM_APIC_ISR_BASE			0x100U

#define VMM_SVM_APIC_VERSION_VALUE	0x00140014U
#define VMM_SVM_APIC_SVR_ENABLE	__BIT(8)
#define VMM_SVM_APIC_SVR_VALID		0x000003ffU
#define VMM_SVM_APIC_DFR_CLUSTER	0x0fffffffU
#define VMM_SVM_APIC_DFR_FLAT		0xffffffffU
#define VMM_SVM_APIC_LVT_VECTOR_MASK	0x000000ffU
#define VMM_SVM_APIC_LVT_DELIVERY_MASK	0x00000700U
#define VMM_SVM_APIC_LVT_SEND_PENDING	0x00001000U
#define VMM_SVM_APIC_LVT_EXTINT		0x00000700U
#define VMM_SVM_APIC_LVT_POLARITY		0x00002000U
#define VMM_SVM_APIC_LVT_REMOTE_IRR		0x00004000U
#define VMM_SVM_APIC_LVT_LEVEL		0x00008000U
#define VMM_SVM_APIC_LVT_MASKED		0x00010000U
#define VMM_SVM_APIC_LVT_TIMER_MODE		0x00060000U
#define VMM_SVM_APIC_LVT_TIMER_ONESHOT		0x00000000U
#define VMM_SVM_APIC_LVT_TIMER_PERIODIC	0x00020000U
#define VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE	0x00040000U
#define VMM_SVM_APIC_LVT_COMMON_VALID	\
	(VMM_SVM_APIC_LVT_VECTOR_MASK | VMM_SVM_APIC_LVT_SEND_PENDING | \
	 VMM_SVM_APIC_LVT_MASKED)
#define VMM_SVM_APIC_LVT_TIMER_VALID	\
	(VMM_SVM_APIC_LVT_COMMON_VALID | VMM_SVM_APIC_LVT_TIMER_MODE)
#define VMM_SVM_APIC_LVT_DELIVERY_VALID	\
	(VMM_SVM_APIC_LVT_COMMON_VALID | VMM_SVM_APIC_LVT_DELIVERY_MASK)
#define VMM_SVM_APIC_LVT_LINT_VALID	\
	(VMM_SVM_APIC_LVT_COMMON_VALID | VMM_SVM_APIC_LVT_DELIVERY_MASK | \
	 VMM_SVM_APIC_LVT_POLARITY | VMM_SVM_APIC_LVT_REMOTE_IRR | \
	 VMM_SVM_APIC_LVT_LEVEL)
#define VMM_SVM_APIC_TIMER_DIVIDE_VALID	0x0000000bU

#define VMM_SVM_APIC_ICR_DELIVERY_MASK	0x00000700U
#define VMM_SVM_APIC_ICR_FIXED		0x00000000U
#define VMM_SVM_APIC_ICR_NMI		0x00000400U
#define VMM_SVM_APIC_ICR_INIT		0x00000500U
#define VMM_SVM_APIC_ICR_SIPI		0x00000600U
#define VMM_SVM_APIC_ICR_DEST_LOGICAL	__BIT(11)
#define VMM_SVM_APIC_ICR_SHORTHAND	0x000c0000U
#define VMM_SVM_APIC_ICR_SELF		0x00040000U
#define VMM_SVM_APIC_ICR_ALL_SELF	0x00080000U
#define VMM_SVM_APIC_ICR_ALL_EXC_SELF	0x000c0000U
#define VMM_SVM_APIC_ICR_DEST_BROADCAST	0xffU
#define VMM_SVM_APIC_ICR_X2_DEST_BROADCAST	0xffffffffU

#define VMM_SVM_MSI_ADDRESS_BASE		0xfee00000ULL
#define VMM_SVM_MSI_ADDRESS_DEST_MASK		0x000ff000ULL
#define VMM_SVM_MSI_ADDRESS_DEST_LOGICAL	__BIT(2)
#define VMM_SVM_MSI_ADDRESS_REDIRECTION_HINT	__BIT(3)
#define VMM_SVM_MSI_ADDRESS_CONTROL_MASK	(VMM_SVM_MSI_ADDRESS_DEST_LOGICAL | \
	VMM_SVM_MSI_ADDRESS_REDIRECTION_HINT)
#define VMM_SVM_MSI_DATA_VECTOR_MASK		0x000000ffU
#define VMM_SVM_MSI_DATA_DELIVERY_MASK		0x00000700U
#define VMM_SVM_MSI_DATA_LEVEL_ASSERT		__BIT(14)
#define VMM_SVM_MSI_DATA_TRIGGER_LEVEL		__BIT(15)
#define VMM_SVM_MSI_DATA_ALLOWED		(VMM_SVM_MSI_DATA_VECTOR_MASK | \
	VMM_SVM_MSI_DATA_DELIVERY_MASK | VMM_SVM_MSI_DATA_LEVEL_ASSERT | \
	VMM_SVM_MSI_DATA_TRIGGER_LEVEL)

#define VMM_SVM_EXIT_AVIC_INCOMPLETE_IPI	0x0401ULL
#define VMM_SVM_EXIT_AVIC_NOACCEL		0x0402ULL
#define VMM_SVM_EXIT_IRET			0x0074ULL
#define VMM_SVM_EXIT_HLT			0x0078ULL
#define VMM_SVM_AVIC_IPI_INVALID_INT_TYPE	0U
#define VMM_SVM_AVIC_IPI_TARGET_NOT_RUNNING	1U
#define VMM_SVM_AVIC_IPI_INVALID_TARGET	2U
#define VMM_SVM_AVIC_IPI_INVALID_BACKING_PAGE	3U
#define VMM_SVM_AVIC_IPI_INVALID_VECTOR	4U
#define VMM_SVM_AVIC_NOACCEL_WRITE		__BIT(0)
#define VMM_SVM_AVIC_NOACCEL_OFFSET		0xff0U

struct vmm_svm_interrupt_machine {
	struct lwkt_token token;
	struct vmspace *vmspace;
	vm_page_t access_page;
	paddr_t access_page_pa;
	uint64_t *physical_table;
	paddr_t physical_table_pa;
	uint32_t *logical_table;
	paddr_t logical_table_pa;
	bool access_page_mapped;
	bool avic;
	uint32_t ioapic_select;
	uint32_t ioapic_id;
	uint64_t ioapic_redir[VMM_SVM_IOAPIC_PINS];
	bool ioapic_level[VMM_SVM_IOAPIC_PINS];
	struct vmm_svm_interrupt_vcpu *targets[VMM_SVM_AVIC_MAX_PHYS_ID + 1];
};

struct vmm_svm_interrupt_vcpu {
	struct vmm_svm_interrupt_machine *machine;
	struct vmm_vcpu *vcpu;
	void *apic_page;
	paddr_t apic_page_pa;
	uint32_t apic_id;
	uint64_t apic_base;
	volatile int host_cpu;
	volatile int host_apic_id;
	volatile int running;
	bool delivery_pending;
	uint8_t delivery_vector;
	volatile u_int init_pending;
	volatile u_int sipi_pending;
	volatile u_int sipi_vector;
	volatile u_int nmi_pending;
	bool nmi_delivery_pending;
	bool wait_sipi;
	/* timer_token protects the LAPIC timer fields and timer_callout. */
	struct lwkt_token timer_token;
	struct callout timer_callout;
	uint32_t timer_lvtt;
	uint32_t timer_tmict;
	uint32_t timer_tdcr;
	uint32_t timer_divisor;
	uint64_t timer_interval_tsc;
	uint64_t timer_deadline_tsc;
	bool timer_active;
};

static int vmm_svm_interrupt_raise_legacy(
    struct vmm_svm_interrupt_machine *, uint8_t);
static int vmm_svm_interrupt_machine_get_ioapic(
    struct vmm_svm_interrupt_machine *, struct vmm_ioapic_state *);
static int vmm_svm_interrupt_machine_set_ioapic(
    struct vmm_svm_interrupt_machine *, const struct vmm_ioapic_state *);
static int vmm_svm_avic_machine_create(struct vmm_machine *,
    struct vmm_svm_interrupt_machine **);
static int vmm_svm_avic_machine_enable(struct vmm_svm_interrupt_machine *);
static int vmm_svm_avic_irq_raise_msi(struct vmm_svm_interrupt_machine *,
    uint64_t, uint32_t);
static int vmm_svm_avic_irq_set(struct vmm_svm_interrupt_machine *, uint32_t,
    bool);
static int vmm_svm_avic_vcpu_mmio(struct vmm_svm_interrupt_vcpu *, uint64_t,
    bool, uint32_t *);
static int vmm_svm_avic_vcpu_io(struct vmm_vcpu *,
    const struct vmm_cpuexit_io *);
static int vmm_svm_avic_vcpu_msr(struct vmm_svm_interrupt_vcpu *, bool,
    uint32_t, uint64_t *);
static void vmm_svm_avic_machine_destroy(struct vmm_svm_interrupt_machine *);
static int vmm_svm_avic_vcpu_create(struct vmm_svm_interrupt_machine *,
    struct vmm_vcpu *, struct vmm_svm_interrupt_vcpu **,
    struct vmm_svm_interrupt_config *);
static void vmm_svm_avic_vcpu_destroy(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_avic_vcpu_reset_lapic(
    struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_avic_vcpu_reset(struct vmm_svm_interrupt_vcpu *);
static int vmm_svm_avic_vcpu_prepare(struct vmm_svm_interrupt_vcpu *);
static bool vmm_svm_avic_vcpu_runnable(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_avic_vcpu_enter(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_avic_vcpu_leave(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_avic_vcpu_event_result(
    struct vmm_svm_interrupt_vcpu *, bool);
static int vmm_svm_avic_vcpu_exit(struct vmm_svm_interrupt_vcpu *, uint64_t,
    uint64_t, uint64_t);
static void vmm_svm_avic_logical_update_locked(
    struct vmm_svm_interrupt_vcpu *);
static int vmm_svm_avic_route_icr(struct vmm_svm_interrupt_vcpu *, uint32_t,
    uint32_t, int);
static void vmm_svm_avic_notify_posted(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_avic_deliver(struct vmm_svm_interrupt_vcpu *, uint8_t);
static bool vmm_svm_avic_lapic_enabled(
    const struct vmm_svm_interrupt_vcpu *);
static bool vmm_svm_avic_lapic_accepts_pic(
    const struct vmm_svm_interrupt_vcpu *);
static uint8_t vmm_svm_avic_lapic_ppr(struct vmm_svm_interrupt_vcpu *);
static uint32_t vmm_svm_avic_read(const struct vmm_svm_interrupt_vcpu *,
    uint32_t);
static void vmm_svm_avic_write(const struct vmm_svm_interrupt_vcpu *,
    uint32_t, uint32_t);
static int vmm_svm_lapic_read(struct vmm_svm_interrupt_vcpu *, uint32_t,
    uint32_t *);
static int vmm_svm_lapic_write(struct vmm_svm_interrupt_vcpu *, uint32_t,
    uint32_t);
static int vmm_svm_interrupt_vcpu_get_lapic(
    struct vmm_svm_interrupt_vcpu *, void *, size_t);
static int vmm_svm_interrupt_vcpu_set_lapic(
    struct vmm_svm_interrupt_vcpu *, const void *, size_t);
static void vmm_svm_lapic_timer_arm(struct vmm_svm_interrupt_vcpu *,
    uint32_t);
static void vmm_svm_lapic_timer_arm_locked(
    struct vmm_svm_interrupt_vcpu *, uint32_t);
static void vmm_svm_lapic_timer_sync(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_lapic_timer_check(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_lapic_timer_timeout(void *);
static bool vmm_svm_lapic_timer_expire_locked(
    struct vmm_svm_interrupt_vcpu *, uint64_t);
static void vmm_svm_lapic_timer_schedule_locked(
    struct vmm_svm_interrupt_vcpu *, uint64_t);
static int vmm_svm_lapic_eoi(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_ioapic_deliver_locked(
    struct vmm_svm_interrupt_machine *, uint32_t);
static void vmm_svm_ioapic_reassert(struct vmm_svm_interrupt_machine *,
    uint8_t);

const struct vmm_svm_interrupt_ops vmm_svm_avic_interrupt_ops = {
	.name = "avic",
	.machine_create = vmm_svm_avic_machine_create,
	.machine_enable = vmm_svm_avic_machine_enable,
	.irq_raise_msi = vmm_svm_avic_irq_raise_msi,
	.irq_set = vmm_svm_avic_irq_set,
	.irq_raise_legacy = vmm_svm_interrupt_raise_legacy,
	.machine_get_ioapic = vmm_svm_interrupt_machine_get_ioapic,
	.machine_set_ioapic = vmm_svm_interrupt_machine_set_ioapic,
	.vcpu_mmio = vmm_svm_avic_vcpu_mmio,
	.vcpu_io = vmm_svm_avic_vcpu_io,
	.vcpu_msr = vmm_svm_avic_vcpu_msr,
	.machine_destroy = vmm_svm_avic_machine_destroy,
	.vcpu_create = vmm_svm_avic_vcpu_create,
	.vcpu_get_lapic = vmm_svm_interrupt_vcpu_get_lapic,
	.vcpu_set_lapic = vmm_svm_interrupt_vcpu_set_lapic,
	.vcpu_destroy = vmm_svm_avic_vcpu_destroy,
	.vcpu_prepare = vmm_svm_avic_vcpu_prepare,
	.vcpu_runnable = vmm_svm_avic_vcpu_runnable,
	.vcpu_enter = vmm_svm_avic_vcpu_enter,
	.vcpu_leave = vmm_svm_avic_vcpu_leave,
	.vcpu_event_result = vmm_svm_avic_vcpu_event_result,
	/* Re-enter after an interrupt-window exit to inject a pending PIC IRQ. */
	.vintr_internal = true,
	.vcpu_exit = vmm_svm_avic_vcpu_exit,
};

bool
vmm_svm_avic_available(void)
{
	cpuid_desc_t desc;
	uint32_t family;
	uint32_t cpu;

	x86_get_cpuid(0x8000000a, &desc);
	if ((desc.edx & CPUID_8_0A_EDX_AVIC) == 0)
		return false;
	for (cpu = 0; cpu < ncpus; ++cpu) {
		if ((CPUID_TO_APICID(cpu) & ~VMM_SVM_AVIC_HOST_APIC_ID_MASK) != 0)
			return false;
	}
	x86_get_cpuid(1, &desc);
	family = (desc.eax >> 8) & 0x0fU;
	if (family == 0x0fU)
		family += (desc.eax >> 20) & 0xffU;
	/*
	 * Family 17h/18h erratum 1235 can lose the sender's incomplete-IPI
	 * wakeup.  Keep AVIC posted interrupts, but clear IsRunning to force
	 * guest IPIs through incomplete-IPI exits on the affected families.
	 *
	 * No Zen3+ machine is available for local validation.  Direct IPI
	 * virtualization intentionally remains enabled there until a
	 * family-specific defect is demonstrated.
	 */
	vmm_svm_avic_ipi_enabled = family != 0x17U && family != 0x18U;
	if (!vmm_svm_avic_ipi_enabled) {
		kprintf("vmm: AVIC direct IPI disabled for AMD family %#x "
		    "(erratum 1235)\n", family);
	}
	return true;
}

static int
vmm_svm_interrupt_raise_legacy(struct vmm_svm_interrupt_machine *machine,
    uint8_t vector)
{
	struct vmm_svm_interrupt_vcpu *target;

	(void)vector;
	lwkt_gettoken(&machine->token);
	target = machine->targets[0];
	if (target == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOENT;
	}
	vmm_vcpu_interrupt(target->vcpu);
	lwkt_reltoken(&machine->token);
	return 0;
}

static int
vmm_svm_interrupt_machine_get_ioapic(
	struct vmm_svm_interrupt_machine *machine, struct vmm_ioapic_state *state)
{
	uint32_t pin;

	if (machine == NULL || state == NULL)
		return EINVAL;
	bzero(state, sizeof(*state));
	state->base = VMM_IOAPIC_BASE;
	lwkt_gettoken(&machine->token);
	state->select = machine->ioapic_select;
	state->id = machine->ioapic_id;
	for (pin = 0; pin < VMM_IOAPIC_PIN_COUNT; ++pin) {
		state->redir[pin] = machine->ioapic_redir[pin];
		if (machine->ioapic_level[pin])
			state->irr |= 1U << pin;
	}
	lwkt_reltoken(&machine->token);
	return 0;
}

static int
vmm_svm_interrupt_machine_set_ioapic(
	struct vmm_svm_interrupt_machine *machine,
	const struct vmm_ioapic_state *state)
{
	uint32_t pin;

	if (machine == NULL || state == NULL || state->base != VMM_IOAPIC_BASE ||
	    state->id > 0x0fU)
		return EINVAL;
	lwkt_gettoken(&machine->token);
	machine->ioapic_select = state->select & 0xffU;
	machine->ioapic_id = state->id;
	for (pin = 0; pin < VMM_IOAPIC_PIN_COUNT; ++pin) {
		machine->ioapic_redir[pin] = state->redir[pin];
		machine->ioapic_level[pin] = (state->irr & (1U << pin)) != 0;
		if (machine->ioapic_level[pin])
			vmm_svm_ioapic_deliver_locked(machine, pin);
	}
	lwkt_reltoken(&machine->token);
	return 0;
}

/* Caller holds machine->token. */
static void
vmm_svm_ioapic_deliver_locked(struct vmm_svm_interrupt_machine *machine,
    uint32_t pin)
{
	struct vmm_svm_interrupt_vcpu *target;
	uint64_t entry;
	uint32_t low;
	uint32_t destination;
	uint32_t id;

	entry = machine->ioapic_redir[pin];
	low = entry;
	if ((low & VMM_SVM_IOAPIC_REDIR_MASKED) != 0 ||
	    (low & VMM_SVM_IOAPIC_REDIR_DELIVERY_MASK) !=
	    VMM_SVM_IOAPIC_REDIR_FIXED || (low & 0xffU) < 32 ||
	    ((low & VMM_SVM_IOAPIC_REDIR_LEVEL) != 0 &&
	    (low & VMM_SVM_IOAPIC_REDIR_REMOTE_IRR) != 0))
		return;
	destination = entry >> 56;
	for (id = 0; id <= VMM_SVM_AVIC_MAX_PHYS_ID; ++id) {
		target = machine->targets[id];
		if (target == NULL)
			continue;
		if ((low & VMM_SVM_IOAPIC_REDIR_DEST_LOGICAL) != 0) {
			if ((destination &
			    (vmm_svm_avic_read(target, VMM_SVM_APIC_LDR) >> 24)) == 0)
				continue;
		} else if (destination != target->apic_id) {
			continue;
		}
		if (!vmm_svm_avic_lapic_enabled(target))
			continue;
		vmm_svm_avic_deliver(target, low & 0xffU);
		if ((low & VMM_SVM_IOAPIC_REDIR_LEVEL) != 0)
			machine->ioapic_redir[pin] |= VMM_SVM_IOAPIC_REDIR_REMOTE_IRR;
	}
}

static void
vmm_svm_ioapic_reassert(struct vmm_svm_interrupt_machine *machine,
    uint8_t vector)
{
	uint32_t pin;

	lwkt_gettoken(&machine->token);
	for (pin = 0; pin < VMM_SVM_IOAPIC_PINS; ++pin) {
		if (!machine->ioapic_level[pin] ||
		    (machine->ioapic_redir[pin] & VMM_SVM_IOAPIC_REDIR_LEVEL) == 0 ||
		    (machine->ioapic_redir[pin] & 0xffU) != vector)
			continue;
		machine->ioapic_redir[pin] &= ~VMM_SVM_IOAPIC_REDIR_REMOTE_IRR;
		vmm_svm_ioapic_deliver_locked(machine, pin);
	}
	lwkt_reltoken(&machine->token);
}

/* Caller holds machine->token. */
static bool
vmm_svm_ioapic_scan_eoi_locked(struct vmm_svm_interrupt_machine *machine)
{
	struct vmm_svm_interrupt_vcpu *target;
	uint64_t entry;
	uint32_t destination;
	uint32_t pending;
	uint32_t id;
	uint32_t word;
	uint32_t bit;
	uint32_t pin;
	uint8_t vector;
	bool redelivered;

	redelivered = false;
	for (pin = 0; pin < VMM_SVM_IOAPIC_PINS; ++pin) {
		entry = machine->ioapic_redir[pin];
		if (!machine->ioapic_level[pin] ||
		    (entry & (VMM_SVM_IOAPIC_REDIR_LEVEL |
		    VMM_SVM_IOAPIC_REDIR_REMOTE_IRR)) !=
		    (VMM_SVM_IOAPIC_REDIR_LEVEL |
		    VMM_SVM_IOAPIC_REDIR_REMOTE_IRR))
			continue;
		vector = entry & 0xffU;
		word = vector / 32;
		bit = __BIT(vector & 31);
		destination = entry >> 56;
		pending = 0;
		for (id = 0; id <= VMM_SVM_AVIC_MAX_PHYS_ID; ++id) {
			target = machine->targets[id];
			if (target == NULL)
				continue;
			if ((entry & VMM_SVM_IOAPIC_REDIR_DEST_LOGICAL) != 0) {
				if ((destination &
				    (vmm_svm_avic_read(target, VMM_SVM_APIC_LDR) >> 24)) == 0)
					continue;
			} else if (destination != target->apic_id) {
				continue;
			}
			pending = atomic_load_acq_int((volatile u_int *)
			    ((uint8_t *)target->apic_page + VMM_SVM_APIC_IRR_BASE +
			    word * 0x10)) |
			    atomic_load_acq_int((volatile u_int *)
			    ((uint8_t *)target->apic_page + VMM_SVM_APIC_ISR_BASE +
			    word * 0x10));
			if ((pending & bit) != 0)
				break;
		}
		if ((pending & bit) != 0)
			continue;
		machine->ioapic_redir[pin] =
		    entry & ~VMM_SVM_IOAPIC_REDIR_REMOTE_IRR;
		vmm_svm_ioapic_deliver_locked(machine, pin);
		if ((machine->ioapic_redir[pin] &
		    VMM_SVM_IOAPIC_REDIR_REMOTE_IRR) != 0)
			redelivered = true;
	}
	return redelivered;
}

static uint32_t
vmm_svm_avic_read(const struct vmm_svm_interrupt_vcpu *vcpu, uint32_t reg)
{
	volatile uint32_t *value;

	value = (volatile uint32_t *)((uint8_t *)vcpu->apic_page + reg);
	return *value;
}

int
vmm_svm_avic_read_register(struct vmm_svm_interrupt_vcpu *vcpu,
    uint32_t reg, uint32_t *value)
{
	if (vcpu == NULL || value == NULL ||
	    reg > PAGE_SIZE - sizeof(*value) || (reg & 3) != 0)
		return EINVAL;
	return vmm_svm_lapic_read(vcpu, reg, value);
}

static int
vmm_svm_interrupt_vcpu_get_lapic(struct vmm_svm_interrupt_vcpu *vcpu,
    void *registers, size_t size)
{
	uint8_t *state;
	uint32_t value;
	uint32_t reg;
	int error;

	if (vcpu == NULL || registers == NULL || size != 0x400U)
		return EINVAL;
	state = registers;
	for (reg = 0; reg < size; reg += sizeof(value)) {
		error = vmm_svm_lapic_read(vcpu, reg, &value);
		if (error != 0)
			return error;
		bcopy(&value, state + reg, sizeof(value));
	}
	return 0;
}

static int
vmm_svm_interrupt_vcpu_set_lapic(struct vmm_svm_interrupt_vcpu *vcpu,
    const void *registers, size_t size)
{
	const uint8_t *state;
	uint32_t divide;
	uint32_t value;

	if (vcpu == NULL || registers == NULL || size != 0x400U)
		return EINVAL;
	state = registers;
	lwkt_gettoken(&vcpu->timer_token);
	bcopy(state, vcpu->apic_page, size);
	vmm_svm_avic_write(vcpu, VMM_SVM_APIC_ID, vcpu->apic_id << 24);
	vmm_svm_avic_write(vcpu, VMM_SVM_APIC_VERSION,
	    VMM_SVM_APIC_VERSION_VALUE);
	vcpu->timer_lvtt = vmm_svm_avic_read(vcpu, VMM_SVM_APIC_LVTT) &
	    VMM_SVM_APIC_LVT_TIMER_VALID;
	vmm_svm_avic_write(vcpu, VMM_SVM_APIC_LVTT, vcpu->timer_lvtt);
	vcpu->timer_tdcr = vmm_svm_avic_read(vcpu, VMM_SVM_APIC_TDCR) &
	    VMM_SVM_APIC_TIMER_DIVIDE_VALID;
	vmm_svm_avic_write(vcpu, VMM_SVM_APIC_TDCR, vcpu->timer_tdcr);
	divide = ((vcpu->timer_tdcr & 0x3U) |
	    ((vcpu->timer_tdcr & 0x8U) >> 1)) + 1;
	vcpu->timer_divisor = 1U << (divide & 0x7U);
	value = vmm_svm_avic_read(vcpu, VMM_SVM_APIC_TMICT);
	vmm_svm_lapic_timer_arm_locked(vcpu, value);
	lwkt_reltoken(&vcpu->timer_token);
	if (vcpu->machine->logical_table != NULL) {
		lwkt_gettoken(&vcpu->machine->token);
		vmm_svm_avic_logical_update_locked(vcpu);
		lwkt_reltoken(&vcpu->machine->token);
	}
	return 0;
}

static int
vmm_svm_avic_vcpu_msr(struct vmm_svm_interrupt_vcpu *vcpu, bool write,
    uint32_t msr, uint64_t *value)
{
	if (msr != VMM_SVM_MSR_APICBASE || value == NULL)
		return ENOENT;
	if (!write) {
		*value = vcpu->apic_base;
		return 0;
	}
	if ((*value & ~VMM_SVM_AVIC_APICBASE_VALID) != 0 ||
	    (*value & VMM_SVM_APICBASE_ADDRESS) != VMM_SVM_AVIC_APIC_BASE)
		return EINVAL;
	vcpu->apic_base = *value;
	return 0;
}

static void
vmm_svm_avic_write(const struct vmm_svm_interrupt_vcpu *vcpu, uint32_t reg,
    uint32_t value)
{
	volatile uint32_t *slot;

	slot = (volatile uint32_t *)((uint8_t *)vcpu->apic_page + reg);
	*slot = value;
}

static void
vmm_svm_lapic_timer_arm(struct vmm_svm_interrupt_vcpu *vcpu,
    uint32_t count)
{
	lwkt_gettoken(&vcpu->timer_token);
	vmm_svm_lapic_timer_arm_locked(vcpu, count);
	lwkt_reltoken(&vcpu->timer_token);
}

/* Caller holds vcpu->timer_token. */
static void
vmm_svm_lapic_timer_arm_locked(struct vmm_svm_interrupt_vcpu *vcpu,
    uint32_t count)
{
	uint64_t delta;
	uint64_t now;

	callout_stop_async(&vcpu->timer_callout);
	vcpu->timer_tmict = count;
	vmm_svm_avic_write(vcpu, VMM_SVM_APIC_TMICT, count);
	vmm_svm_avic_write(vcpu, VMM_SVM_APIC_TMCCT, count);
	if (count == 0 ||
	    (vcpu->timer_lvtt & VMM_SVM_APIC_LVT_TIMER_MODE) ==
	    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE || tsc_frequency == 0) {
		vcpu->timer_active = false;
		vcpu->timer_interval_tsc = 0;
		vcpu->timer_deadline_tsc = 0;
		return;
	}
	delta = ((uint64_t)count * vcpu->timer_divisor * tsc_frequency +
	    10000000U - 1) / 10000000U;
	if (delta == 0)
		delta = 1;
	now = rdtsc();
	vcpu->timer_interval_tsc = delta;
	vcpu->timer_deadline_tsc = UINT64_MAX - now < delta ?
	    UINT64_MAX : now + delta;
	vcpu->timer_active = true;
	vmm_svm_lapic_timer_schedule_locked(vcpu, now);
}

/*
 * AVIC maps the LAPIC access page directly into the guest.  Timer-register
 * writes therefore do not VMEXIT and must be sampled before VMRUN.
 */
static void
vmm_svm_lapic_timer_sync(struct vmm_svm_interrupt_vcpu *vcpu)
{
	uint32_t divide;
	uint32_t lvtt;
	uint32_t tdcr;
	uint32_t tmict;
	bool changed;
	bool rearm;

	lwkt_gettoken(&vcpu->timer_token);
	rearm = false;
	tdcr = vmm_svm_avic_read(vcpu, VMM_SVM_APIC_TDCR) &
	    VMM_SVM_APIC_TIMER_DIVIDE_VALID;
	if (tdcr != vcpu->timer_tdcr) {
		vcpu->timer_tdcr = tdcr;
		divide = ((tdcr & 0x3U) | ((tdcr & 0x8U) >> 1)) + 1;
		vcpu->timer_divisor = 1U << (divide & 0x7U);
		rearm = true;
	}
	lvtt = vmm_svm_avic_read(vcpu, VMM_SVM_APIC_LVTT) &
	    VMM_SVM_APIC_LVT_TIMER_VALID;
	if (lvtt != vcpu->timer_lvtt) {
		vcpu->timer_lvtt = lvtt;
		rearm = true;
	}
	tmict = vmm_svm_avic_read(vcpu, VMM_SVM_APIC_TMICT);
	changed = tmict != vcpu->timer_tmict || (rearm && tmict != 0);
	if (changed)
		vmm_svm_lapic_timer_arm_locked(vcpu, tmict);
	lwkt_reltoken(&vcpu->timer_token);
}

/* Caller holds vcpu->timer_token. */
static void
vmm_svm_lapic_timer_schedule_locked(struct vmm_svm_interrupt_vcpu *vcpu,
    uint64_t now)
{
	uint64_t delay;
	uint64_t callout_ticks;

	if (!vcpu->timer_active || vcpu->timer_deadline_tsc <= now ||
	    tsc_frequency == 0) {
		callout_stop_async(&vcpu->timer_callout);
		return;
	}
	delay = vcpu->timer_deadline_tsc - now;
	callout_ticks = (delay * hz + tsc_frequency - 1) / tsc_frequency;
	if (callout_ticks > INT_MAX)
		callout_ticks = INT_MAX;
	callout_reset(&vcpu->timer_callout, (int)MAX(callout_ticks, 1),
	    vmm_svm_lapic_timer_timeout, vcpu);
}

/* Caller holds vcpu->timer_token. */
static bool
vmm_svm_lapic_timer_expire_locked(struct vmm_svm_interrupt_vcpu *vcpu,
    uint64_t now)
{
	uint64_t periods;

	if (!vcpu->timer_active || now < vcpu->timer_deadline_tsc)
		return false;
	vmm_svm_avic_write(vcpu, VMM_SVM_APIC_TMCCT, 0);
	if ((vcpu->timer_lvtt & VMM_SVM_APIC_LVT_TIMER_MODE) ==
	    VMM_SVM_APIC_LVT_TIMER_PERIODIC && vcpu->timer_interval_tsc != 0 &&
	    vcpu->timer_tmict != 0) {
		periods = (now - vcpu->timer_deadline_tsc) /
	    vcpu->timer_interval_tsc + 1;
		if (periods > (UINT64_MAX - vcpu->timer_deadline_tsc) /
		    vcpu->timer_interval_tsc)
			vcpu->timer_deadline_tsc = UINT64_MAX;
		else
		vcpu->timer_deadline_tsc += periods *
		    vcpu->timer_interval_tsc;
		vmm_svm_avic_write(vcpu, VMM_SVM_APIC_TMCCT,
		    vcpu->timer_tmict);
	} else {
		vcpu->timer_active = false;
	}
	return true;
}

static void
vmm_svm_lapic_timer_timeout(void *argument)
{
	struct vmm_svm_interrupt_vcpu *vcpu;
	uint64_t now;
	uint8_t vector;
	bool deliver;

	vcpu = argument;
	now = rdtsc();
	lwkt_gettoken(&vcpu->timer_token);
	deliver = vmm_svm_lapic_timer_expire_locked(vcpu, now);
	if (vcpu->timer_active)
		vmm_svm_lapic_timer_schedule_locked(vcpu, now);
	vector = vcpu->timer_lvtt & VMM_SVM_APIC_LVT_VECTOR_MASK;
	if ((vcpu->timer_lvtt & VMM_SVM_APIC_LVT_MASKED) != 0 || vector < 32)
		deliver = false;
	lwkt_reltoken(&vcpu->timer_token);
	if (deliver)
		vmm_svm_avic_deliver(vcpu, vector);
}

static void
vmm_svm_lapic_timer_check(struct vmm_svm_interrupt_vcpu *vcpu)
{
	uint64_t now;
	uint8_t vector;
	bool deliver;

	now = rdtsc();
	lwkt_gettoken(&vcpu->timer_token);
	deliver = vmm_svm_lapic_timer_expire_locked(vcpu, now);
	if (vcpu->timer_active)
		vmm_svm_lapic_timer_schedule_locked(vcpu, now);
	vector = vcpu->timer_lvtt & VMM_SVM_APIC_LVT_VECTOR_MASK;
	if ((vcpu->timer_lvtt & VMM_SVM_APIC_LVT_MASKED) != 0 || vector < 32)
		deliver = false;
	lwkt_reltoken(&vcpu->timer_token);
	if (deliver)
		vmm_svm_avic_deliver(vcpu, vector);
}

static int
vmm_svm_lapic_eoi(struct vmm_svm_interrupt_vcpu *vcpu)
{
	volatile uint32_t *isr;
	int word;
	int bit;

	for (word = 7; word >= 0; --word) {
		isr = (volatile uint32_t *)((uint8_t *)vcpu->apic_page +
		    VMM_SVM_APIC_ISR_BASE + word * 0x10);
		if (*isr == 0)
			continue;
		bit = fls(*isr) - 1;
		atomic_clear_int((volatile u_int *)isr, __BIT(bit));
		vmm_svm_ioapic_reassert(vcpu->machine, word * 32 + bit);
		return word * 32 + bit;
	}
	return -1;
}

static int
vmm_svm_lapic_read(struct vmm_svm_interrupt_vcpu *vcpu, uint32_t reg,
    uint32_t *value)
{
	uint64_t now;
	uint8_t vector;
	bool deliver;

	if (reg > PAGE_SIZE - sizeof(*value) || (reg & 3) != 0)
		return EINVAL;
	if (reg != VMM_SVM_APIC_TMCCT) {
		*value = vmm_svm_avic_read(vcpu, reg);
		return 0;
	}
	now = rdtsc();
	lwkt_gettoken(&vcpu->timer_token);
	deliver = vmm_svm_lapic_timer_expire_locked(vcpu, now);
	if (vcpu->timer_active)
		vmm_svm_lapic_timer_schedule_locked(vcpu, now);
	if (vcpu->timer_active && vcpu->timer_interval_tsc != 0 &&
	    now < vcpu->timer_deadline_tsc)
		*value = (uint32_t)((uint64_t)vcpu->timer_tmict *
		    (vcpu->timer_deadline_tsc - now) /
		    vcpu->timer_interval_tsc);
	else
		*value = vmm_svm_avic_read(vcpu, reg);
	vector = vcpu->timer_lvtt & VMM_SVM_APIC_LVT_VECTOR_MASK;
	if ((vcpu->timer_lvtt & VMM_SVM_APIC_LVT_MASKED) != 0 || vector < 32)
		deliver = false;
	lwkt_reltoken(&vcpu->timer_token);
	if (deliver)
		vmm_svm_avic_deliver(vcpu, vector);
	return 0;
}

static int
vmm_svm_lapic_write(struct vmm_svm_interrupt_vcpu *vcpu, uint32_t reg,
    uint32_t value)
{
	uint32_t divide;

	switch (reg) {
	case VMM_SVM_APIC_TPR:
		value &= 0xffU;
		break;
	case VMM_SVM_APIC_LDR:
		value &= 0xff000000U;
		vmm_svm_avic_write(vcpu, reg, value);
		if (vcpu->machine->logical_table != NULL) {
			lwkt_gettoken(&vcpu->machine->token);
			vmm_svm_avic_logical_update_locked(vcpu);
			lwkt_reltoken(&vcpu->machine->token);
		}
		return 0;
	case VMM_SVM_APIC_DFR:
		vmm_svm_avic_write(vcpu, reg, value);
		if (vcpu->machine->logical_table != NULL) {
			lwkt_gettoken(&vcpu->machine->token);
			vmm_svm_avic_logical_update_locked(vcpu);
			lwkt_reltoken(&vcpu->machine->token);
		}
		return 0;
	case VMM_SVM_APIC_EOI:
		(void)vmm_svm_lapic_eoi(vcpu);
		return 0;
	case VMM_SVM_APIC_ESR:
		value = 0;
		break;
	case VMM_SVM_APIC_LVTT:
		value &= VMM_SVM_APIC_LVT_TIMER_VALID;
		lwkt_gettoken(&vcpu->timer_token);
		vcpu->timer_lvtt = value;
		vmm_svm_avic_write(vcpu, reg, value);
		vmm_svm_lapic_timer_arm_locked(vcpu, vcpu->timer_tmict);
		lwkt_reltoken(&vcpu->timer_token);
		return 0;
	case VMM_SVM_APIC_TMICT:
		vmm_svm_lapic_timer_arm(vcpu, value);
		return 0;
	case VMM_SVM_APIC_TDCR:
		value &= VMM_SVM_APIC_TIMER_DIVIDE_VALID;
		lwkt_gettoken(&vcpu->timer_token);
		vcpu->timer_tdcr = value;
		divide = ((value & 0x3U) | ((value & 0x8U) >> 1)) + 1;
		vcpu->timer_divisor = 1U << (divide & 0x7U);
		vmm_svm_avic_write(vcpu, reg, value);
		vmm_svm_lapic_timer_arm_locked(vcpu, vcpu->timer_tmict);
		lwkt_reltoken(&vcpu->timer_token);
		return 0;
	case VMM_SVM_APIC_SVR:
		value &= VMM_SVM_APIC_SVR_VALID;
		break;
	case VMM_SVM_APIC_LVT_ERROR:
	case VMM_SVM_APIC_LVT_THERMAL:
	case VMM_SVM_APIC_LVT_PERF:
	case VMM_SVM_APIC_LVT0:
	case VMM_SVM_APIC_LVT1:
		value &= VMM_SVM_APIC_LVT_COMMON_VALID |
		    VMM_SVM_APIC_LVT_DELIVERY_MASK | VMM_SVM_APIC_LVT_POLARITY |
		    VMM_SVM_APIC_LVT_LEVEL;
		break;
	default:
		return ENOENT;
	}
	vmm_svm_avic_write(vcpu, reg, value);
	return 0;
}

static int
vmm_svm_avic_machine_create(struct vmm_machine *machine,
    struct vmm_svm_interrupt_machine **result)
{
	struct vmm_svm_interrupt_machine *avic;
	int error;

	*result = NULL;
	avic = os_mem_zalloc(sizeof(*avic));
	if (avic == NULL)
		return ENOMEM;
	lwkt_token_init(&avic->token, "vmmavic");
	avic->vmspace = machine->vmspace;
	avic->avic = true;
	avic->ioapic_id = 1;
	for (uint32_t pin = 0; pin < VMM_SVM_IOAPIC_PINS; ++pin)
		avic->ioapic_redir[pin] = VMM_SVM_IOAPIC_REDIR_MASKED;

	avic->access_page = vm_page_alloc(NULL, (vm_pindex_t)ticks,
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_FORCE_ZERO);
	if (avic->access_page == NULL) {
		error = ENOMEM;
		goto fail;
	}
	avic->access_page->valid = VM_PAGE_BITS_ALL;
	vm_page_wire(avic->access_page);
	vm_page_wakeup(avic->access_page);
	avic->access_page_pa = VM_PAGE_TO_PHYS(avic->access_page);

	error = os_contigpa_zalloc(&avic->physical_table_pa,
	    (vaddr_t *)&avic->physical_table, 1);
	if (error != 0)
		goto fail;
	error = os_contigpa_zalloc(&avic->logical_table_pa,
	    (vaddr_t *)&avic->logical_table, 1);
	if (error != 0)
		goto fail;

	*result = avic;
	return 0;

fail:
	vmm_svm_avic_machine_destroy(avic);
	return error;
}

static int
vmm_svm_avic_machine_enable(struct vmm_svm_interrupt_machine *avic)
{
	if (avic == NULL)
		return EINVAL;
	if (avic->access_page_mapped)
		return EALREADY;

	/* The LAPIC access page is architectural guest physical address 0xfee00000. */
	pmap_enter(os_vmspace_pmap(avic->vmspace), VMM_SVM_AVIC_APIC_BASE,
	    avic->access_page, VM_PROT_READ | VM_PROT_WRITE, 0, NULL);
	avic->access_page_mapped = true;
	return 0;
}

static int
vmm_svm_avic_irq_raise_msi(struct vmm_svm_interrupt_machine *machine,
    uint64_t address, uint32_t data)
{
	struct vmm_svm_interrupt_vcpu *target;
	uint32_t destination;
	uint32_t id;
	uint32_t vector;
	bool bridge;
	bool logical;
	bool nmi;
	bool delivered;

	/* QEMU's in-kernel APIC bridge encodes only MSI destination bits. */
	bridge = (address & ~(VMM_SVM_MSI_ADDRESS_DEST_MASK |
	    VMM_SVM_MSI_ADDRESS_CONTROL_MASK)) == 0;
	if (((address & ~(VMM_SVM_MSI_ADDRESS_DEST_MASK |
	    VMM_SVM_MSI_ADDRESS_CONTROL_MASK)) != VMM_SVM_MSI_ADDRESS_BASE &&
	    !bridge) ||
	    (address & VMM_SVM_MSI_ADDRESS_REDIRECTION_HINT) != 0 ||
	    (data & ~(VMM_SVM_MSI_DATA_ALLOWED |
	    (bridge ? VMM_SVM_APIC_ICR_DEST_LOGICAL : 0))) != 0 ||
	    ((data & VMM_SVM_MSI_DATA_DELIVERY_MASK) != 0 &&
	    (data & VMM_SVM_MSI_DATA_DELIVERY_MASK) != VMM_SVM_APIC_ICR_NMI) ||
	    (data & VMM_SVM_MSI_DATA_TRIGGER_LEVEL) != 0)
		return EOPNOTSUPP;
	nmi = (data & VMM_SVM_MSI_DATA_DELIVERY_MASK) ==
	    VMM_SVM_APIC_ICR_NMI;
	vector = data & VMM_SVM_MSI_DATA_VECTOR_MASK;
	if (!nmi && vector < 32)
		return EINVAL;
	destination = (address & VMM_SVM_MSI_ADDRESS_DEST_MASK) >> 12;
	logical = (address & VMM_SVM_MSI_ADDRESS_DEST_LOGICAL) != 0;
	if (!logical && destination > VMM_SVM_AVIC_MAX_PHYS_ID)
		return ENOENT;
	delivered = false;
	lwkt_gettoken(&machine->token);
	if (!logical) {
		target = machine->targets[destination];
		if (target != NULL) {
			if (nmi) {
				atomic_set_int(&target->nmi_pending, 1);
				vmm_vcpu_interrupt(target->vcpu);
			} else {
				vmm_svm_avic_deliver(target, (uint8_t)vector);
			}
			delivered = true;
		}
	} else {
		for (id = 0; id <= VMM_SVM_AVIC_MAX_PHYS_ID; ++id) {
			target = machine->targets[id];
			if (target == NULL || (destination &
			    (vmm_svm_avic_read(target, VMM_SVM_APIC_LDR) >> 24)) == 0)
				continue;
			if (nmi) {
				atomic_set_int(&target->nmi_pending, 1);
				vmm_vcpu_interrupt(target->vcpu);
			} else {
				vmm_svm_avic_deliver(target, (uint8_t)vector);
			}
			delivered = true;
		}
	}
	lwkt_reltoken(&machine->token);
	return delivered ? 0 : ENOENT;
}

static int
vmm_svm_avic_irq_set(struct vmm_svm_interrupt_machine *machine,
    uint32_t gsi, bool level)
{
	if (gsi >= VMM_SVM_IOAPIC_PINS)
		return EINVAL;
	lwkt_gettoken(&machine->token);
	if (level == machine->ioapic_level[gsi]) {
		lwkt_reltoken(&machine->token);
		return 0;
	}
	machine->ioapic_level[gsi] = level;
	if (level)
		vmm_svm_ioapic_deliver_locked(machine, gsi);
	lwkt_reltoken(&machine->token);
	return 0;
}

static int
vmm_svm_avic_vcpu_io(struct vmm_vcpu *vcpu,
    const struct vmm_cpuexit_io *exit)
{
	int error;

	error = vmm_x64_pit_io(vcpu->machine, vcpu->state, exit);
	if (error != ENOENT)
		return error;
	return vmm_x64_pic_io(vcpu->machine, vcpu->state, exit);
}

static void
vmm_svm_avic_machine_destroy(struct vmm_svm_interrupt_machine *avic)
{
	if (avic == NULL)
		return;
	if (avic->access_page_mapped) {
		pmap_remove(os_vmspace_pmap(avic->vmspace), VMM_SVM_AVIC_APIC_BASE,
		    VMM_SVM_AVIC_APIC_BASE + PAGE_SIZE);
	}
	if (avic->logical_table != NULL) {
		os_contigpa_free(avic->logical_table_pa,
		    (vaddr_t)avic->logical_table, 1);
	}
	if (avic->physical_table != NULL) {
		os_contigpa_free(avic->physical_table_pa,
		    (vaddr_t)avic->physical_table, 1);
	}
	if (avic->access_page != NULL) {
		vm_page_busy_wait(avic->access_page, FALSE, "vmmavp");
		vm_page_unwire(avic->access_page, 0);
		vm_page_free(avic->access_page);
	}
	os_mem_free(avic, sizeof(*avic));
}

static int
vmm_svm_avic_vcpu_create(struct vmm_svm_interrupt_machine *machine,
    struct vmm_vcpu *vcpu, struct vmm_svm_interrupt_vcpu **result,
    struct vmm_svm_interrupt_config *config)
{
	struct vmm_svm_interrupt_vcpu *avic;
	uint32_t apic_id;
	int error;

	*result = NULL;
	bzero(config, sizeof(*config));
	if (!vcpu->machine->irqchip)
		return 0;
	apic_id = vcpu->id;
	if (apic_id > VMM_SVM_AVIC_MAX_PHYS_ID)
		return E2BIG;
	avic = os_mem_zalloc(sizeof(*avic));
	if (avic == NULL)
		return ENOMEM;
	lwkt_token_init(&avic->timer_token, "vmmatimer");
	callout_init_mp(&avic->timer_callout);
	avic->machine = machine;
	avic->vcpu = vcpu;
	avic->apic_id = apic_id;
	avic->apic_base = VMM_SVM_AVIC_APIC_BASE |
	    VMM_SVM_APICBASE_ENABLED;
	if (apic_id == 0)
		avic->apic_base |= VMM_SVM_APICBASE_BSP;
	atomic_store_rel_int(&avic->host_cpu, -1);
	atomic_store_rel_int(&avic->host_apic_id, -1);

	error = os_contigpa_zalloc(&avic->apic_page_pa,
	    (vaddr_t *)&avic->apic_page, 1);
	if (error != 0) {
		callout_terminate(&avic->timer_callout);
		os_mem_free(avic, sizeof(*avic));
		return error;
	}
	vmm_svm_avic_write(avic, VMM_SVM_APIC_ID, apic_id << 24);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_VERSION, VMM_SVM_APIC_VERSION_VALUE);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_TPR, 0);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_SVR,
	    VMM_SVM_APIC_SVR_ENABLE | 0xff);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_DFR, VMM_SVM_APIC_DFR_FLAT);
	if (avic->apic_id < 8) {
		vmm_svm_avic_write(avic, VMM_SVM_APIC_LDR,
		    1U << (24 + avic->apic_id));
	}
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVTT,
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT_THERMAL,
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT_PERF,
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT0,
	    apic_id == 0 ? VMM_SVM_APIC_LVT_EXTINT :
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT1,
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT_ERROR,
	    VMM_SVM_APIC_LVT_MASKED);
	if (avic->apic_id != 0)
		vmm_svm_avic_vcpu_reset(avic);
	else
		avic->timer_divisor = 2;

	lwkt_gettoken(&machine->token);
	if (machine->targets[apic_id] != NULL) {
		lwkt_reltoken(&machine->token);
		os_contigpa_free(avic->apic_page_pa, (vaddr_t)avic->apic_page, 1);
		callout_terminate(&avic->timer_callout);
		os_mem_free(avic, sizeof(*avic));
		return EEXIST;
	}
	machine->targets[apic_id] = avic;
	machine->physical_table[apic_id] = avic->apic_page_pa |
	    VMM_SVM_AVIC_PHYS_VALID;
	vmm_svm_avic_logical_update_locked(avic);
	cpu_mfence();
	lwkt_reltoken(&machine->token);

	config->enabled = true;
	config->apic_base = VMM_SVM_AVIC_APIC_BASE;
	config->apic_backing_page = avic->apic_page_pa;
	config->logical_table = machine->logical_table_pa;
	config->physical_table = machine->physical_table_pa;
	config->physical_max_index = VMM_SVM_AVIC_MAX_PHYS_ID;
	*result = avic;
	return 0;
}

static void
vmm_svm_avic_vcpu_reset_lapic(struct vmm_svm_interrupt_vcpu *avic)
{
	lwkt_gettoken(&avic->timer_token);
	callout_stop_async(&avic->timer_callout);
	avic->timer_lvtt = VMM_SVM_APIC_LVT_MASKED;
	avic->timer_tmict = 0;
	avic->timer_tdcr = 0;
	avic->timer_divisor = 2;
	avic->timer_interval_tsc = 0;
	avic->timer_deadline_tsc = 0;
	avic->timer_active = false;
	lwkt_reltoken(&avic->timer_token);

	bzero(avic->apic_page, PAGE_SIZE);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_ID, avic->apic_id << 24);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_VERSION,
	    VMM_SVM_APIC_VERSION_VALUE);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_DFR, VMM_SVM_APIC_DFR_FLAT);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_SVR,
	    VMM_SVM_APIC_SVR_ENABLE | 0xff);
	if (avic->apic_id < 8) {
		vmm_svm_avic_write(avic, VMM_SVM_APIC_LDR,
		    1U << (24 + avic->apic_id));
	}
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVTT,
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT_THERMAL,
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT_PERF,
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT0,
	    avic->apic_id == 0 ? VMM_SVM_APIC_LVT_EXTINT :
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT1,
	    VMM_SVM_APIC_LVT_MASKED);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_LVT_ERROR,
	    VMM_SVM_APIC_LVT_MASKED);
}

static void
vmm_svm_avic_vcpu_reset(struct vmm_svm_interrupt_vcpu *avic)
{
	struct vmm_cpustate *state = avic->vcpu->state;
	uint32_t mxcsr_mask = state->fpu.fx_mxcsr_mask;
	unsigned int index;

	if (mxcsr_mask == 0)
		mxcsr_mask = x86_fpu_mxcsr_mask;
	bzero(state, sizeof(*state));
	for (index = VMM_X64_SEG_ES; index <= VMM_X64_SEG_GS; ++index) {
		state->segs[index].limit = 0xffff;
		state->segs[index].attrib.type = 3;
		state->segs[index].attrib.s = 1;
		state->segs[index].attrib.p = 1;
	}
	state->segs[VMM_X64_SEG_CS].selector = 0xf000;
	state->segs[VMM_X64_SEG_CS].base = 0xffff0000;
	for (index = VMM_X64_SEG_GDT; index <= VMM_X64_SEG_IDT; ++index) {
		state->segs[index].limit = 0xffff;
		state->segs[index].attrib.type = 2;
		state->segs[index].attrib.s = 1;
		state->segs[index].attrib.p = 1;
	}
	state->segs[VMM_X64_SEG_LDT].limit = 0xffff;
	state->segs[VMM_X64_SEG_LDT].attrib.type = 2;
	state->segs[VMM_X64_SEG_LDT].attrib.p = 1;
	state->segs[VMM_X64_SEG_TR].limit = 0xffff;
	state->segs[VMM_X64_SEG_TR].attrib.type = 3;
	state->segs[VMM_X64_SEG_TR].attrib.p = 1;
	state->gprs[VMM_X64_GPR_RDX] = 0x600;
	state->gprs[VMM_X64_GPR_RIP] = 0xfff0;
	state->gprs[VMM_X64_GPR_RFLAGS] = 0x2;
	state->crs[VMM_X64_CR_CR0] = 0x60000010;
	state->crs[VMM_X64_CR_XCR0] = 0x1;
	state->drs[VMM_X64_DR_DR6] = 0xffff0ff0;
	state->drs[VMM_X64_DR_DR7] = 0x400;
	state->msrs[VMM_X64_MSR_PAT] = 0x0007040600070406ULL;
	state->fpu.fx_cw = 0x40;
	state->fpu.fx_tw = 0x55;
	state->fpu.fx_zero = 0x55;
	state->fpu.fx_mxcsr_mask = mxcsr_mask;
	state->fpu.fx_mxcsr = 0x1f80 & mxcsr_mask;
	avic->apic_base = VMM_SVM_AVIC_APIC_BASE |
	    VMM_SVM_APICBASE_ENABLED;
	avic->delivery_pending = false;
	atomic_clear_int(&avic->nmi_pending, 1);
	avic->nmi_delivery_pending = false;
	vmm_svm_avic_vcpu_reset_lapic(avic);
	avic->wait_sipi = true;
}

static int
vmm_svm_avic_vcpu_prepare(struct vmm_svm_interrupt_vcpu *avic)
{
	struct vmm_svm_interrupt_machine *machine;

	if (avic == NULL)
		return 0;
	if (avic->apic_id != 0) {
		for (;;) {
			if (atomic_swap_int(&avic->init_pending, 0) != 0)
				vmm_svm_avic_vcpu_reset(avic);
			if (!avic->wait_sipi)
				break;
			if (atomic_swap_int(&avic->sipi_pending, 0) != 0) {
				avic->vcpu->state->segs[VMM_X64_SEG_CS].selector =
				    (uint16_t)atomic_load_acq_int(&avic->sipi_vector) << 8;
				avic->vcpu->state->segs[VMM_X64_SEG_CS].base =
				    (uint64_t)atomic_load_acq_int(&avic->sipi_vector) << 12;
				avic->vcpu->state->gprs[VMM_X64_GPR_RIP] = 0;
				avic->wait_sipi = false;
				return EINPROGRESS;
			}
			tsleep_interlock(avic->vcpu, 0);
			if (atomic_load_acq_int(&avic->init_pending) == 0 &&
			    atomic_load_acq_int(&avic->sipi_pending) == 0)
				return EAGAIN;
		}
	}

	/* This can take the machine token because VMRUN has not disabled GIF. */
	machine = avic->machine;
	lwkt_gettoken(&machine->token);
	vmm_svm_ioapic_scan_eoi_locked(machine);
	lwkt_reltoken(&machine->token);
	return 0;
}

/*
 * This is the final check before a halted vCPU sleeps.  A direct AVIC IPI
 * updates the backing-page IRR without executing host code, so the normal
 * wakeup latch alone cannot close the HLT-to-sleep race.
 */
static bool
vmm_svm_avic_vcpu_runnable(struct vmm_svm_interrupt_vcpu *avic)
{
	volatile uint32_t *irr;
	uint32_t value;
	uint8_t vector;
	int word;
	int bit;

	if (avic == NULL)
		return false;

	/* Observe an AVIC hardware IRR update before testing its priority. */
	cpu_mfence();
	if (atomic_load_acq_int(&avic->init_pending) != 0 ||
	    atomic_load_acq_int(&avic->nmi_pending) != 0 ||
	    (avic->wait_sipi &&
	    atomic_load_acq_int(&avic->sipi_pending) != 0))
		return true;
	if (!vmm_svm_avic_lapic_enabled(avic) ||
	    !vmm_svm_vcpu_interrupt_allowed(avic->vcpu))
		return false;

	for (word = 7; word >= 1; --word) {
		irr = (volatile uint32_t *)((uint8_t *)avic->apic_page +
		    VMM_SVM_APIC_IRR_BASE + word * 0x10);
		value = atomic_load_acq_int((volatile u_int *)irr);
		if (value == 0)
			continue;
		bit = fls(value) - 1;
		vector = word * 32 + bit;
		if ((vector & 0xf0U) > vmm_svm_avic_lapic_ppr(avic))
			return true;
	}
	return false;
}

static void
vmm_svm_avic_vcpu_destroy(struct vmm_svm_interrupt_vcpu *avic)
{
	struct vmm_svm_interrupt_machine *machine;
	uint32_t index;

	if (avic == NULL)
		return;
	lwkt_gettoken(&avic->timer_token);
	avic->timer_active = false;
	callout_stop_async(&avic->timer_callout);
	lwkt_reltoken(&avic->timer_token);
	callout_drain(&avic->timer_callout);
	callout_terminate(&avic->timer_callout);
	machine = avic->machine;
	lwkt_gettoken(&machine->token);
	if (machine->targets[avic->apic_id] == avic) {
		machine->targets[avic->apic_id] = NULL;
		machine->physical_table[avic->apic_id] = 0;
		for (index = 0; index < PAGE_SIZE / sizeof(uint32_t); ++index) {
			if ((machine->logical_table[index] &
			    (VMM_SVM_AVIC_LOGICAL_VALID | VMM_SVM_AVIC_LOGICAL_APIC_ID)) ==
			    (VMM_SVM_AVIC_LOGICAL_VALID | avic->apic_id))
				machine->logical_table[index] = 0;
		}
		cpu_mfence();
	}
	lwkt_reltoken(&machine->token);
	os_contigpa_free(avic->apic_page_pa, (vaddr_t)avic->apic_page, 1);
	os_mem_free(avic, sizeof(*avic));
}

static void
vmm_svm_avic_vcpu_enter(struct vmm_svm_interrupt_vcpu *avic)
{
	struct vmm_machine *vmm_machine;
	int error;
	uint32_t cpu;
	uint32_t apic_id;
	uint64_t entry;
	uint8_t vector;
	bool nmi_queued;

	if (avic == NULL)
		return;
	vmm_svm_lapic_timer_sync(avic);
	vmm_svm_lapic_timer_check(avic);
	nmi_queued = false;
	if (!avic->delivery_pending &&
	    atomic_swap_int(&avic->nmi_pending, 0) != 0) {
		if (vmm_svm_vcpu_inject_interrupt(avic->vcpu, 2) != 0)
			atomic_set_int(&avic->nmi_pending, 1);
		else {
			avic->nmi_delivery_pending = true;
			nmi_queued = true;
		}
	}
	if (!avic->delivery_pending && !nmi_queued &&
	    vmm_svm_avic_lapic_accepts_pic(avic)) {
		if (!vmm_svm_vcpu_interrupt_allowed(avic->vcpu)) {
			vmm_svm_vcpu_request_interrupt_window(avic->vcpu);
		} else {
			vmm_machine = avic->vcpu->machine;
			lwkt_gettoken(&vmm_machine->token);
			error = vmm_x64_pic_peek_locked(vmm_machine, &vector);
			if (error == 0)
				error = vmm_svm_vcpu_inject_interrupt(avic->vcpu,
				    vector);
			if (error == 0)
				error = vmm_x64_pic_accept_locked(vmm_machine, &vector);
			if (error == 0) {
				avic->delivery_pending = true;
				avic->delivery_vector = vector;
			}
			lwkt_reltoken(&vmm_machine->token);
		}
	}
	cpu = os_curcpu_number();
	apic_id = CPUID_TO_APICID(cpu);
	KKASSERT((apic_id & ~VMM_SVM_AVIC_HOST_APIC_ID_MASK) == 0);
	atomic_store_rel_int(&avic->host_apic_id, apic_id);
	atomic_store_rel_int(&avic->host_cpu, cpu);
	entry = avic->apic_page_pa | VMM_SVM_AVIC_PHYS_VALID | apic_id;
	if (vmm_svm_avic_ipi_enabled)
		entry |= VMM_SVM_AVIC_PHYS_RUNNING;
	atomic_store_rel_64(&avic->machine->physical_table[avic->apic_id], entry);
	cpu_mfence();
	atomic_store_rel_int(&avic->running, 1);
}

static void
vmm_svm_avic_vcpu_event_result(struct vmm_svm_interrupt_vcpu *avic,
    bool reinjected)
{
	if (avic == NULL || !avic->delivery_pending || reinjected)
		return;
	/* The PIC moved IRR to ISR when vcpu_enter committed this event. */
	avic->delivery_pending = false;
}

static void
vmm_svm_avic_vcpu_leave(struct vmm_svm_interrupt_vcpu *avic)
{
	uint64_t entry;

	if (avic == NULL || atomic_swap_int(&avic->running, 0) == 0)
		return;
	entry = avic->apic_page_pa | VMM_SVM_AVIC_PHYS_VALID |
	    atomic_load_acq_int(&avic->host_apic_id);
	atomic_store_rel_64(&avic->machine->physical_table[avic->apic_id], entry);
	cpu_mfence();
}

static void
vmm_svm_avic_notify_posted(struct vmm_svm_interrupt_vcpu *avic)
{
	uint32_t host_cpu;
	uint32_t host_apic_id;

	/*
	 * Preserve a wakeup for the HLT-to-wait race.  This is intentionally not
	 * vmm_vcpu_kick(): a posted fixed interrupt must not force an active VMRUN
	 * out to its frontend.
	 */
	vmm_vcpu_wakeup(avic->vcpu);
	if (!avic->machine->avic ||
	    atomic_load_acq_int(&avic->running) == 0) {
		return;
	}
	host_cpu = atomic_load_acq_int(&avic->host_cpu);
	if (host_cpu == os_curcpu_number())
		return;
	host_apic_id = atomic_load_acq_int(&avic->host_apic_id);
	wrmsr(VMM_SVM_AVIC_DOORBELL_MSR, host_apic_id);
}

static void
vmm_svm_avic_deliver(struct vmm_svm_interrupt_vcpu *avic, uint8_t vector)
{
	volatile uint32_t *irr;

	if (vector < 32 || !vmm_svm_avic_lapic_enabled(avic))
		return;
	irr = (volatile uint32_t *)((uint8_t *)avic->apic_page +
	    VMM_SVM_APIC_IRR_BASE + (vector / 32) * 0x10);
	atomic_set_int((volatile u_int *)irr, __BIT(vector & 31));
	/* Pair the posted IRR update with AVIC running-state observation. */
	cpu_mfence();
	vmm_svm_avic_notify_posted(avic);
}

static bool
vmm_svm_avic_lapic_enabled(const struct vmm_svm_interrupt_vcpu *avic)
{
	return (avic->apic_base & VMM_SVM_APICBASE_ENABLED) != 0 &&
	    (vmm_svm_avic_read(avic, VMM_SVM_APIC_SVR) &
	    VMM_SVM_APIC_SVR_ENABLE) != 0;
}

/* Return the highest in-service priority class, or the complete TPR. */
static uint8_t
vmm_svm_avic_lapic_ppr(struct vmm_svm_interrupt_vcpu *avic)
{
	volatile uint32_t *isr;
	uint8_t tpr;
	int word;
	int bit;
	uint32_t value;

	tpr = vmm_svm_avic_read(avic, VMM_SVM_APIC_TPR);
	for (word = 7; word >= 0; --word) {
		isr = (volatile uint32_t *)((uint8_t *)avic->apic_page +
		    VMM_SVM_APIC_ISR_BASE + word * 0x10);
		value = atomic_load_acq_int((volatile u_int *)isr);
		if (value == 0)
			continue;
		bit = fls(value) - 1;
		if ((word * 2 + (bit >> 4)) > (tpr >> 4))
			return (uint8_t)((word * 2 + (bit >> 4)) << 4);
		break;
	}
	return tpr;
}

/* PIC virtual wire is controlled by BSP LINT0, not the SVR software bit. */
static bool
vmm_svm_avic_lapic_accepts_pic(const struct vmm_svm_interrupt_vcpu *avic)
{
	uint32_t lvt0;

	if ((avic->apic_base & VMM_SVM_APICBASE_ENABLED) == 0)
		return true;
	lvt0 = vmm_svm_avic_read(avic, VMM_SVM_APIC_LVT0);
	return (lvt0 & VMM_SVM_APIC_LVT_MASKED) == 0 &&
	    (lvt0 & VMM_SVM_APIC_LVT_DELIVERY_MASK) ==
	    VMM_SVM_APIC_LVT_EXTINT;
}

static int
vmm_svm_irqchip_mmio(struct vmm_svm_interrupt_vcpu *vcpu, uint64_t address,
    bool write, uint32_t *value)
{
	struct vmm_svm_interrupt_machine *machine = vcpu->machine;
	uint32_t reg;
	uint32_t pin;
	uint64_t entry;

	if (address >= VMM_SVM_AVIC_APIC_BASE &&
	    address < VMM_SVM_AVIC_APIC_BASE + PAGE_SIZE) {
		if ((vcpu->apic_base & VMM_SVM_APICBASE_ENABLED) == 0)
			return ENOENT;
		reg = address - VMM_SVM_AVIC_APIC_BASE;
		if ((reg & 3) != 0)
			return ENOENT;
		if (!write)
			return vmm_svm_lapic_read(vcpu, reg, value);
		if (reg == VMM_SVM_APIC_ICR_LOW) {
			vmm_svm_avic_write(vcpu, reg, *value);
			return vmm_svm_avic_route_icr(vcpu, *value,
			    vmm_svm_avic_read(vcpu, VMM_SVM_APIC_ICR_HIGH), 0) ?
			    0 : ENOTSUP;
		}
		if (reg == VMM_SVM_APIC_ICR_HIGH) {
			vmm_svm_avic_write(vcpu, reg, *value);
			return 0;
		}
		return vmm_svm_lapic_write(vcpu, reg, *value);
	}
	if (address != VMM_SVM_IOAPIC_BASE &&
	    address != VMM_SVM_IOAPIC_BASE + 0x10)
		return ENOENT;
	lwkt_gettoken(&machine->token);
	if (address == VMM_SVM_IOAPIC_BASE) {
		if (write)
			machine->ioapic_select = *value & 0xffU;
		else
			*value = machine->ioapic_select;
		lwkt_reltoken(&machine->token);
		return 0;
	}
	reg = machine->ioapic_select;
	if (!write) {
		if (reg == VMM_SVM_IOAPIC_REG_ID || reg == VMM_SVM_IOAPIC_REG_ARB)
			*value = machine->ioapic_id << 24;
		else if (reg == VMM_SVM_IOAPIC_REG_VERSION)
			*value = VMM_SVM_IOAPIC_VERSION;
		else if (reg >= VMM_SVM_IOAPIC_REDIR_BASE &&
		    reg < VMM_SVM_IOAPIC_REDIR_BASE + VMM_SVM_IOAPIC_PINS * 2) {
			pin = (reg - VMM_SVM_IOAPIC_REDIR_BASE) / 2;
			entry = machine->ioapic_redir[pin];
			*value = (reg & 1) ? entry >> 32 : entry;
		} else
			*value = 0;
		lwkt_reltoken(&machine->token);
		return 0;
	}
	if (reg == VMM_SVM_IOAPIC_REG_ID)
		machine->ioapic_id = (*value >> 24) & 0x0fU;
	else if (reg >= VMM_SVM_IOAPIC_REDIR_BASE &&
	    reg < VMM_SVM_IOAPIC_REDIR_BASE + VMM_SVM_IOAPIC_PINS * 2) {
		pin = (reg - VMM_SVM_IOAPIC_REDIR_BASE) / 2;
		entry = machine->ioapic_redir[pin];
		if (reg & 1)
			entry = (entry & 0xffffffffULL) | ((uint64_t)*value << 32);
		else
			entry = (entry & 0xffffffff00000000ULL) | *value;
		entry = (entry & ~VMM_SVM_IOAPIC_REDIR_REMOTE_IRR) |
		    (machine->ioapic_redir[pin] & VMM_SVM_IOAPIC_REDIR_REMOTE_IRR);
		if ((entry & VMM_SVM_IOAPIC_REDIR_LEVEL) == 0)
			entry &= ~VMM_SVM_IOAPIC_REDIR_REMOTE_IRR;
		machine->ioapic_redir[pin] = entry;
		if (machine->ioapic_level[pin])
			vmm_svm_ioapic_deliver_locked(machine, pin);
	}
	lwkt_reltoken(&machine->token);
	return 0;
}

static int
vmm_svm_avic_vcpu_mmio(struct vmm_svm_interrupt_vcpu *vcpu,
    uint64_t address, bool write, uint32_t *value)
{
	return vmm_svm_irqchip_mmio(vcpu, address, write, value);
}

/* Caller holds machine->token. */
static void
vmm_svm_avic_logical_update_locked(struct vmm_svm_interrupt_vcpu *avic)
{
	struct vmm_svm_interrupt_machine *machine = avic->machine;
	uint32_t dfr;
	uint32_t ldr;
	uint32_t index;
	uint32_t logical_id;
	uint32_t i;

	for (i = 0; i < PAGE_SIZE / sizeof(uint32_t); ++i) {
		if ((machine->logical_table[i] &
		    (VMM_SVM_AVIC_LOGICAL_VALID | VMM_SVM_AVIC_LOGICAL_APIC_ID)) ==
		    (VMM_SVM_AVIC_LOGICAL_VALID | avic->apic_id))
			machine->logical_table[i] = 0;
	}
	dfr = vmm_svm_avic_read(avic, VMM_SVM_APIC_DFR);
	ldr = vmm_svm_avic_read(avic, VMM_SVM_APIC_LDR) >> 24;
	if (ldr == 0 || (ldr & (ldr - 1)) != 0)
		return;
	if (dfr == VMM_SVM_APIC_DFR_FLAT) {
		index = ffs(ldr) - 1;
	} else if (dfr == VMM_SVM_APIC_DFR_CLUSTER) {
		logical_id = ldr & 0x0fU;
		if (logical_id == 0 || (logical_id & (logical_id - 1)) != 0 ||
		    (ldr >> 4) >= 0x0fU)
			return;
		index = ((ldr >> 4) << 2) + ffs(logical_id) - 1;
	} else {
		return;
	}
	if (index < PAGE_SIZE / sizeof(uint32_t))
		machine->logical_table[index] = VMM_SVM_AVIC_LOGICAL_VALID |
		    avic->apic_id;
}

static int
vmm_svm_avic_route_icr(struct vmm_svm_interrupt_vcpu *source,
    uint32_t low, uint32_t high, int x2apic)
{
	struct vmm_svm_interrupt_machine *machine = source->machine;
	struct vmm_svm_interrupt_vcpu *target;
	uint32_t destination;
	uint32_t delivery;
	uint32_t shorthand;
	uint32_t source_dfr;
	uint32_t target_ldr;
	uint32_t id;
	uint32_t vector;

	delivery = low & VMM_SVM_APIC_ICR_DELIVERY_MASK;
	shorthand = low & VMM_SVM_APIC_ICR_SHORTHAND;
	vector = low & 0xffU;
	/*
	 * A broadcast without a recipient is architecturally complete.  In
	 * particular, SeaBIOS issues INIT | all-excluding-self for a one-vCPU
	 * guest.
	 */
	if (delivery != VMM_SVM_APIC_ICR_FIXED &&
	    delivery != VMM_SVM_APIC_ICR_NMI &&
	    delivery != VMM_SVM_APIC_ICR_INIT &&
	    delivery != VMM_SVM_APIC_ICR_SIPI)
		return 0;
	if (delivery == VMM_SVM_APIC_ICR_FIXED && vector < 16)
		return 1;
	destination = x2apic ? high : high >> 24;
	lwkt_gettoken(&machine->token);
	source_dfr = vmm_svm_avic_read(source, VMM_SVM_APIC_DFR);
	for (id = 0; id <= VMM_SVM_AVIC_MAX_PHYS_ID; ++id) {
		target = machine->targets[id];
		if (target == NULL)
			continue;
		if (shorthand == VMM_SVM_APIC_ICR_SELF && target != source)
			continue;
		if (shorthand == VMM_SVM_APIC_ICR_ALL_EXC_SELF && target == source)
			continue;
		if (shorthand == 0) {
			if ((low & VMM_SVM_APIC_ICR_DEST_LOGICAL) == 0) {
				if (destination != target->apic_id &&
				    destination != (x2apic ?
				    VMM_SVM_APIC_ICR_X2_DEST_BROADCAST :
				    VMM_SVM_APIC_ICR_DEST_BROADCAST))
					continue;
			} else {
				target_ldr = vmm_svm_avic_read(target,
				    VMM_SVM_APIC_LDR) >> 24;
				if (source_dfr == VMM_SVM_APIC_DFR_FLAT) {
					if ((destination & target_ldr) == 0)
						continue;
				} else if (source_dfr != VMM_SVM_APIC_DFR_CLUSTER ||
				    (destination & 0xf0U) != (target_ldr & 0xf0U) ||
				    (destination & target_ldr & 0x0fU) == 0) {
					continue;
				}
			}
		}
		switch (delivery) {
		case VMM_SVM_APIC_ICR_FIXED:
			vmm_svm_avic_deliver(target, (uint8_t)vector);
			break;
		case VMM_SVM_APIC_ICR_NMI:
			atomic_set_int(&target->nmi_pending, 1);
			vmm_vcpu_interrupt(target->vcpu);
			break;
		case VMM_SVM_APIC_ICR_INIT:
			atomic_store_rel_int(&target->sipi_pending, 0);
			atomic_store_rel_int(&target->init_pending, 1);
			vmm_vcpu_interrupt(target->vcpu);
			break;
		case VMM_SVM_APIC_ICR_SIPI:
			atomic_store_rel_int(&target->sipi_vector, vector);
			atomic_store_rel_int(&target->sipi_pending, 1);
			vmm_vcpu_interrupt(target->vcpu);
			break;
		}
	}
	lwkt_reltoken(&machine->token);
	return 1;
}

static int
vmm_svm_avic_vcpu_exit(struct vmm_svm_interrupt_vcpu *avic,
    uint64_t exitcode, uint64_t exitinfo1, uint64_t exitinfo2)
{
	struct vmm_svm_interrupt_machine *machine;
	struct vmm_svm_interrupt_vcpu *target;
	uint32_t cause;
	uint32_t delivery;
	uint32_t destination;
	uint32_t offset;
	uint32_t shorthand;
	uint32_t source_dfr;
	uint32_t target_ldr;
	uint32_t value;
	uint32_t id;
	bool redelivered;

	if (avic == NULL)
		return 0;
	if (exitcode == VMM_SVM_EXIT_IRET && avic->nmi_delivery_pending) {
		avic->nmi_delivery_pending = false;
		return 1;
	}
	if (exitcode == VMM_SVM_EXIT_HLT) {
		machine = avic->machine;
		lwkt_gettoken(&machine->token);
		redelivered = vmm_svm_ioapic_scan_eoi_locked(machine);
		lwkt_reltoken(&machine->token);
		return redelivered;
	}
	if (exitcode == VMM_SVM_EXIT_AVIC_INCOMPLETE_IPI) {
		cause = (uint32_t)(exitinfo2 >> 32);
		switch (cause) {
		case VMM_SVM_AVIC_IPI_TARGET_NOT_RUNNING:
			delivery = (uint32_t)exitinfo1 &
			    VMM_SVM_APIC_ICR_DELIVERY_MASK;
			/*
			 * AVIC can retain a fixed IPI in IRR, but INIT, SIPI and NMI
			 * require software state transitions even when the target is
			 * currently parked.  In particular, INIT/SIPI must update the
			 * target's reset and startup-vector state before it is woken.
			 */
			if (delivery == VMM_SVM_APIC_ICR_INIT ||
			    delivery == VMM_SVM_APIC_ICR_SIPI ||
			    delivery == VMM_SVM_APIC_ICR_NMI) {
				return vmm_svm_avic_route_icr(avic, (uint32_t)exitinfo1,
				    (uint32_t)(exitinfo1 >> 32), 0);
			}
			/*
			 * AVIC has already set IRR for every valid target.  IsRunning
			 * was clear.  Do not route or re-deliver this IPI: notify the
			 * selected targets exactly as Linux KVM does for an already-posted
			 * interrupt.  Active targets receive an AVIC doorbell; a blocked
			 * target consumes the wakeup from vmm_vcpu_wait().
			 */
			machine = avic->machine;
			shorthand = exitinfo1 & VMM_SVM_APIC_ICR_SHORTHAND;
			destination = (uint32_t)(exitinfo1 >> 32) >> 24;
			lwkt_gettoken(&machine->token);
			source_dfr = vmm_svm_avic_read(avic,
			    VMM_SVM_APIC_DFR);
			for (id = 0; id <= VMM_SVM_AVIC_MAX_PHYS_ID; ++id) {
				target = machine->targets[id];
				if (target == NULL)
					continue;
				if (shorthand == VMM_SVM_APIC_ICR_SELF && target != avic)
					continue;
				if (shorthand == VMM_SVM_APIC_ICR_ALL_EXC_SELF &&
				    target == avic)
					continue;
				if (shorthand == 0) {
					if ((exitinfo1 &
					    VMM_SVM_APIC_ICR_DEST_LOGICAL) == 0) {
						if (destination != target->apic_id &&
						    destination !=
						    VMM_SVM_APIC_ICR_DEST_BROADCAST)
							continue;
					} else {
						target_ldr = vmm_svm_avic_read(target,
						    VMM_SVM_APIC_LDR) >> 24;
						if (source_dfr ==
						    VMM_SVM_APIC_DFR_FLAT) {
							if ((destination & target_ldr) == 0)
								continue;
						} else if (source_dfr !=
						    VMM_SVM_APIC_DFR_CLUSTER ||
						    (destination & 0xf0U) !=
						    (target_ldr & 0xf0U) ||
						    (destination & target_ldr & 0x0fU) == 0) {
							continue;
						}
					}
				}
				vmm_svm_avic_notify_posted(target);
			}
			lwkt_reltoken(&machine->token);
			return 1;
		case VMM_SVM_AVIC_IPI_INVALID_INT_TYPE:
		case VMM_SVM_AVIC_IPI_INVALID_TARGET:
			return vmm_svm_avic_route_icr(avic,
			    (uint32_t)exitinfo1, (uint32_t)(exitinfo1 >> 32), 0);
		case VMM_SVM_AVIC_IPI_INVALID_VECTOR:
			return 1;
		case VMM_SVM_AVIC_IPI_INVALID_BACKING_PAGE:
			kprintf("vmm: AVIC incomplete IPI invalid backing page\\n");
			return 0;
		default:
			kprintf("vmm: AVIC incomplete IPI unknown cause %#x\\n", cause);
			return 0;
		}
	}
	if (exitcode != VMM_SVM_EXIT_AVIC_NOACCEL ||
	    ((exitinfo1 >> 32) & VMM_SVM_AVIC_NOACCEL_WRITE) == 0)
		return 0;
	offset = exitinfo1 & VMM_SVM_AVIC_NOACCEL_OFFSET;
	value = vmm_svm_avic_read(avic, offset);
	if (offset != VMM_SVM_APIC_ICR_LOW &&
	    offset != VMM_SVM_APIC_ICR_HIGH)
		return vmm_svm_lapic_write(avic, offset, value) == 0;
	switch (offset) {
	case VMM_SVM_APIC_ICR_HIGH:
		vmm_svm_avic_write(avic, offset, value);
		return 1;
	case VMM_SVM_APIC_ICR_LOW:
		vmm_svm_avic_write(avic, offset, value);
		return vmm_svm_avic_route_icr(avic, value,
		    vmm_svm_avic_read(avic, VMM_SVM_APIC_ICR_HIGH), 0);
	default:
		return 0;
	}
}
