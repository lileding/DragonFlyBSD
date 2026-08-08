/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM Advanced Virtual Interrupt Controller.
 *
 * This object owns only AVIC hardware state.  It does not route platform
 * devices, assign PCI functions, or model an IOAPIC.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/thread.h>

#include <machine/atomic.h>
#include <machine/cpufunc.h>
#include <machine/smp.h>
#include <machine/specialreg.h>

#include <vm/pmap.h>
#include <vm/vm_page.h>

#include "../../vmm_machine.h"
#include "../svm/vmm_x64_svm_vmcb.h"
#include "vmm_x64_svm_avic.h"

#define VMM_X64_SVM_AVIC_ACCESS_GPA		0xfee00000ULL
#define VMM_X64_SVM_AVIC_DOORBELL_MSR		0xc001011bU
#define VMM_X64_SVM_AVIC_PHYSICAL_VALID	(1ULL << 63)
#define VMM_X64_SVM_AVIC_PHYSICAL_RUNNING	(1ULL << 62)
#define VMM_X64_SVM_AVIC_HOST_ID_MASK		0xfffULL
#define VMM_X64_SVM_AVIC_MAX_APIC_ID		0xfeU
#define VMM_X64_SVM_AVIC_APIC_ID_REG		0x020U
#define VMM_X64_SVM_AVIC_APIC_VERSION_REG	0x030U
#define VMM_X64_SVM_AVIC_APIC_TPR_REG		0x080U
#define VMM_X64_SVM_AVIC_APIC_LDR_REG		0x0d0U
#define VMM_X64_SVM_AVIC_APIC_DFR_REG		0x0e0U
#define VMM_X64_SVM_AVIC_APIC_SVR_REG		0x0f0U
#define VMM_X64_SVM_AVIC_APIC_IRR_BASE		0x200U
#define VMM_X64_SVM_AVIC_APIC_VERSION		0x00140014U
#define VMM_X64_SVM_AVIC_APIC_DFR_FLAT		0xffffffffU
#define VMM_X64_SVM_AVIC_APIC_SVR_ENABLE	0x100U

struct vmm_x64_svm_avic_machine {
	/* token protects targets, physical_table, and logical_table. */
	struct lwkt_token token;
	vm_page_t access_page;
	uint64_t access_page_pa;
	uint64_t *physical_table;
	uint64_t physical_table_pa;
	uint32_t *logical_table;
	uint64_t logical_table_pa;
	struct vmm_x64_svm_avic_vcpu *targets[VMM_X64_SVM_AVIC_MAX_APIC_ID + 1];
};

struct vmm_x64_svm_avic_vcpu {
	/* token in machine protects table membership; host fields are atomic. */
	struct vmm_x64_svm_avic_machine *machine;
	void *backing_page;
	uint64_t backing_page_pa;
	uint32_t apic_id;
	u_int host_apic_id;
	u_int host_cpu;
	u_int running;
	int bound;
};

static void *vmm_x64_svm_avic_alloc(uint64_t *);
static void vmm_x64_svm_avic_free(void *);
static vm_page_t vmm_x64_svm_avic_access_page_alloc(uint64_t *);
static void vmm_x64_svm_avic_access_page_free(vm_page_t);
static void vmm_x64_svm_avic_write32(struct vmm_x64_svm_avic_vcpu *,
	uint32_t, uint32_t);
static void vmm_x64_svm_avic_configure_vmcb(struct vmm_x64_svm_avic_vcpu *,
	struct vmm_x64_svm_vmcb *);

int
vmm_x64_svm_avic_probe(void)
{
	uint32_t desc[4];
	int cpu;

	do_cpuid(0x8000000a, desc);
	if ((desc[3] & CPUID_AMD_SVM_AVIC) == 0)
		return ENXIO;
	for (cpu = 0; cpu < ncpus; ++cpu) {
		if ((CPUID_TO_APICID(cpu) & ~VMM_X64_SVM_AVIC_HOST_ID_MASK) != 0)
			return EOPNOTSUPP;
	}
	return 0;
}

int
vmm_x64_svm_avic_machine_create(struct vmm_x64_svm_avic_machine **machinep)
{
	struct vmm_x64_svm_avic_machine *machine;

	if (machinep == NULL)
		return EINVAL;
	*machinep = NULL;
	machine = kmalloc(sizeof(*machine), M_VMM, M_WAITOK | M_ZERO);
	if (machine == NULL)
		return ENOMEM;
	lwkt_token_init(&machine->token, "vmmavic");
	machine->physical_table = vmm_x64_svm_avic_alloc(
		&machine->physical_table_pa);
	machine->logical_table = vmm_x64_svm_avic_alloc(
		&machine->logical_table_pa);
	machine->access_page = vmm_x64_svm_avic_access_page_alloc(
		&machine->access_page_pa);
	if (machine->physical_table == NULL || machine->logical_table == NULL ||
	    machine->access_page == NULL) {
		vmm_x64_svm_avic_machine_destroy(machine);
		return ENOMEM;
	}
	*machinep = machine;
	return 0;
}

void
vmm_x64_svm_avic_machine_destroy(struct vmm_x64_svm_avic_machine *machine)
{
	if (machine == NULL)
		return;
	/* The machine pmap has been detached before this page is released. */
	vmm_x64_svm_avic_access_page_free(machine->access_page);
	vmm_x64_svm_avic_free(machine->logical_table);
	vmm_x64_svm_avic_free(machine->physical_table);
	kfree(machine, M_VMM);
}

int
vmm_x64_svm_avic_machine_pmap_init(struct vmm_x64_svm_avic_machine *machine,
	struct pmap *pmap)
{
	if (machine == NULL || pmap == NULL)
		return EINVAL;
	pmap_enter(pmap, VMM_X64_SVM_AVIC_ACCESS_GPA, machine->access_page,
		VM_PROT_READ | VM_PROT_WRITE, 0, NULL);
	return 0;
}

int
vmm_x64_svm_avic_vcpu_create(struct vmm_x64_svm_avic_machine *machine,
	struct vmm_x64_svm_vmcb *vmcb, uint32_t apic_id,
	struct vmm_x64_svm_avic_vcpu **vcpup)
{
	struct vmm_x64_svm_avic_vcpu *vcpu;

	if (machine == NULL || vmcb == NULL || vcpup == NULL ||
	    apic_id > VMM_X64_SVM_AVIC_MAX_APIC_ID)
		return EINVAL;
	*vcpup = NULL;
	vcpu = kmalloc(sizeof(*vcpu), M_VMM, M_WAITOK | M_ZERO);
	if (vcpu == NULL)
		return ENOMEM;
	vcpu->machine = machine;
	vcpu->apic_id = apic_id;
	vcpu->backing_page = vmm_x64_svm_avic_alloc(&vcpu->backing_page_pa);
	if (vcpu->backing_page == NULL) {
		kfree(vcpu, M_VMM);
		return ENOMEM;
	}
	atomic_store_rel_int(&vcpu->host_cpu, (u_int)-1);
	atomic_store_rel_int(&vcpu->host_apic_id, (u_int)-1);
	vmm_x64_svm_avic_write32(vcpu, VMM_X64_SVM_AVIC_APIC_ID_REG,
		apic_id << 24);
	vmm_x64_svm_avic_write32(vcpu, VMM_X64_SVM_AVIC_APIC_VERSION_REG,
		VMM_X64_SVM_AVIC_APIC_VERSION);
	vmm_x64_svm_avic_write32(vcpu, VMM_X64_SVM_AVIC_APIC_TPR_REG, 0);
	vmm_x64_svm_avic_write32(vcpu, VMM_X64_SVM_AVIC_APIC_SVR_REG,
		VMM_X64_SVM_AVIC_APIC_SVR_ENABLE | 0xffU);
	vmm_x64_svm_avic_write32(vcpu, VMM_X64_SVM_AVIC_APIC_DFR_REG,
		VMM_X64_SVM_AVIC_APIC_DFR_FLAT);
	if (apic_id < 8) {
		vmm_x64_svm_avic_write32(vcpu, VMM_X64_SVM_AVIC_APIC_LDR_REG,
			1U << (24 + apic_id));
	}
	lwkt_gettoken(&machine->token);
	if (machine->targets[apic_id] != NULL) {
		lwkt_reltoken(&machine->token);
		vmm_x64_svm_avic_free(vcpu->backing_page);
		kfree(vcpu, M_VMM);
		return EEXIST;
	}
	machine->targets[apic_id] = vcpu;
	machine->physical_table[apic_id] = vcpu->backing_page_pa |
		VMM_X64_SVM_AVIC_PHYSICAL_VALID;
	cpu_mfence();
	lwkt_reltoken(&machine->token);
	vmm_x64_svm_avic_configure_vmcb(vcpu, vmcb);
	*vcpup = vcpu;
	return 0;
}

void
vmm_x64_svm_avic_vcpu_destroy(struct vmm_x64_svm_avic_vcpu *vcpu)
{
	struct vmm_x64_svm_avic_machine *machine;

	if (vcpu == NULL)
		return;
	machine = vcpu->machine;
	lwkt_gettoken(&machine->token);
	if (machine->targets[vcpu->apic_id] == vcpu) {
		machine->targets[vcpu->apic_id] = NULL;
		machine->physical_table[vcpu->apic_id] = 0;
		cpu_mfence();
	}
	lwkt_reltoken(&machine->token);
	vmm_x64_svm_avic_free(vcpu->backing_page);
	kfree(vcpu, M_VMM);
}

void
vmm_x64_svm_avic_vcpu_bind(struct vmm_x64_svm_avic_vcpu *vcpu)
{
	struct vmm_x64_svm_avic_machine *machine;
	uint64_t entry;
	u_int apic_id;
	u_int cpu;

	if (vcpu == NULL)
		return;
	machine = vcpu->machine;
	cpu = mycpu->gd_cpuid;
	apic_id = CPUID_TO_APICID(cpu);
	KKASSERT((apic_id & ~VMM_X64_SVM_AVIC_HOST_ID_MASK) == 0);
	if (!vcpu->bound || atomic_load_acq_int(&vcpu->host_cpu) != cpu) {
		atomic_store_rel_int(&vcpu->host_apic_id, apic_id);
		atomic_store_rel_int(&vcpu->host_cpu, cpu);
		vcpu->bound = 1;
	}
	entry = vcpu->backing_page_pa | VMM_X64_SVM_AVIC_PHYSICAL_VALID |
		VMM_X64_SVM_AVIC_PHYSICAL_RUNNING |
		atomic_load_acq_int(&vcpu->host_apic_id);
	lwkt_gettoken(&machine->token);
	machine->physical_table[vcpu->apic_id] = entry;
	cpu_mfence();
	lwkt_reltoken(&machine->token);
	atomic_store_rel_int(&vcpu->running, 1);
}

void
vmm_x64_svm_avic_vcpu_unbind(struct vmm_x64_svm_avic_vcpu *vcpu)
{
	struct vmm_x64_svm_avic_machine *machine;
	uint64_t entry;

	if (vcpu == NULL || atomic_load_acq_int(&vcpu->running) == 0)
		return;
	machine = vcpu->machine;
	atomic_store_rel_int(&vcpu->running, 0);
	entry = vcpu->backing_page_pa | VMM_X64_SVM_AVIC_PHYSICAL_VALID |
		atomic_load_acq_int(&vcpu->host_apic_id);
	lwkt_gettoken(&machine->token);
	machine->physical_table[vcpu->apic_id] = entry;
	cpu_mfence();
	lwkt_reltoken(&machine->token);
}

int
vmm_x64_svm_avic_deliver(struct vmm_x64_svm_avic_vcpu *vcpu,
	uint8_t vector)
{
	volatile u_int *irr;
	u_int bit;
	u_int host_apic_id;

	if (vcpu == NULL || vector < 32)
		return EINVAL;
	irr = (volatile u_int *)((uint8_t *)vcpu->backing_page +
		VMM_X64_SVM_AVIC_APIC_IRR_BASE + (vector / 32) * 0x10);
	bit = 1U << (vector & 31);
	atomic_set_int(irr, bit);
	cpu_mfence();
	if (atomic_load_acq_int(&vcpu->running) == 0 ||
	    atomic_load_acq_int(&vcpu->host_cpu) == mycpu->gd_cpuid)
		return 0;
	host_apic_id = atomic_load_acq_int(&vcpu->host_apic_id);
	wrmsr(VMM_X64_SVM_AVIC_DOORBELL_MSR, host_apic_id);
	return 0;
}

static void *
vmm_x64_svm_avic_alloc(uint64_t *pap)
{
	void *va;

	va = contigmalloc(PAGE_SIZE, M_VMM, M_WAITOK | M_ZERO, 0, ~0UL,
		PAGE_SIZE, 0);
	if (va == NULL)
		return NULL;
	*pap = vtophys(va);
	return va;
}

static void
vmm_x64_svm_avic_free(void *va)
{
	if (va != NULL)
		contigfree(va, PAGE_SIZE, M_VMM);
}

static vm_page_t
vmm_x64_svm_avic_access_page_alloc(uint64_t *pap)
{
	vm_page_t page;

	page = vm_page_alloczwq(0,
		VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_FORCE_ZERO);
	if (page == NULL)
		return NULL;
	*pap = VM_PAGE_TO_PHYS(page);
	return page;
}

static void
vmm_x64_svm_avic_access_page_free(vm_page_t page)
{
	if (page == NULL)
		return;
	vm_page_freezwq(page);
}

static void
vmm_x64_svm_avic_write32(struct vmm_x64_svm_avic_vcpu *vcpu,
	uint32_t reg, uint32_t value)
{
	volatile uint32_t *ptr;

	ptr = (volatile uint32_t *)((uint8_t *)vcpu->backing_page + reg);
	*ptr = value;
}

static void
vmm_x64_svm_avic_configure_vmcb(struct vmm_x64_svm_avic_vcpu *vcpu,
	struct vmm_x64_svm_vmcb *vmcb)
{
	struct vmm_x64_svm_avic_machine *machine;

	machine = vcpu->machine;
	vmcb->ctrl.v |= VMM_X64_SVM_CTRL_V_INTR_MASKING |
		VMM_X64_SVM_CTRL_V_AVIC_ENABLE;
	vmcb->ctrl.avic = VMM_X64_SVM_AVIC_ACCESS_GPA;
	vmcb->ctrl.avic_backing_page_pa = vcpu->backing_page_pa;
	vmcb->ctrl.avic_logical_table_pa = machine->logical_table_pa;
	vmcb->ctrl.avic_physical_table = machine->physical_table_pa |
		VMM_X64_SVM_AVIC_MAX_APIC_ID;
}
