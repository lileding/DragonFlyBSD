/* -------------------------------------------------------------------------- */

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD x86 CPUID and control-register definitions used by VMM SVM.
 *
 * Copyright (c) 2018-2021 Maxime Villard, m00nbsd.net
 *
 * Derived from the BSD-2-Clause NVMM x86 ABI definitions, retaining the
 * architecture spellings so the imported SVM backend remains reviewable.
 */
#ifndef VMM_SVM_X86DEFS_H
#define VMM_SVM_X86DEFS_H

#include <sys/types.h>
#include <sys/bitops.h>

#ifdef __x86_64__
#undef __BIT
#define __BIT(bit)		__BIT64(bit)
#undef __BITS
#define __BITS(high, low)	__BITS64(high, low)
#endif

/*
 * CPUID defines.
 */

/* Fn0000_0001:EBX */
#define CPUID_0_01_EBX_BRAND_INDEX	__BITS(7,0)
#define CPUID_0_01_EBX_CLFLUSH_SIZE	__BITS(15,8)
#define CPUID_0_01_EBX_HTT_CORES	__BITS(23,16)
#define CPUID_0_01_EBX_LOCAL_APIC_ID	__BITS(31,24)
/* Fn0000_0001:ECX */
#define CPUID_0_01_ECX_SSE3		__BIT(0)
#define CPUID_0_01_ECX_PCLMULQDQ	__BIT(1)
#define CPUID_0_01_ECX_DTES64		__BIT(2)
#define CPUID_0_01_ECX_MONITOR		__BIT(3)
#define CPUID_0_01_ECX_DS_CPL		__BIT(4)
#define CPUID_0_01_ECX_VMX		__BIT(5)
#define CPUID_0_01_ECX_SMX		__BIT(6)
#define CPUID_0_01_ECX_EIST		__BIT(7)
#define CPUID_0_01_ECX_TM2		__BIT(8)
#define CPUID_0_01_ECX_SSSE3		__BIT(9)
#define CPUID_0_01_ECX_CNXTID		__BIT(10)
#define CPUID_0_01_ECX_SDBG		__BIT(11)
#define CPUID_0_01_ECX_FMA		__BIT(12)
#define CPUID_0_01_ECX_CX16		__BIT(13)
#define CPUID_0_01_ECX_XTPR		__BIT(14)
#define CPUID_0_01_ECX_PDCM		__BIT(15)
#define CPUID_0_01_ECX_PCID		__BIT(17)
#define CPUID_0_01_ECX_DCA		__BIT(18)
#define CPUID_0_01_ECX_SSE41		__BIT(19)
#define CPUID_0_01_ECX_SSE42		__BIT(20)
#define CPUID_0_01_ECX_X2APIC		__BIT(21)
#define CPUID_0_01_ECX_MOVBE		__BIT(22)
#define CPUID_0_01_ECX_POPCNT		__BIT(23)
#define CPUID_0_01_ECX_TSC_DEADLINE	__BIT(24)
#define CPUID_0_01_ECX_AESNI		__BIT(25)
#define CPUID_0_01_ECX_XSAVE		__BIT(26)
#define CPUID_0_01_ECX_OSXSAVE		__BIT(27)
#define CPUID_0_01_ECX_AVX		__BIT(28)
#define CPUID_0_01_ECX_F16C		__BIT(29)
#define CPUID_0_01_ECX_RDRAND		__BIT(30)
#define CPUID_0_01_ECX_RAZ		__BIT(31)
/* Fn0000_0001:EDX */
#define CPUID_0_01_EDX_FPU		__BIT(0)
#define CPUID_0_01_EDX_VME		__BIT(1)
#define CPUID_0_01_EDX_DE		__BIT(2)
#define CPUID_0_01_EDX_PSE		__BIT(3)
#define CPUID_0_01_EDX_TSC		__BIT(4)
#define CPUID_0_01_EDX_MSR		__BIT(5)
#define CPUID_0_01_EDX_PAE		__BIT(6)
#define CPUID_0_01_EDX_MCE		__BIT(7)
#define CPUID_0_01_EDX_CX8		__BIT(8)
#define CPUID_0_01_EDX_APIC		__BIT(9)
#define CPUID_0_01_EDX_SEP		__BIT(11)
#define CPUID_0_01_EDX_MTRR		__BIT(12)
#define CPUID_0_01_EDX_PGE		__BIT(13)
#define CPUID_0_01_EDX_MCA		__BIT(14)
#define CPUID_0_01_EDX_CMOV		__BIT(15)
#define CPUID_0_01_EDX_PAT		__BIT(16)
#define CPUID_0_01_EDX_PSE36		__BIT(17)
#define CPUID_0_01_EDX_PSN		__BIT(18)
#define CPUID_0_01_EDX_CLFSH		__BIT(19)
#define CPUID_0_01_EDX_DS		__BIT(21)
#define CPUID_0_01_EDX_ACPI		__BIT(22)
#define CPUID_0_01_EDX_MMX		__BIT(23)
#define CPUID_0_01_EDX_FXSR		__BIT(24)
#define CPUID_0_01_EDX_SSE		__BIT(25)
#define CPUID_0_01_EDX_SSE2		__BIT(26)
#define CPUID_0_01_EDX_SS		__BIT(27)
#define CPUID_0_01_EDX_HTT		__BIT(28)
#define CPUID_0_01_EDX_TM		__BIT(29)
#define CPUID_0_01_EDX_PBE		__BIT(31)

/* Fn0000_0004:EAX (Intel Deterministic Cache Parameter Leaf) */
#define CPUID_0_04_EAX_CACHETYPE	__BITS(4, 0)
#define CPUID_0_04_EAX_CACHETYPE_NULL		0
#define CPUID_0_04_EAX_CACHETYPE_DATA		1
#define CPUID_0_04_EAX_CACHETYPE_INSN		2
#define CPUID_0_04_EAX_CACHETYPE_UNIFIED	3
#define CPUID_0_04_EAX_CACHELEVEL	__BITS(7, 5)
#define CPUID_0_04_EAX_SELFINITCL	__BIT(8)
#define CPUID_0_04_EAX_FULLASSOC	__BIT(9)
#define CPUID_0_04_EAX_SHARING		__BITS(25, 14)
#define CPUID_0_04_EAX_CORE_P_PKG	__BITS(31, 26)

/* [ECX=0] Fn0000_0007:EBX (Structured Extended Features) */
#define CPUID_0_07_EBX_FSGSBASE		__BIT(0)
#define CPUID_0_07_EBX_TSC_ADJUST	__BIT(1)
#define CPUID_0_07_EBX_SGX		__BIT(2)
#define CPUID_0_07_EBX_BMI1		__BIT(3)
#define CPUID_0_07_EBX_HLE		__BIT(4)
#define CPUID_0_07_EBX_AVX2		__BIT(5)
#define CPUID_0_07_EBX_FDPEXONLY	__BIT(6)
#define CPUID_0_07_EBX_SMEP		__BIT(7)
#define CPUID_0_07_EBX_BMI2		__BIT(8)
#define CPUID_0_07_EBX_ERMS		__BIT(9)
#define CPUID_0_07_EBX_INVPCID		__BIT(10)
#define CPUID_0_07_EBX_RTM		__BIT(11)
#define CPUID_0_07_EBX_QM		__BIT(12)
#define CPUID_0_07_EBX_FPUCSDS		__BIT(13)
#define CPUID_0_07_EBX_MPX		__BIT(14)
#define CPUID_0_07_EBX_PQE		__BIT(15)
#define CPUID_0_07_EBX_AVX512F		__BIT(16)
#define CPUID_0_07_EBX_AVX512DQ		__BIT(17)
#define CPUID_0_07_EBX_RDSEED		__BIT(18)
#define CPUID_0_07_EBX_ADX		__BIT(19)
#define CPUID_0_07_EBX_SMAP		__BIT(20)
#define CPUID_0_07_EBX_AVX512_IFMA	__BIT(21)
#define CPUID_0_07_EBX_CLFLUSHOPT	__BIT(23)
#define CPUID_0_07_EBX_CLWB		__BIT(24)
#define CPUID_0_07_EBX_PT		__BIT(25)
#define CPUID_0_07_EBX_AVX512PF		__BIT(26)
#define CPUID_0_07_EBX_AVX512ER		__BIT(27)
#define CPUID_0_07_EBX_AVX512CD		__BIT(28)
#define CPUID_0_07_EBX_SHA		__BIT(29)
#define CPUID_0_07_EBX_AVX512BW		__BIT(30)
#define CPUID_0_07_EBX_AVX512VL		__BIT(31)
/* [ECX=0] Fn0000_0007:ECX (Structured Extended Features) */
#define CPUID_0_07_ECX_PREFETCHWT1	__BIT(0)
#define CPUID_0_07_ECX_AVX512_VBMI	__BIT(1)
#define CPUID_0_07_ECX_UMIP		__BIT(2)
#define CPUID_0_07_ECX_PKU		__BIT(3)
#define CPUID_0_07_ECX_OSPKE		__BIT(4)
#define CPUID_0_07_ECX_WAITPKG		__BIT(5)
#define CPUID_0_07_ECX_AVX512_VBMI2	__BIT(6)
#define CPUID_0_07_ECX_CET_SS		__BIT(7)
#define CPUID_0_07_ECX_GFNI		__BIT(8)
#define CPUID_0_07_ECX_VAES		__BIT(9)
#define CPUID_0_07_ECX_VPCLMULQDQ	__BIT(10)
#define CPUID_0_07_ECX_AVX512_VNNI	__BIT(11)
#define CPUID_0_07_ECX_AVX512_BITALG	__BIT(12)
#define CPUID_0_07_ECX_AVX512_VPOPCNTDQ __BIT(14)
#define CPUID_0_07_ECX_LA57		__BIT(16)
#define CPUID_0_07_ECX_MAWAU		__BITS(21, 17)
#define CPUID_0_07_ECX_RDPID		__BIT(22)
#define CPUID_0_07_ECX_KL		__BIT(23)
#define CPUID_0_07_ECX_CLDEMOTE		__BIT(25)
#define CPUID_0_07_ECX_MOVDIRI		__BIT(27)
#define CPUID_0_07_ECX_MOVDIR64B	__BIT(28)
#define CPUID_0_07_ECX_SGXLC		__BIT(30)
#define CPUID_0_07_ECX_PKS		__BIT(31)
/* [ECX=0] Fn0000_0007:EDX (Structured Extended Features) */
#define CPUID_0_07_EDX_AVX512_4VNNIW	__BIT(2)
#define CPUID_0_07_EDX_AVX512_4FMAPS	__BIT(3)
#define CPUID_0_07_EDX_FSREP_MOV	__BIT(4)
#define CPUID_0_07_EDX_AVX512_VP2INTERSECT __BIT(8)
#define CPUID_0_07_EDX_SRBDS_CTRL	__BIT(9)
#define CPUID_0_07_EDX_MD_CLEAR		__BIT(10)
#define CPUID_0_07_EDX_TSX_FORCE_ABORT	__BIT(13)
#define CPUID_0_07_EDX_SERIALIZE	__BIT(14)
#define CPUID_0_07_EDX_HYBRID		__BIT(15)
#define CPUID_0_07_EDX_TSXLDTRK		__BIT(16)
#define CPUID_0_07_EDX_CET_IBT		__BIT(20)
#define CPUID_0_07_EDX_IBRS		__BIT(26)
#define CPUID_0_07_EDX_STIBP		__BIT(27)
#define CPUID_0_07_EDX_L1D_FLUSH	__BIT(28)
#define CPUID_0_07_EDX_ARCH_CAP		__BIT(29)
#define CPUID_0_07_EDX_CORE_CAP		__BIT(30)
#define CPUID_0_07_EDX_SSBD		__BIT(31)

/* Fn0000_000B:EAX (Extended Topology Enumeration) */
#define CPUID_0_0B_EAX_SHIFTNUM		__BITS(4, 0)
/* Fn0000_000B:ECX (Extended Topology Enumeration) */
#define CPUID_0_0B_ECX_LVLNUM		__BITS(7, 0)
#define CPUID_0_0B_ECX_LVLTYPE		__BITS(15, 8)
#define CPUID_0_0B_ECX_LVLTYPE_INVAL		0
#define CPUID_0_0B_ECX_LVLTYPE_SMT		1
#define CPUID_0_0B_ECX_LVLTYPE_CORE		2

/* [ECX=1] Fn0000_000D:EAX (Processor Extended State Enumeration) */
#define CPUID_0_0D_ECX1_EAX_XSAVEOPT	__BIT(0)
#define CPUID_0_0D_ECX1_EAX_XSAVEC	__BIT(1)
#define CPUID_0_0D_ECX1_EAX_XGETBV	__BIT(2)
#define CPUID_0_0D_ECX1_EAX_XSAVES	__BIT(3)

/* Fn8000_0001:ECX */
#define CPUID_8_01_ECX_LAHF		__BIT(0)
#define CPUID_8_01_ECX_CMPLEGACY	__BIT(1)
#define CPUID_8_01_ECX_SVM		__BIT(2)
#define CPUID_8_01_ECX_EAPIC		__BIT(3)
#define CPUID_8_01_ECX_ALTMOVCR8	__BIT(4)
#define CPUID_8_01_ECX_ABM		__BIT(5)
#define CPUID_8_01_ECX_SSE4A		__BIT(6)
#define CPUID_8_01_ECX_MISALIGNSSE	__BIT(7)
#define CPUID_8_01_ECX_3DNOWPF		__BIT(8)
#define CPUID_8_01_ECX_OSVW		__BIT(9)
#define CPUID_8_01_ECX_IBS		__BIT(10)
#define CPUID_8_01_ECX_XOP		__BIT(11)
#define CPUID_8_01_ECX_SKINIT		__BIT(12)
#define CPUID_8_01_ECX_WDT		__BIT(13)
#define CPUID_8_01_ECX_LWP		__BIT(15)
#define CPUID_8_01_ECX_FMA4		__BIT(16)
#define CPUID_8_01_ECX_TCE		__BIT(17)
#define CPUID_8_01_ECX_NODEID		__BIT(19)
#define CPUID_8_01_ECX_TBM		__BIT(21)
#define CPUID_8_01_ECX_TOPOEXT		__BIT(22)
#define CPUID_8_01_ECX_PCEC		__BIT(23)
#define CPUID_8_01_ECX_PCENB		__BIT(24)
#define CPUID_8_01_ECX_DBE		__BIT(26)
#define CPUID_8_01_ECX_PERFTSC		__BIT(27)
#define CPUID_8_01_ECX_PERFEXTLLC	__BIT(28)
#define CPUID_8_01_ECX_MWAITX		__BIT(29)
/* Fn8000_0001:EDX */
#define CPUID_8_01_EDX_FPU		__BIT(0)
#define CPUID_8_01_EDX_VME		__BIT(1)
#define CPUID_8_01_EDX_DE		__BIT(2)
#define CPUID_8_01_EDX_PSE		__BIT(3)
#define CPUID_8_01_EDX_TSC		__BIT(4)
#define CPUID_8_01_EDX_MSR		__BIT(5)
#define CPUID_8_01_EDX_PAE		__BIT(6)
#define CPUID_8_01_EDX_MCE		__BIT(7)
#define CPUID_8_01_EDX_CX8		__BIT(8)
#define CPUID_8_01_EDX_APIC		__BIT(9)
#define CPUID_8_01_EDX_SYSCALL		__BIT(11)
#define CPUID_8_01_EDX_MTRR		__BIT(12)
#define CPUID_8_01_EDX_PGE		__BIT(13)
#define CPUID_8_01_EDX_MCA		__BIT(14)
#define CPUID_8_01_EDX_CMOV		__BIT(15)
#define CPUID_8_01_EDX_PAT		__BIT(16)
#define CPUID_8_01_EDX_PSE36		__BIT(17)
#define CPUID_8_01_EDX_XD		__BIT(20)
#define CPUID_8_01_EDX_MMXEXT		__BIT(22)
#define CPUID_8_01_EDX_MMX		__BIT(23)
#define CPUID_8_01_EDX_FXSR		__BIT(24)
#define CPUID_8_01_EDX_FFXSR		__BIT(25)
#define CPUID_8_01_EDX_PAGE1GB		__BIT(26)
#define CPUID_8_01_EDX_RDTSCP		__BIT(27)
#define CPUID_8_01_EDX_LM		__BIT(29)
#define CPUID_8_01_EDX_3DNOWEXT		__BIT(30)
#define CPUID_8_01_EDX_3DNOW		__BIT(31)

/* Fn8000_0007:EDX (Advanced Power Management) */
#define CPUID_8_07_EDX_TS		__BIT(0)
#define CPUID_8_07_EDX_FID		__BIT(1)
#define CPUID_8_07_EDX_VID		__BIT(2)
#define CPUID_8_07_EDX_TTP		__BIT(3)
#define CPUID_8_07_EDX_TM		__BIT(4)
#define CPUID_8_07_EDX_100MHzSteps	__BIT(6)
#define CPUID_8_07_EDX_HwPstate		__BIT(7)
#define CPUID_8_07_EDX_TscInvariant	__BIT(8)
#define CPUID_8_07_EDX_CPB		__BIT(9)
#define CPUID_8_07_EDX_EffFreqRO	__BIT(10)
#define CPUID_8_07_EDX_ProcFeedbackIntf	__BIT(11)
#define CPUID_8_07_EDX_ProcPowerReport	__BIT(12)

/* Fn8000_0008:EBX */
#define CPUID_8_08_EBX_CLZERO		__BIT(0)
#define CPUID_8_08_EBX_InstRetCntMsr	__BIT(1)
#define CPUID_8_08_EBX_RstrFpErrPtrs	__BIT(2)
#define CPUID_8_08_EBX_INVLPGB		__BIT(3)
#define CPUID_8_08_EBX_RDPRU		__BIT(4)
#define CPUID_8_08_EBX_MCOMMIT		__BIT(8)
#define CPUID_8_08_EBX_WBNOINVD		__BIT(9)
#define CPUID_8_08_EBX_IBPB		__BIT(12)
#define CPUID_8_08_EBX_INT_WBINVD	__BIT(13)
#define CPUID_8_08_EBX_IBRS		__BIT(14)
#define CPUID_8_08_EBX_STIBP		__BIT(15)
#define CPUID_8_08_EBX_IBRS_ALWAYSON	__BIT(16)
#define CPUID_8_08_EBX_STIBP_ALWAYSON	__BIT(17)
#define CPUID_8_08_EBX_PREFER_IBRS	__BIT(18)
#define CPUID_8_08_EBX_EferLmsleUnsupp	__BIT(20)
#define CPUID_8_08_EBX_INVLPGBnestedPg	__BIT(21)
#define CPUID_8_08_EBX_SSBD		__BIT(24)
#define CPUID_8_08_EBX_VIRT_SSBD	__BIT(25)
#define CPUID_8_08_EBX_SSB_NO		__BIT(26)
/* Fn8000_0008:ECX */
#define CPUID_8_08_ECX_NC		__BITS(7,0)
#define CPUID_8_08_ECX_ApicIdSize	__BITS(15,12)
#define CPUID_8_08_ECX_PerfTscSize	__BITS(17,16)

/* Fn8000_000A:EAX (SVM features) */
#define CPUID_8_0A_EAX_SvmRev		__BITS(7,0)
/* Fn8000_000A:EDX (SVM features) */
#define CPUID_8_0A_EDX_NP		__BIT(0)
#define CPUID_8_0A_EDX_LbrVirt		__BIT(1)
#define CPUID_8_0A_EDX_SVML		__BIT(2)
#define CPUID_8_0A_EDX_NRIPS		__BIT(3)
#define CPUID_8_0A_EDX_TscRateMsr	__BIT(4)
#define CPUID_8_0A_EDX_VmcbClean	__BIT(5)
#define CPUID_8_0A_EDX_FlushByASID	__BIT(6)
#define CPUID_8_0A_EDX_DecodeAssists	__BIT(7)
#define CPUID_8_0A_EDX_PauseFilter	__BIT(10)
#define CPUID_8_0A_EDX_PFThreshold	__BIT(12)
#define CPUID_8_0A_EDX_AVIC		__BIT(13)
#define CPUID_8_0A_EDX_VMSAVEvirt	__BIT(15)
#define CPUID_8_0A_EDX_VGIF		__BIT(16)
#define CPUID_8_0A_EDX_GMET		__BIT(17)
#define CPUID_8_0A_EDX_SSSCheck		__BIT(19)
#define CPUID_8_0A_EDX_SpecCtrl		__BIT(20)
#define CPUID_8_0A_EDX_TlbiCtl		__BIT(24)

/* -------------------------------------------------------------------------- */

/*
 * Register defines.
 */

/* Bits in CR0 control register */
#define CR0_PE	__BIT(0)	/* Protected mode Enable */
#define CR0_MP	__BIT(1)	/* "Math" Present (NPX or NPX emulator) */
#define CR0_EM	__BIT(2)	/* EMulate non-NPX coproc. (trap ESC only) */
#define CR0_TS	__BIT(3)	/* Task Switched (if MP, trap ESC and WAIT) */
#define CR0_ET	__BIT(4)	/* Extension Type (387 (if set) vs 287) */
#define CR0_NE	__BIT(5)	/* Numeric Error enable (EX16 vs IRQ13) */
#define CR0_WP	__BIT(16)	/* Write Protect (honor page protect in all modes) */
#define CR0_AM	__BIT(18)	/* Alignment Mask (set to enable AC flag) */
#define CR0_NW	__BIT(29)	/* Not Write-through */
#define CR0_CD	__BIT(30)	/* Cache Disable */
#define CR0_PG	__BIT(31)	/* PaGing enable */

/* Bits in CR4 control register */
#define CR4_VME		__BIT(0)	/* Virtual 8086 mode extensions */
#define CR4_PVI		__BIT(1)	/* Protected-mode virtual interrupts */
#define CR4_TSD		__BIT(2)	/* Time stamp disable */
#define CR4_DE		__BIT(3)	/* Debugging extensions */
#define CR4_PSE		__BIT(4)	/* Page size extensions */
#define CR4_PAE		__BIT(5)	/* Physical address extension */
#define CR4_MCE		__BIT(6)	/* Machine check enable */
#define CR4_PGE		__BIT(7)	/* Page global enable */
#define CR4_PCE		__BIT(8)	/* Performance monitoring counter enable */
#define CR4_OSFXSR	__BIT(9)	/* Fast FPU save/restore used by OS */
#define CR4_OSXMMEXCPT	__BIT(10)	/* Enable SIMD/MMX2 to use except 16 */
#define CR4_UMIP	__BIT(11)	/* User Mode Instruction Prevention */
#define CR4_LA57	__BIT(12)	/* Enable 57-bit linear address */
#define CR4_VMXE	__BIT(13)	/* Enable VMX - Intel specific */
#define CR4_SMXE	__BIT(14)	/* Enable SMX - Intel specific */
#define CR4_FSGSBASE	__BIT(16)	/* Enable *FSBASE and *GSBASE instructions */
#define CR4_PCIDE	__BIT(17)	/* Enable Process Context IDentifiers */
#define CR4_OSXSAVE	__BIT(18)	/* Enable XSave (for AVX Instructions) */
#define CR4_SMEP	__BIT(20)	/* Supervisor-Mode Execution Prevent */
#define CR4_SMAP	__BIT(21)	/* Supervisor-Mode Access Prevent */
#define CR4_PKE		__BIT(22)	/* Protection Keys Enable for user pages */
#define CR4_CET		__BIT(23)	/* Enable CET */
#define CR4_PKS		__BIT(24)	/* Protection Keys Enable for kern pages */

/* Extended Control Register XCR0 */
#define XCR0_X87	__BIT(0)	/* x87 FPU/MMX state */
#define XCR0_SSE	__BIT(1)	/* SSE state */
#define XCR0_AVX	__BIT(2)	/* AVX state */

#define MSR_TSC			0x0010
#define MSR_SYSENTER_CS		0x0174
#define MSR_SYSENTER_ESP	0x0175
#define MSR_SYSENTER_EIP	0x0176
#define MSR_CR_PAT		0x0277		/* Page Attribute Table (PAT) */
#define MSR_STAR		0xC0000081	/* legacy mode SYSCALL target/cs/ss */
#define MSR_LSTAR		0xC0000082	/* long mode SYSCALL target rip */
#define MSR_CSTAR		0xC0000083	/* compat mode SYSCALL target rip */
#define MSR_SFMASK		0xC0000084	/* SYSCALL Flag Mask */
#define MSR_KERNELGSBASE	0xC0000102	/* Kernel GS Base Register */

#define MSR_EFER	0xC0000080	/* Extended Feature Enable Register */
#define		EFER_SCE	__BIT(0)	/* SYSCALL Enable (R/W) */
#define		EFER_LME	__BIT(8)	/* Long Mode Enable (R/W) */
#define		EFER_LMA	__BIT(10)	/* Long Mode Active (R) */
#define		EFER_NXE	__BIT(11)	/* PTE No-Execute Enable (R/W) */
#define		EFER_SVME	__BIT(12)	/* SVM Enable (R/W) */
#define		EFER_LMSLE	__BIT(13)	/* Long Mode Segment Limit Enable */
#define		EFER_FFXSR	__BIT(14)	/* Fast FXSAVE/FXRSTOR Enable */
#define		EFER_TCE	__BIT(15)	/* Translation Cache Extension */

#endif /* VMM_SVM_X86DEFS_H */
