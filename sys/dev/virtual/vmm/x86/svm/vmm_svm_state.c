/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM VMCB architectural state.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/systm.h>

#include <machine/npx.h>
#include <machine/specialreg.h>

#include "../../vmm.h"
#include "vmm_svm.h"

#define VMM_SVM_V_TPR	0x00fULL

#define VMM_XCR0_X87		(1ULL << 0)
#define VMM_XCR0_SSE		(1ULL << 1)
#define VMM_XCR0_AVX		(1ULL << 2)
#define VMM_XCR0_MPX		((1ULL << 3) | (1ULL << 4))
#define VMM_XCR0_AVX512	((1ULL << 5) | (1ULL << 6) | (1ULL << 7))
#define VMM_XCR0_PKRU		(1ULL << 9)
#define VMM_XCR0_XTILE		((1ULL << 17) | (1ULL << 18))
#define VMM_XCR0_KNOWN		(VMM_XCR0_X87 | VMM_XCR0_SSE | \
				 VMM_XCR0_AVX | VMM_XCR0_MPX | \
				 VMM_XCR0_AVX512 | VMM_XCR0_PKRU | \
				 VMM_XCR0_XTILE)

CTASSERT(sizeof(struct vmm_svm_ctrl) == 0x400);
CTASSERT(__offsetof(struct vmm_svm_ctrl, iopm_base_pa) == 0x040);
CTASSERT(__offsetof(struct vmm_svm_ctrl, msrpm_base_pa) == 0x048);
CTASSERT(__offsetof(struct vmm_svm_ctrl, guest_asid) == 0x058);
CTASSERT(__offsetof(struct vmm_svm_ctrl, tlb_ctrl) == 0x05c);
CTASSERT(__offsetof(struct vmm_svm_ctrl, enable1) == 0x090);
CTASSERT(__offsetof(struct vmm_svm_ctrl, n_cr3) == 0x0b0);
CTASSERT(sizeof(struct vmm_svm_segment) == 0x10);
CTASSERT(__offsetof(struct vmm_svm_state, efer) == 0x0d0);
CTASSERT(__offsetof(struct vmm_svm_state, cr4) == 0x148);
CTASSERT(__offsetof(struct vmm_svm_state, cr3) == 0x150);
CTASSERT(__offsetof(struct vmm_svm_state, cr0) == 0x158);
CTASSERT(__offsetof(struct vmm_svm_state, rflags) == 0x170);
CTASSERT(__offsetof(struct vmm_svm_state, rip) == 0x178);
CTASSERT(__offsetof(struct vmm_svm_state, rsp) == 0x1d8);
CTASSERT(__offsetof(struct vmm_svm_state, rax) == 0x1f8);
CTASSERT(__offsetof(struct vmm_svm_state, cr2) == 0x240);
CTASSERT(__offsetof(struct vmm_svm_state, g_pat) == 0x268);
CTASSERT(sizeof(struct vmm_svm_state) == 0xc00);
CTASSERT(sizeof(struct vmm_svm_vmcb) == PAGE_SIZE);
CTASSERT(__offsetof(struct vmm_svm_vmcb, state) == 0x400);

int
vmm_svm_state_init(struct vmm_svm_vmcb *vmcb,
	const struct vmm_cpustate *state, uint64_t *xcr0)
{
	uint64_t value;

	value = state->cr[VMM_X64_CR_XCR0];
	if ((value & VMM_XCR0_X87) == 0)
		return EINVAL;
	if ((value & VMM_XCR0_AVX) != 0 &&
	    (value & VMM_XCR0_SSE) == 0)
		return EINVAL;
	if ((value & VMM_XCR0_MPX) != 0 &&
	    (value & VMM_XCR0_MPX) != VMM_XCR0_MPX)
		return EINVAL;
	if ((value & VMM_XCR0_AVX512) != 0 &&
	    ((value & VMM_XCR0_AVX512) != VMM_XCR0_AVX512 ||
	     (value & VMM_XCR0_AVX) == 0))
		return EINVAL;
	if ((value & VMM_XCR0_XTILE) != 0 &&
	    (value & VMM_XCR0_XTILE) != VMM_XCR0_XTILE)
		return EINVAL;
	if ((state->cr[VMM_X64_CR_CR8] & ~VMM_SVM_V_TPR) != 0)
		return EINVAL;
	if ((value & ~VMM_XCR0_KNOWN) != 0 ||
	    (value & ~npx_xcr0_mask) != 0)
		return EOPNOTSUPP;

	vmcb->state.rax = state->gpr[VMM_X64_GPR_RAX];
	vmcb->state.rsp = state->gpr[VMM_X64_GPR_RSP];
	vmcb->state.rip = state->gpr[VMM_X64_GPR_RIP];
	vmcb->state.rflags = state->gpr[VMM_X64_GPR_RFLAGS];
	if (vmcb->state.rflags == 0)
		vmcb->state.rflags = 2;
	vmcb->state.cr0 = state->cr[VMM_X64_CR_CR0] | CR0_ET | CR0_NE;
	vmcb->state.cr2 = state->cr[VMM_X64_CR_CR2];
	vmcb->state.cr3 = state->cr[VMM_X64_CR_CR3];
	vmcb->state.cr4 = state->cr[VMM_X64_CR_CR4];
	vmcb->state.dr6 = 0xffff0ff0ULL;
	vmcb->state.dr7 = 0x400ULL;
	vmcb->ctrl.v = (vmcb->ctrl.v & ~VMM_SVM_V_TPR) |
	    (state->cr[VMM_X64_CR_CR8] & VMM_SVM_V_TPR);
	*xcr0 = value;
	vmcb->state.efer = state->msr[VMM_X64_MSR_EFER] | EFER_SVME;
	vmcb->state.g_pat = state->msr[VMM_X64_MSR_PAT];
	vmcb->state.star = state->msr[VMM_X64_MSR_STAR];
	vmcb->state.lstar = state->msr[VMM_X64_MSR_LSTAR];
	vmcb->state.cstar = state->msr[VMM_X64_MSR_CSTAR];
	vmcb->state.sfmask = state->msr[VMM_X64_MSR_SFMASK];
	vmcb->state.kernelgsbase = state->msr[VMM_X64_MSR_KERNELGSBASE];
	vmcb->state.sysenter_cs = state->msr[VMM_X64_MSR_SYSENTER_CS];
	vmcb->state.sysenter_esp = state->msr[VMM_X64_MSR_SYSENTER_ESP];
	vmcb->state.sysenter_eip = state->msr[VMM_X64_MSR_SYSENTER_EIP];

	bcopy(&state->seg[VMM_X64_SEG_ES], &vmcb->state.es,
	    sizeof(vmcb->state.es));
	bcopy(&state->seg[VMM_X64_SEG_CS], &vmcb->state.cs,
	    sizeof(vmcb->state.cs));
	bcopy(&state->seg[VMM_X64_SEG_SS], &vmcb->state.ss,
	    sizeof(vmcb->state.ss));
	bcopy(&state->seg[VMM_X64_SEG_DS], &vmcb->state.ds,
	    sizeof(vmcb->state.ds));
	bcopy(&state->seg[VMM_X64_SEG_FS], &vmcb->state.fs,
	    sizeof(vmcb->state.fs));
	bcopy(&state->seg[VMM_X64_SEG_GS], &vmcb->state.gs,
	    sizeof(vmcb->state.gs));
	bcopy(&state->seg[VMM_X64_SEG_GDT], &vmcb->state.gdt,
	    sizeof(vmcb->state.gdt));
	bcopy(&state->seg[VMM_X64_SEG_IDT], &vmcb->state.idt,
	    sizeof(vmcb->state.idt));
	bcopy(&state->seg[VMM_X64_SEG_LDT], &vmcb->state.ldt,
	    sizeof(vmcb->state.ldt));
	bcopy(&state->seg[VMM_X64_SEG_TR], &vmcb->state.tr,
	    sizeof(vmcb->state.tr));
	vmcb->state.cpl = (vmcb->state.ss.attrib >> 5) & 3;
	return 0;
}
