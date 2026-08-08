/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM backend registration.
 *
 * This is intentionally only the backend-selection skeleton.  VMCB state,
 * VMRUN, NPT, and AVIC hardware state are added in later steps.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>

#include <machine/cpufunc.h>
#include <machine/specialreg.h>

#include "../../vmm_backend.h"
#include "../../vmm_vcpu.h"
#include "vmm_svm_backend.h"

#define VMM_SVM_MSR_VM_CR		0xc0010114U
#define VMM_SVM_MSR_VM_CR_LOCK		(1ULL << 3)
#define VMM_SVM_MSR_VM_CR_SVME_DISABLE	(1ULL << 4)

static int vmm_svm_probe(void);
static int vmm_svm_init(void);
static void vmm_svm_fini(void);
static int vmm_svm_machine_create(struct vmm_machine *);
static void vmm_svm_machine_destroy(struct vmm_machine *);
static int vmm_svm_machine_pmap_init(struct vmm_machine *, struct pmap *);
static int vmm_svm_vcpu_create(struct vmm_vcpu *);
static void vmm_svm_vcpu_destroy(struct vmm_vcpu *);
static int vmm_svm_vcpu_run(struct vmm_vcpu *, struct vmm_cpuexit **);
static void vmm_svm_vcpu_kick(struct vmm_vcpu *);

const struct vmm_backend_ops vmm_svm_backend = {
	.name = "x86/svm",
	.probe = vmm_svm_probe,
	.init = vmm_svm_init,
	.fini = vmm_svm_fini,
	.machine_create = vmm_svm_machine_create,
	.machine_destroy = vmm_svm_machine_destroy,
	.machine_pmap_init = vmm_svm_machine_pmap_init,
	.vcpu_create = vmm_svm_vcpu_create,
	.vcpu_destroy = vmm_svm_vcpu_destroy,
	.vcpu_run = vmm_svm_vcpu_run,
	.vcpu_kick = vmm_svm_vcpu_kick,
};

VMM_BACKEND_SET(vmm_svm_backend);

static int
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

static int
vmm_svm_init(void)
{
	return 0;
}

static void
vmm_svm_fini(void)
{
}

static int
vmm_svm_machine_create(struct vmm_machine *machine)
{
	(void)machine;
	return 0;
}

static void
vmm_svm_machine_destroy(struct vmm_machine *machine)
{
	(void)machine;
}

static int
vmm_svm_machine_pmap_init(struct vmm_machine *machine, struct pmap *pmap)
{
	(void)machine;
	(void)pmap;
	return 0;
}

static int
vmm_svm_vcpu_create(struct vmm_vcpu *vcpu)
{
	(void)vcpu;
	return 0;
}

static void
vmm_svm_vcpu_destroy(struct vmm_vcpu *vcpu)
{
	(void)vcpu;
}

static int
vmm_svm_vcpu_run(struct vmm_vcpu *vcpu, struct vmm_cpuexit **reason)
{
	(void)vcpu;
	*reason = NULL;
	return ENOTSUP;
}

static void
vmm_svm_vcpu_kick(struct vmm_vcpu *vcpu)
{
	(void)vcpu;
}
