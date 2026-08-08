/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM per-vCPU host and guest context.
 */
#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include <machine/cpufunc.h>
#include <machine/globaldata.h>
#include <machine/md_var.h>
#include <machine/specialreg.h>

#include "../../vmm_machine.h"
#include "vmm_svm_context.h"

CTASSERT(__offsetof(struct vmm_svm_context, guest_fpu) % 64 == 0);

int
vmm_svm_context_create(struct vmm_svm_context **context)
{
	struct vmm_svm_context *svm_context;

	if (context == NULL)
		return EINVAL;
	*context = NULL;
	svm_context = kmalloc(sizeof(*svm_context), M_VMM,
	    M_WAITOK | M_ZERO | M_POWEROF2);
	if (svm_context == NULL)
		return ENOMEM;
	/* gd_zerofpu is the architectural initial XSAVE image. */
	bcopy(&mdcpu->gd_zerofpu, &svm_context->guest_fpu,
	    sizeof(svm_context->guest_fpu));
	*context = svm_context;
	return 0;
}

void
vmm_svm_context_destroy(struct vmm_svm_context *context)
{
	if (context != NULL)
		kfree(context, M_VMM);
}

void
vmm_svm_context_enter(struct vmm_svm_context *context, uint64_t guest_xcr0)
{
	context->host_xcr0 = rxcr(0);
	context->host_dr0 = rdr0();
	context->host_dr1 = rdr1();
	context->host_dr2 = rdr2();
	context->host_dr3 = rdr3();
	context->host_dr6 = rdr6();
	context->host_dr7 = rdr7();
	context->host_fsbase = rdmsr(MSR_FSBASE);
	context->host_gsbase = rdmsr(MSR_GSBASE);
	context->host_kernelgsbase = rdmsr(MSR_KGSBASE);
	context->host_star = rdmsr(MSR_STAR);
	context->host_lstar = rdmsr(MSR_LSTAR);
	context->host_cstar = rdmsr(MSR_CSTAR);
	context->host_sfmask = rdmsr(MSR_SF_MASK);
	context->host_sysenter_cs = rdmsr(MSR_SYSENTER_CS);
	context->host_sysenter_esp = rdmsr(MSR_SYSENTER_ESP);
	context->host_sysenter_eip = rdmsr(MSR_SYSENTER_EIP);

	npxpush(&context->host_fpu);
	clts();
	fpurstor(&context->guest_fpu, npx_xcr0_mask);
	load_xcr(0, guest_xcr0);
	load_dr7(0);
	load_dr0(context->guest_dr0);
	load_dr1(context->guest_dr1);
	load_dr2(context->guest_dr2);
	load_dr3(context->guest_dr3);
}

void
vmm_svm_context_leave(struct vmm_svm_context *context)
{
	context->guest_dr0 = rdr0();
	context->guest_dr1 = rdr1();
	context->guest_dr2 = rdr2();
	context->guest_dr3 = rdr3();
	load_dr0(context->host_dr0);
	load_dr1(context->host_dr1);
	load_dr2(context->host_dr2);
	load_dr3(context->host_dr3);
	load_dr6(context->host_dr6);
	load_dr7(context->host_dr7);
	wrmsr(MSR_FSBASE, context->host_fsbase);
	wrmsr(MSR_GSBASE, context->host_gsbase);
	wrmsr(MSR_KGSBASE, context->host_kernelgsbase);
	wrmsr(MSR_STAR, context->host_star);
	wrmsr(MSR_LSTAR, context->host_lstar);
	wrmsr(MSR_CSTAR, context->host_cstar);
	wrmsr(MSR_SF_MASK, context->host_sfmask);
	wrmsr(MSR_SYSENTER_CS, context->host_sysenter_cs);
	wrmsr(MSR_SYSENTER_ESP, context->host_sysenter_esp);
	wrmsr(MSR_SYSENTER_EIP, context->host_sysenter_eip);
	load_xcr(0, context->host_xcr0);
	fpusave(&context->guest_fpu, npx_xcr0_mask);
	load_cr0(rcr0() | CR0_TS);
	npxpop(&context->host_fpu);
}
