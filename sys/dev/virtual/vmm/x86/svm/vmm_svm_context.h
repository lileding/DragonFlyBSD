/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM per-vCPU host and guest context.
 */
#ifndef VMM_SVM_CONTEXT_H
#define VMM_SVM_CONTEXT_H

#include <machine/ucontext.h>
#include <machine/npx.h>

/*
 * State that VMRUN does not preserve between the host and the guest.
 * enter() and leave() must always be paired on the vCPU's fixed host CPU.
 */
struct vmm_svm_context {
	mcontext_t host_fpu;
	union savefpu guest_fpu;
	uint64_t host_xcr0;
	uint64_t host_dr0;
	uint64_t host_dr1;
	uint64_t host_dr2;
	uint64_t host_dr3;
	uint64_t host_dr6;
	uint64_t host_dr7;
	uint64_t guest_dr0;
	uint64_t guest_dr1;
	uint64_t guest_dr2;
	uint64_t guest_dr3;
	uint64_t host_fsbase;
	uint64_t host_gsbase;
	uint64_t host_kernelgsbase;
	uint64_t host_star;
	uint64_t host_lstar;
	uint64_t host_cstar;
	uint64_t host_sfmask;
	uint64_t host_sysenter_cs;
	uint64_t host_sysenter_esp;
	uint64_t host_sysenter_eip;
};

int vmm_svm_context_create(struct vmm_svm_context **);
void vmm_svm_context_destroy(struct vmm_svm_context *);
void vmm_svm_context_enter(struct vmm_svm_context *, uint64_t);
void vmm_svm_context_leave(struct vmm_svm_context *);

#endif /* VMM_SVM_CONTEXT_H */
