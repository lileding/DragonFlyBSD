/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM core backend.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/globaldata.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/thread2.h>

#include <machine/cpufunc.h>
#include <machine/smp.h>
#include <machine/specialreg.h>

#include <vm/pmap.h>
#include <vm/vm_extern.h>

#include "../../vmm_machine.h"
#include "../../vmm_vcpu.h"
#include "vmm_svm.h"

#define VMM_SVM_VM_CR_LOCK		(1ULL << 3)
#define VMM_SVM_VM_CR_SVME_DISABLED	(1ULL << 4)

#define VMM_SVM_VMCB_PAGES	1
#define VMM_SVM_IOPM_PAGES	3
#define VMM_SVM_MSRPM_PAGES	2

/* Module-lifetime SVM state for one host CPU. */
struct vmm_svm_cpu {
	void *hsave;
	vm_paddr_t hsave_pa;
	uint64_t saved_vm_cr;
	uint64_t saved_efer;
	uint64_t saved_hsave_pa;
	bool captured;
};

/* Hardware pages owned by one SVM vCPU. */
struct vmm_svm_vcpu {
	void *vmcb;
	vm_paddr_t vmcb_pa;
	void *iopm;
	vm_paddr_t iopm_pa;
	void *msrpm;
	vm_paddr_t msrpm_pa;
};

static struct vmm_svm_cpu vmm_svm_cpus[MAXCPU];
static bool vmm_svm_initialized;

static void vmm_svm_cpu_capture(void *);
static void vmm_svm_cpu_enable(void *);
static void vmm_svm_cpu_restore(void *);
static void vmm_svm_free_hsave(void);

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
	do_cpuid(0x8000000a, desc);
	if ((desc[0] & CPUID_AMD_SVM_REV) == 0 || desc[1] == 0 ||
	    (desc[3] & (CPUID_AMD_SVM_NP | CPUID_AMD_SVM_NRIPS)) !=
	    (CPUID_AMD_SVM_NP | CPUID_AMD_SVM_NRIPS))
		return EOPNOTSUPP;
	vm_cr = rdmsr(MSR_AMD_VM_CR);
	if ((vm_cr & VMM_SVM_VM_CR_SVME_DISABLED) != 0 &&
	    (vm_cr & VMM_SVM_VM_CR_LOCK) != 0)
		return EOPNOTSUPP;
	return 0;
}

int
vmm_svm_init(void)
{
	struct vmm_svm_cpu *cpu;
	unsigned int cpu_id;
	int error;

	if (vmm_svm_initialized)
		return EALREADY;
	if (ncpus == 0 || ncpus > MAXCPU)
		return E2BIG;
	for (cpu_id = 0; cpu_id < ncpus; ++cpu_id) {
		cpu = &vmm_svm_cpus[cpu_id];
		cpu->hsave = contigmalloc(PAGE_SIZE, M_VMM, M_WAITOK | M_ZERO,
		    0, ~0UL, PAGE_SIZE, 0);
		if (cpu->hsave == NULL) {
			vmm_svm_free_hsave();
			return ENOMEM;
		}
		cpu->hsave_pa = vtophys(cpu->hsave);
	}

	error = 0;
	lwkt_cpusync_simple(smp_active_mask, vmm_svm_cpu_capture, &error);
	if (error != 0) {
		vmm_svm_free_hsave();
		return error;
	}
	lwkt_cpusync_simple(smp_active_mask, vmm_svm_cpu_enable, NULL);
	vmm_svm_initialized = true;
	return 0;
}

void
vmm_svm_fini(void)
{
	if (!vmm_svm_initialized)
		return;
	lwkt_cpusync_simple(smp_active_mask, vmm_svm_cpu_restore, NULL);
	vmm_svm_free_hsave();
	vmm_svm_initialized = false;
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

static void
vmm_svm_cpu_capture(void *arg)
{
	struct vmm_svm_cpu *cpu;
	int *error;
	uint64_t vm_cr;

	error = arg;
	if (mycpu->gd_cpuid >= ncpus || mycpu->gd_cpuid >= MAXCPU) {
		atomic_cmpset_int(error, 0, EINVAL);
		return;
	}
	cpu = &vmm_svm_cpus[mycpu->gd_cpuid];
	if (cpu->hsave == NULL) {
		atomic_cmpset_int(error, 0, ENOMEM);
		return;
	}
	vm_cr = rdmsr(MSR_AMD_VM_CR);
	cpu->saved_vm_cr = vm_cr;
	cpu->saved_efer = rdmsr(MSR_EFER);
	cpu->saved_hsave_pa = rdmsr(MSR_AMD_VM_HSAVE_PA);
	cpu->captured = true;
	if ((vm_cr & VMM_SVM_VM_CR_SVME_DISABLED) != 0 &&
	    (vm_cr & VMM_SVM_VM_CR_LOCK) != 0) {
		atomic_cmpset_int(error, 0, EOPNOTSUPP);
		return;
	}
	if ((cpu->saved_efer & EFER_SVME) != 0 || cpu->saved_hsave_pa != 0)
		atomic_cmpset_int(error, 0, EBUSY);
}

static void
vmm_svm_cpu_enable(void *arg)
{
	struct vmm_svm_cpu *cpu;

	(void)arg;
	KKASSERT(mycpu->gd_cpuid < ncpus);
	cpu = &vmm_svm_cpus[mycpu->gd_cpuid];
	KKASSERT(cpu->captured);
	if ((cpu->saved_vm_cr & VMM_SVM_VM_CR_SVME_DISABLED) != 0)
		wrmsr(MSR_AMD_VM_CR,
		    cpu->saved_vm_cr & ~VMM_SVM_VM_CR_SVME_DISABLED);
	wrmsr(MSR_EFER, cpu->saved_efer | EFER_SVME);
	wrmsr(MSR_AMD_VM_HSAVE_PA, cpu->hsave_pa);
}

static void
vmm_svm_cpu_restore(void *arg)
{
	struct vmm_svm_cpu *cpu;

	(void)arg;
	KKASSERT(mycpu->gd_cpuid < ncpus);
	cpu = &vmm_svm_cpus[mycpu->gd_cpuid];
	if (!cpu->captured)
		return;
	wrmsr(MSR_AMD_VM_HSAVE_PA, cpu->saved_hsave_pa);
	wrmsr(MSR_EFER, cpu->saved_efer);
	wrmsr(MSR_AMD_VM_CR, cpu->saved_vm_cr);
}

static void
vmm_svm_free_hsave(void)
{
	struct vmm_svm_cpu *cpu;
	unsigned int cpu_id;

	for (cpu_id = 0; cpu_id < ncpus; ++cpu_id) {
		cpu = &vmm_svm_cpus[cpu_id];
		if (cpu->hsave != NULL)
			contigfree(cpu->hsave, PAGE_SIZE, M_VMM);
		bzero(cpu, sizeof(*cpu));
	}
}
