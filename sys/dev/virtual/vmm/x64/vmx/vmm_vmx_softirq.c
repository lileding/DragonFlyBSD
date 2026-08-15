/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMX software interrupt controller.
 *
 * This implementation owns software LAPIC, IOAPIC, MSI and legacy interrupt
 * state for a VMX machine with an irqchip but without APICv.  It never owns a
 * VMCS or executes VM entry; vmm_vmx.c calls it at VM entry and for the
 * LAPIC/IOAPIC VMEXITs it claims.
 */
#include <sys/errno.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <machine/clock.h>
#include "../../vmm.h"
#include "../../vmm_machine.h"
#include "../../vmm_vcpu.h"
#include "../vmm_x64_pic.h"
#include "../vmm_x64_pit.h"
#include "vmm_vmx.h"
#include "vmm_vmx_apicv.h"
#include "vmm_vmx_os.h"
#include "vmm_vmx_x86defs.h"

#define VMM_VMX_SOFTIRQ_LAPIC_BASE		0xfee00000ULL
#define VMM_VMX_SOFTIRQ_MAX_APIC_ID	0xfeU
#define VMM_VMX_MSR_APICBASE			0x01bU
#define VMM_VMX_APICBASE_BSP			0x00000100ULL
#define VMM_VMX_APICBASE_ENABLED		0x00000800ULL
#define VMM_VMX_APICBASE_ADDRESS		0xfffff000ULL

#define VMM_VMX_IOAPIC_BASE		0xfec00000ULL
#define VMM_VMX_IOAPIC_PINS		24U
#define VMM_VMX_IOAPIC_REG_ID		0x00U
#define VMM_VMX_IOAPIC_REG_VERSION	0x01U
#define VMM_VMX_IOAPIC_REG_ARB		0x02U
#define VMM_VMX_IOAPIC_REDIR_BASE	0x10U
#define VMM_VMX_IOAPIC_VERSION		(((VMM_VMX_IOAPIC_PINS - 1U) << 16) | 0x11U)
#define VMM_VMX_IOAPIC_REDIR_REMOTE_IRR	__BIT(14)
#define VMM_VMX_IOAPIC_REDIR_MASKED	__BIT(16)
#define VMM_VMX_IOAPIC_REDIR_FIXED	0x00000000U
#define VMM_VMX_IOAPIC_REDIR_DELIVERY_MASK	0x00000700U
#define VMM_VMX_IOAPIC_REDIR_DEST_LOGICAL	__BIT(11)
#define VMM_VMX_IOAPIC_REDIR_LEVEL	__BIT(15)

#define VMM_VMX_APIC_ID			0x020U
#define VMM_VMX_APIC_VERSION		0x030U
#define VMM_VMX_APIC_TPR			0x080U
#define VMM_VMX_APIC_PPR			0x0a0U
#define VMM_VMX_APIC_EOI			0x0b0U
#define VMM_VMX_APIC_LDR			0x0d0U
#define VMM_VMX_APIC_DFR			0x0e0U
#define VMM_VMX_APIC_SVR			0x0f0U
#define VMM_VMX_APIC_IRR_BASE		0x200U
#define VMM_VMX_APIC_ESR			0x280U
#define VMM_VMX_APIC_ICR_LOW		0x300U
#define VMM_VMX_APIC_ICR_HIGH		0x310U
#define VMM_VMX_APIC_LVTT			0x320U
#define VMM_VMX_APIC_LVT_THERMAL		0x330U
#define VMM_VMX_APIC_LVT_PERF			0x340U
#define VMM_VMX_APIC_LVT0			0x350U
#define VMM_VMX_APIC_LVT1			0x360U
#define VMM_VMX_APIC_LVT_ERROR		0x370U
#define VMM_VMX_APIC_TMICT			0x380U
#define VMM_VMX_APIC_TMCCT			0x390U
#define VMM_VMX_APIC_TDCR			0x3e0U
#define VMM_VMX_APIC_ISR_BASE			0x100U

#define VMM_VMX_APIC_VERSION_VALUE	0x00140014U
#define VMM_VMX_APIC_SVR_ENABLE	__BIT(8)
#define VMM_VMX_APIC_SVR_VALID		0x000003ffU
#define VMM_VMX_APIC_DFR_CLUSTER	0x0fffffffU
#define VMM_VMX_APIC_DFR_FLAT		0xffffffffU
#define VMM_VMX_APIC_LVT_VECTOR_MASK	0x000000ffU
#define VMM_VMX_APIC_LVT_DELIVERY_MASK	0x00000700U
#define VMM_VMX_APIC_LVT_SEND_PENDING	0x00001000U
#define VMM_VMX_APIC_LVT_EXTINT		0x00000700U
#define VMM_VMX_APIC_LVT_POLARITY		0x00002000U
#define VMM_VMX_APIC_LVT_REMOTE_IRR		0x00004000U
#define VMM_VMX_APIC_LVT_LEVEL		0x00008000U
#define VMM_VMX_APIC_LVT_MASKED		0x00010000U
#define VMM_VMX_APIC_LVT_TIMER_MODE		0x00060000U
#define VMM_VMX_APIC_LVT_TIMER_ONESHOT		0x00000000U
#define VMM_VMX_APIC_LVT_TIMER_PERIODIC	0x00020000U
#define VMM_VMX_APIC_LVT_TIMER_TSCDEADLINE	0x00040000U
#define VMM_VMX_APIC_LVT_COMMON_VALID	\
	(VMM_VMX_APIC_LVT_VECTOR_MASK | VMM_VMX_APIC_LVT_SEND_PENDING | \
	 VMM_VMX_APIC_LVT_MASKED)
#define VMM_VMX_APIC_LVT_TIMER_VALID	\
	(VMM_VMX_APIC_LVT_COMMON_VALID | VMM_VMX_APIC_LVT_TIMER_MODE)
#define VMM_VMX_APIC_LVT_DELIVERY_VALID	\
	(VMM_VMX_APIC_LVT_COMMON_VALID | VMM_VMX_APIC_LVT_DELIVERY_MASK)
#define VMM_VMX_APIC_LVT_LINT_VALID	\
	(VMM_VMX_APIC_LVT_COMMON_VALID | VMM_VMX_APIC_LVT_DELIVERY_MASK | \
	 VMM_VMX_APIC_LVT_POLARITY | VMM_VMX_APIC_LVT_REMOTE_IRR | \
	 VMM_VMX_APIC_LVT_LEVEL)
#define VMM_VMX_APIC_TIMER_DIVIDE_VALID	0x0000000bU

#define VMM_VMX_SOFTIRQ_APICBASE_VALID	(VMM_VMX_APICBASE_BSP | \
	VMM_VMX_APICBASE_ENABLED | VMM_VMX_APICBASE_ADDRESS)

#define VMM_VMX_APIC_ICR_DELIVERY_MASK	0x00000700U
#define VMM_VMX_APIC_ICR_FIXED		0x00000000U
#define VMM_VMX_APIC_ICR_NMI		0x00000400U
#define VMM_VMX_APIC_ICR_INIT		0x00000500U
#define VMM_VMX_APIC_ICR_SIPI		0x00000600U
#define VMM_VMX_APIC_ICR_DEST_LOGICAL	__BIT(11)
#define VMM_VMX_APIC_ICR_SHORTHAND	0x000c0000U
#define VMM_VMX_APIC_ICR_SELF		0x00040000U
#define VMM_VMX_APIC_ICR_ALL_SELF	0x00080000U
#define VMM_VMX_APIC_ICR_ALL_EXC_SELF	0x000c0000U
#define VMM_VMX_APIC_ICR_DEST_BROADCAST	0xffU
#define VMM_VMX_APIC_ICR_X2_DEST_BROADCAST	0xffffffffU

#define VMM_VMX_MSI_ADDRESS_BASE		0xfee00000ULL
#define VMM_VMX_MSI_ADDRESS_DEST_MASK		0x000ff000ULL
#define VMM_VMX_MSI_ADDRESS_DEST_LOGICAL	__BIT(2)
#define VMM_VMX_MSI_ADDRESS_REDIRECTION_HINT	__BIT(3)
#define VMM_VMX_MSI_ADDRESS_CONTROL_MASK	(VMM_VMX_MSI_ADDRESS_DEST_LOGICAL | \
	VMM_VMX_MSI_ADDRESS_REDIRECTION_HINT)
#define VMM_VMX_MSI_DATA_VECTOR_MASK		0x000000ffU
#define VMM_VMX_MSI_DATA_DELIVERY_MASK		0x00000700U
#define VMM_VMX_MSI_DATA_LEVEL_ASSERT		__BIT(14)
#define VMM_VMX_MSI_DATA_TRIGGER_LEVEL		__BIT(15)
#define VMM_VMX_MSI_DATA_ALLOWED		(VMM_VMX_MSI_DATA_VECTOR_MASK | \
	VMM_VMX_MSI_DATA_DELIVERY_MASK | VMM_VMX_MSI_DATA_LEVEL_ASSERT | \
	VMM_VMX_MSI_DATA_TRIGGER_LEVEL)

struct vmm_vmx_interrupt_machine {
	struct lwkt_token token;
	uint32_t ioapic_select;
	uint32_t ioapic_id;
	uint64_t ioapic_redir[VMM_VMX_IOAPIC_PINS];
	bool ioapic_level[VMM_VMX_IOAPIC_PINS];
	struct vmm_vmx_interrupt_vcpu *targets[VMM_VMX_SOFTIRQ_MAX_APIC_ID + 1];
};

struct vmm_vmx_interrupt_vcpu {
	struct vmm_vmx_interrupt_machine *machine;
	struct vmm_vcpu *vcpu;
	void *apic_page;
	paddr_t apic_page_pa;
	uint32_t apic_id;
	uint64_t apic_base;
	bool delivery_pending;
	bool delivery_legacy;
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
	bool timer_expired;
};

static int vmm_vmx_softirq_machine_create(struct vmm_machine *,
    struct vmm_vmx_interrupt_machine **);
static int vmm_vmx_softirq_machine_enable(struct vmm_vmx_interrupt_machine *);
static int vmm_vmx_softirq_irq_raise_msi(struct vmm_vmx_interrupt_machine *,
    uint64_t, uint32_t);
static int vmm_vmx_softirq_irq_set(struct vmm_vmx_interrupt_machine *, uint32_t,
    bool);
static int vmm_vmx_interrupt_raise_legacy(
    struct vmm_vmx_interrupt_machine *, uint8_t);
static int vmm_vmx_interrupt_machine_get_ioapic(
    struct vmm_vmx_interrupt_machine *, struct vmm_ioapic_state *);
static int vmm_vmx_interrupt_machine_set_ioapic(
    struct vmm_vmx_interrupt_machine *, const struct vmm_ioapic_state *);
static int vmm_vmx_softirq_vcpu_mmio(struct vmm_vmx_interrupt_vcpu *, uint64_t,
    bool, uint32_t *);
static int vmm_vmx_softirq_vcpu_io(struct vmm_vcpu *,
    const struct vmm_cpuexit_io *);
static int vmm_vmx_softirq_vcpu_msr(struct vmm_vmx_interrupt_vcpu *, bool,
    uint32_t, uint64_t *);
static void vmm_vmx_softirq_machine_destroy(struct vmm_vmx_interrupt_machine *);
static int vmm_vmx_softirq_vcpu_create(struct vmm_vmx_interrupt_machine *,
    struct vmm_vcpu *, struct vmm_vmx_interrupt_vcpu **,
    struct vmm_vmx_interrupt_config *);
static void vmm_vmx_softirq_vcpu_destroy(struct vmm_vmx_interrupt_vcpu *);
static int vmm_vmx_softirq_vcpu_prepare(struct vmm_vmx_interrupt_vcpu *);
static void vmm_vmx_softirq_vcpu_enter(struct vmm_vmx_interrupt_vcpu *);
static void vmm_vmx_softirq_vcpu_leave(struct vmm_vmx_interrupt_vcpu *);
static void vmm_vmx_softirq_vcpu_event_result(
    struct vmm_vmx_interrupt_vcpu *, bool);
static int vmm_vmx_softirq_vcpu_exit(struct vmm_vmx_interrupt_vcpu *, uint64_t,
    uint64_t, uint64_t);
static int vmm_vmx_softirq_route_icr(struct vmm_vmx_interrupt_vcpu *, uint32_t,
    uint32_t, int);
static void vmm_vmx_softirq_deliver(struct vmm_vmx_interrupt_vcpu *, uint8_t);
static bool vmm_vmx_lapic_enabled(const struct vmm_vmx_interrupt_vcpu *);
static bool vmm_vmx_lapic_accepts_pic(const struct vmm_vmx_interrupt_vcpu *);
static uint32_t vmm_vmx_softirq_read(const struct vmm_vmx_interrupt_vcpu *,
    uint32_t);
static void vmm_vmx_softirq_write(const struct vmm_vmx_interrupt_vcpu *,
    uint32_t, uint32_t);
static int vmm_vmx_lapic_read(struct vmm_vmx_interrupt_vcpu *, uint32_t,
    uint32_t *);
static int vmm_vmx_lapic_write(struct vmm_vmx_interrupt_vcpu *, uint32_t,
    uint32_t);
static int vmm_vmx_interrupt_vcpu_get_lapic(
    struct vmm_vmx_interrupt_vcpu *, void *, size_t);
static int vmm_vmx_interrupt_vcpu_set_lapic(
    struct vmm_vmx_interrupt_vcpu *, const void *, size_t);
static void vmm_vmx_lapic_timer_arm(struct vmm_vmx_interrupt_vcpu *,
    uint32_t);
static void vmm_vmx_lapic_timer_arm_locked(
    struct vmm_vmx_interrupt_vcpu *, uint32_t);
static void vmm_vmx_lapic_timer_check(struct vmm_vmx_interrupt_vcpu *);
static void vmm_vmx_lapic_timer_timeout(void *);
static bool vmm_vmx_lapic_timer_expire_locked(
    struct vmm_vmx_interrupt_vcpu *, uint64_t);
static void vmm_vmx_lapic_timer_schedule_locked(
    struct vmm_vmx_interrupt_vcpu *, uint64_t);
static int vmm_vmx_lapic_eoi(struct vmm_vmx_interrupt_vcpu *);
static uint8_t vmm_vmx_lapic_ppr(struct vmm_vmx_interrupt_vcpu *);
static void vmm_vmx_ioapic_deliver_locked(
    struct vmm_vmx_interrupt_machine *, uint32_t);
static void vmm_vmx_ioapic_reassert(struct vmm_vmx_interrupt_machine *,
    uint8_t);
static void vmm_vmx_softirq_vcpu_reset(struct vmm_vmx_interrupt_vcpu *);
static void vmm_vmx_softirq_vcpu_reset_lapic(
    struct vmm_vmx_interrupt_vcpu *);

const struct vmm_vmx_interrupt_ops vmm_vmx_softirq_interrupt_ops = {
	.name = "software",
	.machine_create = vmm_vmx_softirq_machine_create,
	.machine_enable = vmm_vmx_softirq_machine_enable,
	.irq_raise_msi = vmm_vmx_softirq_irq_raise_msi,
	.irq_set = vmm_vmx_softirq_irq_set,
	.irq_raise_legacy = vmm_vmx_interrupt_raise_legacy,
	.machine_get_ioapic = vmm_vmx_interrupt_machine_get_ioapic,
	.machine_set_ioapic = vmm_vmx_interrupt_machine_set_ioapic,
	.vcpu_mmio = vmm_vmx_softirq_vcpu_mmio,
	.vcpu_io = vmm_vmx_softirq_vcpu_io,
	.vcpu_msr = vmm_vmx_softirq_vcpu_msr,
	.machine_destroy = vmm_vmx_softirq_machine_destroy,
	.vcpu_create = vmm_vmx_softirq_vcpu_create,
	.vcpu_get_lapic = vmm_vmx_interrupt_vcpu_get_lapic,
	.vcpu_set_lapic = vmm_vmx_interrupt_vcpu_set_lapic,
	.vcpu_destroy = vmm_vmx_softirq_vcpu_destroy,
	.vcpu_prepare = vmm_vmx_softirq_vcpu_prepare,
	.vcpu_enter = vmm_vmx_softirq_vcpu_enter,
	.vcpu_leave = vmm_vmx_softirq_vcpu_leave,
	.vcpu_event_result = vmm_vmx_softirq_vcpu_event_result,
	.vintr_internal = true,
	.vcpu_exit = vmm_vmx_softirq_vcpu_exit,
};

static int
vmm_vmx_softirq_machine_create(struct vmm_machine *machine,
    struct vmm_vmx_interrupt_machine **result)
{
	struct vmm_vmx_interrupt_machine *soft;
	uint32_t pin;

	soft = os_mem_zalloc(sizeof(*soft));
	if (soft == NULL)
		return ENOMEM;
	lwkt_token_init(&soft->token, "vmmirq");
	soft->ioapic_id = 1;
	for (pin = 0; pin < VMM_VMX_IOAPIC_PINS; ++pin)
		soft->ioapic_redir[pin] = VMM_VMX_IOAPIC_REDIR_MASKED;
	*result = soft;
	return 0;
}

static int
vmm_vmx_softirq_machine_enable(struct vmm_vmx_interrupt_machine *machine)
{
	return machine == NULL ? EINVAL : 0;
}

static int
vmm_vmx_softirq_irq_raise_msi(struct vmm_vmx_interrupt_machine *machine,
    uint64_t address, uint32_t data)
{
	struct vmm_vmx_interrupt_vcpu *target;
	uint32_t destination;
	uint32_t id;
	uint32_t vector;
	bool bridge;
	bool logical;
	bool nmi;
	bool delivered;

	/* QEMU's in-kernel APIC bridge encodes only MSI destination bits. */
	bridge = (address & ~(VMM_VMX_MSI_ADDRESS_DEST_MASK |
	    VMM_VMX_MSI_ADDRESS_CONTROL_MASK)) == 0;
	if (((address & ~(VMM_VMX_MSI_ADDRESS_DEST_MASK |
	    VMM_VMX_MSI_ADDRESS_CONTROL_MASK)) != VMM_VMX_MSI_ADDRESS_BASE &&
	    !bridge) ||
	    (address & VMM_VMX_MSI_ADDRESS_REDIRECTION_HINT) != 0 ||
	    (data & ~(VMM_VMX_MSI_DATA_ALLOWED |
	    (bridge ? VMM_VMX_APIC_ICR_DEST_LOGICAL : 0))) != 0 ||
	    ((data & VMM_VMX_MSI_DATA_DELIVERY_MASK) != 0 &&
	    (data & VMM_VMX_MSI_DATA_DELIVERY_MASK) != VMM_VMX_APIC_ICR_NMI) ||
	    (data & VMM_VMX_MSI_DATA_TRIGGER_LEVEL) != 0)
		return EOPNOTSUPP;
	nmi = (data & VMM_VMX_MSI_DATA_DELIVERY_MASK) ==
	    VMM_VMX_APIC_ICR_NMI;
	vector = data & VMM_VMX_MSI_DATA_VECTOR_MASK;
	if (!nmi && vector < 32)
		return EINVAL;
	destination = (address & VMM_VMX_MSI_ADDRESS_DEST_MASK) >> 12;
	logical = (address & VMM_VMX_MSI_ADDRESS_DEST_LOGICAL) != 0;
	if (!logical && destination > VMM_VMX_SOFTIRQ_MAX_APIC_ID)
		return ENOENT;
	delivered = false;
	lwkt_gettoken(&machine->token);
	if (!logical) {
		target = machine->targets[destination];
		if (target != NULL) {
			if (nmi) {
				atomic_set_int(&target->nmi_pending, 1);
				(void)vmm_vcpu_kick(target->vcpu);
			} else {
				vmm_vmx_softirq_deliver(target, (uint8_t)vector);
			}
			delivered = true;
		}
	} else {
		for (id = 0; id <= VMM_VMX_SOFTIRQ_MAX_APIC_ID; ++id) {
			target = machine->targets[id];
			if (target == NULL || (destination &
			    (vmm_vmx_softirq_read(target, VMM_VMX_APIC_LDR) >> 24)) == 0)
				continue;
			if (nmi) {
				atomic_set_int(&target->nmi_pending, 1);
				(void)vmm_vcpu_kick(target->vcpu);
			} else {
				vmm_vmx_softirq_deliver(target, (uint8_t)vector);
			}
			delivered = true;
		}
	}
	lwkt_reltoken(&machine->token);
	return delivered ? 0 : ENOENT;
}

static int
vmm_vmx_softirq_irq_set(struct vmm_vmx_interrupt_machine *machine,
    uint32_t gsi, bool level)
{
	if (gsi >= VMM_VMX_IOAPIC_PINS)
		return EINVAL;
	lwkt_gettoken(&machine->token);
	if (level == machine->ioapic_level[gsi]) {
		lwkt_reltoken(&machine->token);
		return 0;
	}
	machine->ioapic_level[gsi] = level;
	if (!level) {
		lwkt_reltoken(&machine->token);
		return 0;
	}
	vmm_vmx_ioapic_deliver_locked(machine, gsi);
	lwkt_reltoken(&machine->token);
	return 0;
}

static int
vmm_vmx_softirq_vcpu_io(struct vmm_vcpu *vcpu,
    const struct vmm_cpuexit_io *exit)
{
	int error;

	error = vmm_x64_pit_io(vcpu->machine, vcpu->state, exit);
	if (error != ENOENT)
		return error;
	return vmm_x64_pic_io(vcpu->machine, vcpu->state, exit);
}

static int
vmm_vmx_interrupt_raise_legacy(struct vmm_vmx_interrupt_machine *machine,
    uint8_t vector)
{
	struct vmm_vmx_interrupt_vcpu *target;

	(void)vector;
	lwkt_gettoken(&machine->token);
	target = machine->targets[0];
	if (target == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOENT;
	}
	(void)vmm_vcpu_kick(target->vcpu);
	lwkt_reltoken(&machine->token);
	return 0;
}

static int
vmm_vmx_interrupt_machine_get_ioapic(
	struct vmm_vmx_interrupt_machine *machine, struct vmm_ioapic_state *state)
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
vmm_vmx_interrupt_machine_set_ioapic(
	struct vmm_vmx_interrupt_machine *machine,
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
			vmm_vmx_ioapic_deliver_locked(machine, pin);
	}
	lwkt_reltoken(&machine->token);
	return 0;
}

/* Caller holds machine->token. */
static void
vmm_vmx_ioapic_deliver_locked(struct vmm_vmx_interrupt_machine *machine,
    uint32_t pin)
{
	struct vmm_vmx_interrupt_vcpu *target;
	uint64_t entry;
	uint32_t low;
	uint32_t destination;
	uint32_t id;
	int delivered;

	entry = machine->ioapic_redir[pin];
	low = entry;
	if ((low & VMM_VMX_IOAPIC_REDIR_MASKED) != 0 ||
	    ((low & VMM_VMX_IOAPIC_REDIR_LEVEL) != 0 &&
	    (low & VMM_VMX_IOAPIC_REDIR_REMOTE_IRR) != 0) ||
	    (low & VMM_VMX_IOAPIC_REDIR_DELIVERY_MASK) !=
	    VMM_VMX_IOAPIC_REDIR_FIXED || (low & 0xffU) < 32)
		return;
	destination = entry >> 56;
	delivered = 0;
	for (id = 0; id <= VMM_VMX_SOFTIRQ_MAX_APIC_ID; ++id) {
		target = machine->targets[id];
		if (target == NULL)
			continue;
		if ((low & VMM_VMX_IOAPIC_REDIR_DEST_LOGICAL) != 0) {
			if ((destination &
			    (vmm_vmx_softirq_read(target, VMM_VMX_APIC_LDR) >> 24)) == 0)
				continue;
		} else if (destination != target->apic_id) {
			continue;
		}
		vmm_vmx_softirq_deliver(target, low & 0xffU);
		delivered = 1;
	}
	if (delivered && (low & VMM_VMX_IOAPIC_REDIR_LEVEL) != 0)
		machine->ioapic_redir[pin] |= VMM_VMX_IOAPIC_REDIR_REMOTE_IRR;
}

static void
vmm_vmx_ioapic_reassert(struct vmm_vmx_interrupt_machine *machine,
    uint8_t vector)
{
	uint32_t pin;

	lwkt_gettoken(&machine->token);
	for (pin = 0; pin < VMM_VMX_IOAPIC_PINS; ++pin) {
		if (!machine->ioapic_level[pin] ||
		    (machine->ioapic_redir[pin] & VMM_VMX_IOAPIC_REDIR_LEVEL) == 0 ||
		    (machine->ioapic_redir[pin] & 0xffU) != vector)
			continue;
		machine->ioapic_redir[pin] &= ~VMM_VMX_IOAPIC_REDIR_REMOTE_IRR;
		vmm_vmx_ioapic_deliver_locked(machine, pin);
	}
	lwkt_reltoken(&machine->token);
}

static void
vmm_vmx_softirq_machine_destroy(struct vmm_vmx_interrupt_machine *machine)
{
	if (machine != NULL)
		os_mem_free(machine, sizeof(*machine));
}

static int
vmm_vmx_softirq_vcpu_create(struct vmm_vmx_interrupt_machine *machine,
    struct vmm_vcpu *vcpu, struct vmm_vmx_interrupt_vcpu **result,
    struct vmm_vmx_interrupt_config *config)
{
	struct vmm_vmx_interrupt_vcpu *soft;

	bzero(config, sizeof(*config));
	*result = NULL;
	if (!vcpu->machine->irqchip)
		return 0;
	if (vcpu->id > VMM_VMX_SOFTIRQ_MAX_APIC_ID)
		return E2BIG;
	soft = os_mem_zalloc(sizeof(*soft));
	if (soft == NULL)
		return ENOMEM;
	lwkt_token_init(&soft->timer_token, "vmmltimer");
	callout_init_mp(&soft->timer_callout);
	soft->machine = machine;
	soft->vcpu = vcpu;
	soft->apic_id = vcpu->id;
	soft->apic_base = VMM_VMX_SOFTIRQ_LAPIC_BASE |
	    VMM_VMX_APICBASE_ENABLED;
	if (soft->apic_id == 0)
		soft->apic_base |= VMM_VMX_APICBASE_BSP;
	if (os_contigpa_zalloc(&soft->apic_page_pa,
	    (vaddr_t *)&soft->apic_page, 1) != 0) {
		callout_terminate(&soft->timer_callout);
		os_mem_free(soft, sizeof(*soft));
		return ENOMEM;
	}
	soft->wait_sipi = soft->apic_id != 0;
	vmm_vmx_softirq_vcpu_reset_lapic(soft);
	lwkt_gettoken(&machine->token);
	if (machine->targets[soft->apic_id] != NULL) {
		lwkt_reltoken(&machine->token);
		os_contigpa_free(soft->apic_page_pa, (vaddr_t)soft->apic_page, 1);
		callout_terminate(&soft->timer_callout);
		os_mem_free(soft, sizeof(*soft));
		return EEXIST;
	}
	machine->targets[soft->apic_id] = soft;
	lwkt_reltoken(&machine->token);
	*result = soft;
	return 0;
}

static void
vmm_vmx_softirq_vcpu_reset_lapic(struct vmm_vmx_interrupt_vcpu *vcpu)
{

	lwkt_gettoken(&vcpu->timer_token);
	callout_stop_async(&vcpu->timer_callout);
	vcpu->timer_lvtt = VMM_VMX_APIC_LVT_MASKED;
	vcpu->timer_tmict = 0;
	vcpu->timer_tdcr = 0;
	vcpu->timer_divisor = 2;
	vcpu->timer_interval_tsc = 0;
	vcpu->timer_deadline_tsc = 0;
	vcpu->timer_active = false;
	vcpu->timer_expired = false;
	lwkt_reltoken(&vcpu->timer_token);

	bzero(vcpu->apic_page, PAGE_SIZE);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_ID, vcpu->apic_id << 24);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_VERSION,
	    VMM_VMX_APIC_VERSION_VALUE);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_DFR, VMM_VMX_APIC_DFR_FLAT);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_SVR, 0xff);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_LVTT,
	    VMM_VMX_APIC_LVT_MASKED);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_LVT_THERMAL,
	    VMM_VMX_APIC_LVT_MASKED);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_LVT_PERF,
	    VMM_VMX_APIC_LVT_MASKED);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_LVT0,
	    vcpu->apic_id == 0 ? VMM_VMX_APIC_LVT_EXTINT :
	    VMM_VMX_APIC_LVT_MASKED);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_LVT1,
	    VMM_VMX_APIC_LVT_MASKED);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_LVT_ERROR,
	    VMM_VMX_APIC_LVT_MASKED);
}

static void
vmm_vmx_softirq_vcpu_reset(struct vmm_vmx_interrupt_vcpu *vcpu)
{
	struct vmm_cpustate *state = vcpu->vcpu->state;
	uint32_t mxcsr_mask = state->fpu.fx_mxcsr_mask;
	unsigned int index;

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
	vcpu->apic_base = VMM_VMX_SOFTIRQ_LAPIC_BASE |
	    VMM_VMX_APICBASE_ENABLED;
	vcpu->delivery_pending = false;
	vcpu->delivery_legacy = false;
	atomic_clear_int(&vcpu->nmi_pending, 1);
	vcpu->nmi_delivery_pending = false;
	vmm_vmx_softirq_vcpu_reset_lapic(vcpu);
	vcpu->wait_sipi = true;
}

static int
vmm_vmx_softirq_vcpu_prepare(struct vmm_vmx_interrupt_vcpu *vcpu)
{
	if (vcpu == NULL || vcpu->apic_id == 0)
		return 0;
	for (;;) {
		if (atomic_swap_int(&vcpu->init_pending, 0) != 0)
			vmm_vmx_softirq_vcpu_reset(vcpu);
		if (!vcpu->wait_sipi)
			return 0;
		if (atomic_swap_int(&vcpu->sipi_pending, 0) != 0) {
			vcpu->vcpu->state->segs[VMM_X64_SEG_CS].selector =
			    (uint16_t)atomic_load_acq_int(&vcpu->sipi_vector) << 8;
			vcpu->vcpu->state->segs[VMM_X64_SEG_CS].base =
			    (uint64_t)atomic_load_acq_int(&vcpu->sipi_vector) << 12;
			vcpu->vcpu->state->gprs[VMM_X64_GPR_RIP] = 0;
			vcpu->wait_sipi = false;
			return EINPROGRESS;
		}
		tsleep_interlock(vcpu->vcpu, 0);
		if (atomic_load_acq_int(&vcpu->init_pending) == 0 &&
		    atomic_load_acq_int(&vcpu->sipi_pending) == 0)
			return EAGAIN;
	}
}

static void
vmm_vmx_softirq_vcpu_destroy(struct vmm_vmx_interrupt_vcpu *vcpu)
{
	if (vcpu == NULL)
		return;
	lwkt_gettoken(&vcpu->timer_token);
	vcpu->timer_active = false;
	vcpu->timer_expired = false;
	callout_stop_async(&vcpu->timer_callout);
	lwkt_reltoken(&vcpu->timer_token);
	callout_drain(&vcpu->timer_callout);
	callout_terminate(&vcpu->timer_callout);
	lwkt_gettoken(&vcpu->machine->token);
	if (vcpu->machine->targets[vcpu->apic_id] == vcpu)
		vcpu->machine->targets[vcpu->apic_id] = NULL;
	lwkt_reltoken(&vcpu->machine->token);
	os_contigpa_free(vcpu->apic_page_pa, (vaddr_t)vcpu->apic_page, 1);
	os_mem_free(vcpu, sizeof(*vcpu));
}

static void
vmm_vmx_softirq_vcpu_enter(struct vmm_vmx_interrupt_vcpu *vcpu)
{
	volatile uint32_t *irr;
	struct vmm_machine *machine;
	int error;
	int word;
	int bit;
	u_int pending;
	uint32_t value;
	uint8_t vector;

	if (vcpu == NULL)
		return;
	vmm_vmx_lapic_timer_check(vcpu);
	if (vcpu->delivery_pending)
		return;
	pending = atomic_swap_int(&vcpu->nmi_pending, 0);
	if (pending != 0) {
		if (vmm_vmx_vcpu_inject_interrupt(vcpu->vcpu, 2) != 0)
			atomic_set_int(&vcpu->nmi_pending, pending);
		else
			vcpu->nmi_delivery_pending = true;
		return;
	}
	if (vmm_vmx_lapic_accepts_pic(vcpu)) {
		if (!vmm_vmx_vcpu_interrupt_allowed(vcpu->vcpu)) {
			vmm_vmx_vcpu_request_interrupt_window(vcpu->vcpu);
			return;
		}
		machine = vcpu->vcpu->machine;
		lwkt_gettoken(&machine->token);
		error = vmm_x64_pic_peek_locked(machine, &vector);
		if (error == 0)
			error = vmm_vmx_vcpu_inject_interrupt(vcpu->vcpu, vector);
		if (error == 0)
			error = vmm_x64_pic_accept_locked(machine, &vector);
		if (error == 0) {
			vcpu->delivery_pending = true;
			vcpu->delivery_legacy = true;
			vcpu->delivery_vector = vector;
		}
		lwkt_reltoken(&machine->token);
		if (error != ENOENT)
			return;
	}
	if (!vmm_vmx_lapic_enabled(vcpu))
		return;
	for (word = 7; word >= 1; --word) {
		irr = (volatile uint32_t *)((uint8_t *)vcpu->apic_page +
		    VMM_VMX_APIC_IRR_BASE + word * 0x10);
		value = atomic_load_acq_int((volatile u_int *)irr);
		if (value == 0)
			continue;
		bit = fls(value) - 1;
		vector = word * 32 + bit;
		if ((vector & 0xf0U) <= vmm_vmx_lapic_ppr(vcpu))
			return;
		if (!vmm_vmx_vcpu_interrupt_allowed(vcpu->vcpu)) {
			vmm_vmx_vcpu_request_interrupt_window(vcpu->vcpu);
			return;
		}
		if (vmm_vmx_vcpu_inject_interrupt(vcpu->vcpu, vector) == 0) {
			vcpu->delivery_pending = true;
			vcpu->delivery_legacy = false;
			vcpu->delivery_vector = vector;
		}
		return;
	}
}

static void
vmm_vmx_softirq_vcpu_leave(struct vmm_vmx_interrupt_vcpu *vcpu)
{
	(void)vcpu;
}

static void
vmm_vmx_softirq_vcpu_event_result(struct vmm_vmx_interrupt_vcpu *vcpu,
    bool reinjected)
{
	volatile uint32_t *irr;
	volatile uint32_t *isr;
	uint8_t vector;

	if (vcpu == NULL || !vcpu->delivery_pending || reinjected)
		return;
	vector = vcpu->delivery_vector;
	if (vcpu->delivery_legacy) {
		/* The PIC moved IRR to ISR when vcpu_enter committed this event. */
	} else {
		irr = (volatile uint32_t *)((uint8_t *)vcpu->apic_page +
	    VMM_VMX_APIC_IRR_BASE + (vector / 32) * 0x10);
		isr = (volatile uint32_t *)((uint8_t *)vcpu->apic_page +
	    VMM_VMX_APIC_ISR_BASE + (vector / 32) * 0x10);
		atomic_clear_int((volatile u_int *)irr, __BIT(vector & 31));
		atomic_set_int((volatile u_int *)isr, __BIT(vector & 31));
	}
	vcpu->delivery_pending = false;
}

static int
vmm_vmx_softirq_vcpu_exit(struct vmm_vmx_interrupt_vcpu *vcpu,
    uint64_t exitcode, uint64_t exitinfo1, uint64_t exitinfo2)
{
	if (vcpu != NULL && exitcode == VMM_VMX_EXIT_NMI_WINDOW &&
	    vcpu->nmi_delivery_pending) {
		vcpu->nmi_delivery_pending = false;
		return 1;
	}
	(void)exitinfo1;
	(void)exitinfo2;
	return 0;
}

static uint32_t
vmm_vmx_softirq_read(const struct vmm_vmx_interrupt_vcpu *vcpu, uint32_t reg)
{
	volatile uint32_t *value;

	value = (volatile uint32_t *)((uint8_t *)vcpu->apic_page + reg);
	return *value;
}

static int
vmm_vmx_interrupt_vcpu_get_lapic(struct vmm_vmx_interrupt_vcpu *vcpu,
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
		error = vmm_vmx_lapic_read(vcpu, reg, &value);
		if (error != 0)
			return error;
		bcopy(&value, state + reg, sizeof(value));
	}
	return 0;
}

static int
vmm_vmx_interrupt_vcpu_set_lapic(struct vmm_vmx_interrupt_vcpu *vcpu,
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
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_ID, vcpu->apic_id << 24);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_VERSION,
	    VMM_VMX_APIC_VERSION_VALUE);
	vcpu->timer_lvtt = vmm_vmx_softirq_read(vcpu, VMM_VMX_APIC_LVTT) &
	    VMM_VMX_APIC_LVT_TIMER_VALID;
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_LVTT, vcpu->timer_lvtt);
	vcpu->timer_tdcr = vmm_vmx_softirq_read(vcpu, VMM_VMX_APIC_TDCR) &
	    VMM_VMX_APIC_TIMER_DIVIDE_VALID;
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_TDCR, vcpu->timer_tdcr);
	divide = ((vcpu->timer_tdcr & 0x3U) |
	    ((vcpu->timer_tdcr & 0x8U) >> 1)) + 1;
	vcpu->timer_divisor = 1U << (divide & 0x7U);
	value = vmm_vmx_softirq_read(vcpu, VMM_VMX_APIC_TMICT);
	vmm_vmx_lapic_timer_arm_locked(vcpu, value);
	lwkt_reltoken(&vcpu->timer_token);
	return 0;
}

static void
vmm_vmx_softirq_write(const struct vmm_vmx_interrupt_vcpu *vcpu, uint32_t reg,
    uint32_t value)
{
	volatile uint32_t *slot;

	slot = (volatile uint32_t *)((uint8_t *)vcpu->apic_page + reg);
	*slot = value;
}

static void
vmm_vmx_lapic_timer_arm(struct vmm_vmx_interrupt_vcpu *vcpu,
    uint32_t count)
{
	lwkt_gettoken(&vcpu->timer_token);
	vmm_vmx_lapic_timer_arm_locked(vcpu, count);
	lwkt_reltoken(&vcpu->timer_token);
}

/* Caller holds vcpu->timer_token. */
static void
vmm_vmx_lapic_timer_arm_locked(struct vmm_vmx_interrupt_vcpu *vcpu,
    uint32_t count)
{
	uint64_t delta;
	uint64_t now;

	callout_stop_async(&vcpu->timer_callout);
	vcpu->timer_expired = false;
	vcpu->timer_tmict = count;
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_TMICT, count);
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_TMCCT, count);
	if (count == 0 ||
	    (vcpu->timer_lvtt & VMM_VMX_APIC_LVT_TIMER_MODE) ==
	    VMM_VMX_APIC_LVT_TIMER_TSCDEADLINE || tsc_frequency == 0) {
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
	vmm_vmx_lapic_timer_schedule_locked(vcpu, now);
}

/* Caller holds vcpu->timer_token. */
static void
vmm_vmx_lapic_timer_schedule_locked(struct vmm_vmx_interrupt_vcpu *vcpu,
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
	    vmm_vmx_lapic_timer_timeout, vcpu);
}

/* Caller holds vcpu->timer_token. */
static bool
vmm_vmx_lapic_timer_expire_locked(struct vmm_vmx_interrupt_vcpu *vcpu,
    uint64_t now)
{
	uint64_t periods;

	if (!vcpu->timer_active || now < vcpu->timer_deadline_tsc)
		return false;
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_TMCCT, 0);
	vcpu->timer_expired = true;
	if ((vcpu->timer_lvtt & VMM_VMX_APIC_LVT_TIMER_MODE) !=
	    VMM_VMX_APIC_LVT_TIMER_PERIODIC || vcpu->timer_interval_tsc == 0 ||
	    vcpu->timer_tmict == 0) {
		vcpu->timer_active = false;
		return true;
	}
	periods = (now - vcpu->timer_deadline_tsc) / vcpu->timer_interval_tsc +
	    1;
	if (periods > (UINT64_MAX - vcpu->timer_deadline_tsc) /
	    vcpu->timer_interval_tsc) {
		vcpu->timer_active = false;
		vcpu->timer_deadline_tsc = 0;
		return true;
	}
	vcpu->timer_deadline_tsc += periods * vcpu->timer_interval_tsc;
	vmm_vmx_softirq_write(vcpu, VMM_VMX_APIC_TMCCT,
	    vcpu->timer_tmict);
	return true;
}

static void
vmm_vmx_lapic_timer_timeout(void *argument)
{
	struct vmm_vmx_interrupt_vcpu *vcpu;
	uint64_t now;
	bool expired;

	vcpu = argument;
	now = rdtsc();
	lwkt_gettoken(&vcpu->timer_token);
	expired = vmm_vmx_lapic_timer_expire_locked(vcpu, now);
	if (vcpu->timer_active)
		vmm_vmx_lapic_timer_schedule_locked(vcpu, now);
	lwkt_reltoken(&vcpu->timer_token);
	if (expired)
		(void)vmm_vcpu_kick(vcpu->vcpu);
}

static void
vmm_vmx_lapic_timer_check(struct vmm_vmx_interrupt_vcpu *vcpu)
{
	uint64_t now;
	uint8_t vector;
	bool deliver;

	now = rdtsc();
	lwkt_gettoken(&vcpu->timer_token);
	if (vmm_vmx_lapic_timer_expire_locked(vcpu, now) &&
	    vcpu->timer_active)
		vmm_vmx_lapic_timer_schedule_locked(vcpu, now);
	deliver = vcpu->timer_expired;
	vcpu->timer_expired = false;
	vector = vcpu->timer_lvtt & VMM_VMX_APIC_LVT_VECTOR_MASK;
	if ((vcpu->timer_lvtt & VMM_VMX_APIC_LVT_MASKED) != 0 || vector < 32)
		deliver = false;
	lwkt_reltoken(&vcpu->timer_token);
	if (deliver)
		vmm_vmx_softirq_deliver(vcpu, vector);
}

static int
vmm_vmx_lapic_eoi(struct vmm_vmx_interrupt_vcpu *vcpu)
{
	volatile uint32_t *isr;
	int word;
	int bit;

	for (word = 7; word >= 0; --word) {
		isr = (volatile uint32_t *)((uint8_t *)vcpu->apic_page +
		    VMM_VMX_APIC_ISR_BASE + word * 0x10);
		if (*isr == 0)
			continue;
		bit = fls(*isr) - 1;
		atomic_clear_int((volatile u_int *)isr, __BIT(bit));
		vmm_vmx_ioapic_reassert(vcpu->machine, word * 32 + bit);
		return word * 32 + bit;
	}
	return -1;
}

static int
vmm_vmx_lapic_read(struct vmm_vmx_interrupt_vcpu *vcpu, uint32_t reg,
    uint32_t *value)
{
	uint64_t now;

	if (reg > PAGE_SIZE - sizeof(*value) || (reg & 3) != 0)
		return EINVAL;
	if (reg == VMM_VMX_APIC_PPR) {
		*value = vmm_vmx_lapic_ppr(vcpu);
		return 0;
	}
	if (reg != VMM_VMX_APIC_TMCCT) {
		*value = vmm_vmx_softirq_read(vcpu, reg);
		return 0;
	}
	now = rdtsc();
	lwkt_gettoken(&vcpu->timer_token);
	if (vcpu->timer_active && vcpu->timer_interval_tsc != 0 &&
	    now < vcpu->timer_deadline_tsc) {
		*value = (uint32_t)((uint64_t)vcpu->timer_tmict *
		    (vcpu->timer_deadline_tsc - now) /
		    vcpu->timer_interval_tsc);
	} else {
		*value = vmm_vmx_softirq_read(vcpu, reg);
	}
	lwkt_reltoken(&vcpu->timer_token);
	return 0;
}

/*
 * PPR is the active task priority.  An in-service vector overrides the TPR
 * priority class; otherwise the complete TPR value remains visible.
 */
static uint8_t
vmm_vmx_lapic_ppr(struct vmm_vmx_interrupt_vcpu *vcpu)
{
	volatile uint32_t *isr;
	uint8_t tpr;
	int word;
	int bit;
	uint32_t value;

	tpr = vmm_vmx_softirq_read(vcpu, VMM_VMX_APIC_TPR);
	for (word = 7; word >= 0; --word) {
		isr = (volatile uint32_t *)((uint8_t *)vcpu->apic_page +
		    VMM_VMX_APIC_ISR_BASE + word * 0x10);
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

static int
vmm_vmx_softirq_vcpu_msr(struct vmm_vmx_interrupt_vcpu *vcpu, bool write,
    uint32_t msr, uint64_t *value)
{

	if (msr != VMM_VMX_MSR_APICBASE || value == NULL)
		return ENOENT;
	if (!write) {
		*value = vcpu->apic_base;
		return 0;
	}
	if ((*value & ~VMM_VMX_SOFTIRQ_APICBASE_VALID) != 0 ||
	    (*value & VMM_VMX_APICBASE_ADDRESS) != VMM_VMX_SOFTIRQ_LAPIC_BASE)
		return EINVAL;
	vcpu->apic_base = *value;
	return 0;
}

static int
vmm_vmx_lapic_write(struct vmm_vmx_interrupt_vcpu *vcpu, uint32_t reg,
    uint32_t value)
{
	uint32_t divide;

	switch (reg) {
	case VMM_VMX_APIC_TPR:
		value &= 0xffU;
		break;
	case VMM_VMX_APIC_LDR:
		value &= 0xff000000U;
		vmm_vmx_softirq_write(vcpu, reg, value);
		return 0;
	case VMM_VMX_APIC_DFR:
		vmm_vmx_softirq_write(vcpu, reg, value);
		return 0;
	case VMM_VMX_APIC_EOI:
		(void)vmm_vmx_lapic_eoi(vcpu);
		return 0;
	case VMM_VMX_APIC_ESR:
		value = 0;
		break;
	case VMM_VMX_APIC_LVTT:
		value &= VMM_VMX_APIC_LVT_TIMER_VALID;
		lwkt_gettoken(&vcpu->timer_token);
		vcpu->timer_lvtt = value;
		vmm_vmx_softirq_write(vcpu, reg, value);
		vmm_vmx_lapic_timer_arm_locked(vcpu, vcpu->timer_tmict);
		lwkt_reltoken(&vcpu->timer_token);
		return 0;
	case VMM_VMX_APIC_TMICT:
		vmm_vmx_lapic_timer_arm(vcpu, value);
		return 0;
	case VMM_VMX_APIC_TDCR:
		value &= VMM_VMX_APIC_TIMER_DIVIDE_VALID;
		lwkt_gettoken(&vcpu->timer_token);
		vcpu->timer_tdcr = value;
		divide = ((value & 0x3U) | ((value & 0x8U) >> 1)) + 1;
		vcpu->timer_divisor = 1U << (divide & 0x7U);
		vmm_vmx_softirq_write(vcpu, reg, value);
		vmm_vmx_lapic_timer_arm_locked(vcpu, vcpu->timer_tmict);
		lwkt_reltoken(&vcpu->timer_token);
		return 0;
	case VMM_VMX_APIC_SVR:
		value &= VMM_VMX_APIC_SVR_VALID;
		break;
	case VMM_VMX_APIC_LVT_ERROR:
	case VMM_VMX_APIC_LVT_THERMAL:
	case VMM_VMX_APIC_LVT_PERF:
	case VMM_VMX_APIC_LVT0:
	case VMM_VMX_APIC_LVT1:
		value &= VMM_VMX_APIC_LVT_COMMON_VALID |
		    VMM_VMX_APIC_LVT_DELIVERY_MASK | VMM_VMX_APIC_LVT_POLARITY |
		    VMM_VMX_APIC_LVT_LEVEL;
		break;
	default:
		return ENOENT;
	}
	vmm_vmx_softirq_write(vcpu, reg, value);
	return 0;
}

static bool
vmm_vmx_lapic_enabled(const struct vmm_vmx_interrupt_vcpu *vcpu)
{

	return (vcpu->apic_base & VMM_VMX_APICBASE_ENABLED) != 0 &&
	    (vmm_vmx_softirq_read(vcpu, VMM_VMX_APIC_SVR) &
	    VMM_VMX_APIC_SVR_ENABLE) != 0;
}

/*
 * The legacy PIC reaches a hardware-enabled local APIC through LINT0 ExtINT.
 * SVR software enable controls LAPIC IRR delivery and does not gate this wire.
 */
static bool
vmm_vmx_lapic_accepts_pic(const struct vmm_vmx_interrupt_vcpu *vcpu)
{
	uint32_t lvt0;

	if ((vcpu->apic_base & VMM_VMX_APICBASE_ENABLED) == 0)
		return true;
	lvt0 = vmm_vmx_softirq_read(vcpu, VMM_VMX_APIC_LVT0);
	return (lvt0 & VMM_VMX_APIC_LVT_MASKED) == 0 &&
	    (lvt0 & VMM_VMX_APIC_LVT_DELIVERY_MASK) ==
	    VMM_VMX_APIC_LVT_EXTINT;
}

static void
vmm_vmx_softirq_deliver(struct vmm_vmx_interrupt_vcpu *vcpu, uint8_t vector)
{
	volatile uint32_t *irr;

	if (vector < 32 || !vmm_vmx_lapic_enabled(vcpu))
		return;
	irr = (volatile uint32_t *)((uint8_t *)vcpu->apic_page +
	    VMM_VMX_APIC_IRR_BASE + (vector / 32) * 0x10);
	atomic_set_int((volatile u_int *)irr, __BIT(vector & 31));
	cpu_mfence();
	(void)vmm_vcpu_kick(vcpu->vcpu);
}

static int
vmm_vmx_softirq_vcpu_mmio(struct vmm_vmx_interrupt_vcpu *vcpu,
    uint64_t address,
    bool write, uint32_t *value)
{
	struct vmm_vmx_interrupt_machine *machine = vcpu->machine;
	uint32_t reg;
	uint32_t pin;
	uint64_t entry;

	if (address >= VMM_VMX_SOFTIRQ_LAPIC_BASE &&
	    address < VMM_VMX_SOFTIRQ_LAPIC_BASE + PAGE_SIZE) {
		if ((vcpu->apic_base & VMM_VMX_APICBASE_ENABLED) == 0)
			return ENOENT;
		reg = address - VMM_VMX_SOFTIRQ_LAPIC_BASE;
		if ((reg & 3) != 0)
			return ENOENT;
		if (!write)
			return vmm_vmx_lapic_read(vcpu, reg, value);
		if (reg == VMM_VMX_APIC_ICR_LOW) {
			vmm_vmx_softirq_write(vcpu, reg, *value);
			return vmm_vmx_softirq_route_icr(vcpu, *value,
			    vmm_vmx_softirq_read(vcpu, VMM_VMX_APIC_ICR_HIGH), 0) ?
			    0 : ENOTSUP;
		}
		if (reg == VMM_VMX_APIC_ICR_HIGH) {
			vmm_vmx_softirq_write(vcpu, reg, *value);
			return 0;
		}
		return vmm_vmx_lapic_write(vcpu, reg, *value);
	}
	if (address != VMM_VMX_IOAPIC_BASE &&
	    address != VMM_VMX_IOAPIC_BASE + 0x10)
		return ENOENT;
	lwkt_gettoken(&machine->token);
	if (address == VMM_VMX_IOAPIC_BASE) {
		if (write)
			machine->ioapic_select = *value & 0xffU;
		else
			*value = machine->ioapic_select;
		lwkt_reltoken(&machine->token);
		return 0;
	}
	reg = machine->ioapic_select;
	if (!write) {
		if (reg == VMM_VMX_IOAPIC_REG_ID || reg == VMM_VMX_IOAPIC_REG_ARB)
			*value = machine->ioapic_id << 24;
		else if (reg == VMM_VMX_IOAPIC_REG_VERSION)
			*value = VMM_VMX_IOAPIC_VERSION;
		else if (reg >= VMM_VMX_IOAPIC_REDIR_BASE &&
		    reg < VMM_VMX_IOAPIC_REDIR_BASE + VMM_VMX_IOAPIC_PINS * 2) {
			pin = (reg - VMM_VMX_IOAPIC_REDIR_BASE) / 2;
			entry = machine->ioapic_redir[pin];
			*value = (reg & 1) ? entry >> 32 : entry;
		} else
			*value = 0;
		lwkt_reltoken(&machine->token);
		return 0;
	}
	if (reg == VMM_VMX_IOAPIC_REG_ID)
		machine->ioapic_id = (*value >> 24) & 0x0fU;
	else if (reg >= VMM_VMX_IOAPIC_REDIR_BASE &&
	    reg < VMM_VMX_IOAPIC_REDIR_BASE + VMM_VMX_IOAPIC_PINS * 2) {
		pin = (reg - VMM_VMX_IOAPIC_REDIR_BASE) / 2;
		entry = machine->ioapic_redir[pin];
		if (reg & 1)
			entry = (entry & 0xffffffffULL) | ((uint64_t)*value << 32);
		else
			entry = (entry & 0xffffffff00000000ULL) | *value;
		entry = (entry & ~VMM_VMX_IOAPIC_REDIR_REMOTE_IRR) |
		    (machine->ioapic_redir[pin] &
		    VMM_VMX_IOAPIC_REDIR_REMOTE_IRR);
		if ((entry & VMM_VMX_IOAPIC_REDIR_LEVEL) == 0)
			entry &= ~VMM_VMX_IOAPIC_REDIR_REMOTE_IRR;
		machine->ioapic_redir[pin] = entry;
		if (machine->ioapic_level[pin])
			vmm_vmx_ioapic_deliver_locked(machine, pin);
	}
	lwkt_reltoken(&machine->token);
	return 0;
}

static int
vmm_vmx_softirq_route_icr(struct vmm_vmx_interrupt_vcpu *source,
    uint32_t low, uint32_t high, int x2apic)
{
	struct vmm_vmx_interrupt_machine *machine = source->machine;
	struct vmm_vmx_interrupt_vcpu *target;
	uint32_t destination;
	uint32_t delivery;
	uint32_t shorthand;
	uint32_t source_dfr;
	uint32_t target_ldr;
	uint32_t id;
	uint32_t vector;

	delivery = low & VMM_VMX_APIC_ICR_DELIVERY_MASK;
	shorthand = low & VMM_VMX_APIC_ICR_SHORTHAND;
	vector = low & 0xffU;
	/*
	 * A broadcast without a recipient is architecturally complete.  In
	 * particular, SeaBIOS issues INIT | all-excluding-self for a one-vCPU
	 * guest.  A targeted NMI, INIT, or SIPI remains unhandled until the
	 * vCPU lifecycle implements it.
	 */
	if (delivery != VMM_VMX_APIC_ICR_FIXED &&
	    delivery != VMM_VMX_APIC_ICR_NMI &&
	    delivery != VMM_VMX_APIC_ICR_INIT &&
	    delivery != VMM_VMX_APIC_ICR_SIPI)
		return 0;
	if (delivery == VMM_VMX_APIC_ICR_FIXED && vector < 16)
		return 1;
	destination = x2apic ? high : high >> 24;
	lwkt_gettoken(&machine->token);
	source_dfr = vmm_vmx_softirq_read(source, VMM_VMX_APIC_DFR);
	for (id = 0; id <= VMM_VMX_SOFTIRQ_MAX_APIC_ID; ++id) {
		target = machine->targets[id];
		if (target == NULL)
			continue;
		if (shorthand == VMM_VMX_APIC_ICR_SELF && target != source)
			continue;
		if (shorthand == VMM_VMX_APIC_ICR_ALL_EXC_SELF && target == source)
			continue;
		if (shorthand == 0) {
			if ((low & VMM_VMX_APIC_ICR_DEST_LOGICAL) == 0) {
				if (destination != target->apic_id &&
				    destination != (x2apic ?
				    VMM_VMX_APIC_ICR_X2_DEST_BROADCAST :
				    VMM_VMX_APIC_ICR_DEST_BROADCAST))
					continue;
			} else {
				target_ldr = vmm_vmx_softirq_read(target,
				    VMM_VMX_APIC_LDR) >> 24;
				if (source_dfr == VMM_VMX_APIC_DFR_FLAT) {
					if ((destination & target_ldr) == 0)
						continue;
				} else if (source_dfr != VMM_VMX_APIC_DFR_CLUSTER ||
				    (destination & 0xf0U) != (target_ldr & 0xf0U) ||
				    (destination & target_ldr & 0x0fU) == 0) {
					continue;
				}
			}
		}
		switch (delivery) {
		case VMM_VMX_APIC_ICR_FIXED:
			vmm_vmx_softirq_deliver(target, (uint8_t)vector);
			break;
		case VMM_VMX_APIC_ICR_NMI:
			atomic_set_int(&target->nmi_pending, 1);
			(void)vmm_vcpu_kick(target->vcpu);
			wakeup(target->vcpu);
			break;
		case VMM_VMX_APIC_ICR_INIT:
			atomic_store_rel_int(&target->sipi_pending, 0);
			atomic_store_rel_int(&target->init_pending, 1);
			(void)vmm_vcpu_kick(target->vcpu);
			wakeup(target->vcpu);
			break;
		case VMM_VMX_APIC_ICR_SIPI:
			atomic_store_rel_int(&target->sipi_vector, vector);
			atomic_store_rel_int(&target->sipi_pending, 1);
			(void)vmm_vcpu_kick(target->vcpu);
			wakeup(target->vcpu);
			break;
		}
	}
	lwkt_reltoken(&machine->token);
	return 1;
}
