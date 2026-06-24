/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM VMRUN wrapper, adapted from DragonFly's NVMM SVM entry code.
 */
#include <machine/asmacros.h>
#include "assym.s"
#include <machine/segments.h>
#include <machine/specialreg.h>

	.text

#define HOST_SAVE_GPRS		\
	pushq	%rbx		;\
	pushq	%rbp		;\
	pushq	%r12		;\
	pushq	%r13		;\
	pushq	%r14		;\
	pushq	%r15

#define HOST_RESTORE_GPRS	\
	popq	%r15		;\
	popq	%r14		;\
	popq	%r13		;\
	popq	%r12		;\
	popq	%rbp		;\
	popq	%rbx

#define HOST_SAVE_MSR(msr)	\
	movq	$msr,%rcx	;\
	rdmsr			;\
	pushq	%rdx		;\
	pushq	%rax

#define HOST_RESTORE_MSR(msr)	\
	popq	%rax		;\
	popq	%rdx		;\
	movq	$msr,%rcx	;\
	wrmsr

#define HOST_SAVE_TR		\
	strw	%ax		;\
	pushq	%rax

#define HOST_RESTORE_TR				\
	popq	%rax				;\
	movzwq	%ax,%rdx			;\
	movq	PCPU(tss_gdt),%rax		;\
	andq	$~0x0200,4(%rax)		;\
	ltrw	%dx

#define HOST_SAVE_LDT		\
	sldtw	%ax		;\
	pushq	%rax

#define HOST_RESTORE_LDT	\
	popq	%rax		;\
	lldtw	%ax

#define VMM_X64_GPR_RCX	1
#define VMM_X64_GPR_RDX	2
#define VMM_X64_GPR_RBX	3
#define VMM_X64_GPR_RBP	5
#define VMM_X64_GPR_RSI	6
#define VMM_X64_GPR_RDI	7
#define VMM_X64_GPR_R8		8
#define VMM_X64_GPR_R9		9
#define VMM_X64_GPR_R10	10
#define VMM_X64_GPR_R11	11
#define VMM_X64_GPR_R12	12
#define VMM_X64_GPR_R13	13
#define VMM_X64_GPR_R14	14
#define VMM_X64_GPR_R15	15

#define GUEST_SAVE_GPRS(reg)				\
	movq	%rcx,(VMM_X64_GPR_RCX * 8)(reg)	;\
	movq	%rdx,(VMM_X64_GPR_RDX * 8)(reg)	;\
	movq	%rbx,(VMM_X64_GPR_RBX * 8)(reg)	;\
	movq	%rbp,(VMM_X64_GPR_RBP * 8)(reg)	;\
	movq	%rsi,(VMM_X64_GPR_RSI * 8)(reg)	;\
	movq	%rdi,(VMM_X64_GPR_RDI * 8)(reg)	;\
	movq	%r8,(VMM_X64_GPR_R8 * 8)(reg)	;\
	movq	%r9,(VMM_X64_GPR_R9 * 8)(reg)	;\
	movq	%r10,(VMM_X64_GPR_R10 * 8)(reg)	;\
	movq	%r11,(VMM_X64_GPR_R11 * 8)(reg)	;\
	movq	%r12,(VMM_X64_GPR_R12 * 8)(reg)	;\
	movq	%r13,(VMM_X64_GPR_R13 * 8)(reg)	;\
	movq	%r14,(VMM_X64_GPR_R14 * 8)(reg)	;\
	movq	%r15,(VMM_X64_GPR_R15 * 8)(reg)

#define GUEST_RESTORE_GPRS(reg)				\
	movq	(VMM_X64_GPR_RCX * 8)(reg),%rcx	;\
	movq	(VMM_X64_GPR_RDX * 8)(reg),%rdx	;\
	movq	(VMM_X64_GPR_RBX * 8)(reg),%rbx	;\
	movq	(VMM_X64_GPR_RBP * 8)(reg),%rbp	;\
	movq	(VMM_X64_GPR_RSI * 8)(reg),%rsi	;\
	movq	(VMM_X64_GPR_RDI * 8)(reg),%rdi	;\
	movq	(VMM_X64_GPR_R8 * 8)(reg),%r8	;\
	movq	(VMM_X64_GPR_R9 * 8)(reg),%r9	;\
	movq	(VMM_X64_GPR_R10 * 8)(reg),%r10	;\
	movq	(VMM_X64_GPR_R11 * 8)(reg),%r11	;\
	movq	(VMM_X64_GPR_R12 * 8)(reg),%r12	;\
	movq	(VMM_X64_GPR_R13 * 8)(reg),%r13	;\
	movq	(VMM_X64_GPR_R14 * 8)(reg),%r14	;\
	movq	(VMM_X64_GPR_R15 * 8)(reg),%r15

/*
 * %rdi = PA of VMCB
 * %rsi = VA of guest GPR array
 */
ENTRY(vmm_svm_vmrun)
	HOST_SAVE_GPRS
	HOST_SAVE_TR
	HOST_SAVE_MSR(MSR_GSBASE)

	movq	$GSEL(GUDATA_SEL, SEL_UPL),%rax
	movw	%ax,%ds
	movw	%ax,%es

	HOST_SAVE_LDT

	pushq	%rsi
	pushq	%rdi

	movq	%rsi,%rax
	GUEST_RESTORE_GPRS(%rax)

	popq	%rax
	vmload	%rax

	/*
	 * AMD SVM defines two separate gates here:
	 *
	 * - CLGI keeps GIF clear while host state is half-swapped.
	 * - With V_INTR_MASKING set, VMRUN samples host RFLAGS.IF and uses
	 *   that sampled value for physical INTR delivery while the guest runs.
	 *
	 * Set IF before VMRUN and leave it set after #VMEXIT.  Hardware sets
	 * GIF to 1 when guest execution starts and clears GIF again on #VMEXIT,
	 * so this does not deliver host interrupts in the half-swapped host
	 * window.  Keeping IF set lets the pending physical interrupt that
	 * caused VMEXIT run as soon as the C side restores host state and STGI
	 * opens GIF.
	 */
	pushfq
	orq	$0x200,(%rsp)
	popfq
	vmrun	%rax
	vmsave	%rax

	popq	%rax
	GUEST_SAVE_GPRS(%rax)

	HOST_RESTORE_LDT

	xorq	%rax,%rax
	movw	%ax,%fs
	movw	%ax,%gs

	HOST_RESTORE_MSR(MSR_GSBASE)
	HOST_RESTORE_TR
	HOST_RESTORE_GPRS
	retq
END(vmm_svm_vmrun)
