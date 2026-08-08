/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM core backend.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>

#include <machine/cpufunc.h>
#include <machine/specialreg.h>

#include "vmm_svm.h"

#define VMM_SVM_MSR_VM_CR		0xc0010114U
#define VMM_SVM_MSR_VM_CR_LOCK		(1ULL << 3)
#define VMM_SVM_MSR_VM_CR_SVME_DISABLE	(1ULL << 4)

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
	(void)machine;
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
	(void)vcpu;
	return 0;
}

void
vmm_svm_vcpu_destroy(struct vmm_vcpu *vcpu)
{
	(void)vcpu;
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
