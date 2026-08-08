/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM vCPU entry and VMEXIT dispatch.
 */
#include <sys/errno.h>
#include <sys/globaldata.h>
#include <sys/systm.h>
#include <sys/thread2.h>

#include <machine/globaldata.h>

#include "../../vmm_vcpu.h"
#include "vmm_svm.h"
#include "vmm_svm_context.h"

#define VMM_SVM_CLGI() __asm volatile("clgi" ::: "memory")
#define VMM_SVM_STGI() __asm volatile("stgi" ::: "memory")

#define VMM_SVM_EXIT_INTR		0x060ULL
#define VMM_SVM_EXIT_NMI		0x061ULL
#define VMM_SVM_EXIT_VMMCALL		0x081ULL

#define VMM_SVM_EVENTINJ_VALID		(1ULL << 31)
#define VMM_SVM_EVENTINJ_TYPE_EXCEPTION	(3ULL << 8)
#define VMM_X86_EXCEPTION_UD		6U

static void vmm_svm_kick_ipiq(void *);

int
vmm_svm_vcpu_run(struct vmm_vcpu *vcpu, struct vmm_cpuexit **reason)
{
	struct vmm_svm_vcpu *svm;
	struct vmm_svm_vmcb *vmcb;
	int error;

	if (vcpu->backend == NULL)
		return EINVAL;
	svm = vcpu->backend;
	vmcb = svm->vmcb;
	*reason = NULL;

	for (;;) {
		atomic_store_rel_int(&svm->run_cpu, mycpu->gd_cpuid);
		/* VMRUN re-enables GIF for guest execution; this only protects handoff. */
		VMM_SVM_CLGI();
		if (atomic_load_acq_int(&vcpu->kick_pending) != 0) {
			atomic_store_rel_int(&svm->run_cpu, -1);
			VMM_SVM_STGI();
			return 0;
		}
		error = vmm_svm_state_load(vmcb, vcpu->state, &svm->xcr0);
		if (error != 0) {
			atomic_store_rel_int(&svm->run_cpu, -1);
			VMM_SVM_STGI();
			return error;
		}

		vmm_svm_context_enter(svm->context, svm->xcr0);
		vmm_svm_vmrun(svm->vmcb_pa, vcpu->state->gpr);
		/* VMEXIT clears GIF before this host code resumes. */
		vmm_svm_context_leave(svm->context);
		atomic_store_rel_int(&svm->run_cpu, -1);
		vmm_svm_state_store(vmcb, vcpu->state, svm->xcr0);

		switch (vmcb->ctrl.exitcode) {
		case VMM_SVM_EXIT_INTR:
		case VMM_SVM_EXIT_NMI:
			VMM_SVM_STGI();
			lwkt_user_yield();
			continue;
		case VMM_SVM_EXIT_VMMCALL:
			vmcb->ctrl.eventinj = VMM_SVM_EVENTINJ_VALID |
			    VMM_SVM_EVENTINJ_TYPE_EXCEPTION |
			    VMM_X86_EXCEPTION_UD;
			continue;
		default:
			vcpu->exit.code = vmcb->ctrl.exitcode;
			vcpu->exit.info1 = vmcb->ctrl.exitinfo1;
			vcpu->exit.info2 = vmcb->ctrl.exitinfo2;
			vcpu->exit.rip = vmcb->state.rip;
			vcpu->exit.inst_len = vmcb->ctrl.inst_len;
			bcopy(vmcb->ctrl.inst_bytes, vcpu->exit.inst_bytes,
			    sizeof(vcpu->exit.inst_bytes));
			*reason = &vcpu->exit;
			VMM_SVM_STGI();
			return 0;
		}
	}
}

void
vmm_svm_vcpu_kick(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_vcpu *svm;
	int cpu_id;

	if (vcpu->backend == NULL)
		return;
	svm = vcpu->backend;
	cpu_id = atomic_load_acq_int(&svm->run_cpu);
	if (cpu_id >= 0 && cpu_id != mycpu->gd_cpuid)
		lwkt_send_ipiq_bycpu(cpu_id, vmm_svm_kick_ipiq, vcpu);
}

static void
vmm_svm_kick_ipiq(void *arg)
{
	(void)arg;
}
