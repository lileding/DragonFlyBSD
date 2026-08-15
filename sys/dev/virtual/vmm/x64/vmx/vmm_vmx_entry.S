/*
 * Copyright (c) 2018-2021 Maxime Villard, m00nbsd.net
 * All rights reserved.
 *
 * This code is part of the VMM hypervisor.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define LOCORE
#include <machine/asmacros.h>

#define VMM_X64_GPR_RAX 0
#define VMM_X64_GPR_RCX 1
#define VMM_X64_GPR_RDX 2
#define VMM_X64_GPR_RBX 3
#define VMM_X64_GPR_RBP 5
#define VMM_X64_GPR_RSI 6
#define VMM_X64_GPR_RDI 7
#define VMM_X64_GPR_R8 8
#define VMM_X64_GPR_R9 9
#define VMM_X64_GPR_R10 10
#define VMM_X64_GPR_R11 11
#define VMM_X64_GPR_R12 12
#define VMM_X64_GPR_R13 13
#define VMM_X64_GPR_R14 14
#define VMM_X64_GPR_R15 15

	.text

/* redef */
#define VMCS_HOST_RSP				0x00006C14

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

#define HOST_SAVE_RAX		\
	pushq	%rax

#define HOST_RESTORE_RAX	\
	popq	%rax

#define HOST_SAVE_LDT		\
	sldtw	%ax		;\
	pushq	%rax

#define HOST_RESTORE_LDT	\
	popq	%rax		;\
	lldtw	%ax

/*
 * We don't save RAX (done manually), but we do restore it.
 */

#define GUEST_SAVE_GPRS(reg)				\
	movq	%rcx,(VMM_X64_GPR_RCX * 8)(reg)	;\
	movq	%rdx,(VMM_X64_GPR_RDX * 8)(reg)	;\
	movq	%rbx,(VMM_X64_GPR_RBX * 8)(reg)	;\
	movq	%rbp,(VMM_X64_GPR_RBP * 8)(reg)	;\
	movq	%rsi,(VMM_X64_GPR_RSI * 8)(reg)	;\
	movq	%rdi,(VMM_X64_GPR_RDI * 8)(reg)	;\
	movq	%r8,(VMM_X64_GPR_R8 * 8)(reg)		;\
	movq	%r9,(VMM_X64_GPR_R9 * 8)(reg)		;\
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
	movq	(VMM_X64_GPR_R8 * 8)(reg),%r8		;\
	movq	(VMM_X64_GPR_R9 * 8)(reg),%r9		;\
	movq	(VMM_X64_GPR_R10 * 8)(reg),%r10	;\
	movq	(VMM_X64_GPR_R11 * 8)(reg),%r11	;\
	movq	(VMM_X64_GPR_R12 * 8)(reg),%r12	;\
	movq	(VMM_X64_GPR_R13 * 8)(reg),%r13	;\
	movq	(VMM_X64_GPR_R14 * 8)(reg),%r14	;\
	movq	(VMM_X64_GPR_R15 * 8)(reg),%r15	;\
	movq	(VMM_X64_GPR_RAX * 8)(reg),%rax

/*
 * %rdi = VA of guest GPR state
 */
ENTRY(vmm_vmx_vmlaunch)
	/* Save the Host GPRs. */
	HOST_SAVE_GPRS

	/* Save the Host LDT. */
	HOST_SAVE_LDT

	/* Save the Host RAX. */
	movq	%rdi,%rax
	pushq	%rax

	/* Save the Host RSP. */
	movq	$VMCS_HOST_RSP,%rdi
	movq	%rsp,%rsi
	vmwrite	%rsi,%rdi

	/* Restore the Guest GPRs. */
	GUEST_RESTORE_GPRS(%rax)

	/* Run the VM. */
	vmlaunch

	/* Failure. */
	addq	$8,%rsp
	HOST_RESTORE_LDT
	HOST_RESTORE_GPRS
	movq	$-1,%rax
	retq
END(vmm_vmx_vmlaunch)

/*
 * %rdi = VA of guest GPR state
 */
ENTRY(vmm_vmx_vmresume)
	/* Save the Host GPRs. */
	HOST_SAVE_GPRS

	/* Save the Host LDT. */
	HOST_SAVE_LDT

	/* Save the Host RAX. */
	movq	%rdi,%rax
	pushq	%rax

	/* Save the Host RSP. */
	movq	$VMCS_HOST_RSP,%rdi
	movq	%rsp,%rsi
	vmwrite	%rsi,%rdi

	/* Restore the Guest GPRs. */
	GUEST_RESTORE_GPRS(%rax)

	/* Run the VM. */
	vmresume

	/* Failure. */
	addq	$8,%rsp
	HOST_RESTORE_LDT
	HOST_RESTORE_GPRS
	movq	$-1,%rax
	retq
END(vmm_vmx_vmresume)

/*
 * The CPU jumps here after a #VMEXIT.
 */
ENTRY(vmm_vmx_resume_rip)
	/* Save the Guest GPRs. RAX done manually. */
	pushq	%rax
	movq	8(%rsp),%rax
	GUEST_SAVE_GPRS(%rax)
	popq	%rbx
	movq	%rbx,(VMM_X64_GPR_RAX * 8)(%rax)
	addq	$8,%rsp

	/* Restore the Host LDT. */
	HOST_RESTORE_LDT

	/* Restore the Host GPRs. */
	HOST_RESTORE_GPRS

	xorq	%rax,%rax
	retq
END(vmm_vmx_resume_rip)

ENTRY(vmm_vmx_insn_failvalid)
	movq	$.Lvmx_validstr,%rdi
	call	panic
END(vmm_vmx_insn_failvalid)

ENTRY(vmm_vmx_insn_failinvalid)
	movq	$.Lvmx_invalidstr,%rdi
	call	panic
END(vmm_vmx_insn_failinvalid)

	.section ".rodata"

.Lvmx_validstr:
	.string	"VMX fail valid\0"
.Lvmx_invalidstr:
	.string "VMX fail invalid\0"
