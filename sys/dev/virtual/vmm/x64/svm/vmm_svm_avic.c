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
#include <sys/kernel.h>
#include <sys/systm.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_page.h>

#include "../../vmm_machine.h"
#include "../../vmm_vcpu.h"
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
#define VMM_SVM_APIC_TDCR			0x3e0U

#define VMM_SVM_APIC_VERSION_VALUE	0x00140014U
#define VMM_SVM_APIC_SVR_ENABLE	__BIT(8)
#define VMM_SVM_APIC_SVR_VALID		0x000003ffU
#define VMM_SVM_APIC_DFR_CLUSTER	0x0fffffffU
#define VMM_SVM_APIC_DFR_FLAT		0xffffffffU
#define VMM_SVM_APIC_LVT_VECTOR_MASK	0x000000ffU
#define VMM_SVM_APIC_LVT_DELIVERY_MASK	0x00000700U
#define VMM_SVM_APIC_LVT_SEND_PENDING	0x00001000U
#define VMM_SVM_APIC_LVT_POLARITY		0x00002000U
#define VMM_SVM_APIC_LVT_REMOTE_IRR		0x00004000U
#define VMM_SVM_APIC_LVT_LEVEL		0x00008000U
#define VMM_SVM_APIC_LVT_MASKED		0x00010000U
#define VMM_SVM_APIC_LVT_TIMER_MODE		0x00060000U
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

#define VMM_SVM_EXIT_AVIC_INCOMPLETE_IPI	0x0401ULL
#define VMM_SVM_EXIT_AVIC_NOACCEL		0x0402ULL
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
	struct vmm_svm_interrupt_vcpu *targets[VMM_SVM_AVIC_MAX_PHYS_ID + 1];
};

struct vmm_svm_interrupt_vcpu {
	struct vmm_svm_interrupt_machine *machine;
	struct vmm_vcpu *vcpu;
	void *apic_page;
	paddr_t apic_page_pa;
	uint32_t apic_id;
	volatile int host_cpu;
	volatile int host_apic_id;
	volatile int running;
};

static int vmm_svm_soft_machine_create(struct vmm_machine *,
    struct vmm_svm_interrupt_machine **);
static int vmm_svm_soft_machine_enable(struct vmm_svm_interrupt_machine *);
static void vmm_svm_soft_machine_destroy(struct vmm_svm_interrupt_machine *);
static int vmm_svm_soft_vcpu_create(struct vmm_svm_interrupt_machine *,
    struct vmm_vcpu *, struct vmm_svm_interrupt_vcpu **,
    struct vmm_svm_interrupt_config *);
static void vmm_svm_soft_vcpu_destroy(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_soft_vcpu_enter(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_soft_vcpu_leave(struct vmm_svm_interrupt_vcpu *);
static int vmm_svm_soft_vcpu_exit(struct vmm_svm_interrupt_vcpu *, uint64_t,
    uint64_t, uint64_t);

static int vmm_svm_avic_machine_create(struct vmm_machine *,
    struct vmm_svm_interrupt_machine **);
static int vmm_svm_avic_machine_enable(struct vmm_svm_interrupt_machine *);
static void vmm_svm_avic_machine_destroy(struct vmm_svm_interrupt_machine *);
static int vmm_svm_avic_vcpu_create(struct vmm_svm_interrupt_machine *,
    struct vmm_vcpu *, struct vmm_svm_interrupt_vcpu **,
    struct vmm_svm_interrupt_config *);
static void vmm_svm_avic_vcpu_destroy(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_avic_vcpu_enter(struct vmm_svm_interrupt_vcpu *);
static void vmm_svm_avic_vcpu_leave(struct vmm_svm_interrupt_vcpu *);
static int vmm_svm_avic_vcpu_exit(struct vmm_svm_interrupt_vcpu *, uint64_t,
    uint64_t, uint64_t);
static void vmm_svm_avic_logical_update_locked(
    struct vmm_svm_interrupt_vcpu *);
static int vmm_svm_avic_route_icr(struct vmm_svm_interrupt_vcpu *, uint32_t,
    uint32_t, int);
static void vmm_svm_avic_deliver(struct vmm_svm_interrupt_vcpu *, uint8_t);

const struct vmm_svm_interrupt_ops vmm_svm_soft_interrupt_ops = {
	.name = "software",
	.machine_create = vmm_svm_soft_machine_create,
	.machine_enable = vmm_svm_soft_machine_enable,
	.machine_destroy = vmm_svm_soft_machine_destroy,
	.vcpu_create = vmm_svm_soft_vcpu_create,
	.vcpu_destroy = vmm_svm_soft_vcpu_destroy,
	.vcpu_enter = vmm_svm_soft_vcpu_enter,
	.vcpu_leave = vmm_svm_soft_vcpu_leave,
	.vcpu_exit = vmm_svm_soft_vcpu_exit,
};

const struct vmm_svm_interrupt_ops vmm_svm_avic_interrupt_ops = {
	.name = "avic",
	.machine_create = vmm_svm_avic_machine_create,
	.machine_enable = vmm_svm_avic_machine_enable,
	.machine_destroy = vmm_svm_avic_machine_destroy,
	.vcpu_create = vmm_svm_avic_vcpu_create,
	.vcpu_destroy = vmm_svm_avic_vcpu_destroy,
	.vcpu_enter = vmm_svm_avic_vcpu_enter,
	.vcpu_leave = vmm_svm_avic_vcpu_leave,
	.vcpu_exit = vmm_svm_avic_vcpu_exit,
};

bool
vmm_svm_avic_available(void)
{
	cpuid_desc_t desc;
	uint32_t cpu;

	x86_get_cpuid(0x8000000a, &desc);
	if ((desc.edx & CPUID_8_0A_EDX_AVIC) == 0)
		return false;
	for (cpu = 0; cpu < ncpus; ++cpu) {
		if ((CPUID_TO_APICID(cpu) & ~VMM_SVM_AVIC_HOST_APIC_ID_MASK) != 0)
			return false;
	}
	return true;
}

static int
vmm_svm_soft_machine_create(struct vmm_machine *machine,
    struct vmm_svm_interrupt_machine **result)
{
	(void)machine;
	*result = NULL;
	return 0;
}

static int
vmm_svm_soft_machine_enable(struct vmm_svm_interrupt_machine *machine)
{
	(void)machine;
	return ENOTSUP;
}

static void
vmm_svm_soft_machine_destroy(struct vmm_svm_interrupt_machine *machine)
{
	(void)machine;
}

static int
vmm_svm_soft_vcpu_create(struct vmm_svm_interrupt_machine *machine,
    struct vmm_vcpu *vcpu, struct vmm_svm_interrupt_vcpu **result,
    struct vmm_svm_interrupt_config *config)
{
	(void)machine;
	(void)vcpu;
	*result = NULL;
	bzero(config, sizeof(*config));
	return 0;
}

static void
vmm_svm_soft_vcpu_destroy(struct vmm_svm_interrupt_vcpu *vcpu)
{
	(void)vcpu;
}

static void
vmm_svm_soft_vcpu_enter(struct vmm_svm_interrupt_vcpu *vcpu)
{
	(void)vcpu;
}

static void
vmm_svm_soft_vcpu_leave(struct vmm_svm_interrupt_vcpu *vcpu)
{
	(void)vcpu;
}

static int
vmm_svm_soft_vcpu_exit(struct vmm_svm_interrupt_vcpu *vcpu,
    uint64_t exitcode, uint64_t exitinfo1, uint64_t exitinfo2)
{
	(void)vcpu;
	(void)exitcode;
	(void)exitinfo1;
	(void)exitinfo2;
	return 0;
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
	*value = vmm_svm_avic_read(vcpu, reg);
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
	avic->machine = machine;
	avic->vcpu = vcpu;
	avic->apic_id = apic_id;
	atomic_store_rel_int(&avic->host_cpu, -1);
	atomic_store_rel_int(&avic->host_apic_id, -1);

	error = os_contigpa_zalloc(&avic->apic_page_pa,
	    (vaddr_t *)&avic->apic_page, 1);
	if (error != 0) {
		os_mem_free(avic, sizeof(*avic));
		return error;
	}
	vmm_svm_avic_write(avic, VMM_SVM_APIC_ID, apic_id << 24);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_VERSION, VMM_SVM_APIC_VERSION_VALUE);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_TPR, 0);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_SVR,
	    VMM_SVM_APIC_SVR_ENABLE | 0xff);
	vmm_svm_avic_write(avic, VMM_SVM_APIC_DFR, VMM_SVM_APIC_DFR_FLAT);
	if (apic_id < 8)
		vmm_svm_avic_write(avic, VMM_SVM_APIC_LDR, 1U << (24 + apic_id));

	lwkt_gettoken(&machine->token);
	if (machine->targets[apic_id] != NULL) {
		lwkt_reltoken(&machine->token);
		os_contigpa_free(avic->apic_page_pa, (vaddr_t)avic->apic_page, 1);
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
vmm_svm_avic_vcpu_destroy(struct vmm_svm_interrupt_vcpu *avic)
{
	struct vmm_svm_interrupt_machine *machine;
	uint32_t index;

	if (avic == NULL)
		return;
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
	struct vmm_svm_interrupt_machine *machine;
	uint32_t cpu;
	uint32_t apic_id;
	uint64_t entry;

	if (avic == NULL)
		return;
	machine = avic->machine;
	cpu = os_curcpu_number();
	apic_id = CPUID_TO_APICID(cpu);
	KKASSERT((apic_id & ~VMM_SVM_AVIC_HOST_APIC_ID_MASK) == 0);
	atomic_store_rel_int(&avic->host_apic_id, apic_id);
	atomic_store_rel_int(&avic->host_cpu, cpu);
	lwkt_gettoken(&machine->token);
	entry = avic->apic_page_pa | VMM_SVM_AVIC_PHYS_VALID |
	    VMM_SVM_AVIC_PHYS_RUNNING | apic_id;
	machine->physical_table[avic->apic_id] = entry;
	cpu_mfence();
	lwkt_reltoken(&machine->token);
	atomic_store_rel_int(&avic->running, 1);
}

static void
vmm_svm_avic_vcpu_leave(struct vmm_svm_interrupt_vcpu *avic)
{
	struct vmm_svm_interrupt_machine *machine;
	uint64_t entry;

	if (avic == NULL || atomic_swap_int(&avic->running, 0) == 0)
		return;
	machine = avic->machine;
	lwkt_gettoken(&machine->token);
	entry = avic->apic_page_pa | VMM_SVM_AVIC_PHYS_VALID |
	    atomic_load_acq_int(&avic->host_apic_id);
	machine->physical_table[avic->apic_id] = entry;
	cpu_mfence();
	lwkt_reltoken(&machine->token);
}

static void
vmm_svm_avic_deliver(struct vmm_svm_interrupt_vcpu *avic, uint8_t vector)
{
	volatile uint32_t *irr;
	uint32_t host_cpu;
	uint32_t host_apic_id;

	if (vector < 32)
		return;
	irr = (volatile uint32_t *)((uint8_t *)avic->apic_page +
	    VMM_SVM_APIC_IRR_BASE + (vector / 32) * 0x10);
	atomic_set_int((volatile u_int *)irr, __BIT(vector & 31));
	cpu_mfence();
	if (atomic_load_acq_int(&avic->running) == 0)
		return;
	host_cpu = atomic_load_acq_int(&avic->host_cpu);
	if (host_cpu == os_curcpu_number())
		return;
	host_apic_id = atomic_load_acq_int(&avic->host_apic_id);
	wrmsr(VMM_SVM_AVIC_DOORBELL_MSR, host_apic_id);
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
	 * Only fixed-vector IPIs have an AVIC delivery path today.  Returning
	 * unhandled preserves the exit for the frontend instead of silently
	 * dropping NMI, INIT, or SIPI.
	 */
	if (delivery != VMM_SVM_APIC_ICR_FIXED)
		return 0;
	if (vector < 16)
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
		vmm_svm_avic_deliver(target, (uint8_t)vector);
	}
	lwkt_reltoken(&machine->token);
	return 1;
}

static int
vmm_svm_avic_vcpu_exit(struct vmm_svm_interrupt_vcpu *avic,
    uint64_t exitcode, uint64_t exitinfo1, uint64_t exitinfo2)
{
	uint32_t offset;
	uint32_t value;

	(void)exitinfo2;
	if (avic == NULL)
		return 0;
	if (exitcode == VMM_SVM_EXIT_AVIC_INCOMPLETE_IPI) {
		return vmm_svm_avic_route_icr(avic, (uint32_t)exitinfo1,
		    (uint32_t)(exitinfo1 >> 32), 0);
	}
	if (exitcode != VMM_SVM_EXIT_AVIC_NOACCEL ||
	    ((exitinfo1 >> 32) & VMM_SVM_AVIC_NOACCEL_WRITE) == 0)
		return 0;
	offset = exitinfo1 & VMM_SVM_AVIC_NOACCEL_OFFSET;
	value = vmm_svm_avic_read(avic, offset);
	switch (offset) {
	case VMM_SVM_APIC_TPR:
		vmm_svm_avic_write(avic, offset, value & 0xffU);
		return 1;
	case VMM_SVM_APIC_LDR:
		vmm_svm_avic_write(avic, offset, value & 0xff000000U);
		lwkt_gettoken(&avic->machine->token);
		vmm_svm_avic_logical_update_locked(avic);
		cpu_mfence();
		lwkt_reltoken(&avic->machine->token);
		return 1;
	case VMM_SVM_APIC_DFR:
		vmm_svm_avic_write(avic, offset, value);
		lwkt_gettoken(&avic->machine->token);
		vmm_svm_avic_logical_update_locked(avic);
		cpu_mfence();
		lwkt_reltoken(&avic->machine->token);
		return 1;
	case VMM_SVM_APIC_SVR:
		vmm_svm_avic_write(avic, offset, value & VMM_SVM_APIC_SVR_VALID);
		return 1;
	case VMM_SVM_APIC_ESR:
		vmm_svm_avic_write(avic, offset, 0);
		return 1;
	case VMM_SVM_APIC_LVT_ERROR:
		vmm_svm_avic_write(avic, offset,
		    value & VMM_SVM_APIC_LVT_COMMON_VALID);
		return 1;
	case VMM_SVM_APIC_LVTT:
		vmm_svm_avic_write(avic, offset,
		    value & VMM_SVM_APIC_LVT_TIMER_VALID);
		return 1;
	case VMM_SVM_APIC_TMICT:
		vmm_svm_avic_write(avic, offset, value);
		return 1;
	case VMM_SVM_APIC_TDCR:
		vmm_svm_avic_write(avic, offset,
		    value & VMM_SVM_APIC_TIMER_DIVIDE_VALID);
		return 1;
	case VMM_SVM_APIC_ICR_HIGH:
		vmm_svm_avic_write(avic, offset, value);
		return 1;
	case VMM_SVM_APIC_ICR_LOW:
		vmm_svm_avic_write(avic, offset, value);
		return vmm_svm_avic_route_icr(avic, value,
		    vmm_svm_avic_read(avic, VMM_SVM_APIC_ICR_HIGH), 0);
	case VMM_SVM_APIC_EOI:
		return 1;
	case VMM_SVM_APIC_LVT0:
	case VMM_SVM_APIC_LVT1:
		vmm_svm_avic_write(avic, offset,
		    value & VMM_SVM_APIC_LVT_LINT_VALID);
		return 1;
	case VMM_SVM_APIC_LVT_THERMAL:
	case VMM_SVM_APIC_LVT_PERF:
		vmm_svm_avic_write(avic, offset,
		    value & VMM_SVM_APIC_LVT_DELIVERY_VALID);
		return 1;
	default:
		return 0;
	}
}
