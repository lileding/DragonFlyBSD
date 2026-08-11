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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mman.h>
#include <sys/thread2.h>

#include "../../vmm_machine.h"
#include "../../vmm_vcpu.h"
#include "vmm_svm.h"
#include "vmm_svm_avic.h"
#include "vmm_svm_os.h"

#include "../vmm_x64.h"
#include "vmm_svm_x86defs.h"

#define SVM_NCPUID_ENTRIES	64

struct vmm_svm_cpuid_filter {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
};

struct vmm_svm_xsave {
	struct vmm_cpustate_fpu fpu;
	uint64_t xstate_bv;
	uint64_t xcomp_bv;
	uint8_t reserved0[8];
	uint8_t reserved[40];
};
CTASSERT(sizeof(struct vmm_svm_xsave) == 512 + 64);
CTASSERT(VMM_X64_GPR_RAX == 0);
CTASSERT(VMM_X64_GPR_RCX == 1);
CTASSERT(VMM_X64_GPR_RDX == 2);
CTASSERT(VMM_X64_GPR_RBX == 3);
CTASSERT(VMM_X64_GPR_RSP == 4);
CTASSERT(VMM_X64_GPR_RBP == 5);
CTASSERT(VMM_X64_GPR_RSI == 6);
CTASSERT(VMM_X64_GPR_RDI == 7);
CTASSERT(VMM_X64_GPR_R8 == 8);
CTASSERT(VMM_X64_GPR_R9 == 9);
CTASSERT(VMM_X64_GPR_R10 == 10);
CTASSERT(VMM_X64_GPR_R11 == 11);
CTASSERT(VMM_X64_GPR_R12 == 12);
CTASSERT(VMM_X64_GPR_R13 == 13);
CTASSERT(VMM_X64_GPR_R14 == 14);
CTASSERT(VMM_X64_GPR_R15 == 15);

static const struct vmm_svm_cpuid_filter vmm_svm_cpuid_00000001 = {
	.eax = ~0,
	.ebx = ~0,
	.ecx =
	    CPUID_0_01_ECX_SSE3 |
	    CPUID_0_01_ECX_PCLMULQDQ |
	    CPUID_0_01_ECX_SSSE3 |
	    CPUID_0_01_ECX_CX16 |
	    CPUID_0_01_ECX_SSE41 |
	    CPUID_0_01_ECX_SSE42 |
	    CPUID_0_01_ECX_MOVBE |
	    CPUID_0_01_ECX_POPCNT |
	    CPUID_0_01_ECX_AESNI |
	    CPUID_0_01_ECX_XSAVE |
	    CPUID_0_01_ECX_OSXSAVE |
	    CPUID_0_01_ECX_RDRAND,
	.edx =
	    CPUID_0_01_EDX_FPU |
	    CPUID_0_01_EDX_VME |
	    CPUID_0_01_EDX_DE |
	    CPUID_0_01_EDX_PSE |
	    CPUID_0_01_EDX_TSC |
	    CPUID_0_01_EDX_MSR |
	    CPUID_0_01_EDX_PAE |
	    CPUID_0_01_EDX_CX8 |
	    CPUID_0_01_EDX_APIC |
	    CPUID_0_01_EDX_SEP |
	    CPUID_0_01_EDX_PGE |
	    CPUID_0_01_EDX_CMOV |
	    CPUID_0_01_EDX_PAT |
	    CPUID_0_01_EDX_PSE36 |
	    CPUID_0_01_EDX_CLFSH |
	    CPUID_0_01_EDX_MMX |
	    CPUID_0_01_EDX_FXSR |
	    CPUID_0_01_EDX_SSE |
	    CPUID_0_01_EDX_SSE2 |
	    CPUID_0_01_EDX_SS |
	    CPUID_0_01_EDX_HTT |
	    CPUID_0_01_EDX_PBE
};

static const struct vmm_svm_cpuid_filter vmm_svm_cpuid_00000007 = {
	.eax = ~0,
	.ebx =
	    CPUID_0_07_EBX_FSGSBASE |
	    CPUID_0_07_EBX_BMI1 |
	    CPUID_0_07_EBX_FDPEXONLY |
	    CPUID_0_07_EBX_SMEP |
	    CPUID_0_07_EBX_BMI2 |
	    CPUID_0_07_EBX_ERMS |
	    CPUID_0_07_EBX_FPUCSDS |
	    CPUID_0_07_EBX_RDSEED |
	    CPUID_0_07_EBX_ADX |
	    CPUID_0_07_EBX_SMAP |
	    CPUID_0_07_EBX_CLFLUSHOPT |
	    CPUID_0_07_EBX_CLWB,
	.ecx =
	    CPUID_0_07_ECX_PREFETCHWT1 |
	    CPUID_0_07_ECX_UMIP |
	    CPUID_0_07_ECX_GFNI |
	    CPUID_0_07_ECX_VAES |
	    CPUID_0_07_ECX_VPCLMULQDQ |
	    CPUID_0_07_ECX_CLDEMOTE |
	    CPUID_0_07_ECX_MOVDIRI |
	    CPUID_0_07_ECX_MOVDIR64B,
	.edx =
	    CPUID_0_07_EDX_FSREP_MOV |
	    CPUID_0_07_EDX_MD_CLEAR |
	    CPUID_0_07_EDX_SERIALIZE
};

static const struct vmm_svm_cpuid_filter vmm_svm_cpuid_80000001 = {
	.eax = ~0,
	.ebx = ~0,
	.ecx =
	    CPUID_8_01_ECX_LAHF |
	    CPUID_8_01_ECX_CMPLEGACY |
	    CPUID_8_01_ECX_ALTMOVCR8 |
	    CPUID_8_01_ECX_ABM |
	    CPUID_8_01_ECX_SSE4A |
	    CPUID_8_01_ECX_MISALIGNSSE |
	    CPUID_8_01_ECX_3DNOWPF |
	    CPUID_8_01_ECX_XOP |
	    CPUID_8_01_ECX_FMA4 |
	    CPUID_8_01_ECX_TCE |
	    CPUID_8_01_ECX_TBM |
	    CPUID_8_01_ECX_TOPOEXT,
	.edx =
	    CPUID_8_01_EDX_FPU |
	    CPUID_8_01_EDX_VME |
	    CPUID_8_01_EDX_DE |
	    CPUID_8_01_EDX_PSE |
	    CPUID_8_01_EDX_TSC |
	    CPUID_8_01_EDX_MSR |
	    CPUID_8_01_EDX_PAE |
	    CPUID_8_01_EDX_CX8 |
	    CPUID_8_01_EDX_APIC |
	    CPUID_8_01_EDX_SYSCALL |
	    CPUID_8_01_EDX_PGE |
	    CPUID_8_01_EDX_CMOV |
	    CPUID_8_01_EDX_PAT |
	    CPUID_8_01_EDX_PSE36 |
	    CPUID_8_01_EDX_XD |
	    CPUID_8_01_EDX_MMXEXT |
	    CPUID_8_01_EDX_MMX |
	    CPUID_8_01_EDX_FXSR |
	    CPUID_8_01_EDX_FFXSR |
	    CPUID_8_01_EDX_PAGE1GB |
	    CPUID_8_01_EDX_LM |
	    CPUID_8_01_EDX_3DNOWEXT |
	    CPUID_8_01_EDX_3DNOW
};

static const struct vmm_svm_cpuid_filter vmm_svm_cpuid_80000007 = {
	.edx = CPUID_8_07_EDX_TscInvariant
};

static const struct vmm_svm_cpuid_filter vmm_svm_cpuid_80000008 = {
	.eax = ~0,
	.ebx =
	    CPUID_8_08_EBX_CLZERO |
	    CPUID_8_08_EBX_RstrFpErrPtrs |
	    CPUID_8_08_EBX_WBNOINVD
};

static uint32_t
vmm_svm_xsave_size(uint64_t xcr0)
{
	uint32_t size;

	if (xcr0 & XCR0_SSE)
		size = 512;
	else
		size = 108;
	size += 64;
	return size;
}

void vmm_svm_vmrun(paddr_t, uint64_t *);

static inline void
vmm_svm_clgi(void)
{
	__asm volatile ("clgi" ::: "memory");
}

static inline void
vmm_svm_stgi(void)
{
	__asm volatile ("stgi" ::: "memory");
}

#define MSR_NB_CFG		0xC001001F	/* Northbridge Configuration */
#define		NB_CFG_INITAPICCPUIDLO	__BIT(54)

#define MSR_CMPHALT		0xC0010055	/* Interrupt Pending and CMP-Halt */
#define MSR_VM_HSAVE_PA		0xC0010117	/* Host Save Area Physical Address */
#define MSR_IC_CFG		0xC0011021	/* Instruction Cache Configuration */
#define MSR_DE_CFG		0xC0011029	/* Decode Configuration */
#define MSR_UCODE_AMD_PATCHLEVEL 0x0000008B

#define MSR_VM_CR	0xC0010114	/* Virtual Machine Control Register */
#define		VM_CR_DPD	__BIT(0)	/* Debug port disable */
#define		VM_CR_RINIT	__BIT(1)	/* Intercept init */
#define		VM_CR_DISA20	__BIT(2)	/* Disable A20 masking */
#define		VM_CR_LOCK	__BIT(3)	/* SVM Lock */
#define		VM_CR_SVMED	__BIT(4)	/* SVME Disable */

/* -------------------------------------------------------------------------- */

#define VMCB_EXITCODE_CR0_READ		0x0000
#define VMCB_EXITCODE_CR1_READ		0x0001
#define VMCB_EXITCODE_CR2_READ		0x0002
#define VMCB_EXITCODE_CR3_READ		0x0003
#define VMCB_EXITCODE_CR4_READ		0x0004
#define VMCB_EXITCODE_CR5_READ		0x0005
#define VMCB_EXITCODE_CR6_READ		0x0006
#define VMCB_EXITCODE_CR7_READ		0x0007
#define VMCB_EXITCODE_CR8_READ		0x0008
#define VMCB_EXITCODE_CR9_READ		0x0009
#define VMCB_EXITCODE_CR10_READ		0x000A
#define VMCB_EXITCODE_CR11_READ		0x000B
#define VMCB_EXITCODE_CR12_READ		0x000C
#define VMCB_EXITCODE_CR13_READ		0x000D
#define VMCB_EXITCODE_CR14_READ		0x000E
#define VMCB_EXITCODE_CR15_READ		0x000F
#define VMCB_EXITCODE_CR0_WRITE		0x0010
#define VMCB_EXITCODE_CR1_WRITE		0x0011
#define VMCB_EXITCODE_CR2_WRITE		0x0012
#define VMCB_EXITCODE_CR3_WRITE		0x0013
#define VMCB_EXITCODE_CR4_WRITE		0x0014
#define VMCB_EXITCODE_CR5_WRITE		0x0015
#define VMCB_EXITCODE_CR6_WRITE		0x0016
#define VMCB_EXITCODE_CR7_WRITE		0x0017
#define VMCB_EXITCODE_CR8_WRITE		0x0018
#define VMCB_EXITCODE_CR9_WRITE		0x0019
#define VMCB_EXITCODE_CR10_WRITE	0x001A
#define VMCB_EXITCODE_CR11_WRITE	0x001B
#define VMCB_EXITCODE_CR12_WRITE	0x001C
#define VMCB_EXITCODE_CR13_WRITE	0x001D
#define VMCB_EXITCODE_CR14_WRITE	0x001E
#define VMCB_EXITCODE_CR15_WRITE	0x001F
#define VMCB_EXITCODE_DR0_READ		0x0020
#define VMCB_EXITCODE_DR1_READ		0x0021
#define VMCB_EXITCODE_DR2_READ		0x0022
#define VMCB_EXITCODE_DR3_READ		0x0023
#define VMCB_EXITCODE_DR4_READ		0x0024
#define VMCB_EXITCODE_DR5_READ		0x0025
#define VMCB_EXITCODE_DR6_READ		0x0026
#define VMCB_EXITCODE_DR7_READ		0x0027
#define VMCB_EXITCODE_DR8_READ		0x0028
#define VMCB_EXITCODE_DR9_READ		0x0029
#define VMCB_EXITCODE_DR10_READ		0x002A
#define VMCB_EXITCODE_DR11_READ		0x002B
#define VMCB_EXITCODE_DR12_READ		0x002C
#define VMCB_EXITCODE_DR13_READ		0x002D
#define VMCB_EXITCODE_DR14_READ		0x002E
#define VMCB_EXITCODE_DR15_READ		0x002F
#define VMCB_EXITCODE_DR0_WRITE		0x0030
#define VMCB_EXITCODE_DR1_WRITE		0x0031
#define VMCB_EXITCODE_DR2_WRITE		0x0032
#define VMCB_EXITCODE_DR3_WRITE		0x0033
#define VMCB_EXITCODE_DR4_WRITE		0x0034
#define VMCB_EXITCODE_DR5_WRITE		0x0035
#define VMCB_EXITCODE_DR6_WRITE		0x0036
#define VMCB_EXITCODE_DR7_WRITE		0x0037
#define VMCB_EXITCODE_DR8_WRITE		0x0038
#define VMCB_EXITCODE_DR9_WRITE		0x0039
#define VMCB_EXITCODE_DR10_WRITE	0x003A
#define VMCB_EXITCODE_DR11_WRITE	0x003B
#define VMCB_EXITCODE_DR12_WRITE	0x003C
#define VMCB_EXITCODE_DR13_WRITE	0x003D
#define VMCB_EXITCODE_DR14_WRITE	0x003E
#define VMCB_EXITCODE_DR15_WRITE	0x003F
#define VMCB_EXITCODE_EXCP0		0x0040
#define VMCB_EXITCODE_EXCP1		0x0041
#define VMCB_EXITCODE_EXCP2		0x0042
#define VMCB_EXITCODE_EXCP3		0x0043
#define VMCB_EXITCODE_EXCP4		0x0044
#define VMCB_EXITCODE_EXCP5		0x0045
#define VMCB_EXITCODE_EXCP6		0x0046
#define VMCB_EXITCODE_EXCP7		0x0047
#define VMCB_EXITCODE_EXCP8		0x0048
#define VMCB_EXITCODE_EXCP9		0x0049
#define VMCB_EXITCODE_EXCP10		0x004A
#define VMCB_EXITCODE_EXCP11		0x004B
#define VMCB_EXITCODE_EXCP12		0x004C
#define VMCB_EXITCODE_EXCP13		0x004D
#define VMCB_EXITCODE_EXCP14		0x004E
#define VMCB_EXITCODE_EXCP15		0x004F
#define VMCB_EXITCODE_EXCP16		0x0050
#define VMCB_EXITCODE_EXCP17		0x0051
#define VMCB_EXITCODE_EXCP18		0x0052
#define VMCB_EXITCODE_EXCP19		0x0053
#define VMCB_EXITCODE_EXCP20		0x0054
#define VMCB_EXITCODE_EXCP21		0x0055
#define VMCB_EXITCODE_EXCP22		0x0056
#define VMCB_EXITCODE_EXCP23		0x0057
#define VMCB_EXITCODE_EXCP24		0x0058
#define VMCB_EXITCODE_EXCP25		0x0059
#define VMCB_EXITCODE_EXCP26		0x005A
#define VMCB_EXITCODE_EXCP27		0x005B
#define VMCB_EXITCODE_EXCP28		0x005C
#define VMCB_EXITCODE_EXCP29		0x005D
#define VMCB_EXITCODE_EXCP30		0x005E
#define VMCB_EXITCODE_EXCP31		0x005F
#define VMCB_EXITCODE_INTR		0x0060
#define VMCB_EXITCODE_NMI		0x0061
#define VMCB_EXITCODE_SMI		0x0062
#define VMCB_EXITCODE_INIT		0x0063
#define VMCB_EXITCODE_VINTR		0x0064
#define VMCB_EXITCODE_CR0_SEL_WRITE	0x0065
#define VMCB_EXITCODE_IDTR_READ		0x0066
#define VMCB_EXITCODE_GDTR_READ		0x0067
#define VMCB_EXITCODE_LDTR_READ		0x0068
#define VMCB_EXITCODE_TR_READ		0x0069
#define VMCB_EXITCODE_IDTR_WRITE	0x006A
#define VMCB_EXITCODE_GDTR_WRITE	0x006B
#define VMCB_EXITCODE_LDTR_WRITE	0x006C
#define VMCB_EXITCODE_TR_WRITE		0x006D
#define VMCB_EXITCODE_RDTSC		0x006E
#define VMCB_EXITCODE_RDPMC		0x006F
#define VMCB_EXITCODE_PUSHF		0x0070
#define VMCB_EXITCODE_POPF		0x0071
#define VMCB_EXITCODE_CPUID		0x0072
#define VMCB_EXITCODE_RSM		0x0073
#define VMCB_EXITCODE_IRET		0x0074
#define VMCB_EXITCODE_SWINT		0x0075
#define VMCB_EXITCODE_INVD		0x0076
#define VMCB_EXITCODE_PAUSE		0x0077
#define VMCB_EXITCODE_HLT		0x0078
#define VMCB_EXITCODE_INVLPG		0x0079
#define VMCB_EXITCODE_INVLPGA		0x007A
#define VMCB_EXITCODE_IOIO		0x007B
#define VMCB_EXITCODE_MSR		0x007C
#define VMCB_EXITCODE_TASK_SWITCH	0x007D
#define VMCB_EXITCODE_FERR_FREEZE	0x007E
#define VMCB_EXITCODE_SHUTDOWN		0x007F
#define VMCB_EXITCODE_VMRUN		0x0080
#define VMCB_EXITCODE_VMMCALL		0x0081
#define VMCB_EXITCODE_VMLOAD		0x0082
#define VMCB_EXITCODE_VMSAVE		0x0083
#define VMCB_EXITCODE_STGI		0x0084
#define VMCB_EXITCODE_CLGI		0x0085
#define VMCB_EXITCODE_SKINIT		0x0086
#define VMCB_EXITCODE_RDTSCP		0x0087
#define VMCB_EXITCODE_ICEBP		0x0088
#define VMCB_EXITCODE_WBINVD		0x0089
#define VMCB_EXITCODE_MONITOR		0x008A
#define VMCB_EXITCODE_MWAIT		0x008B
#define VMCB_EXITCODE_MWAIT_CONDITIONAL	0x008C
#define VMCB_EXITCODE_XSETBV		0x008D
#define VMCB_EXITCODE_RDPRU		0x008E
#define VMCB_EXITCODE_EFER_WRITE_TRAP	0x008F
#define VMCB_EXITCODE_CR0_WRITE_TRAP	0x0090
#define VMCB_EXITCODE_CR1_WRITE_TRAP	0x0091
#define VMCB_EXITCODE_CR2_WRITE_TRAP	0x0092
#define VMCB_EXITCODE_CR3_WRITE_TRAP	0x0093
#define VMCB_EXITCODE_CR4_WRITE_TRAP	0x0094
#define VMCB_EXITCODE_CR5_WRITE_TRAP	0x0095
#define VMCB_EXITCODE_CR6_WRITE_TRAP	0x0096
#define VMCB_EXITCODE_CR7_WRITE_TRAP	0x0097
#define VMCB_EXITCODE_CR8_WRITE_TRAP	0x0098
#define VMCB_EXITCODE_CR9_WRITE_TRAP	0x0099
#define VMCB_EXITCODE_CR10_WRITE_TRAP	0x009A
#define VMCB_EXITCODE_CR11_WRITE_TRAP	0x009B
#define VMCB_EXITCODE_CR12_WRITE_TRAP	0x009C
#define VMCB_EXITCODE_CR13_WRITE_TRAP	0x009D
#define VMCB_EXITCODE_CR14_WRITE_TRAP	0x009E
#define VMCB_EXITCODE_CR15_WRITE_TRAP	0x009F
#define VMCB_EXITCODE_INVLPGB		0x00A0
#define VMCB_EXITCODE_INVLPGB_ILLEGAL	0x00A1
#define VMCB_EXITCODE_INVPCID		0x00A2
#define VMCB_EXITCODE_MCOMMIT		0x00A3
#define VMCB_EXITCODE_TLBSYNC		0x00A4
#define VMCB_EXITCODE_NPF		0x0400
#define VMCB_EXITCODE_AVIC_INCOMP_IPI	0x0401
#define VMCB_EXITCODE_AVIC_NOACCEL	0x0402
#define VMCB_EXITCODE_VMGEXIT		0x0403
#define VMCB_EXITCODE_BUSY		-2ULL
#define VMCB_EXITCODE_INVALID		-1ULL

/* -------------------------------------------------------------------------- */

struct vmcb_ctrl {
	uint32_t intercept_cr;
#define VMCB_CTRL_INTERCEPT_RCR(x)	__BIT( 0 + x)
#define VMCB_CTRL_INTERCEPT_WCR(x)	__BIT(16 + x)

	uint32_t intercept_dr;
#define VMCB_CTRL_INTERCEPT_RDR(x)	__BIT( 0 + x)
#define VMCB_CTRL_INTERCEPT_WDR(x)	__BIT(16 + x)

	uint32_t intercept_vec;
#define VMCB_CTRL_INTERCEPT_VEC(x)	__BIT(x)

	uint32_t intercept_misc1;
#define VMCB_CTRL_INTERCEPT_INTR	__BIT(0)
#define VMCB_CTRL_INTERCEPT_NMI		__BIT(1)
#define VMCB_CTRL_INTERCEPT_SMI		__BIT(2)
#define VMCB_CTRL_INTERCEPT_INIT	__BIT(3)
#define VMCB_CTRL_INTERCEPT_VINTR	__BIT(4)
#define VMCB_CTRL_INTERCEPT_CR0_SEL	__BIT(5)
#define VMCB_CTRL_INTERCEPT_RIDTR	__BIT(6)
#define VMCB_CTRL_INTERCEPT_RGDTR	__BIT(7)
#define VMCB_CTRL_INTERCEPT_RLDTR	__BIT(8)
#define VMCB_CTRL_INTERCEPT_RTR		__BIT(9)
#define VMCB_CTRL_INTERCEPT_WIDTR	__BIT(10)
#define VMCB_CTRL_INTERCEPT_WGDTR	__BIT(11)
#define VMCB_CTRL_INTERCEPT_WLDTR	__BIT(12)
#define VMCB_CTRL_INTERCEPT_WTR		__BIT(13)
#define VMCB_CTRL_INTERCEPT_RDTSC	__BIT(14)
#define VMCB_CTRL_INTERCEPT_RDPMC	__BIT(15)
#define VMCB_CTRL_INTERCEPT_PUSHF	__BIT(16)
#define VMCB_CTRL_INTERCEPT_POPF	__BIT(17)
#define VMCB_CTRL_INTERCEPT_CPUID	__BIT(18)
#define VMCB_CTRL_INTERCEPT_RSM		__BIT(19)
#define VMCB_CTRL_INTERCEPT_IRET	__BIT(20)
#define VMCB_CTRL_INTERCEPT_INTN	__BIT(21)
#define VMCB_CTRL_INTERCEPT_INVD	__BIT(22)
#define VMCB_CTRL_INTERCEPT_PAUSE	__BIT(23)
#define VMCB_CTRL_INTERCEPT_HLT		__BIT(24)
#define VMCB_CTRL_INTERCEPT_INVLPG	__BIT(25)
#define VMCB_CTRL_INTERCEPT_INVLPGA	__BIT(26)
#define VMCB_CTRL_INTERCEPT_IOIO_PROT	__BIT(27)
#define VMCB_CTRL_INTERCEPT_MSR_PROT	__BIT(28)
#define VMCB_CTRL_INTERCEPT_TASKSW	__BIT(29)
#define VMCB_CTRL_INTERCEPT_FERR_FREEZE	__BIT(30)
#define VMCB_CTRL_INTERCEPT_SHUTDOWN	__BIT(31)

	uint32_t intercept_misc2;
#define VMCB_CTRL_INTERCEPT_VMRUN	__BIT(0)
#define VMCB_CTRL_INTERCEPT_VMMCALL	__BIT(1)
#define VMCB_CTRL_INTERCEPT_VMLOAD	__BIT(2)
#define VMCB_CTRL_INTERCEPT_VMSAVE	__BIT(3)
#define VMCB_CTRL_INTERCEPT_STGI	__BIT(4)
#define VMCB_CTRL_INTERCEPT_CLGI	__BIT(5)
#define VMCB_CTRL_INTERCEPT_SKINIT	__BIT(6)
#define VMCB_CTRL_INTERCEPT_RDTSCP	__BIT(7)
#define VMCB_CTRL_INTERCEPT_ICEBP	__BIT(8)
#define VMCB_CTRL_INTERCEPT_WBINVD	__BIT(9)
#define VMCB_CTRL_INTERCEPT_MONITOR	__BIT(10)
#define VMCB_CTRL_INTERCEPT_MWAIT	__BIT(11)
#define VMCB_CTRL_INTERCEPT_MWAIT_ARMED	__BIT(12)
#define VMCB_CTRL_INTERCEPT_XSETBV	__BIT(13)
#define VMCB_CTRL_INTERCEPT_RDPRU	__BIT(14)
#define VMCB_CTRL_INTERCEPT_EFER_SPEC	__BIT(15)
#define VMCB_CTRL_INTERCEPT_WCR_SPEC(x)	__BIT(16 + x)

	uint32_t intercept_misc3;
#define VMCB_CTRL_INTERCEPT_INVLPGB_ALL	__BIT(0)
#define VMCB_CTRL_INTERCEPT_INVLPGB_ILL	__BIT(1)
#define VMCB_CTRL_INTERCEPT_PCID	__BIT(2)
#define VMCB_CTRL_INTERCEPT_MCOMMIT	__BIT(3)
#define VMCB_CTRL_INTERCEPT_TLBSYNC	__BIT(4)

	uint8_t  rsvd1[36];
	uint16_t pause_filt_thresh;
	uint16_t pause_filt_cnt;
	uint64_t iopm_base_pa;
	uint64_t msrpm_base_pa;
	uint64_t tsc_offset;
	uint32_t guest_asid;

	uint32_t tlb_ctrl;
#define VMCB_CTRL_TLB_CTRL_FLUSH_ALL			0x01
#define VMCB_CTRL_TLB_CTRL_FLUSH_GUEST			0x03
#define VMCB_CTRL_TLB_CTRL_FLUSH_GUEST_NONGLOBAL	0x07

	uint64_t v;
#define VMCB_CTRL_V_TPR			__BITS(3,0)
#define VMCB_CTRL_V_IRQ			__BIT(8)
#define VMCB_CTRL_V_VGIF		__BIT(9)
#define VMCB_CTRL_V_INTR_PRIO		__BITS(19,16)
#define VMCB_CTRL_V_IGN_TPR		__BIT(20)
#define VMCB_CTRL_V_INTR_MASKING	__BIT(24)
#define VMCB_CTRL_V_GUEST_VGIF		__BIT(25)
#define VMCB_CTRL_V_AVIC_EN		__BIT(31)
#define VMCB_CTRL_V_INTR_VECTOR		__BITS(39,32)

	uint64_t intr;
#define VMCB_CTRL_INTR_SHADOW		__BIT(0)
#define VMCB_CTRL_INTR_MASK		__BIT(1)

	uint64_t exitcode;
	uint64_t exitinfo1;
	uint64_t exitinfo2;

	uint64_t exitintinfo;
#define VMCB_CTRL_EXITINTINFO_VECTOR	__BITS(7,0)
#define VMCB_CTRL_EXITINTINFO_TYPE	__BITS(10,8)
#define VMCB_CTRL_EXITINTINFO_EV	__BIT(11)
#define VMCB_CTRL_EXITINTINFO_V		__BIT(31)
#define VMCB_CTRL_EXITINTINFO_ERRORCODE	__BITS(63,32)

	uint64_t enable1;
#define VMCB_CTRL_ENABLE_NP		__BIT(0)
#define VMCB_CTRL_ENABLE_SEV		__BIT(1)
#define VMCB_CTRL_ENABLE_ES_SEV		__BIT(2)
#define VMCB_CTRL_ENABLE_GMET		__BIT(3)
#define VMCB_CTRL_ENABLE_SSS		__BIT(4)
#define VMCB_CTRL_ENABLE_VTE		__BIT(5)

	uint64_t avic;
#define VMCB_CTRL_AVIC_APIC_BAR		__BITS(51,0)

	uint64_t ghcb;

	uint64_t eventinj;
#define VMCB_CTRL_EVENTINJ_VECTOR	__BITS(7,0)
#define VMCB_CTRL_EVENTINJ_TYPE		__BITS(10,8)
#define VMCB_CTRL_EVENTINJ_EV		__BIT(11)
#define VMCB_CTRL_EVENTINJ_V		__BIT(31)
#define VMCB_CTRL_EVENTINJ_ERRORCODE	__BITS(63,32)

	uint64_t n_cr3;

	uint64_t enable2;
#define VMCB_CTRL_ENABLE_LBR		__BIT(0)
#define VMCB_CTRL_ENABLE_VVMSAVE	__BIT(1)

	uint32_t vmcb_clean;
#define VMCB_CTRL_VMCB_CLEAN_I		__BIT(0)
#define VMCB_CTRL_VMCB_CLEAN_IOPM	__BIT(1)
#define VMCB_CTRL_VMCB_CLEAN_ASID	__BIT(2)
#define VMCB_CTRL_VMCB_CLEAN_TPR	__BIT(3)
#define VMCB_CTRL_VMCB_CLEAN_NP		__BIT(4)
#define VMCB_CTRL_VMCB_CLEAN_CR		__BIT(5)
#define VMCB_CTRL_VMCB_CLEAN_DR		__BIT(6)
#define VMCB_CTRL_VMCB_CLEAN_DT		__BIT(7)
#define VMCB_CTRL_VMCB_CLEAN_SEG	__BIT(8)
#define VMCB_CTRL_VMCB_CLEAN_CR2	__BIT(9)
#define VMCB_CTRL_VMCB_CLEAN_LBR	__BIT(10)
#define VMCB_CTRL_VMCB_CLEAN_AVIC	__BIT(11)
#define VMCB_CTRL_VMCB_CLEAN_CET	__BIT(12)

	uint32_t rsvd2;
	uint64_t nrip;
	uint8_t	inst_len;
	uint8_t	inst_bytes[15];
	uint64_t avic_abpp;
	uint64_t rsvd3;
	uint64_t avic_ltp;

	uint64_t avic_phys;
#define VMCB_CTRL_AVIC_PHYS_TABLE_PTR	__BITS(51,12)
#define VMCB_CTRL_AVIC_PHYS_MAX_INDEX	__BITS(7,0)

	uint64_t rsvd4;
	uint64_t vmsa_ptr;

	uint8_t	pad[752];
} __packed;

CTASSERT(sizeof(struct vmcb_ctrl) == 1024);

struct vmcb_segment {
	uint16_t selector;
	uint16_t attrib;	/* hidden */
	uint32_t limit;		/* hidden */
	uint64_t base;		/* hidden */
} __packed;

CTASSERT(sizeof(struct vmcb_segment) == 16);

struct vmcb_state {
	struct   vmcb_segment es;
	struct   vmcb_segment cs;
	struct   vmcb_segment ss;
	struct   vmcb_segment ds;
	struct   vmcb_segment fs;
	struct   vmcb_segment gs;
	struct   vmcb_segment gdt;
	struct   vmcb_segment ldt;
	struct   vmcb_segment idt;
	struct   vmcb_segment tr;
	uint8_t	 rsvd1[43];
	uint8_t	 cpl;
	uint8_t  rsvd2[4];
	uint64_t efer;
	uint8_t	 rsvd3[112];
	uint64_t cr4;
	uint64_t cr3;
	uint64_t cr0;
	uint64_t dr7;
	uint64_t dr6;
	uint64_t rflags;
	uint64_t rip;
	uint8_t	 rsvd4[88];
	uint64_t rsp;
	uint64_t s_cet;
	uint64_t ssp;
	uint64_t isst_addr;
	uint64_t rax;
	uint64_t star;
	uint64_t lstar;
	uint64_t cstar;
	uint64_t sfmask;
	uint64_t kernelgsbase;
	uint64_t sysenter_cs;
	uint64_t sysenter_esp;
	uint64_t sysenter_eip;
	uint64_t cr2;
	uint8_t	 rsvd6[32];
	uint64_t g_pat;
	uint64_t dbgctl;
	uint64_t br_from;
	uint64_t br_to;
	uint64_t int_from;
	uint64_t int_to;
	uint8_t	 pad[2408];
} __packed;

CTASSERT(sizeof(struct vmcb_state) == 0xC00);

struct vmcb {
	struct vmcb_ctrl ctrl;
	struct vmcb_state state;
} __packed;

CTASSERT(sizeof(struct vmcb) == PAGE_SIZE);
CTASSERT(offsetof(struct vmcb, state) == 0x400);

/* -------------------------------------------------------------------------- */

static void vmm_svm_vcpu_state_provide(struct vmm_vcpu *, uint64_t);
static void vmm_svm_vcpu_setstate(struct vmm_vcpu *, uint64_t);
static int vmm_svm_avic_modrm_size(const uint8_t *, int, int);

/*
 * These host values are static, they do not change at runtime and are the same
 * on all CPUs. We save them here because they are not saved in the VMCB.
 */
static struct {
	uint64_t xcr0;
	uint64_t star;
	uint64_t lstar;
	uint64_t cstar;
	uint64_t sfmask;
} vmm_svm_global_hstate __cacheline_aligned;

struct vmm_svm_hsave {
	paddr_t pa;
};

static struct vmm_svm_hsave hsave[OS_MAXCPUS];

static uint8_t *vmm_svm_asidmap __read_mostly;
static uint32_t vmm_svm_maxasid __read_mostly;
static os_mtx_t vmm_svm_asidlock __cacheline_aligned;

static bool vmm_svm_decode_assist __read_mostly;
static uint32_t vmm_svm_ctrl_tlb_flush __read_mostly;

#define SVM_XCR0_MASK_DEFAULT	(XCR0_X87|XCR0_SSE)
static uint64_t vmm_svm_xcr0_mask __read_mostly;

#define VMCB_NPAGES	1

#define MSRBM_NPAGES	2
#define MSRBM_SIZE	(MSRBM_NPAGES * PAGE_SIZE)

#define IOBM_NPAGES	3
#define IOBM_SIZE	(IOBM_NPAGES * PAGE_SIZE)

/*
 * CR0 bits that must be handled specially:
 * - CR0_ET: hardwired to 1 in modern CPUs; must always be 1
 * - CR0_NE: proper FPU error handling; must always be 1
 * - CR0_CD, CR0_NW: cache control; must be forced to 0 for performance
 */
#define CR0_FORCE_ZERO \
	(CR0_NW | CR0_CD)
#define CR0_FORCE_ONE \
	(CR0_ET | CR0_NE)

/* Does not include EFER_LMSLE. */
#define EFER_VALID \
	(EFER_SCE|EFER_LME|EFER_LMA|EFER_NXE|EFER_SVME|EFER_FFXSR|EFER_TCE)

#define EFER_TLB_FLUSH \
	(EFER_NXE|EFER_LMA|EFER_LME)
#define CR0_TLB_FLUSH \
	(CR0_PG|CR0_WP|CR0_CD|CR0_NW)
#define CR4_TLB_FLUSH \
	(CR4_PSE|CR4_PAE|CR4_PGE|CR4_PCIDE|CR4_SMEP)

/* -------------------------------------------------------------------------- */

struct vmm_svm_cpudata {
	/* General. */
	bool shared_asid;
	bool gtlb_want_flush;
	bool htlb_want_flush;
	bool gtsc_want_update;
	uint64_t vcpu_htlb_gen;
	int hcpu_last;
	volatile int running_cpu;

	/* VMCB. */
	struct vmcb *vmcb;
	paddr_t vmcb_pa;

	/* I/O bitmap. */
	uint8_t *iobm;
	paddr_t iobm_pa;

	/* MSR bitmap. */
	uint8_t *msrbm;
	paddr_t msrbm_pa;

	/* Percpu host state, absent from VMCB. */
	struct {
		uint64_t fsbase;
		uint64_t kernelgsbase;
		uint64_t sysenter_cs;
		uint64_t sysenter_esp;
		uint64_t sysenter_eip;
		uint64_t drs[VMM_X64_DR_COUNT];
#ifdef __DragonFly__
		mcontext_t hmctx;  /* TODO: remove this like NetBSD */
#endif
	} hstate;

	/* Intr state. */
	bool int_window_exit;
	bool nmi_window_exit;
	bool evt_pending;

	/* Guest state. */
	uint64_t gxcr0;
	uint64_t gprs[VMM_X64_GPR_COUNT];
	uint64_t drs[VMM_X64_DR_COUNT];
	uint64_t gtsc_offset;
	uint64_t gtsc_match;
	struct vmm_svm_xsave gxsave __aligned(64);
	size_t cpuid_entry_count;
	struct vmm_cpuid_entry cpuid_entries[SVM_NCPUID_ENTRIES];
	struct vmm_svm_interrupt_vcpu *interrupt;
};

static void
vmm_svm_vmcb_cache_default(struct vmcb *vmcb)
{
	vmcb->ctrl.vmcb_clean =
	    VMCB_CTRL_VMCB_CLEAN_I |
	    VMCB_CTRL_VMCB_CLEAN_IOPM |
	    VMCB_CTRL_VMCB_CLEAN_ASID |
	    VMCB_CTRL_VMCB_CLEAN_TPR |
	    VMCB_CTRL_VMCB_CLEAN_NP |
	    VMCB_CTRL_VMCB_CLEAN_CR |
	    VMCB_CTRL_VMCB_CLEAN_DR |
	    VMCB_CTRL_VMCB_CLEAN_DT |
	    VMCB_CTRL_VMCB_CLEAN_SEG |
	    VMCB_CTRL_VMCB_CLEAN_CR2 |
	    VMCB_CTRL_VMCB_CLEAN_LBR |
	    VMCB_CTRL_VMCB_CLEAN_AVIC;
}

static void
vmm_svm_vmcb_cache_update(struct vmcb *vmcb, uint64_t flags)
{
	if (flags & VMM_X64_STATE_SEGS) {
		vmcb->ctrl.vmcb_clean &=
		    ~(VMCB_CTRL_VMCB_CLEAN_SEG | VMCB_CTRL_VMCB_CLEAN_DT);
	}
	if (flags & VMM_X64_STATE_CRS) {
		vmcb->ctrl.vmcb_clean &=
		    ~(VMCB_CTRL_VMCB_CLEAN_CR | VMCB_CTRL_VMCB_CLEAN_CR2 |
		      VMCB_CTRL_VMCB_CLEAN_TPR);
	}
	if (flags & VMM_X64_STATE_DRS) {
		vmcb->ctrl.vmcb_clean &= ~VMCB_CTRL_VMCB_CLEAN_DR;
	}
	if (flags & VMM_X64_STATE_MSRS) {
		/* CR for EFER, NP for PAT. */
		vmcb->ctrl.vmcb_clean &=
		    ~(VMCB_CTRL_VMCB_CLEAN_CR | VMCB_CTRL_VMCB_CLEAN_NP);
	}
}

static inline void
vmm_svm_vmcb_cache_flush(struct vmcb *vmcb, uint64_t flags)
{
	vmcb->ctrl.vmcb_clean &= ~flags;
}

static inline void
vmm_svm_vmcb_cache_flush_all(struct vmcb *vmcb)
{
	vmcb->ctrl.vmcb_clean = 0;
}

#define SVM_EVENT_TYPE_HW_INT	0
#define SVM_EVENT_TYPE_NMI	2
#define SVM_EVENT_TYPE_EXC	3
#define SVM_EVENT_TYPE_SW_INT	4

static void
vmm_svm_event_waitexit_enable(struct vmm_vcpu *vcpu, bool nmi)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;

	if (nmi) {
		vmcb->ctrl.intercept_misc1 |= VMCB_CTRL_INTERCEPT_IRET;
		cpudata->nmi_window_exit = true;
	} else {
		vmcb->ctrl.intercept_misc1 |= VMCB_CTRL_INTERCEPT_VINTR;
		vmcb->ctrl.v |= (VMCB_CTRL_V_IRQ | VMCB_CTRL_V_IGN_TPR);
		vmm_svm_vmcb_cache_flush(vmcb, VMCB_CTRL_VMCB_CLEAN_TPR);
		cpudata->int_window_exit = true;
	}

	vmm_svm_vmcb_cache_flush(vmcb, VMCB_CTRL_VMCB_CLEAN_I);
}

static void
vmm_svm_event_waitexit_disable(struct vmm_vcpu *vcpu, bool nmi)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;

	if (nmi) {
		vmcb->ctrl.intercept_misc1 &= ~VMCB_CTRL_INTERCEPT_IRET;
		cpudata->nmi_window_exit = false;
	} else {
		vmcb->ctrl.intercept_misc1 &= ~VMCB_CTRL_INTERCEPT_VINTR;
		vmcb->ctrl.v &= ~(VMCB_CTRL_V_IRQ | VMCB_CTRL_V_IGN_TPR);
		vmm_svm_vmcb_cache_flush(vmcb, VMCB_CTRL_VMCB_CLEAN_TPR);
		cpudata->int_window_exit = false;
	}

	vmm_svm_vmcb_cache_flush(vmcb, VMCB_CTRL_VMCB_CLEAN_I);
}

static inline bool
vmm_svm_excp_has_rf(uint8_t vector)
{
	switch (vector) {
	case 1:		/* #DB */
	case 4:		/* #OF */
	case 8:		/* #DF */
	case 18:	/* #MC */
		return false;
	default:
		return true;
	}
}

static inline int
vmm_svm_excp_has_error(uint8_t vector)
{
	switch (vector) {
	case 8:		/* #DF */
	case 10:	/* #TS */
	case 11:	/* #NP */
	case 12:	/* #SS */
	case 13:	/* #GP */
	case 14:	/* #PF */
	case 17:	/* #AC */
	case 21:	/* #CP */
	case 30:	/* #SX */
		return 1;
	default:
		return 0;
	}
}

static int
vmm_svm_vcpu_commit_event(struct vmm_vcpu *vcpu,
    const struct vmm_cpuevent *event)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;
	u_int evtype = event->type;
	uint8_t vector = event->vector;
	uint64_t error = event->error;
	int type = 0, err = 0;

	switch (evtype) {
	case VMM_CPUEVENT_EXCP:
		type = SVM_EVENT_TYPE_EXC;
		if (vector == 2 || vector >= 32)
			return EINVAL;
		if (vector == 3 || vector == 0)
			return EINVAL;
		if (vmm_svm_excp_has_rf(vector)) {
			vmcb->state.rflags |= PSL_RF;
		}
		err = vmm_svm_excp_has_error(vector);
		break;
	case VMM_CPUEVENT_INTR:
		type = SVM_EVENT_TYPE_HW_INT;
		if (vector == 2) {
			type = SVM_EVENT_TYPE_NMI;
			vmm_svm_event_waitexit_enable(vcpu, true);
		}
		err = 0;
		break;
	default:
		return EINVAL;
	}

	vmcb->ctrl.eventinj =
	    __SHIFTIN((uint64_t)vector, VMCB_CTRL_EVENTINJ_VECTOR) |
	    __SHIFTIN((uint64_t)type, VMCB_CTRL_EVENTINJ_TYPE) |
	    __SHIFTIN((uint64_t)err, VMCB_CTRL_EVENTINJ_EV) |
	    __SHIFTIN((uint64_t)1, VMCB_CTRL_EVENTINJ_V) |
	    __SHIFTIN((uint64_t)error, VMCB_CTRL_EVENTINJ_ERRORCODE);

	cpudata->evt_pending = true;

	return 0;
}

int
vmm_svm_vcpu_inject_interrupt(struct vmm_vcpu *vcpu, uint8_t vector)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmm_cpuevent event = {
		.type = VMM_CPUEVENT_INTR,
		.vector = vector,
	};

	if (cpudata->evt_pending)
		return EBUSY;
	return vmm_svm_vcpu_commit_event(vcpu, &event);
}

static void
vmm_svm_inject_ud(struct vmm_vcpu *vcpu)
{
	struct vmm_cpuevent event = {
		.type = VMM_CPUEVENT_EXCP,
		.vector = 6,
	};
	int ret __diagused;

	ret = vmm_svm_vcpu_commit_event(vcpu, &event);
	OS_ASSERT(ret == 0);
}

static void
vmm_svm_inject_gp(struct vmm_vcpu *vcpu)
{
	struct vmm_cpuevent event = {
		.type = VMM_CPUEVENT_EXCP,
		.vector = 13,
	};
	int ret __diagused;

	ret = vmm_svm_vcpu_commit_event(vcpu, &event);
	OS_ASSERT(ret == 0);
}

static inline void
vmm_svm_inkernel_advance(struct vmcb *vmcb)
{
	/*
	 * Maybe we should also apply single-stepping and debug exceptions.
	 * Matters for guest-ring3, because it can execute 'cpuid' under a
	 * debugger.
	 */
	vmcb->state.rip = vmcb->ctrl.nrip;
	vmcb->state.rflags &= ~PSL_RF;
	vmcb->ctrl.intr &= ~VMCB_CTRL_INTR_SHADOW;
}

#define SVM_CPUID_MAX_BASIC		0xD
#define SVM_CPUID_MAX_HYPERVISOR	0x40000000
#define SVM_CPUID_MAX_EXTENDED		0x8000001F
static uint32_t vmm_svm_cpuid_max_basic __read_mostly;
static uint32_t vmm_svm_cpuid_max_extended __read_mostly;

static void
vmm_svm_inkernel_exec_cpuid(struct vmm_svm_cpudata *cpudata, uint32_t eax, uint32_t ecx)
{
	cpuid_desc_t descs;

	x86_get_cpuid2(eax, ecx, &descs);
	cpudata->vmcb->state.rax = descs.eax;
	cpudata->gprs[VMM_X64_GPR_RBX] = descs.ebx;
	cpudata->gprs[VMM_X64_GPR_RCX] = descs.ecx;
	cpudata->gprs[VMM_X64_GPR_RDX] = descs.edx;
}

static void
vmm_svm_inkernel_handle_cpuid(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    uint32_t eax, uint32_t ecx)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	unsigned int ncpus;
	uint64_t cr4;

	if (eax < 0x40000000) {
		if (__predict_false(eax > vmm_svm_cpuid_max_basic)) {
			eax = vmm_svm_cpuid_max_basic;
			vmm_svm_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	} else if (eax < 0x80000000) {
		if (__predict_false(eax > SVM_CPUID_MAX_HYPERVISOR)) {
			eax = vmm_svm_cpuid_max_basic;
			vmm_svm_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	} else {
		if (__predict_false(eax > vmm_svm_cpuid_max_extended)) {
			eax = vmm_svm_cpuid_max_basic;
			vmm_svm_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	}

	switch (eax) {
	case 0x00000000:
		cpudata->vmcb->state.rax = vmm_svm_cpuid_max_basic;
		break;
	case 0x00000001:
		cpudata->vmcb->state.rax &= vmm_svm_cpuid_00000001.eax;

		cpudata->gprs[VMM_X64_GPR_RBX] &= ~CPUID_0_01_EBX_LOCAL_APIC_ID;
		cpudata->gprs[VMM_X64_GPR_RBX] |= __SHIFTIN(vcpu->id,
		    CPUID_0_01_EBX_LOCAL_APIC_ID);

		lwkt_gettoken(&mach->token);
		ncpus = mach->vcpu_count;
		lwkt_reltoken(&mach->token);
		cpudata->gprs[VMM_X64_GPR_RBX] &= ~CPUID_0_01_EBX_HTT_CORES;
		cpudata->gprs[VMM_X64_GPR_RBX] |= __SHIFTIN(ncpus,
		    CPUID_0_01_EBX_HTT_CORES);

		cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_svm_cpuid_00000001.ecx;
		cpudata->gprs[VMM_X64_GPR_RCX] |= CPUID_0_01_ECX_RAZ;

		cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_svm_cpuid_00000001.edx;

		/* CPUID_0_01_ECX_OSXSAVE depends on CR4. */
		cr4 = cpudata->vmcb->state.cr4;
		if (!(cr4 & CR4_OSXSAVE)) {
			cpudata->gprs[VMM_X64_GPR_RCX] &= ~CPUID_0_01_ECX_OSXSAVE;
		}
		break;
	case 0x00000002: /* Empty */
	case 0x00000003: /* Empty */
	case 0x00000004: /* Empty */
	case 0x00000005: /* Monitor/MWait */
	case 0x00000006: /* Power Management Related Features */
		cpudata->vmcb->state.rax = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000007: /* Structured Extended Features */
		switch (ecx) {
		case 0:
			cpudata->vmcb->state.rax = 0;
			cpudata->gprs[VMM_X64_GPR_RBX] &= vmm_svm_cpuid_00000007.ebx;
			cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_svm_cpuid_00000007.ecx;
			cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_svm_cpuid_00000007.edx;
			break;
		default:
			cpudata->vmcb->state.rax = 0;
			cpudata->gprs[VMM_X64_GPR_RBX] = 0;
			cpudata->gprs[VMM_X64_GPR_RCX] = 0;
			cpudata->gprs[VMM_X64_GPR_RDX] = 0;
			break;
		}
		break;
	case 0x00000008: /* Empty */
	case 0x00000009: /* Empty */
	case 0x0000000A: /* Empty */
	case 0x0000000B: /* Empty */
	case 0x0000000C: /* Empty */
		cpudata->vmcb->state.rax = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x0000000D: /* Processor Extended State Enumeration */
		if (vmm_svm_xcr0_mask == 0) {
			break;
		}
		switch (ecx) {
		case 0:
			/* Supported XCR0 bits. */
			cpudata->vmcb->state.rax = vmm_svm_xcr0_mask & 0xFFFFFFFF;
			cpudata->gprs[VMM_X64_GPR_RDX] = vmm_svm_xcr0_mask >> 32;
			/* XSAVE size for currently enabled XCR0 features. */
			cpudata->gprs[VMM_X64_GPR_RBX] =
			    vmm_svm_xsave_size(cpudata->gxcr0);
			/* XSAVE size for all supported XCR0 features. */
			cpudata->gprs[VMM_X64_GPR_RCX] =
			    vmm_svm_xsave_size(vmm_svm_xcr0_mask);
			break;
		case 1:
			cpudata->vmcb->state.rax &=
			    (CPUID_0_0D_ECX1_EAX_XSAVEOPT |
			     CPUID_0_0D_ECX1_EAX_XSAVEC |
			     CPUID_0_0D_ECX1_EAX_XGETBV);
			cpudata->gprs[VMM_X64_GPR_RBX] =
			    vmm_svm_xsave_size(cpudata->gxcr0);
			cpudata->gprs[VMM_X64_GPR_RCX] = 0;
			cpudata->gprs[VMM_X64_GPR_RDX] = 0;
			break;
		default:
			cpudata->vmcb->state.rax = 0;
			cpudata->gprs[VMM_X64_GPR_RBX] = 0;
			cpudata->gprs[VMM_X64_GPR_RCX] = 0;
			cpudata->gprs[VMM_X64_GPR_RDX] = 0;
			break;
		}
		break;

	case 0x40000000: /* Hypervisor Information */
		cpudata->vmcb->state.rax = SVM_CPUID_MAX_HYPERVISOR;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		memcpy(&cpudata->gprs[VMM_X64_GPR_RBX], "___ ", 4);
		memcpy(&cpudata->gprs[VMM_X64_GPR_RCX], "VMM ", 4);
		memcpy(&cpudata->gprs[VMM_X64_GPR_RDX], " ___", 4);
		break;

	case 0x80000000:
		cpudata->vmcb->state.rax = vmm_svm_cpuid_max_extended;
		break;
	case 0x80000001:
		cpudata->vmcb->state.rax &= vmm_svm_cpuid_80000001.eax;
		cpudata->gprs[VMM_X64_GPR_RBX] &= vmm_svm_cpuid_80000001.ebx;
		cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_svm_cpuid_80000001.ecx;
		cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_svm_cpuid_80000001.edx;
		break;
	case 0x80000002: /* Extended Processor Name String */
	case 0x80000003: /* Extended Processor Name String */
	case 0x80000004: /* Extended Processor Name String */
	case 0x80000005: /* L1 Cache and TLB Information */
	case 0x80000006: /* L2 Cache and TLB and L3 Cache Information */
		break;
	case 0x80000007: /* Processor Power Management and RAS Capabilities */
		cpudata->vmcb->state.rax &= vmm_svm_cpuid_80000007.eax;
		cpudata->gprs[VMM_X64_GPR_RBX] &= vmm_svm_cpuid_80000007.ebx;
		cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_svm_cpuid_80000007.ecx;
		cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_svm_cpuid_80000007.edx;
		break;
	case 0x80000008: /* Processor Capacity Parameters and Ext Feat Ident */
		lwkt_gettoken(&mach->token);
		ncpus = mach->vcpu_count;
		lwkt_reltoken(&mach->token);
		cpudata->vmcb->state.rax &= vmm_svm_cpuid_80000008.eax;
		cpudata->gprs[VMM_X64_GPR_RBX] &= vmm_svm_cpuid_80000008.ebx;
		cpudata->gprs[VMM_X64_GPR_RCX] =
		    __SHIFTIN(ncpus - 1, CPUID_8_08_ECX_NC) |
		    __SHIFTIN(ncpus > 1 ? fls(ncpus - 1) : 0,
		    CPUID_8_08_ECX_ApicIdSize);
		cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_svm_cpuid_80000008.edx;
		break;
	case 0x80000009: /* Empty */
	case 0x8000000A: /* SVM Features */
	case 0x8000000B: /* Empty */
	case 0x8000000C: /* Empty */
	case 0x8000000D: /* Empty */
	case 0x8000000E: /* Empty */
	case 0x8000000F: /* Empty */
	case 0x80000010: /* Empty */
	case 0x80000011: /* Empty */
	case 0x80000012: /* Empty */
	case 0x80000013: /* Empty */
	case 0x80000014: /* Empty */
	case 0x80000015: /* Empty */
	case 0x80000016: /* Empty */
	case 0x80000017: /* Empty */
	case 0x80000018: /* Empty */
		cpudata->vmcb->state.rax = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x80000019: /* TLB Characteristics for 1GB pages */
	case 0x8000001A: /* Instruction Optimizations */
		break;
	case 0x8000001B: /* Instruction-Based Sampling Capabilities */
	case 0x8000001C: /* Lightweight Profiling Capabilities */
		cpudata->vmcb->state.rax = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x8000001D: /* Cache Topology Information */
		break;
	case 0x8000001E: /* Processor Topology Information */
		cpudata->vmcb->state.rax = vcpu->id;
		cpudata->gprs[VMM_X64_GPR_RBX] = vcpu->id;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x8000001F: /* Encrypted Memory Capabilities */
		cpudata->vmcb->state.rax = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;

	default:
		break;
	}
}

static void
vmm_svm_supported_cpuid_entry(struct vmm_cpuid_entry *entry)
{
	cpuid_desc_t descs;

	x86_get_cpuid2(entry->leaf, entry->subleaf, &descs);
	entry->eax = descs.eax;
	entry->ebx = descs.ebx;
	entry->ecx = descs.ecx;
	entry->edx = descs.edx;

	switch (entry->leaf) {
	case 0x00000000:
		entry->eax = vmm_svm_cpuid_max_basic;
		break;
	case 0x00000001:
		entry->eax &= vmm_svm_cpuid_00000001.eax;
		entry->ebx &= ~(CPUID_0_01_EBX_LOCAL_APIC_ID |
		    CPUID_0_01_EBX_HTT_CORES);
		entry->ecx &= vmm_svm_cpuid_00000001.ecx;
		entry->ecx |= CPUID_0_01_ECX_RAZ;
		entry->edx &= vmm_svm_cpuid_00000001.edx;
		break;
	case 0x00000002:
	case 0x00000003:
	case 0x00000004:
	case 0x00000005:
	case 0x00000006:
	case 0x00000008:
	case 0x00000009:
	case 0x0000000A:
	case 0x0000000B:
	case 0x0000000C:
		entry->eax = 0;
		entry->ebx = 0;
		entry->ecx = 0;
		entry->edx = 0;
		break;
	case 0x00000007:
		entry->eax = 0;
		entry->ebx &= vmm_svm_cpuid_00000007.ebx;
		entry->ecx &= vmm_svm_cpuid_00000007.ecx;
		entry->edx &= vmm_svm_cpuid_00000007.edx;
		break;
	case 0x0000000D:
		if (vmm_svm_xcr0_mask == 0)
			break;
		if (entry->subleaf == 0) {
			entry->eax = vmm_svm_xcr0_mask & 0xFFFFFFFF;
			entry->edx = vmm_svm_xcr0_mask >> 32;
			entry->ebx = vmm_svm_xsave_size(vmm_svm_xcr0_mask);
			entry->ecx = vmm_svm_xsave_size(vmm_svm_xcr0_mask);
		} else {
			entry->eax &= CPUID_0_0D_ECX1_EAX_XSAVEOPT |
			    CPUID_0_0D_ECX1_EAX_XSAVEC |
			    CPUID_0_0D_ECX1_EAX_XGETBV;
			entry->ebx = vmm_svm_xsave_size(vmm_svm_xcr0_mask);
			entry->ecx = 0;
			entry->edx = 0;
		}
		break;
	case 0x40000000:
		entry->eax = SVM_CPUID_MAX_HYPERVISOR;
		entry->ebx = 0;
		entry->ecx = 0;
		entry->edx = 0;
		memcpy(&entry->ebx, "___ ", 4);
		memcpy(&entry->ecx, "VMM ", 4);
		memcpy(&entry->edx, " ___", 4);
		break;
	case 0x80000000:
		entry->eax = vmm_svm_cpuid_max_extended;
		break;
	case 0x80000001:
		entry->eax &= vmm_svm_cpuid_80000001.eax;
		entry->ebx &= vmm_svm_cpuid_80000001.ebx;
		entry->ecx &= vmm_svm_cpuid_80000001.ecx;
		entry->edx &= vmm_svm_cpuid_80000001.edx;
		break;
	case 0x80000007:
		entry->eax &= vmm_svm_cpuid_80000007.eax;
		entry->ebx &= vmm_svm_cpuid_80000007.ebx;
		entry->ecx &= vmm_svm_cpuid_80000007.ecx;
		entry->edx &= vmm_svm_cpuid_80000007.edx;
		break;
	case 0x80000008:
		entry->eax &= vmm_svm_cpuid_80000008.eax;
		entry->ebx &= vmm_svm_cpuid_80000008.ebx;
		entry->ecx = 0;
		entry->edx &= vmm_svm_cpuid_80000008.edx;
		break;
	case 0x8000001E:
		entry->eax = 0;
		entry->ebx = 0;
		entry->ecx = 0;
		entry->edx = 0;
		break;
	case 0x80000009:
	case 0x8000000A:
	case 0x8000000B:
	case 0x8000000C:
	case 0x8000000D:
	case 0x8000000E:
	case 0x8000000F:
	case 0x80000010:
	case 0x80000011:
	case 0x80000012:
	case 0x80000013:
	case 0x80000014:
	case 0x80000015:
	case 0x80000016:
	case 0x80000017:
	case 0x80000018:
	case 0x8000001B:
	case 0x8000001C:
	case 0x8000001F:
		entry->eax = 0;
		entry->ebx = 0;
		entry->ecx = 0;
		entry->edx = 0;
		break;
	default:
		break;
	}
}

static size_t
vmm_svm_supported_cpuid_count(void)
{
	return (size_t)vmm_svm_cpuid_max_basic + 1 +
	    (vmm_svm_cpuid_max_basic >= 0x0000000D ? 1 : 0) + 1 +
	    (size_t)(vmm_svm_cpuid_max_extended - 0x80000000) + 1;
}

static void
vmm_svm_exit_insn(struct vmcb *vmcb, struct vmm_cpuexit *exit, uint64_t reason)
{
	exit->u.insn.npc = vmcb->ctrl.nrip;
	exit->reason = reason;
}

static void
vmm_svm_exit_cpuid(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	const struct vmm_cpuid_entry *entry;
	const struct vmm_cpuid_entry *leaf_entry;
	uint32_t eax, ecx;
	size_t i;

	eax = (cpudata->vmcb->state.rax & 0xFFFFFFFF);
	ecx = (cpudata->gprs[VMM_X64_GPR_RCX] & 0xFFFFFFFF);
	vmm_svm_inkernel_exec_cpuid(cpudata, eax, ecx);
	vmm_svm_inkernel_handle_cpuid(mach, vcpu, eax, ecx);
	entry = NULL;
	leaf_entry = NULL;
	for (i = 0; i < cpudata->cpuid_entry_count; ++i) {
		const struct vmm_cpuid_entry *candidate =
		    &cpudata->cpuid_entries[i];

		if (candidate->leaf != eax)
			continue;
		if ((candidate->flags &
		    VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF) == 0) {
			leaf_entry = candidate;
			continue;
		}
		if (candidate->subleaf == ecx) {
			entry = candidate;
			break;
		}
	}
	if (entry == NULL)
		entry = leaf_entry;
	if (entry != NULL) {
		cpudata->vmcb->state.rax = entry->eax;
		cpudata->gprs[VMM_X64_GPR_RBX] = entry->ebx;
		cpudata->gprs[VMM_X64_GPR_RCX] = entry->ecx;
		cpudata->gprs[VMM_X64_GPR_RDX] = entry->edx;
	}

	/* These fields are properties of the live vCPU, not its CPUID template. */
	switch (eax) {
	case 0x00000001:
		cpudata->gprs[VMM_X64_GPR_RBX] &=
		    ~(CPUID_0_01_EBX_LOCAL_APIC_ID | CPUID_0_01_EBX_HTT_CORES);
		cpudata->gprs[VMM_X64_GPR_RBX] |= __SHIFTIN(vcpu->id,
		    CPUID_0_01_EBX_LOCAL_APIC_ID);
		lwkt_gettoken(&mach->token);
		cpudata->gprs[VMM_X64_GPR_RBX] |= __SHIFTIN(mach->vcpu_count,
		    CPUID_0_01_EBX_HTT_CORES);
		lwkt_reltoken(&mach->token);
		if (!(cpudata->vmcb->state.cr4 & CR4_OSXSAVE))
			cpudata->gprs[VMM_X64_GPR_RCX] &= ~CPUID_0_01_ECX_OSXSAVE;
		break;
	case 0x0000000D:
		if (vmm_svm_xcr0_mask != 0 && ecx <= 1)
			cpudata->gprs[VMM_X64_GPR_RBX] =
			    vmm_svm_xsave_size(cpudata->gxcr0);
		break;
	case 0x80000008:
		lwkt_gettoken(&mach->token);
		cpudata->gprs[VMM_X64_GPR_RCX] &=
		    ~(CPUID_8_08_ECX_NC | CPUID_8_08_ECX_ApicIdSize);
		cpudata->gprs[VMM_X64_GPR_RCX] |=
		    __SHIFTIN(mach->vcpu_count - 1, CPUID_8_08_ECX_NC) |
		    __SHIFTIN(mach->vcpu_count > 1 ? fls(mach->vcpu_count - 1) : 0,
		    CPUID_8_08_ECX_ApicIdSize);
		lwkt_reltoken(&mach->token);
		break;
	case 0x8000001E:
		/* One vCPU is one compute unit in the single-node topology. */
		cpudata->vmcb->state.rax = vcpu->id;
		cpudata->gprs[VMM_X64_GPR_RBX] = vcpu->id;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	}

	vmm_svm_inkernel_advance(cpudata->vmcb);
	exit->reason = VMM_CPUEXIT_NONE;
}

static void
vmm_svm_exit_hlt(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;

	if (cpudata->int_window_exit && (vmcb->state.rflags & PSL_I)) {
		vmm_svm_event_waitexit_disable(vcpu, false);
	}

	vmm_svm_inkernel_advance(cpudata->vmcb);
	exit->reason = VMM_CPUEXIT_HALTED;
}

#define SVM_EXIT_CR_GPR		__BITS(3,0)	/* GPR number */
#define SVM_EXIT_CR_MOV		__BIT(63)	/* instruction was MOV */

static void
vmm_svm_exit_cr0(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;
	uint64_t info = cpudata->vmcb->ctrl.exitinfo1;
	uint64_t gpr, cr0, oldcr0, efer;

	if (__predict_false(!(info & SVM_EXIT_CR_MOV))) {
		/*
		 * Instruction wasn't MOV; must be LMSW since we're
		 * intercepting a selective CR0 write (changing any bits
		 * other than CR0.TS or CR0.MP).
		 *
		 * XXX: Should delegate to userland emulation.
		 */
		goto handled;
	}

	gpr = __SHIFTOUT(info, SVM_EXIT_CR_GPR);
	if (gpr == VMM_X64_GPR_RAX) {
		cr0 = vmcb->state.rax;
	} else if (gpr == VMM_X64_GPR_RSP) {
		cr0 = vmcb->state.rsp;
	} else {
		cr0 = cpudata->gprs[gpr];
	}
	cr0 = (cr0 & ~CR0_FORCE_ZERO) | CR0_FORCE_ONE;

	if (cr0 & CR0_PG) {
		efer = vmcb->state.efer;
		if (efer & EFER_LME) {
			efer |= EFER_LMA;
		} else {
			efer &= ~EFER_LMA;
		}
		if (efer != vmcb->state.efer) {
			vmcb->state.efer = efer;
			vmm_svm_vmcb_cache_flush(vmcb, VMCB_CTRL_VMCB_CLEAN_CR);
		}
	}

	oldcr0 = vmcb->state.cr0;
	if ((cr0 ^ oldcr0) & CR0_TLB_FLUSH) {
		cpudata->gtlb_want_flush = true;
	}
	if (cr0 != oldcr0) {
		vmcb->state.cr0 = cr0;
		vmm_svm_vmcb_cache_flush(vmcb, VMCB_CTRL_VMCB_CLEAN_CR);
	}

handled:
	exit->reason = VMM_CPUEXIT_NONE;
	vmm_svm_inkernel_advance(cpudata->vmcb);
}

#define SVM_EXIT_IO_PORT	__BITS(31,16)
#define SVM_EXIT_IO_SEG		__BITS(12,10)
#define SVM_EXIT_IO_A64		__BIT(9)
#define SVM_EXIT_IO_A32		__BIT(8)
#define SVM_EXIT_IO_A16		__BIT(7)
#define SVM_EXIT_IO_SZ32	__BIT(6)
#define SVM_EXIT_IO_SZ16	__BIT(5)
#define SVM_EXIT_IO_SZ8		__BIT(4)
#define SVM_EXIT_IO_REP		__BIT(3)
#define SVM_EXIT_IO_STR		__BIT(2)
#define SVM_EXIT_IO_IN		__BIT(0)

static void
vmm_svm_exit_io(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	uint64_t info = cpudata->vmcb->ctrl.exitinfo1;
	uint64_t nextpc = cpudata->vmcb->ctrl.exitinfo2;

	exit->reason = VMM_CPUEXIT_IO;

	exit->u.io.in = (info & SVM_EXIT_IO_IN) != 0;
	exit->u.io.port = __SHIFTOUT(info, SVM_EXIT_IO_PORT);

	if (__predict_true(vmm_svm_decode_assist)) {
		OS_ASSERT(__SHIFTOUT(info, SVM_EXIT_IO_SEG) < 6);
		exit->u.io.seg = __SHIFTOUT(info, SVM_EXIT_IO_SEG);
	} else {
		exit->u.io.seg = -1;
	}

	if (info & SVM_EXIT_IO_A64) {
		exit->u.io.address_size = 8;
	} else if (info & SVM_EXIT_IO_A32) {
		exit->u.io.address_size = 4;
	} else if (info & SVM_EXIT_IO_A16) {
		exit->u.io.address_size = 2;
	}

	if (info & SVM_EXIT_IO_SZ32) {
		exit->u.io.operand_size = 4;
	} else if (info & SVM_EXIT_IO_SZ16) {
		exit->u.io.operand_size = 2;
	} else if (info & SVM_EXIT_IO_SZ8) {
		exit->u.io.operand_size = 1;
	}

	exit->u.io.rep = (info & SVM_EXIT_IO_REP) != 0;
	exit->u.io.str = (info & SVM_EXIT_IO_STR) != 0;
	exit->u.io.npc = nextpc;

	vmm_svm_vcpu_state_provide(vcpu,
	    VMM_X64_STATE_GPRS | VMM_X64_STATE_SEGS |
	    VMM_X64_STATE_CRS | VMM_X64_STATE_MSRS);
}

static const uint64_t msr_ignore_list[] = {
	MSR_CMPHALT,
	MSR_DE_CFG,
	MSR_IC_CFG,
	MSR_UCODE_AMD_PATCHLEVEL
};

static bool
vmm_svm_inkernel_handle_msr(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;
	uint64_t val;
	size_t i;

	if (exit->reason == VMM_CPUEXIT_RDMSR) {
		if (exit->u.rdmsr.msr == MSR_EFER) {
			val = vmcb->state.efer & ~EFER_SVME;
			vmcb->state.rax = (val & 0xFFFFFFFF);
			cpudata->gprs[VMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
		if (exit->u.rdmsr.msr == MSR_NB_CFG) {
			val = NB_CFG_INITAPICCPUIDLO;
			vmcb->state.rax = (val & 0xFFFFFFFF);
			cpudata->gprs[VMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
		for (i = 0; i < __arraycount(msr_ignore_list); i++) {
			if (msr_ignore_list[i] != exit->u.rdmsr.msr)
				continue;
			val = 0;
			vmcb->state.rax = (val & 0xFFFFFFFF);
			cpudata->gprs[VMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
	} else {
		if (exit->u.wrmsr.msr == MSR_EFER) {
			if (__predict_false(exit->u.wrmsr.val & ~EFER_VALID)) {
				goto error;
			}
			if ((vmcb->state.efer ^ exit->u.wrmsr.val) &
			     EFER_TLB_FLUSH) {
				cpudata->gtlb_want_flush = true;
			}
			vmcb->state.efer = exit->u.wrmsr.val | EFER_SVME;
			vmm_svm_vmcb_cache_flush(vmcb, VMCB_CTRL_VMCB_CLEAN_CR);
			goto handled;
		}
		if (exit->u.wrmsr.msr == MSR_TSC) {
			cpudata->gtsc_offset = exit->u.wrmsr.val - rdtsc();
			cpudata->gtsc_want_update = true;
			goto handled;
		}
		for (i = 0; i < __arraycount(msr_ignore_list); i++) {
			if (msr_ignore_list[i] != exit->u.wrmsr.msr)
				continue;
			goto handled;
		}
	}

	return false;

handled:
	vmm_svm_inkernel_advance(cpudata->vmcb);
	return true;

error:
	vmm_svm_inject_gp(vcpu);
	return true;
}

static inline void
vmm_svm_exit_rdmsr(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

	exit->reason = VMM_CPUEXIT_RDMSR;
	exit->u.rdmsr.msr = (cpudata->gprs[VMM_X64_GPR_RCX] & 0xFFFFFFFF);
	exit->u.rdmsr.npc = cpudata->vmcb->ctrl.nrip;

	if (vmm_svm_inkernel_handle_msr(mach, vcpu, exit)) {
		exit->reason = VMM_CPUEXIT_NONE;
		return;
	}

	vmm_svm_vcpu_state_provide(vcpu, VMM_X64_STATE_GPRS);
}

static inline void
vmm_svm_exit_wrmsr(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	uint64_t rdx, rax;

	rdx = cpudata->gprs[VMM_X64_GPR_RDX];
	rax = cpudata->vmcb->state.rax;

	exit->reason = VMM_CPUEXIT_WRMSR;
	exit->u.wrmsr.msr = (cpudata->gprs[VMM_X64_GPR_RCX] & 0xFFFFFFFF);
	exit->u.wrmsr.val = (rdx << 32) | (rax & 0xFFFFFFFF);
	exit->u.wrmsr.npc = cpudata->vmcb->ctrl.nrip;

	if (vmm_svm_inkernel_handle_msr(mach, vcpu, exit)) {
		exit->reason = VMM_CPUEXIT_NONE;
		return;
	}

	vmm_svm_vcpu_state_provide(vcpu, VMM_X64_STATE_GPRS);
}

static void
vmm_svm_exit_msr(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	uint64_t info = cpudata->vmcb->ctrl.exitinfo1;

	if (info == 0) {
		vmm_svm_exit_rdmsr(mach, vcpu, exit);
	} else {
		vmm_svm_exit_wrmsr(mach, vcpu, exit);
	}
}

static void
vmm_svm_exit_npf(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	uint64_t gpa = cpudata->vmcb->ctrl.exitinfo2;
	const uint8_t *bytes = cpudata->vmcb->ctrl.inst_bytes;
	uint32_t value;
	uint8_t opcode;
	uint8_t modrm;
	unsigned int reg;
	int length;
	int offset;
	int rex;
	int modrm_size;
	bool data16;
	bool write;

	if (vcpu->machine->irqchip && cpudata->interrupt != NULL) {
		length = cpudata->vmcb->ctrl.inst_len;
		if (length != 0 && length <= (int)sizeof(cpudata->vmcb->ctrl.inst_bytes)) {
			offset = 0;
			rex = 0;
			data16 = false;
			while (offset < length) {
				if (bytes[offset] == 0x66) {
					data16 = true;
					++offset;
					continue;
				}
				if (bytes[offset] >= 0x40 && bytes[offset] <= 0x4f) {
					rex = bytes[offset++];
					continue;
				}
				break;
			}
			if (offset < length && !data16 && (rex & 0x08) == 0) {
				opcode = bytes[offset++];
				switch (opcode) {
				case 0x8b:
					write = false;
					break;
				case 0x89:
					write = true;
					break;
				case 0xc7:
					if (offset >= length ||
					    ((bytes[offset] >> 3) & 7) != 0)
						goto not_irqchip;
					write = true;
					break;
				default:
					goto not_irqchip;
				}
				if (offset >= length)
					goto not_irqchip;
				modrm = bytes[offset];
				modrm_size = vmm_svm_avic_modrm_size(bytes, length, offset);
				if (modrm_size == 0)
					goto not_irqchip;
				reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
				if (reg >= VMM_X64_GPR_RIP)
					goto not_irqchip;
				if (opcode == 0xc7) {
					if (offset + modrm_size + 4 > length)
						goto not_irqchip;
					offset += modrm_size;
					value = bytes[offset] | ((uint32_t)bytes[offset + 1] << 8) |
					    ((uint32_t)bytes[offset + 2] << 16) |
					    ((uint32_t)bytes[offset + 3] << 24);
				} else if (write) {
					value = cpudata->gprs[reg];
				} else {
					value = 0;
				}
				if (vmm_svm_interrupt_ops->vcpu_mmio(cpudata->interrupt,
				    gpa, write, &value) == 0) {
					if (!write) {
						cpudata->gprs[reg] = value;
						if (reg == VMM_X64_GPR_RAX)
							cpudata->vmcb->state.rax = value;
					}
					vmm_svm_inkernel_advance(cpudata->vmcb);
					exit->reason = VMM_CPUEXIT_NONE;
					return;
				}
			}
		}
	}

not_irqchip:

	exit->reason = VMM_CPUEXIT_MEMORY;
	if (cpudata->vmcb->ctrl.exitinfo1 & PGEX_W)
		exit->u.mem.prot = PROT_WRITE;
	else if (cpudata->vmcb->ctrl.exitinfo1 & PGEX_I)
		exit->u.mem.prot = PROT_EXEC;
	else
		exit->u.mem.prot = PROT_READ;
	exit->u.mem.gpa = gpa;
	exit->u.mem.inst_len = cpudata->vmcb->ctrl.inst_len;
	memcpy(exit->u.mem.inst_bytes, cpudata->vmcb->ctrl.inst_bytes,
	    sizeof(exit->u.mem.inst_bytes));

	vmm_svm_vcpu_state_provide(vcpu,
	    VMM_X64_STATE_GPRS | VMM_X64_STATE_SEGS |
	    VMM_X64_STATE_CRS | VMM_X64_STATE_MSRS);
}

static void
vmm_svm_exit_xsetbv(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;
	uint64_t val;

	exit->reason = VMM_CPUEXIT_NONE;

	val = (cpudata->gprs[VMM_X64_GPR_RDX] << 32) |
	    (vmcb->state.rax & 0xFFFFFFFF);

	if (__predict_false(cpudata->gprs[VMM_X64_GPR_RCX] != 0)) {
		goto error;
	} else if (__predict_false(vmcb->state.cpl != 0)) {
		goto error;
	} else if (__predict_false((val & ~vmm_svm_xcr0_mask) != 0)) {
		goto error;
	} else if (__predict_false((val & XCR0_X87) == 0)) {
		goto error;
	}

	cpudata->gxcr0 = val;

	vmm_svm_inkernel_advance(cpudata->vmcb);
	return;

error:
	vmm_svm_inject_gp(vcpu);
}

static void
vmm_svm_exit_invalid(struct vmm_cpuexit *exit, uint64_t code)
{
	exit->u.inv.hwcode = code;
	exit->reason = VMM_CPUEXIT_INVALID;
}

/* -------------------------------------------------------------------------- */

static void
vmm_svm_vcpu_guest_fpu_enter(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

#if defined(__NetBSD__)
	x86_curthread_save_fpu();
#elif defined(__DragonFly__)
	/*
	 * NOTE: Host FPU state depends on whether the user program used the
	 *       FPU or not.  Need to use npxpush()/npxpop() to handle this.
	 */
	npxpush(&cpudata->hstate.hmctx);
#endif

	x86_restore_fpu(&cpudata->gxsave, vmm_svm_xcr0_mask);
	if (vmm_svm_xcr0_mask != 0) {
		x86_set_xcr(0, cpudata->gxcr0);
	}
}

static void
vmm_svm_vcpu_guest_fpu_leave(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

	if (vmm_svm_xcr0_mask != 0) {
		x86_set_xcr(0, vmm_svm_global_hstate.xcr0);
	}
	x86_save_fpu(&cpudata->gxsave, vmm_svm_xcr0_mask);

#if defined(__NetBSD__)
	x86_curthread_restore_fpu();
#elif defined(__DragonFly__)
	npxpop(&cpudata->hstate.hmctx);
#endif
}

static void
vmm_svm_vcpu_guest_dbregs_enter(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

	if (curthread->td_lwp == NULL) {
		cpudata->hstate.drs[VMM_X64_DR_DR0] = x86_get_dr0();
		cpudata->hstate.drs[VMM_X64_DR_DR1] = x86_get_dr1();
		cpudata->hstate.drs[VMM_X64_DR_DR2] = x86_get_dr2();
		cpudata->hstate.drs[VMM_X64_DR_DR3] = x86_get_dr3();
		cpudata->hstate.drs[VMM_X64_DR_DR6] = x86_get_dr6();
		cpudata->hstate.drs[VMM_X64_DR_DR7] = x86_get_dr7();
	} else {
		x86_curthread_save_dbregs(cpudata->hstate.drs);
	}

	x86_set_dr7(0);

	x86_set_dr0(cpudata->drs[VMM_X64_DR_DR0]);
	x86_set_dr1(cpudata->drs[VMM_X64_DR_DR1]);
	x86_set_dr2(cpudata->drs[VMM_X64_DR_DR2]);
	x86_set_dr3(cpudata->drs[VMM_X64_DR_DR3]);
}

static void
vmm_svm_vcpu_guest_dbregs_leave(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

	cpudata->drs[VMM_X64_DR_DR0] = x86_get_dr0();
	cpudata->drs[VMM_X64_DR_DR1] = x86_get_dr1();
	cpudata->drs[VMM_X64_DR_DR2] = x86_get_dr2();
	cpudata->drs[VMM_X64_DR_DR3] = x86_get_dr3();

	if (curthread->td_lwp == NULL) {
		x86_set_dr0(cpudata->hstate.drs[VMM_X64_DR_DR0]);
		x86_set_dr1(cpudata->hstate.drs[VMM_X64_DR_DR1]);
		x86_set_dr2(cpudata->hstate.drs[VMM_X64_DR_DR2]);
		x86_set_dr3(cpudata->hstate.drs[VMM_X64_DR_DR3]);
		x86_set_dr6(cpudata->hstate.drs[VMM_X64_DR_DR6]);
		x86_set_dr7(cpudata->hstate.drs[VMM_X64_DR_DR7]);
	} else {
		x86_curthread_restore_dbregs(cpudata->hstate.drs);
	}
}

static void
vmm_svm_vcpu_guest_misc_enter(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

	/* Save the percpu host state. */
	cpudata->hstate.fsbase = rdmsr(MSR_FSBASE);
	cpudata->hstate.kernelgsbase = rdmsr(MSR_KERNELGSBASE);
	cpudata->hstate.sysenter_cs = rdmsr(MSR_SYSENTER_CS);
	cpudata->hstate.sysenter_esp = rdmsr(MSR_SYSENTER_ESP);
	cpudata->hstate.sysenter_eip = rdmsr(MSR_SYSENTER_EIP);
}

static void
vmm_svm_vcpu_guest_misc_leave(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

	/* Restore the global host state. */
	wrmsr(MSR_STAR, vmm_svm_global_hstate.star);
	wrmsr(MSR_LSTAR, vmm_svm_global_hstate.lstar);
	wrmsr(MSR_CSTAR, vmm_svm_global_hstate.cstar);
	wrmsr(MSR_SFMASK, vmm_svm_global_hstate.sfmask);
	wrmsr(MSR_SYSENTER_CS, cpudata->hstate.sysenter_cs);
	wrmsr(MSR_SYSENTER_ESP, cpudata->hstate.sysenter_esp);
	wrmsr(MSR_SYSENTER_EIP, cpudata->hstate.sysenter_eip);

	/* Restore the percpu host state. */
	wrmsr(MSR_FSBASE, cpudata->hstate.fsbase);
	wrmsr(MSR_KERNELGSBASE, cpudata->hstate.kernelgsbase);
}

/* -------------------------------------------------------------------------- */

static inline void
vmm_svm_gtlb_catchup(struct vmm_vcpu *vcpu, int hcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

	if (cpudata->hcpu_last != hcpu || cpudata->shared_asid) {
		cpudata->gtlb_want_flush = true;
	}
}

static inline void
vmm_svm_htlb_catchup(struct vmm_vcpu *vcpu, int hcpu)
{
	/*
	 * Nothing to do. If an hTLB flush was needed, either the VCPU was
	 * executing on this hCPU and the hTLB already got flushed, or it
	 * was executing on another hCPU in which case the catchup is done
	 * indirectly when svm_gtlb_catchup() sets gtlb_want_flush.
	 */
}

static inline uint64_t
vmm_svm_htlb_flush(struct vmm_machine *mach, struct vmm_svm_cpudata *cpudata)
{
	struct vmcb *vmcb = cpudata->vmcb;
	uint64_t machgen;

#if defined(__NetBSD__)
	machgen = ((struct vmm_svm_machdata *)mach->backend_state)->mach_htlb_gen;
#elif defined(__DragonFly__)
	clear_xinvltlb();
	machgen = vmspace_pmap(mach->vmspace)->pm_invgen;
#endif
	if (__predict_true(machgen == cpudata->vcpu_htlb_gen)) {
		return machgen;
	}

	cpudata->htlb_want_flush = true;
	vmcb->ctrl.tlb_ctrl = vmm_svm_ctrl_tlb_flush;
	return machgen;
}

static inline void
vmm_svm_htlb_flush_ack(struct vmm_svm_cpudata *cpudata, uint64_t machgen)
{
	struct vmcb *vmcb = cpudata->vmcb;

	if (__predict_true(vmcb->ctrl.exitcode != VMCB_EXITCODE_INVALID)) {
		cpudata->vcpu_htlb_gen = machgen;
		cpudata->htlb_want_flush = false;
	}
}

static inline void
vmm_svm_exit_evt(struct vmm_svm_cpudata *cpudata, struct vmcb *vmcb)
{
	cpudata->evt_pending = false;

	if (__predict_false(vmcb->ctrl.exitintinfo & VMCB_CTRL_EXITINTINFO_V)) {
		vmcb->ctrl.eventinj = vmcb->ctrl.exitintinfo;
		cpudata->evt_pending = true;
	}
}

void
vmm_svm_restore_tr(uint16_t selector)
{
	struct mdglobaldata *globaldata;
	volatile uint64_t *descriptor;

	/* VMRUN marks the host TSS descriptor busy; LTR requires it clear. */
	globaldata = (struct mdglobaldata *)(void *)mycpu;
	descriptor = (volatile uint64_t *)(void *)
	    ((char *)globaldata->gd_tss_gdt + 4);
	*descriptor &= ~0x0200ULL;
	ltr(selector);
}

static int
vmm_svm_avic_modrm_size(const uint8_t *bytes, int length, int offset)
{
	uint8_t modrm;
	uint8_t mod;
	uint8_t rm;
	uint8_t sib;
	int size;

	if (offset >= length)
		return 0;
	modrm = bytes[offset];
	mod = modrm >> 6;
	rm = modrm & 7;
	if (mod == 3)
		return 0;
	size = 1;
	if (rm == 4) {
		if (offset + size >= length)
			return 0;
		sib = bytes[offset + size++];
		if (mod == 0 && (sib & 7) == 5)
			size += 4;
	}
	if (mod == 0 && rm == 5)
		size += 4;
	else if (mod == 1)
		size++;
	else if (mod == 2)
		size += 4;
	return offset + size <= length ? size : 0;
}

static bool
vmm_svm_avic_noaccel_read(struct vmm_svm_cpudata *cpudata,
    uint64_t exitinfo1)
{
	struct vmcb *vmcb = cpudata->vmcb;
	const uint8_t *bytes = vmcb->ctrl.inst_bytes;
	uint32_t value;
	uint8_t modrm;
	unsigned int reg;
	int length;
	int offset;
	int rex;
	int modrm_size;
	bool data16;

	if ((exitinfo1 >> 32) & __BIT(0))
		return false;
	if (vmm_svm_avic_read_register(cpudata->interrupt,
	    exitinfo1 & 0xff0U, &value) != 0)
		return false;
	length = vmcb->ctrl.inst_len;
	if (length == 0 || length > (int)sizeof(vmcb->ctrl.inst_bytes))
		return false;
	offset = 0;
	rex = 0;
	data16 = false;
	while (offset < length) {
		if (bytes[offset] == 0x66) {
			data16 = true;
			offset++;
			continue;
		}
		if (bytes[offset] >= 0x40 && bytes[offset] <= 0x4f) {
			rex = bytes[offset++];
			continue;
		}
		break;
	}
	if (offset >= length || data16 || (rex & 0x08) != 0 ||
	    bytes[offset++] != 0x8b || offset >= length)
		return false;
	modrm = bytes[offset];
	modrm_size = vmm_svm_avic_modrm_size(bytes, length, offset);
	if (modrm_size == 0)
		return false;
	reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
	if (reg >= VMM_X64_GPR_RIP)
		return false;
	cpudata->gprs[reg] = value;
	if (reg == VMM_X64_GPR_RAX)
		vmcb->state.rax = value;
	vmm_svm_inkernel_advance(vmcb);
	return true;
}

int
vmm_svm_vcpu_run(struct vmm_vcpu *vcpu, struct vmm_cpuexit **reason)
{
	struct vmm_machine *mach = vcpu->machine;
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;
	struct vmm_cpuexit *exit = &vcpu->exit;
	uint64_t machgen;
	int hcpu;
	int error = 0;

	vmm_svm_vcpu_setstate(vcpu, VMM_X64_STATE_ALL);

	hcpu = os_curcpu_number();

	vmm_svm_gtlb_catchup(vcpu, hcpu);
	vmm_svm_htlb_catchup(vcpu, hcpu);

	if (cpudata->hcpu_last != hcpu) {
		vmm_svm_vmcb_cache_flush_all(vmcb);
		cpudata->gtsc_want_update = true;

#ifdef __DragonFly__
		/*
		 * XXX: We aren't tracking overloaded CPUs (multiple vCPUs
		 *      scheduled on the same physical CPU) yet so there are
		 *      currently no calls to pmap_del_cpu().
		 */
		pmap_add_cpu(mach->vmspace, hcpu);
#endif
	}

	while (1) {
		if (__predict_false(cpudata->gtlb_want_flush ||
				    cpudata->htlb_want_flush))
		{
			vmcb->ctrl.tlb_ctrl = vmm_svm_ctrl_tlb_flush;
		} else {
			vmcb->ctrl.tlb_ctrl = 0;
		}

		if (__predict_false(cpudata->gtsc_want_update)) {
			vmcb->ctrl.tsc_offset = cpudata->gtsc_offset;
			vmm_svm_vmcb_cache_flush(vmcb, VMCB_CTRL_VMCB_CLEAN_I);
		}

		vmm_svm_clgi();
		vmm_svm_vcpu_guest_dbregs_enter(vcpu);
		vmm_svm_vcpu_guest_misc_enter(vcpu);
		vmm_svm_vcpu_guest_fpu_enter(vcpu);
		machgen = vmm_svm_htlb_flush(mach, cpudata);

#ifdef __DragonFly__
		/*
		 * Check for pending host events (e.g., interrupt, AST)
		 * to make the state safe to VM Entry.  This check must
		 * be done after the clgi to avoid gd_reqflags pending
		 * races.
		 *
		 * Emulators may assume that event injection succeeds, but
		 * we have to return to process these events.  To deal with
		 * this, use ERESTART mechanics.
		 */
		if (__predict_false(mycpu->gd_reqflags & RQF_HVM_MASK)) {
			/* No hTLB flush ack, because it's not executed. */
			vmm_svm_vcpu_guest_fpu_leave(vcpu);
			vmm_svm_vcpu_guest_misc_leave(vcpu);
			vmm_svm_vcpu_guest_dbregs_leave(vcpu);
			vmm_svm_stgi();
			exit->reason = VMM_CPUEXIT_NONE;
			error = ERESTART;
			break;
		}

#endif

		/*
		 * Commit caller events only after host work can no longer prevent
		 * this VM entry.  State load above never contains delivery controls.
		 */
		if (__predict_false(vcpu->event_pending)) {
			struct vmm_cpuevent event = vcpu->event;

			vcpu->event_pending = 0;
			if (vmm_svm_vcpu_commit_event(vcpu, &event) != 0) {
				/* No hTLB flush ack, because VMRUN is not executed. */
				vmm_svm_vcpu_guest_fpu_leave(vcpu);
				vmm_svm_vcpu_guest_misc_leave(vcpu);
				vmm_svm_vcpu_guest_dbregs_leave(vcpu);
				vmm_svm_stgi();
				exit->reason = VMM_CPUEXIT_NONE;
				error = EINVAL;
				break;
			}
		}

		vmm_svm_interrupt_ops->vcpu_enter(cpudata->interrupt);
		atomic_store_rel_int(&cpudata->running_cpu, hcpu);
		vmm_svm_vmrun(cpudata->vmcb_pa, cpudata->gprs);
		vmm_stat_vmexit();
		atomic_store_rel_int(&cpudata->running_cpu, -1);
		vmm_svm_interrupt_ops->vcpu_leave(cpudata->interrupt);
		vmm_svm_htlb_flush_ack(cpudata, machgen);
		vmm_svm_vcpu_guest_fpu_leave(vcpu);
		vmm_svm_vcpu_guest_misc_leave(vcpu);
		vmm_svm_vcpu_guest_dbregs_leave(vcpu);
		vmm_svm_stgi();

		vmm_svm_vmcb_cache_default(vmcb);

		if (vmcb->ctrl.exitcode != VMCB_EXITCODE_INVALID) {
			cpudata->gtlb_want_flush = false;
			cpudata->gtsc_want_update = false;
			cpudata->hcpu_last = hcpu;
		}
		vmm_svm_exit_evt(cpudata, vmcb);

		switch (vmcb->ctrl.exitcode) {
		case VMCB_EXITCODE_INTR:
		case VMCB_EXITCODE_NMI:
			exit->reason = VMM_CPUEXIT_NONE;
			break;
		case VMCB_EXITCODE_VINTR:
			vmm_svm_event_waitexit_disable(vcpu, false);
			exit->reason = VMM_CPUEXIT_INT_READY;
			break;
		case VMCB_EXITCODE_CR0_SEL_WRITE:
			vmm_svm_exit_cr0(mach, vcpu, exit);
			break;
		case VMCB_EXITCODE_IRET:
			vmm_svm_event_waitexit_disable(vcpu, true);
			exit->reason = VMM_CPUEXIT_NMI_READY;
			break;
		case VMCB_EXITCODE_CPUID:
			vmm_svm_exit_cpuid(mach, vcpu, exit);
			break;
		case VMCB_EXITCODE_HLT:
			vmm_svm_exit_hlt(mach, vcpu, exit);
			break;
		case VMCB_EXITCODE_IOIO:
			vmm_svm_exit_io(mach, vcpu, exit);
			break;
		case VMCB_EXITCODE_MSR:
			vmm_svm_exit_msr(mach, vcpu, exit);
			break;
		case VMCB_EXITCODE_SHUTDOWN:
			exit->reason = VMM_CPUEXIT_SHUTDOWN;
			break;
		case VMCB_EXITCODE_RDPMC:
		case VMCB_EXITCODE_RSM:
		case VMCB_EXITCODE_INVLPGA:
		case VMCB_EXITCODE_VMRUN:
		case VMCB_EXITCODE_VMMCALL:
		case VMCB_EXITCODE_VMLOAD:
		case VMCB_EXITCODE_VMSAVE:
		case VMCB_EXITCODE_STGI:
		case VMCB_EXITCODE_CLGI:
		case VMCB_EXITCODE_SKINIT:
		case VMCB_EXITCODE_RDTSCP:
		case VMCB_EXITCODE_RDPRU:
		case VMCB_EXITCODE_INVLPGB:
		case VMCB_EXITCODE_INVPCID:
		case VMCB_EXITCODE_MCOMMIT:
		case VMCB_EXITCODE_TLBSYNC:
			vmm_svm_inject_ud(vcpu);
			exit->reason = VMM_CPUEXIT_NONE;
			break;
		case VMCB_EXITCODE_MONITOR:
			vmm_svm_exit_insn(vmcb, exit, VMM_CPUEXIT_MONITOR);
			break;
		case VMCB_EXITCODE_MWAIT:
		case VMCB_EXITCODE_MWAIT_CONDITIONAL:
			vmm_svm_exit_insn(vmcb, exit, VMM_CPUEXIT_MWAIT);
			break;
		case VMCB_EXITCODE_XSETBV:
			vmm_svm_exit_xsetbv(mach, vcpu, exit);
			break;
		case VMCB_EXITCODE_NPF:
			vmm_svm_exit_npf(mach, vcpu, exit);
			break;
		case VMCB_EXITCODE_AVIC_INCOMP_IPI:
		case VMCB_EXITCODE_AVIC_NOACCEL:
			if (vmm_svm_interrupt_ops->vcpu_exit(cpudata->interrupt,
			    vmcb->ctrl.exitcode, vmcb->ctrl.exitinfo1,
			    vmcb->ctrl.exitinfo2) ||
			    vmm_svm_avic_noaccel_read(cpudata, vmcb->ctrl.exitinfo1)) {
				exit->reason = VMM_CPUEXIT_NONE;
			} else {
				vmm_svm_exit_invalid(exit, vmcb->ctrl.exitcode);
			}
			break;
		case VMCB_EXITCODE_FERR_FREEZE: /* ? */
		default:
			vmm_svm_exit_invalid(exit, vmcb->ctrl.exitcode);
			break;
		}

		/* If no reason to return to userland, keep rolling. */
		if (os_return_needed()) {
			break;
		}
		if (exit->reason != VMM_CPUEXIT_NONE) {
			break;
		}
	}

	exit->exitstate.rflags = vmcb->state.rflags;
	exit->exitstate.cr8 = __SHIFTOUT(vmcb->ctrl.v, VMCB_CTRL_V_TPR);
	exit->exitstate.int_shadow =
	    ((vmcb->ctrl.intr & VMCB_CTRL_INTR_SHADOW) != 0);
	exit->exitstate.int_window_exiting = cpudata->int_window_exit;
	exit->exitstate.nmi_window_exiting = cpudata->nmi_window_exit;
	exit->exitstate.evt_pending = cpudata->evt_pending;

	if (error == 0)
		*reason = exit;
	return error;
}

static void
vmm_svm_kick_ipiq(void *arg)
{
	(void)arg;
}

void
vmm_svm_vcpu_kick(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	int cpu;

	cpu = atomic_load_acq_int(&cpudata->running_cpu);
	if (cpu >= 0 && cpu != os_curcpu_number())
		lwkt_send_ipiq_bycpu(cpu, vmm_svm_kick_ipiq, vcpu);
}

/* -------------------------------------------------------------------------- */

#define SVM_MSRBM_READ	__BIT(0)
#define SVM_MSRBM_WRITE	__BIT(1)

static void
vmm_svm_vcpu_msr_allow(uint8_t *bitmap, uint64_t msr, bool read, bool write)
{
	uint64_t byte;
	uint8_t bitoff;

	if (msr < 0x00002000) {
		/* Range 1 */
		byte = ((msr - 0x00000000) >> 2UL) + 0x0000;
	} else if (msr >= 0xC0000000 && msr < 0xC0002000) {
		/* Range 2 */
		byte = ((msr - 0xC0000000) >> 2UL) + 0x0800;
	} else if (msr >= 0xC0010000 && msr < 0xC0012000) {
		/* Range 3 */
		byte = ((msr - 0xC0010000) >> 2UL) + 0x1000;
	} else {
		panic("%s: wrong range", __func__);
	}

	bitoff = (msr & 0x3) << 1;

	if (read) {
		bitmap[byte] &= ~(SVM_MSRBM_READ << bitoff);
	}
	if (write) {
		bitmap[byte] &= ~(SVM_MSRBM_WRITE << bitoff);
	}
}

#define SVM_SEG_ATTRIB_TYPE		__BITS(3,0)
#define SVM_SEG_ATTRIB_S		__BIT(4)
#define SVM_SEG_ATTRIB_DPL		__BITS(6,5)
#define SVM_SEG_ATTRIB_P		__BIT(7)
#define SVM_SEG_ATTRIB_AVL		__BIT(8)
#define SVM_SEG_ATTRIB_L		__BIT(9)
#define SVM_SEG_ATTRIB_DEF		__BIT(10)
#define SVM_SEG_ATTRIB_G		__BIT(11)

static void
vmm_svm_vcpu_setstate_seg(const struct vmm_segment *seg,
    struct vmcb_segment *vseg)
{
	vseg->selector = seg->selector;
	vseg->attrib =
	    __SHIFTIN(seg->attrib.type, SVM_SEG_ATTRIB_TYPE) |
	    __SHIFTIN(seg->attrib.s, SVM_SEG_ATTRIB_S) |
	    __SHIFTIN(seg->attrib.dpl, SVM_SEG_ATTRIB_DPL) |
	    __SHIFTIN(seg->attrib.p, SVM_SEG_ATTRIB_P) |
	    __SHIFTIN(seg->attrib.avl, SVM_SEG_ATTRIB_AVL) |
	    __SHIFTIN(seg->attrib.l, SVM_SEG_ATTRIB_L) |
	    __SHIFTIN(seg->attrib.def, SVM_SEG_ATTRIB_DEF) |
	    __SHIFTIN(seg->attrib.g, SVM_SEG_ATTRIB_G);
	vseg->limit = seg->limit;
	vseg->base = seg->base;
}

static void
vmm_svm_vcpu_getstate_seg(struct vmm_segment *seg,
    const struct vmcb_segment *vseg)
{
	seg->selector = vseg->selector;
	seg->attrib.type = __SHIFTOUT(vseg->attrib, SVM_SEG_ATTRIB_TYPE);
	seg->attrib.s = __SHIFTOUT(vseg->attrib, SVM_SEG_ATTRIB_S);
	seg->attrib.dpl = __SHIFTOUT(vseg->attrib, SVM_SEG_ATTRIB_DPL);
	seg->attrib.p = __SHIFTOUT(vseg->attrib, SVM_SEG_ATTRIB_P);
	seg->attrib.avl = __SHIFTOUT(vseg->attrib, SVM_SEG_ATTRIB_AVL);
	seg->attrib.l = __SHIFTOUT(vseg->attrib, SVM_SEG_ATTRIB_L);
	seg->attrib.def = __SHIFTOUT(vseg->attrib, SVM_SEG_ATTRIB_DEF);
	seg->attrib.g = __SHIFTOUT(vseg->attrib, SVM_SEG_ATTRIB_G);
	seg->limit = vseg->limit;
	seg->base = vseg->base;
}

static inline bool
vmm_svm_state_gtlb_flush(const struct vmcb *vmcb,
    const struct vmm_cpustate *state, uint64_t flags)
{
	if (flags & VMM_X64_STATE_CRS) {
		if ((vmcb->state.cr0 ^
		     state->crs[VMM_X64_CR_CR0]) & CR0_TLB_FLUSH) {
			return true;
		}
		if (vmcb->state.cr3 != state->crs[VMM_X64_CR_CR3]) {
			return true;
		}
		if ((vmcb->state.cr4 ^
		     state->crs[VMM_X64_CR_CR4]) & CR4_TLB_FLUSH) {
			return true;
		}
	}

	if (flags & VMM_X64_STATE_MSRS) {
		if ((vmcb->state.efer ^
		     state->msrs[VMM_X64_MSR_EFER]) & EFER_TLB_FLUSH) {
			return true;
		}
	}

	return false;
}

static void
vmm_svm_vcpu_setstate(struct vmm_vcpu *vcpu, uint64_t flags)
{
	const struct vmm_cpustate *state = vcpu->state;
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;
	struct vmm_cpustate_fpu *fpustate;

	if (vmm_svm_state_gtlb_flush(vmcb, state, flags)) {
		cpudata->gtlb_want_flush = true;
	}

	if (flags & VMM_X64_STATE_SEGS) {
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_CS],
		    &vmcb->state.cs);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_DS],
		    &vmcb->state.ds);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_ES],
		    &vmcb->state.es);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_FS],
		    &vmcb->state.fs);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_GS],
		    &vmcb->state.gs);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_SS],
		    &vmcb->state.ss);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_GDT],
		    &vmcb->state.gdt);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_IDT],
		    &vmcb->state.idt);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_LDT],
		    &vmcb->state.ldt);
		vmm_svm_vcpu_setstate_seg(&state->segs[VMM_X64_SEG_TR],
		    &vmcb->state.tr);

		vmcb->state.cpl = state->segs[VMM_X64_SEG_SS].attrib.dpl;
	}

	CTASSERT(sizeof(cpudata->gprs) == sizeof(state->gprs));
	if (flags & VMM_X64_STATE_GPRS) {
		memcpy(cpudata->gprs, state->gprs, sizeof(state->gprs));

		vmcb->state.rip = state->gprs[VMM_X64_GPR_RIP];
		vmcb->state.rsp = state->gprs[VMM_X64_GPR_RSP];
		vmcb->state.rax = state->gprs[VMM_X64_GPR_RAX];
		vmcb->state.rflags = state->gprs[VMM_X64_GPR_RFLAGS];
	}

	if (flags & VMM_X64_STATE_CRS) {
		vmcb->state.cr0 =
		    (state->crs[VMM_X64_CR_CR0] & ~CR0_FORCE_ZERO) |
		    CR0_FORCE_ONE;
		vmcb->state.cr2 = state->crs[VMM_X64_CR_CR2];
		vmcb->state.cr3 = state->crs[VMM_X64_CR_CR3];
		vmcb->state.cr4 = state->crs[VMM_X64_CR_CR4];

		vmcb->ctrl.v &= ~VMCB_CTRL_V_TPR;
		vmcb->ctrl.v |= __SHIFTIN(state->crs[VMM_X64_CR_CR8],
		    VMCB_CTRL_V_TPR);

		if (vmm_svm_xcr0_mask != 0) {
			/* Clear illegal XCR0 bits, set mandatory X87 bit. */
			cpudata->gxcr0 = state->crs[VMM_X64_CR_XCR0];
			cpudata->gxcr0 &= vmm_svm_xcr0_mask;
			cpudata->gxcr0 |= XCR0_X87;
		}
	}

	CTASSERT(sizeof(cpudata->drs) == sizeof(state->drs));
	if (flags & VMM_X64_STATE_DRS) {
		memcpy(cpudata->drs, state->drs, sizeof(state->drs));

		vmcb->state.dr6 = state->drs[VMM_X64_DR_DR6];
		vmcb->state.dr7 = state->drs[VMM_X64_DR_DR7];
	}

	if (flags & VMM_X64_STATE_MSRS) {
		/*
		 * EFER_SVME is mandatory.
		 */
		vmcb->state.efer = state->msrs[VMM_X64_MSR_EFER] | EFER_SVME;
		vmcb->state.star = state->msrs[VMM_X64_MSR_STAR];
		vmcb->state.lstar = state->msrs[VMM_X64_MSR_LSTAR];
		vmcb->state.cstar = state->msrs[VMM_X64_MSR_CSTAR];
		vmcb->state.sfmask = state->msrs[VMM_X64_MSR_SFMASK];
		vmcb->state.kernelgsbase =
		    state->msrs[VMM_X64_MSR_KERNELGSBASE];
		vmcb->state.sysenter_cs =
		    state->msrs[VMM_X64_MSR_SYSENTER_CS];
		vmcb->state.sysenter_esp =
		    state->msrs[VMM_X64_MSR_SYSENTER_ESP];
		vmcb->state.sysenter_eip =
		    state->msrs[VMM_X64_MSR_SYSENTER_EIP];
		vmcb->state.g_pat = state->msrs[VMM_X64_MSR_PAT];

		/*
		 * The emulator might NOT want to set the TSC, because doing
		 * so would destroy TSC MP-synchronization across CPUs.  Try
		 * to figure out what the emulator meant to do.
		 *
		 * If writing the last TSC value we reported via getstate or
		 * a zero value, assume that the emulator does not want to
		 * write to the TSC.
		 */
		if (state->msrs[VMM_X64_MSR_TSC] != cpudata->gtsc_match &&
		    state->msrs[VMM_X64_MSR_TSC] != 0) {
			cpudata->gtsc_offset =
			    state->msrs[VMM_X64_MSR_TSC] - rdtsc();
			cpudata->gtsc_want_update = true;
		}
	}

	if (flags & VMM_X64_STATE_INTR) {
		if (state->intr.int_shadow) {
			vmcb->ctrl.intr |= VMCB_CTRL_INTR_SHADOW;
		} else {
			vmcb->ctrl.intr &= ~VMCB_CTRL_INTR_SHADOW;
		}
	}

	CTASSERT(sizeof(cpudata->gxsave.fpu) == sizeof(state->fpu));
	if (flags & VMM_X64_STATE_FPU) {
		memcpy(&cpudata->gxsave.fpu, &state->fpu, sizeof(state->fpu));

		fpustate = (struct vmm_cpustate_fpu *)&cpudata->gxsave.fpu;
		fpustate->fx_mxcsr_mask &= x86_fpu_mxcsr_mask;
		fpustate->fx_mxcsr &= fpustate->fx_mxcsr_mask;

		if (vmm_svm_xcr0_mask != 0) {
			/* Reset XSTATE_BV, to force a reload. */
			cpudata->gxsave.xstate_bv = vmm_svm_xcr0_mask;
		}
	}

	vmm_svm_vmcb_cache_update(vmcb, flags);

}

static void
vmm_svm_vcpu_getstate_all(struct vmm_vcpu *vcpu, uint64_t flags)
{
	struct vmm_cpustate *state = vcpu->state;
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	const struct vmcb *vmcb = cpudata->vmcb;

	if (flags & VMM_X64_STATE_SEGS) {
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_CS],
		    &vmcb->state.cs);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_DS],
		    &vmcb->state.ds);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_ES],
		    &vmcb->state.es);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_FS],
		    &vmcb->state.fs);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_GS],
		    &vmcb->state.gs);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_SS],
		    &vmcb->state.ss);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_GDT],
		    &vmcb->state.gdt);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_IDT],
		    &vmcb->state.idt);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_LDT],
		    &vmcb->state.ldt);
		vmm_svm_vcpu_getstate_seg(&state->segs[VMM_X64_SEG_TR],
		    &vmcb->state.tr);

		state->segs[VMM_X64_SEG_SS].attrib.dpl = vmcb->state.cpl;
	}

	CTASSERT(sizeof(cpudata->gprs) == sizeof(state->gprs));
	if (flags & VMM_X64_STATE_GPRS) {
		memcpy(state->gprs, cpudata->gprs, sizeof(state->gprs));

		state->gprs[VMM_X64_GPR_RIP] = vmcb->state.rip;
		state->gprs[VMM_X64_GPR_RSP] = vmcb->state.rsp;
		state->gprs[VMM_X64_GPR_RAX] = vmcb->state.rax;
		state->gprs[VMM_X64_GPR_RFLAGS] = vmcb->state.rflags;
	}

	if (flags & VMM_X64_STATE_CRS) {
		state->crs[VMM_X64_CR_CR0] = vmcb->state.cr0;
		state->crs[VMM_X64_CR_CR2] = vmcb->state.cr2;
		state->crs[VMM_X64_CR_CR3] = vmcb->state.cr3;
		state->crs[VMM_X64_CR_CR4] = vmcb->state.cr4;
		state->crs[VMM_X64_CR_CR8] = __SHIFTOUT(vmcb->ctrl.v,
		    VMCB_CTRL_V_TPR);
		state->crs[VMM_X64_CR_XCR0] = cpudata->gxcr0;
	}

	CTASSERT(sizeof(cpudata->drs) == sizeof(state->drs));
	if (flags & VMM_X64_STATE_DRS) {
		memcpy(state->drs, cpudata->drs, sizeof(state->drs));

		state->drs[VMM_X64_DR_DR6] = vmcb->state.dr6;
		state->drs[VMM_X64_DR_DR7] = vmcb->state.dr7;
	}

	if (flags & VMM_X64_STATE_MSRS) {
		state->msrs[VMM_X64_MSR_EFER] = vmcb->state.efer;
		state->msrs[VMM_X64_MSR_STAR] = vmcb->state.star;
		state->msrs[VMM_X64_MSR_LSTAR] = vmcb->state.lstar;
		state->msrs[VMM_X64_MSR_CSTAR] = vmcb->state.cstar;
		state->msrs[VMM_X64_MSR_SFMASK] = vmcb->state.sfmask;
		state->msrs[VMM_X64_MSR_KERNELGSBASE] =
		    vmcb->state.kernelgsbase;
		state->msrs[VMM_X64_MSR_SYSENTER_CS] =
		    vmcb->state.sysenter_cs;
		state->msrs[VMM_X64_MSR_SYSENTER_ESP] =
		    vmcb->state.sysenter_esp;
		state->msrs[VMM_X64_MSR_SYSENTER_EIP] =
		    vmcb->state.sysenter_eip;
		state->msrs[VMM_X64_MSR_PAT] = vmcb->state.g_pat;
		state->msrs[VMM_X64_MSR_TSC] = rdtsc() + cpudata->gtsc_offset;

		/* Hide SVME. */
		state->msrs[VMM_X64_MSR_EFER] &= ~EFER_SVME;

		/* Save reported TSC value for later setstate check. */
		cpudata->gtsc_match = state->msrs[VMM_X64_MSR_TSC];
	}

	if (flags & VMM_X64_STATE_INTR) {
		state->intr.int_shadow =
		    (vmcb->ctrl.intr & VMCB_CTRL_INTR_SHADOW) != 0;
	}

	CTASSERT(sizeof(cpudata->gxsave.fpu) == sizeof(state->fpu));
	if (flags & VMM_X64_STATE_FPU) {
		memcpy(&state->fpu, &cpudata->gxsave.fpu, sizeof(state->fpu));
	}

}

static void
vmm_svm_vcpu_state_provide(struct vmm_vcpu *vcpu, uint64_t flags)
{
	vmm_svm_vcpu_getstate_all(vcpu, flags);
}

void
vmm_svm_vcpu_getstate(struct vmm_vcpu *vcpu)
{
	vmm_svm_vcpu_getstate_all(vcpu, VMM_X64_STATE_ALL);
}

/* -------------------------------------------------------------------------- */

static void
vmm_svm_asid_alloc(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;
	size_t i, oct, bit;

	os_mtx_lock(&vmm_svm_asidlock);

	for (i = 0; i < vmm_svm_maxasid; i++) {
		oct = i / 8;
		bit = i % 8;

		if (vmm_svm_asidmap[oct] & __BIT(bit)) {
			continue;
		}

		vmm_svm_asidmap[oct] |= __BIT(bit);
		vmcb->ctrl.guest_asid = i;
		os_mtx_unlock(&vmm_svm_asidlock);
		return;
	}

	/*
	 * No free ASID. Use the last one, which is shared and requires
	 * special TLB handling.
	 */
	cpudata->shared_asid = true;
	vmcb->ctrl.guest_asid = vmm_svm_maxasid - 1;
	os_mtx_unlock(&vmm_svm_asidlock);
}

static void
vmm_svm_asid_free(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;
	size_t oct, bit;

	if (cpudata->shared_asid) {
		return;
	}

	oct = vmcb->ctrl.guest_asid / 8;
	bit = vmcb->ctrl.guest_asid % 8;

	os_mtx_lock(&vmm_svm_asidlock);
	vmm_svm_asidmap[oct] &= ~__BIT(bit);
	os_mtx_unlock(&vmm_svm_asidlock);
}

static void
vmm_svm_vcpu_init(struct vmm_machine *mach, struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	struct vmcb *vmcb = cpudata->vmcb;

	/*
	 * Allow reads/writes of Control Registers.
	 * However, selective CR0 write is actually intercepted below with
	 * VMCB_CTRL_INTERCEPT_CR0_SEL.
	 */
	vmcb->ctrl.intercept_cr = 0;

	/* Allow reads/writes of Debug Registers. */
	vmcb->ctrl.intercept_dr = 0;

	/* Allow exceptions 0 to 31. */
	vmcb->ctrl.intercept_vec = 0;

	/*
	 * Allow:
	 *  - SMI [smm interrupts]
	 *  - VINTR [virtual interrupts]
	 *  - RIDTR [reads of IDTR]
	 *  - RGDTR [reads of GDTR]
	 *  - RLDTR [reads of LDTR]
	 *  - RTR [reads of TR]
	 *  - WIDTR [writes of IDTR]
	 *  - WGDTR [writes of GDTR]
	 *  - WLDTR [writes of LDTR]
	 *  - WTR [writes of TR]
	 *  - RDTSC [rdtsc instruction]
	 *  - PUSHF [pushf instruction]
	 *  - POPF [popf instruction]
	 *  - IRET [iret instruction]
	 *  - INTN [int $n instructions]
	 *  - PAUSE [pause instruction]
	 *  - INVLPG [invplg instruction]
	 *  - TASKSW [task switches]
	 *
	 * Intercept the rest below.
	 */
	vmcb->ctrl.intercept_misc1 =
	    VMCB_CTRL_INTERCEPT_INTR |
	    VMCB_CTRL_INTERCEPT_NMI |
	    VMCB_CTRL_INTERCEPT_INIT |
	    VMCB_CTRL_INTERCEPT_RDPMC |
	    VMCB_CTRL_INTERCEPT_CPUID |
	    VMCB_CTRL_INTERCEPT_RSM |
	    VMCB_CTRL_INTERCEPT_INVD |
	    VMCB_CTRL_INTERCEPT_HLT |
	    VMCB_CTRL_INTERCEPT_INVLPGA |
	    VMCB_CTRL_INTERCEPT_IOIO_PROT |
	    VMCB_CTRL_INTERCEPT_MSR_PROT |
	    VMCB_CTRL_INTERCEPT_FERR_FREEZE |
	    VMCB_CTRL_INTERCEPT_SHUTDOWN;
	if (vmm_svm_decode_assist) {
		vmcb->ctrl.intercept_misc1 |= VMCB_CTRL_INTERCEPT_CR0_SEL;
	}

	/*
	 * Allow:
	 *  - ICEBP [icebp instruction]
	 *  - WBINVD [wbinvd instruction]
	 *  - WCR_SPEC(0..15) [writes of CR0-15, received after instruction]
	 *
	 * Intercept the rest below.
	 */
	vmcb->ctrl.intercept_misc2 =
	    VMCB_CTRL_INTERCEPT_VMRUN |
	    VMCB_CTRL_INTERCEPT_VMMCALL |
	    VMCB_CTRL_INTERCEPT_VMLOAD |
	    VMCB_CTRL_INTERCEPT_VMSAVE |
	    VMCB_CTRL_INTERCEPT_STGI |
	    VMCB_CTRL_INTERCEPT_CLGI |
	    VMCB_CTRL_INTERCEPT_SKINIT |
	    VMCB_CTRL_INTERCEPT_RDTSCP |
	    VMCB_CTRL_INTERCEPT_MONITOR |
	    VMCB_CTRL_INTERCEPT_MWAIT |
	    VMCB_CTRL_INTERCEPT_XSETBV |
	    VMCB_CTRL_INTERCEPT_RDPRU;

	/*
	 * Intercept everything.
	 */
	vmcb->ctrl.intercept_misc3 =
	    VMCB_CTRL_INTERCEPT_INVLPGB_ALL |
	    VMCB_CTRL_INTERCEPT_PCID |
	    VMCB_CTRL_INTERCEPT_MCOMMIT |
	    VMCB_CTRL_INTERCEPT_TLBSYNC;

	/* Intercept all I/O accesses. */
	memset(cpudata->iobm, 0xFF, IOBM_SIZE);
	vmcb->ctrl.iopm_base_pa = cpudata->iobm_pa;

	/* Allow direct access to certain MSRs. */
	memset(cpudata->msrbm, 0xFF, MSRBM_SIZE);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_STAR, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_LSTAR, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_CSTAR, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_SFMASK, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_KERNELGSBASE, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_CS, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_ESP, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_EIP, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_FSBASE, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_GSBASE, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_CR_PAT, true, true);
	vmm_svm_vcpu_msr_allow(cpudata->msrbm, MSR_TSC, true, false);
	vmcb->ctrl.msrpm_base_pa = cpudata->msrbm_pa;

	/* Generate ASID. */
	vmm_svm_asid_alloc(vcpu);

	/* Virtual TPR. */
	vmcb->ctrl.v = VMCB_CTRL_V_INTR_MASKING;

	/* Enable Nested Paging. */
	vmcb->ctrl.enable1 = VMCB_CTRL_ENABLE_NP;
	vmcb->ctrl.n_cr3 = os_vmspace_pdirpa(mach->vmspace);

	/* Init XSAVE header. */
	cpudata->gxsave.xstate_bv = vmm_svm_xcr0_mask;
	cpudata->gxsave.xcomp_bv = 0;

	/* The caller owns the initial architectural state. */
	vmm_svm_vcpu_setstate(vcpu, VMM_X64_STATE_ALL);
}

int
vmm_svm_vcpu_create(struct vmm_vcpu *vcpu)
{
	struct vmm_machine *mach = vcpu->machine;
	struct vmm_svm_cpudata *cpudata;
	struct vmm_svm_machdata *machdata;
	struct vmm_svm_interrupt_config interrupt_config;
	int error;

	/* Allocate the SVM cpudata. */
	cpudata = (struct vmm_svm_cpudata *)os_pagemem_zalloc(sizeof(*cpudata));
	if (cpudata == NULL)
		return ENOMEM;

	vcpu->backend = cpudata;
	cpudata->hcpu_last = -1;
	atomic_store_rel_int(&cpudata->running_cpu, -1);

	/* VMCB */
	error = os_contigpa_zalloc(&cpudata->vmcb_pa,
	    (vaddr_t *)&cpudata->vmcb, VMCB_NPAGES);
	if (error)
		goto error;

	/* I/O Bitmap */
	error = os_contigpa_zalloc(&cpudata->iobm_pa,
	    (vaddr_t *)&cpudata->iobm, IOBM_NPAGES);
	if (error)
		goto error;

	/* MSR Bitmap */
	error = os_contigpa_zalloc(&cpudata->msrbm_pa,
	    (vaddr_t *)&cpudata->msrbm, MSRBM_NPAGES);
	if (error)
		goto error;

	/* Init the VCPU info. */
	vmm_svm_vcpu_init(mach, vcpu);
	machdata = mach->backend_state;
	error = vmm_svm_interrupt_ops->vcpu_create(machdata->interrupt, vcpu,
	    &cpudata->interrupt, &interrupt_config);
	if (error != 0)
		goto error;
	if (interrupt_config.enabled) {
		cpudata->vmcb->ctrl.v |= VMCB_CTRL_V_INTR_MASKING |
		    VMCB_CTRL_V_AVIC_EN;
		cpudata->vmcb->ctrl.avic = interrupt_config.apic_base;
		cpudata->vmcb->ctrl.avic_abpp = interrupt_config.apic_backing_page;
		cpudata->vmcb->ctrl.avic_ltp = interrupt_config.logical_table;
		cpudata->vmcb->ctrl.avic_phys = interrupt_config.physical_table |
		    interrupt_config.physical_max_index;
		vmm_svm_vmcb_cache_flush(cpudata->vmcb,
		    VMCB_CTRL_VMCB_CLEAN_AVIC);
	}

	return 0;

error:
	vmm_svm_interrupt_ops->vcpu_destroy(cpudata->interrupt);
	if (cpudata->vmcb_pa) {
		os_contigpa_free(cpudata->vmcb_pa, (vaddr_t)cpudata->vmcb,
		    VMCB_NPAGES);
	}
	if (cpudata->iobm_pa) {
		os_contigpa_free(cpudata->iobm_pa, (vaddr_t)cpudata->iobm,
		    IOBM_NPAGES);
	}
	if (cpudata->msrbm_pa) {
		os_contigpa_free(cpudata->msrbm_pa, (vaddr_t)cpudata->msrbm,
		    MSRBM_NPAGES);
	}
	os_pagemem_free(cpudata, sizeof(*cpudata));
	return error;
}

void
vmm_svm_vcpu_destroy(struct vmm_vcpu *vcpu)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;

	vmm_svm_interrupt_ops->vcpu_destroy(cpudata->interrupt);
	vmm_svm_asid_free(vcpu);

	os_contigpa_free(cpudata->vmcb_pa, (vaddr_t)cpudata->vmcb,
	    VMCB_NPAGES);
	os_contigpa_free(cpudata->iobm_pa, (vaddr_t)cpudata->iobm,
	    IOBM_NPAGES);
	os_contigpa_free(cpudata->msrbm_pa, (vaddr_t)cpudata->msrbm,
	    MSRBM_NPAGES);

	os_pagemem_free(cpudata, sizeof(*cpudata));
}

/* -------------------------------------------------------------------------- */

int
vmm_svm_capability(struct vmm_x64_capability *capability)
{
	if (capability == NULL)
		return EINVAL;

	capability->xcr0_mask = vmm_svm_xcr0_mask;
	capability->mxcsr_mask = x86_fpu_mxcsr_mask;
	return 0;
}

int
vmm_svm_get_supported_cpuid(struct vmm_cpuid_entry *entries,
	size_t *entry_count)
{
	uint32_t leaf;
	size_t count;
	size_t i;

	if (entry_count == NULL)
		return EINVAL;
	count = vmm_svm_supported_cpuid_count();
	if (entries == NULL) {
		*entry_count = count;
		return 0;
	}
	if (*entry_count < count) {
		*entry_count = count;
		return E2BIG;
	}

	i = 0;
	for (leaf = 0; leaf <= vmm_svm_cpuid_max_basic; ++leaf) {
		entries[i].leaf = leaf;
		entries[i].subleaf = 0;
		entries[i].flags = leaf == 0x00000007 || leaf == 0x0000000D ?
		    VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF : 0;
		vmm_svm_supported_cpuid_entry(&entries[i]);
		++i;
		if (leaf == 0x0000000D) {
			entries[i].leaf = leaf;
			entries[i].subleaf = 1;
			entries[i].flags = VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF;
			vmm_svm_supported_cpuid_entry(&entries[i]);
			++i;
		}
	}
	entries[i].leaf = 0x40000000;
	entries[i].subleaf = 0;
	entries[i].flags = 0;
	vmm_svm_supported_cpuid_entry(&entries[i]);
	++i;
	for (leaf = 0x80000000; leaf <= vmm_svm_cpuid_max_extended; ++leaf) {
		entries[i].leaf = leaf;
		entries[i].subleaf = 0;
		entries[i].flags = 0;
		vmm_svm_supported_cpuid_entry(&entries[i]);
		++i;
	}
	KKASSERT(i == count);
	*entry_count = count;
	return 0;
}

int
vmm_svm_vcpu_set_cpuid(struct vmm_vcpu *vcpu,
	const struct vmm_cpuid_entry *entries, size_t entry_count)
{
	struct vmm_svm_cpudata *cpudata = vcpu->backend;
	size_t i, j;

	if (entry_count > SVM_NCPUID_ENTRIES)
		return ENOBUFS;
	for (i = 0; i < entry_count; ++i) {
		if ((entries[i].flags &
		    ~VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF) != 0)
			return EINVAL;
		for (j = 0; j < i; ++j) {
			if (entries[j].leaf != entries[i].leaf)
				continue;
			if ((entries[j].flags &
			    VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF) == 0 &&
			    (entries[i].flags &
			    VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF) == 0)
				return EINVAL;
			if ((entries[j].flags &
			    VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF) != 0 &&
			    (entries[i].flags &
			    VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF) != 0 &&
			    entries[j].subleaf == entries[i].subleaf)
				return EINVAL;
		}
	}
	if (entry_count != 0)
		bcopy(entries, cpudata->cpuid_entries,
		    entry_count * sizeof(*entries));
	cpudata->cpuid_entry_count = entry_count;
	return 0;
}

int
vmm_svm_machine_create(struct vmm_machine *mach)
{
	struct pmap *pmap = os_vmspace_pmap(mach->vmspace);
	struct vmm_svm_machdata *machdata;
	int error;

	/* Transform the caller-owned guest pmap for nested paging. */
	pmap_npt_transform(pmap, 0);

	machdata = os_mem_zalloc(sizeof(struct vmm_svm_machdata));
	if (machdata == NULL)
		return ENOMEM;
	mach->backend_state = machdata;
	error = vmm_svm_interrupt_ops->machine_create(mach,
	    &machdata->interrupt);
	if (error != 0) {
		mach->backend_state = NULL;
		os_mem_free(machdata, sizeof(*machdata));
		return error;
	}

	/* Start with an hTLB flush everywhere. */
	machdata->mach_htlb_gen = 1;
	return 0;
}

int
vmm_svm_machine_create_irqchip(struct vmm_machine *mach)
{
	struct vmm_svm_machdata *machdata = mach->backend_state;

	return vmm_svm_interrupt_ops->machine_enable(machdata->interrupt);
}

bool
vmm_svm_irqchip_available(void)
{
	return vmm_svm_interrupt_ops != NULL;
}

int
vmm_svm_irq_raise_msi(struct vmm_machine *mach, uint64_t address,
    uint32_t data)
{
	struct vmm_svm_machdata *machdata = mach->backend_state;

	return vmm_svm_interrupt_ops->irq_raise_msi(machdata->interrupt,
	    address, data);
}

int
vmm_svm_machine_raise_irq(struct vmm_machine *mach, uint32_t gsi)
{
	int error;

	error = vmm_svm_machine_set_irq(mach, gsi, true);
	if (error != 0)
		return error;
	return vmm_svm_machine_set_irq(mach, gsi, false);
}

int
vmm_svm_machine_set_irq(struct vmm_machine *mach, uint32_t gsi, bool level)
{
	struct vmm_svm_machdata *machdata = mach->backend_state;

	return vmm_svm_interrupt_ops->irq_set(machdata->interrupt, gsi, level);
}

void
vmm_svm_machine_destroy(struct vmm_machine *mach)
{
	struct vmm_svm_machdata *machdata = mach->backend_state;

	vmm_svm_interrupt_ops->machine_destroy(machdata->interrupt);
	os_mem_free(machdata, sizeof(*machdata));
}

/* -------------------------------------------------------------------------- */

bool
vmm_svm_ident(void)
{
	cpuid_desc_t descs;
	uint64_t msr;

	/* Must be AMD CPU. */
	x86_get_cpuid(0x00000000, &descs);
	if (memcmp(&descs.ebx, "Auth", 4) ||
	    memcmp(&descs.edx, "enti", 4) ||
	    memcmp(&descs.ecx, "cAMD", 4)) {
		return false;
	}

	/* Want leaf Fn8000_000A. */
	x86_get_cpuid(0x80000000, &descs);
	if (descs.eax < 0x8000000a) {
		os_printf("vmm: CPUID leaf not available\n");
		return false;
	}

	/* Want SVM support. */
	x86_get_cpuid(0x80000001, &descs);
	if (!(descs.ecx & CPUID_8_01_ECX_SVM)) {
		os_printf("vmm: SVM not supported\n");
		return false;
	}

	/* Want SVM revision 1. */
	x86_get_cpuid(0x8000000a, &descs);
	if (__SHIFTOUT(descs.eax, CPUID_8_0A_EAX_SvmRev) != 1) {
		os_printf("vmm: SVM revision not supported\n");
		return false;
	}

	/* Want Nested Paging. */
	if (!(descs.edx & CPUID_8_0A_EDX_NP)) {
		os_printf("vmm: SVM-NP not supported\n");
		return false;
	}

	/* Want nRIP. */
	if (!(descs.edx & CPUID_8_0A_EDX_NRIPS)) {
		os_printf("vmm: SVM-NRIPS not supported\n");
		return false;
	}

	vmm_svm_decode_assist = (descs.edx & CPUID_8_0A_EDX_DecodeAssists) != 0;
	if (!vmm_svm_decode_assist) {
		os_printf("vmm: DecodeAssists not available; "
		    "performance may be reduced\n");
	}

	msr = rdmsr(MSR_VM_CR);
	if ((msr & VM_CR_SVMED) && (msr & VM_CR_LOCK)) {
		os_printf("vmm: SVM disabled in BIOS\n");
		return false;
	}

	return true;
}

static void
vmm_svm_init_asid(uint32_t maxasid)
{
	size_t i, j, allocsz;

	os_mtx_init(&vmm_svm_asidlock);

	/* Arbitrarily limit. */
	maxasid = uimin(maxasid, 8192);

	vmm_svm_maxasid = maxasid;
	allocsz = roundup(maxasid, 8) / 8;
	vmm_svm_asidmap = os_mem_zalloc(allocsz);

	/* ASID 0 is reserved for the host. */
	vmm_svm_asidmap[0] |= __BIT(0);

	/* ASID n-1 is special, we share it. */
	i = (maxasid - 1) / 8;
	j = (maxasid - 1) % 8;
	vmm_svm_asidmap[i] |= __BIT(j);
}

static
OS_IPI_FUNC(vmm_svm_change_cpu)
{
	bool enable = arg != NULL;
	uint64_t msr;

	msr = rdmsr(MSR_VM_CR);
	if (msr & VM_CR_SVMED) {
		wrmsr(MSR_VM_CR, msr & ~VM_CR_SVMED);
	}

	if (!enable) {
		wrmsr(MSR_VM_HSAVE_PA, 0);
	}

	msr = rdmsr(MSR_EFER);
	if (enable) {
		msr |= EFER_SVME;
	} else {
		msr &= ~EFER_SVME;
	}
	wrmsr(MSR_EFER, msr);

	if (enable) {
		wrmsr(MSR_VM_HSAVE_PA, hsave[os_curcpu_number()].pa);
	}
}

int
vmm_svm_init(void)
{
	cpuid_desc_t descs;
	os_cpu_t *cpu;

	x86_get_cpuid(0x8000000a, &descs);

	/* The guest TLB flush command. */
	if (descs.edx & CPUID_8_0A_EDX_FlushByASID) {
		vmm_svm_ctrl_tlb_flush = VMCB_CTRL_TLB_CTRL_FLUSH_GUEST;
	} else {
		vmm_svm_ctrl_tlb_flush = VMCB_CTRL_TLB_CTRL_FLUSH_ALL;
	}

	/* Init the ASID. */
	vmm_svm_init_asid(descs.ebx);

	/* Init the XCR0 mask. */
	vmm_svm_xcr0_mask = SVM_XCR0_MASK_DEFAULT & x86_xsave_features;

	/* Init the max basic CPUID leaf. */
	x86_get_cpuid(0x00000000, &descs);
	vmm_svm_cpuid_max_basic = uimin(descs.eax, SVM_CPUID_MAX_BASIC);

	/* Init the max extended CPUID leaf. */
	x86_get_cpuid(0x80000000, &descs);
	vmm_svm_cpuid_max_extended = uimin(descs.eax, SVM_CPUID_MAX_EXTENDED);

	/* Init the global host state. */
	if (vmm_svm_xcr0_mask != 0) {
		vmm_svm_global_hstate.xcr0 = x86_get_xcr(0);
	}
	vmm_svm_global_hstate.star = rdmsr(MSR_STAR);
	vmm_svm_global_hstate.lstar = rdmsr(MSR_LSTAR);
	vmm_svm_global_hstate.cstar = rdmsr(MSR_CSTAR);
	vmm_svm_global_hstate.sfmask = rdmsr(MSR_SFMASK);

	memset(hsave, 0, sizeof(hsave));
	OS_CPU_FOREACH(cpu) {
		hsave[os_cpu_number(cpu)].pa = os_pa_zalloc();
	}

	os_ipi_broadcast(vmm_svm_change_cpu, (void *)true);
	return 0;
}

static void
vmm_svm_fini_asid(void)
{
	size_t allocsz;

	allocsz = roundup(vmm_svm_maxasid, 8) / 8;
	os_mem_free(vmm_svm_asidmap, allocsz);

	os_mtx_destroy(&vmm_svm_asidlock);
}

void
vmm_svm_fini(void)
{
	size_t i;

	os_ipi_broadcast(vmm_svm_change_cpu, (void *)false);

	for (i = 0; i < OS_MAXCPUS; i++) {
		if (hsave[i].pa != 0)
			os_pa_free(hsave[i].pa);
	}

	vmm_svm_fini_asid();
}
