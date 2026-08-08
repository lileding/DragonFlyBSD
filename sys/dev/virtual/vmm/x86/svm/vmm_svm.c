/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM core backend.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <machine/cpufunc.h>
#include <machine/specialreg.h>

#include <vm/pmap.h>
#include <vm/vm_extern.h>

#include "../../vmm_machine.h"
#include "../../vmm_vcpu.h"
#include "vmm_svm.h"

#define VMM_SVM_MSR_VM_CR		0xc0010114U
#define VMM_SVM_MSR_VM_CR_LOCK		(1ULL << 3)
#define VMM_SVM_MSR_VM_CR_SVME_DISABLE	(1ULL << 4)

#define VMM_SVM_VMCB_PAGES	1
#define VMM_SVM_IOPM_PAGES	3
#define VMM_SVM_MSRPM_PAGES	2

/* Hardware pages owned by one SVM vCPU. */
struct vmm_svm_vcpu {
	void *vmcb;
	vm_paddr_t vmcb_pa;
	void *iopm;
	vm_paddr_t iopm_pa;
	void *msrpm;
	vm_paddr_t msrpm_pa;
};

int
vmm_svm_probe(void)
{
	uint32_t desc[4];
	uint64_t vm_cr;

	do_cpuid(0x80000000, desc);
	if (desc[0] < 0x8000000a)
		return ENXIO;
	do_cpuid(0x80000001, desc);
	if ((desc[2] & CPUID_SVM) == 0)
		return ENXIO;
	vm_cr = rdmsr(VMM_SVM_MSR_VM_CR);
	if ((vm_cr & VMM_SVM_MSR_VM_CR_SVME_DISABLE) != 0 &&
	    (vm_cr & VMM_SVM_MSR_VM_CR_LOCK) != 0)
		return EOPNOTSUPP;
	return 0;
}

int
vmm_svm_init(void)
{
	return 0;
}

void
vmm_svm_fini(void)
{
}

int
vmm_svm_machine_create(struct vmm_machine *machine)
{
	struct pmap *pmap;

	pmap = vmspace_pmap(machine->vmspace);
	pmap_maybethreaded(pmap);
	pmap_npt_transform(pmap, 0);
	return 0;
}

void
vmm_svm_machine_destroy(struct vmm_machine *machine)
{
	(void)machine;
}

int
vmm_svm_vcpu_create(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_vcpu *svm;

	svm = kmalloc(sizeof(*svm), M_VMM, M_WAITOK | M_ZERO);
	if (svm == NULL)
		return ENOMEM;
	vcpu->backend = svm;

	svm->vmcb = contigmalloc(VMM_SVM_VMCB_PAGES * PAGE_SIZE, M_VMM,
	    M_WAITOK | M_ZERO, 0, ~0UL, PAGE_SIZE, 0);
	if (svm->vmcb == NULL)
		goto fail;
	svm->vmcb_pa = vtophys(svm->vmcb);

	svm->iopm = contigmalloc(VMM_SVM_IOPM_PAGES * PAGE_SIZE, M_VMM,
	    M_WAITOK | M_ZERO, 0, ~0UL, PAGE_SIZE, 0);
	if (svm->iopm == NULL)
		goto fail;
	svm->iopm_pa = vtophys(svm->iopm);

	svm->msrpm = contigmalloc(VMM_SVM_MSRPM_PAGES * PAGE_SIZE, M_VMM,
	    M_WAITOK | M_ZERO, 0, ~0UL, PAGE_SIZE, 0);
	if (svm->msrpm == NULL)
		goto fail;
	svm->msrpm_pa = vtophys(svm->msrpm);
	return 0;

fail:
	vmm_svm_vcpu_destroy(vcpu);
	return ENOMEM;
}

void
vmm_svm_vcpu_destroy(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_vcpu *svm;

	svm = vcpu->backend;
	if (svm == NULL)
		return;
	if (svm->msrpm != NULL)
		contigfree(svm->msrpm, VMM_SVM_MSRPM_PAGES * PAGE_SIZE, M_VMM);
	if (svm->iopm != NULL)
		contigfree(svm->iopm, VMM_SVM_IOPM_PAGES * PAGE_SIZE, M_VMM);
	if (svm->vmcb != NULL)
		contigfree(svm->vmcb, VMM_SVM_VMCB_PAGES * PAGE_SIZE, M_VMM);
	kfree(svm, M_VMM);
	vcpu->backend = NULL;
}

int
vmm_svm_vcpu_run(struct vmm_vcpu *vcpu, struct vmm_cpuexit **reason)
{
	(void)vcpu;
	*reason = NULL;
	return ENOTSUP;
}

void
vmm_svm_vcpu_kick(struct vmm_vcpu *vcpu)
{
	(void)vcpu;
}
