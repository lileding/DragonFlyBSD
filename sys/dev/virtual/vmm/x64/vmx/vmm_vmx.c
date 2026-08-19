/*
 * Copyright (c) 2018-2026 Maxime Villard, m00nbsd.net
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

#include "../../vmm_backend.h"
#include "../../vmm_internal.h"
#include "../../vmm_machine.h"
#include "../../vmm_vcpu.h"
#include "../vmm_x64.h"
#include "vmm_vmx.h"
#include "vmm_vmx_apicv.h"
#include "vmm_vmx_os.h"
#include "vmm_vmx_x86defs.h"

int vmm_vmx_vmlaunch(uint64_t *gprs);
int vmm_vmx_vmresume(uint64_t *gprs);
void vmm_vmx_resume_rip(void);

struct ept_desc {
	uint64_t eptp;
	uint64_t mbz;
} __packed;

struct vpid_desc {
	uint64_t vpid;
	uint64_t addr;
} __packed;

static inline void
vmm_vmx_vmxon(paddr_t *pa)
{
	__asm volatile (
		"vmxon		%[pa];"
		"jz		vmm_vmx_insn_failvalid;"
		"jc		vmm_vmx_insn_failinvalid;"
		:
		: [pa] "m" (*pa)
		: "memory", "cc"
	);
}

static inline void
vmm_vmx_vmxoff(void)
{
	__asm volatile (
		"vmxoff;"
		"jz		vmm_vmx_insn_failvalid;"
		"jc		vmm_vmx_insn_failinvalid;"
		:
		:
		: "memory", "cc"
	);
}

static inline void
vmm_vmx_invept(uint64_t op, struct ept_desc *desc)
{
	__asm volatile (
		"invept		%[desc],%[op];"
		"jz		vmm_vmx_insn_failvalid;"
		"jc		vmm_vmx_insn_failinvalid;"
		:
		: [desc] "m" (*desc), [op] "r" (op)
		: "memory", "cc"
	);
}

static inline void
vmm_vmx_invvpid(uint64_t op, struct vpid_desc *desc)
{
	__asm volatile (
		"invvpid	%[desc],%[op];"
		"jz		vmm_vmx_insn_failvalid;"
		"jc		vmm_vmx_insn_failinvalid;"
		:
		: [desc] "m" (*desc), [op] "r" (op)
		: "memory", "cc"
	);
}

static inline uint64_t
vmm_vmx_vmread(uint64_t field)
{
	uint64_t value;

	__asm volatile (
		"vmread		%[field],%[value];"
		"jz		vmm_vmx_insn_failvalid;"
		"jc		vmm_vmx_insn_failinvalid;"
		: [value] "=r" (value)
		: [field] "r" (field)
		: "cc"
	);

	return value;
}

static inline void
vmm_vmx_vmwrite(uint64_t field, uint64_t value)
{
	__asm volatile (
		"vmwrite	%[value],%[field];"
		"jz		vmm_vmx_insn_failvalid;"
		"jc		vmm_vmx_insn_failinvalid;"
		:
		: [field] "r" (field), [value] "r" (value)
		: "cc"
	);
}

static inline paddr_t __diagused
vmm_vmx_vmptrst(void)
{
	paddr_t pa;

	__asm volatile (
		"vmptrst	%[pa];"
		:
		: [pa] "m" (*(paddr_t *)&pa)
		: "memory"
	);

	return pa;
}

static inline void
vmm_vmx_vmptrld(paddr_t *pa)
{
	__asm volatile (
		"vmptrld	%[pa];"
		"jz		vmm_vmx_insn_failvalid;"
		"jc		vmm_vmx_insn_failinvalid;"
		:
		: [pa] "m" (*pa)
		: "memory", "cc"
	);
}

static inline void
vmm_vmx_vmclear(paddr_t *pa)
{
	__asm volatile (
		"vmclear	%[pa];"
		"jz		vmm_vmx_insn_failvalid;"
		"jc		vmm_vmx_insn_failinvalid;"
		:
		: [pa] "m" (*pa)
		: "memory", "cc"
	);
}

static inline void
vmm_vmx_cli(void)
{
	__asm volatile ("cli" ::: "memory");
}

static inline void
vmm_vmx_sti(void)
{
	__asm volatile ("sti" ::: "memory");
}

#define	MSR_IA32_PLATFORM_ID		0x0017

#define MSR_IA32_FEATURE_CONTROL	0x003A
#define		IA32_FEATURE_CONTROL_LOCK	__BIT(0)
#define		IA32_FEATURE_CONTROL_IN_SMX	__BIT(1)
#define		IA32_FEATURE_CONTROL_OUT_SMX	__BIT(2)

#define	MSR_IA32_BIOS_SIGN_ID		0x008B

#define MSR_IA32_ARCH_CAPABILITIES	0x010A
#define		IA32_ARCH_RDCL_NO		__BIT(0)
#define		IA32_ARCH_IBRS_ALL		__BIT(1)
#define		IA32_ARCH_RSBA			__BIT(2)
#define		IA32_ARCH_SKIP_L1DFL_VMENTRY	__BIT(3)
#define		IA32_ARCH_SSB_NO		__BIT(4)
#define		IA32_ARCH_MDS_NO		__BIT(5)
#define		IA32_ARCH_IF_PSCHANGE_MC_NO	__BIT(6)
#define		IA32_ARCH_TSX_CTRL		__BIT(7)
#define		IA32_ARCH_TAA_NO		__BIT(8)
#define		IA32_ARCH_MCU_CONTROL		__BIT(9)
#define		IA32_ARCH_MISC_PACKAGE_CTLS	__BIT(10)
#define		IA32_ARCH_ENERGY_FILTERING_CTL	__BIT(11)
#define		IA32_ARCH_DOITM			__BIT(12)
#define		IA32_ARCH_SBDR_SSDP_NO		__BIT(13)
#define		IA32_ARCH_FBSDP_NO		__BIT(14)
#define		IA32_ARCH_PSDP_NO		__BIT(15)
#define		IA32_ARCH_MCU_ENUMERATION	__BIT(16)
#define		IA32_ARCH_FB_CLEAR		__BIT(17)
#define		IA32_ARCH_FB_CLEAR_CTRL		__BIT(18)
#define		IA32_ARCH_RRSBA			__BIT(19)
#define		IA32_ARCH_BHI_NO		__BIT(20)
#define		IA32_ARCH_XAPIC_DISABLE_STATUS	__BIT(21)
#define		IA32_ARCH_MCU_EXTENDED_SERVICE	__BIT(22)
#define		IA32_ARCH_OVERCLOCKING_STATUS	__BIT(23)
#define		IA32_ARCH_PBRSB_NO		__BIT(24)
#define		IA32_ARCH_GDS_CTRL		__BIT(25)
#define		IA32_ARCH_GDS_NO		__BIT(26)
#define		IA32_ARCH_RFDS_NO		__BIT(27)
#define		IA32_ARCH_RFDS_CLEAR		__BIT(28)
#define		IA32_ARCH_IGN_UMONITOR_SUPPORT	__BIT(29)
#define		IA32_ARCH_MON_UMON_MITG_SUPPORT	__BIT(30)
#define		IA32_ARCH_PBOPT_SUPPORT		__BIT(32)

#define MSR_IA32_FLUSH_CMD		0x010B
#define		IA32_FLUSH_CMD_L1D_FLUSH	__BIT(0)

#define MSR_IA32_MISC_ENABLE		0x01A0
#define		IA32_MISC_PERFMON_EN		__BIT(7)
#define		IA32_MISC_BTS_UNAVAIL		__BIT(11)
#define		IA32_MISC_PEBS_UNAVAIL		__BIT(12)
#define		IA32_MISC_EISST_EN		__BIT(16)
#define		IA32_MISC_MWAIT_EN		__BIT(18)

#define MSR_IA32_VMX_BASIC		0x0480
#define		IA32_VMX_BASIC_IDENT		__BITS(30,0)
#define		IA32_VMX_BASIC_DATA_SIZE	__BITS(44,32)
#define		IA32_VMX_BASIC_MEM_WIDTH	__BIT(48)
#define		IA32_VMX_BASIC_DUAL		__BIT(49)
#define		IA32_VMX_BASIC_MEM_TYPE		__BITS(53,50)
#define			MEM_TYPE_UC		0
#define			MEM_TYPE_WB		6
#define		IA32_VMX_BASIC_IO_REPORT	__BIT(54)
#define		IA32_VMX_BASIC_TRUE_CTLS	__BIT(55)

#define MSR_IA32_VMX_PINBASED_CTLS		0x0481
#define MSR_IA32_VMX_PROCBASED_CTLS		0x0482
#define MSR_IA32_VMX_EXIT_CTLS			0x0483
#define MSR_IA32_VMX_ENTRY_CTLS			0x0484
#define MSR_IA32_VMX_PROCBASED_CTLS2		0x048B

#define MSR_IA32_VMX_TRUE_PINBASED_CTLS		0x048D
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS	0x048E
#define MSR_IA32_VMX_TRUE_EXIT_CTLS		0x048F
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS		0x0490

#define MSR_IA32_VMX_CR0_FIXED0			0x0486
#define MSR_IA32_VMX_CR0_FIXED1			0x0487
#define MSR_IA32_VMX_CR4_FIXED0			0x0488
#define MSR_IA32_VMX_CR4_FIXED1			0x0489

#define MSR_IA32_VMX_EPT_VPID_CAP	0x048C
#define		IA32_VMX_EPT_VPID_XO			__BIT(0)
#define		IA32_VMX_EPT_VPID_WALKLENGTH_4		__BIT(6)
#define		IA32_VMX_EPT_VPID_UC			__BIT(8)
#define		IA32_VMX_EPT_VPID_WB			__BIT(14)
#define		IA32_VMX_EPT_VPID_2MB			__BIT(16)
#define		IA32_VMX_EPT_VPID_1GB			__BIT(17)
#define		IA32_VMX_EPT_VPID_INVEPT		__BIT(20)
#define		IA32_VMX_EPT_VPID_FLAGS_AD		__BIT(21)
#define		IA32_VMX_EPT_VPID_ADVANCED_VMEXIT_INFO	__BIT(22)
#define		IA32_VMX_EPT_VPID_SHSTK			__BIT(23)
#define		IA32_VMX_EPT_VPID_INVEPT_CONTEXT	__BIT(25)
#define		IA32_VMX_EPT_VPID_INVEPT_ALL		__BIT(26)
#define		IA32_VMX_EPT_VPID_INVVPID		__BIT(32)
#define		IA32_VMX_EPT_VPID_INVVPID_ADDR		__BIT(40)
#define		IA32_VMX_EPT_VPID_INVVPID_CONTEXT	__BIT(41)
#define		IA32_VMX_EPT_VPID_INVVPID_ALL		__BIT(42)
#define		IA32_VMX_EPT_VPID_INVVPID_CONTEXT_NOG	__BIT(43)

/* -------------------------------------------------------------------------- */

/* 16-bit control fields */
#define VMCS_VPID				0x00000000
#define VMCS_PIR_VECTOR				0x00000002
#define VMCS_EPTP_INDEX				0x00000004
/* 16-bit guest-state fields */
#define VMCS_GUEST_ES_SELECTOR			0x00000800
#define VMCS_GUEST_CS_SELECTOR			0x00000802
#define VMCS_GUEST_SS_SELECTOR			0x00000804
#define VMCS_GUEST_DS_SELECTOR			0x00000806
#define VMCS_GUEST_FS_SELECTOR			0x00000808
#define VMCS_GUEST_GS_SELECTOR			0x0000080A
#define VMCS_GUEST_LDTR_SELECTOR		0x0000080C
#define VMCS_GUEST_TR_SELECTOR			0x0000080E
#define VMCS_GUEST_INTR_STATUS			0x00000810
#define VMCS_PML_INDEX				0x00000812
/* 16-bit host-state fields */
#define VMCS_HOST_ES_SELECTOR			0x00000C00
#define VMCS_HOST_CS_SELECTOR			0x00000C02
#define VMCS_HOST_SS_SELECTOR			0x00000C04
#define VMCS_HOST_DS_SELECTOR			0x00000C06
#define VMCS_HOST_FS_SELECTOR			0x00000C08
#define VMCS_HOST_GS_SELECTOR			0x00000C0A
#define VMCS_HOST_TR_SELECTOR			0x00000C0C
/* 64-bit control fields */
#define VMCS_IO_BITMAP_A			0x00002000
#define VMCS_IO_BITMAP_B			0x00002002
#define VMCS_MSR_BITMAP				0x00002004
#define VMCS_EXIT_MSR_STORE_ADDRESS		0x00002006
#define VMCS_EXIT_MSR_LOAD_ADDRESS		0x00002008
#define VMCS_ENTRY_MSR_LOAD_ADDRESS		0x0000200A
#define VMCS_EXECUTIVE_VMCS			0x0000200C
#define VMCS_PML_ADDRESS			0x0000200E
#define VMCS_TSC_OFFSET				0x00002010
#define VMCS_VIRTUAL_APIC			0x00002012
#define VMCS_APIC_ACCESS			0x00002014
#define VMCS_PIR_DESC				0x00002016
#define VMCS_VM_CONTROL				0x00002018
#define VMCS_EPTP				0x0000201A
#define		EPTP_TYPE			__BITS(2,0)
#define			EPTP_TYPE_UC		0
#define			EPTP_TYPE_WB		6
#define		EPTP_WALKLEN			__BITS(5,3)
#define		EPTP_FLAGS_AD			__BIT(6)
#define		EPTP_SSS			__BIT(7)
#define		EPTP_PHYSADDR			__BITS(63,12)
#define VMCS_EOI_EXIT0				0x0000201C
#define VMCS_EOI_EXIT1				0x0000201E
#define VMCS_EOI_EXIT2				0x00002020
#define VMCS_EOI_EXIT3				0x00002022
#define VMCS_EPTP_LIST				0x00002024
#define VMCS_VMREAD_BITMAP			0x00002026
#define VMCS_VMWRITE_BITMAP			0x00002028
#define VMCS_VIRTUAL_EXCEPTION			0x0000202A
#define VMCS_XSS_EXIT_BITMAP			0x0000202C
#define VMCS_ENCLS_EXIT_BITMAP			0x0000202E
#define VMCS_SUBPAGE_PERM_TABLE_PTR		0x00002030
#define VMCS_TSC_MULTIPLIER			0x00002032
#define VMCS_ENCLV_EXIT_BITMAP			0x00002036
/* 64-bit read-only fields */
#define VMCS_GUEST_PHYSICAL_ADDRESS		0x00002400
/* 64-bit guest-state fields */
#define VMCS_LINK_POINTER			0x00002800
#define VMCS_GUEST_IA32_DEBUGCTL		0x00002802
#define VMCS_GUEST_IA32_PAT			0x00002804
#define VMCS_GUEST_IA32_EFER			0x00002806
#define VMCS_GUEST_IA32_PERF_GLOBAL_CTRL	0x00002808
#define VMCS_GUEST_PDPTE0			0x0000280A
#define VMCS_GUEST_PDPTE1			0x0000280C
#define VMCS_GUEST_PDPTE2			0x0000280E
#define VMCS_GUEST_PDPTE3			0x00002810
#define VMCS_GUEST_BNDCFGS			0x00002812
#define VMCS_GUEST_RTIT_CTL			0x00002814
#define VMCS_GUEST_PKRS				0x00002818
/* 64-bit host-state fields */
#define VMCS_HOST_IA32_PAT			0x00002C00
#define VMCS_HOST_IA32_EFER			0x00002C02
#define VMCS_HOST_IA32_PERF_GLOBAL_CTRL		0x00002C04
#define VMCS_HOST_IA32_PKRS			0x00002C06
/* 32-bit control fields */
#define VMCS_PINBASED_CTLS			0x00004000
#define		PIN_CTLS_INT_EXITING		__BIT(0)
#define		PIN_CTLS_NMI_EXITING		__BIT(3)
#define		PIN_CTLS_VIRTUAL_NMIS		__BIT(5)
#define		PIN_CTLS_ACTIVATE_PREEMPT_TIMER	__BIT(6)
#define		PIN_CTLS_PROCESS_POSTED_INTS	__BIT(7)
#define VMCS_PROCBASED_CTLS			0x00004002
#define		PROC_CTLS_INT_WINDOW_EXITING	__BIT(2)
#define		PROC_CTLS_USE_TSC_OFFSETTING	__BIT(3)
#define		PROC_CTLS_HLT_EXITING		__BIT(7)
#define		PROC_CTLS_INVLPG_EXITING	__BIT(9)
#define		PROC_CTLS_MWAIT_EXITING		__BIT(10)
#define		PROC_CTLS_RDPMC_EXITING		__BIT(11)
#define		PROC_CTLS_RDTSC_EXITING		__BIT(12)
#define		PROC_CTLS_RCR3_EXITING		__BIT(15)
#define		PROC_CTLS_LCR3_EXITING		__BIT(16)
#define		PROC_CTLS_RCR8_EXITING		__BIT(19)
#define		PROC_CTLS_LCR8_EXITING		__BIT(20)
#define		PROC_CTLS_USE_TPR_SHADOW	__BIT(21)
#define		PROC_CTLS_NMI_WINDOW_EXITING	__BIT(22)
#define		PROC_CTLS_DR_EXITING		__BIT(23)
#define		PROC_CTLS_UNCOND_IO_EXITING	__BIT(24)
#define		PROC_CTLS_USE_IO_BITMAPS	__BIT(25)
#define		PROC_CTLS_MONITOR_TRAP_FLAG	__BIT(27)
#define		PROC_CTLS_USE_MSR_BITMAPS	__BIT(28)
#define		PROC_CTLS_MONITOR_EXITING	__BIT(29)
#define		PROC_CTLS_PAUSE_EXITING		__BIT(30)
#define		PROC_CTLS_ACTIVATE_CTLS2	__BIT(31)
#define VMCS_EXCEPTION_BITMAP			0x00004004
#define VMCS_PF_ERROR_MASK			0x00004006
#define VMCS_PF_ERROR_MATCH			0x00004008
#define VMCS_CR3_TARGET_COUNT			0x0000400A
#define VMCS_EXIT_CTLS				0x0000400C
#define		EXIT_CTLS_SAVE_DEBUG_CONTROLS	__BIT(2)
#define		EXIT_CTLS_HOST_LONG_MODE	__BIT(9)
#define		EXIT_CTLS_LOAD_PERFGLOBALCTRL	__BIT(12)
#define		EXIT_CTLS_ACK_INTERRUPT		__BIT(15)
#define		EXIT_CTLS_SAVE_PAT		__BIT(18)
#define		EXIT_CTLS_LOAD_PAT		__BIT(19)
#define		EXIT_CTLS_SAVE_EFER		__BIT(20)
#define		EXIT_CTLS_LOAD_EFER		__BIT(21)
#define		EXIT_CTLS_SAVE_PREEMPT_TIMER	__BIT(22)
#define		EXIT_CTLS_CLEAR_BNDCFGS		__BIT(23)
#define		EXIT_CTLS_CONCEAL_PT		__BIT(24)
#define		EXIT_CTLS_CLEAR_RTIT_CTL	__BIT(25)
#define		EXIT_CTLS_LOAD_CET		__BIT(28)
#define		EXIT_CTLS_LOAD_PKRS		__BIT(29)
#define VMCS_EXIT_MSR_STORE_COUNT		0x0000400E
#define VMCS_EXIT_MSR_LOAD_COUNT		0x00004010
#define VMCS_ENTRY_CTLS				0x00004012
#define		ENTRY_CTLS_LOAD_DEBUG_CONTROLS	__BIT(2)
#define		ENTRY_CTLS_LONG_MODE		__BIT(9)
#define		ENTRY_CTLS_SMM			__BIT(10)
#define		ENTRY_CTLS_DISABLE_DUAL		__BIT(11)
#define		ENTRY_CTLS_LOAD_PERFGLOBALCTRL	__BIT(13)
#define		ENTRY_CTLS_LOAD_PAT		__BIT(14)
#define		ENTRY_CTLS_LOAD_EFER		__BIT(15)
#define		ENTRY_CTLS_LOAD_BNDCFGS		__BIT(16)
#define		ENTRY_CTLS_CONCEAL_PT		__BIT(17)
#define		ENTRY_CTLS_LOAD_RTIT_CTL	__BIT(18)
#define		ENTRY_CTLS_LOAD_CET		__BIT(20)
#define		ENTRY_CTLS_LOAD_PKRS		__BIT(22)
#define VMCS_ENTRY_MSR_LOAD_COUNT		0x00004014
#define VMCS_ENTRY_INTR_INFO			0x00004016
#define		INTR_INFO_VECTOR		__BITS(7,0)
#define		INTR_INFO_TYPE			__BITS(10,8)
#define			INTR_TYPE_EXT_INT	0
#define			INTR_TYPE_NMI		2
#define			INTR_TYPE_HW_EXC	3
#define			INTR_TYPE_SW_INT	4
#define			INTR_TYPE_PRIV_SW_EXC	5
#define			INTR_TYPE_SW_EXC	6
#define			INTR_TYPE_OTHER		7
#define		INTR_INFO_ERROR			__BIT(11)
#define		INTR_INFO_VALID			__BIT(31)
#define VMCS_ENTRY_EXCEPTION_ERROR		0x00004018
#define VMCS_ENTRY_INSTRUCTION_LENGTH		0x0000401A
#define VMCS_TPR_THRESHOLD			0x0000401C
#define VMCS_PROCBASED_CTLS2			0x0000401E
#define VMCS_GUEST_INTR_STATUS			0x00000810
#define		PROC_CTLS2_VIRT_APIC_ACCESSES	__BIT(0)
#define		PROC_CTLS2_ENABLE_EPT		__BIT(1)
#define		PROC_CTLS2_DESC_TABLE_EXITING	__BIT(2)
#define		PROC_CTLS2_ENABLE_RDTSCP	__BIT(3)
#define		PROC_CTLS2_VIRT_X2APIC		__BIT(4)
#define		PROC_CTLS2_ENABLE_VPID		__BIT(5)
#define		PROC_CTLS2_WBINVD_EXITING	__BIT(6)
#define		PROC_CTLS2_UNRESTRICTED_GUEST	__BIT(7)
#define		PROC_CTLS2_APIC_REG_VIRT	__BIT(8)
#define		PROC_CTLS2_VIRT_INT_DELIVERY	__BIT(9)
#define		PROC_CTLS2_PAUSE_LOOP_EXITING	__BIT(10)
#define		PROC_CTLS2_RDRAND_EXITING	__BIT(11)
#define		PROC_CTLS2_INVPCID_ENABLE	__BIT(12)
#define		PROC_CTLS2_VMFUNC_ENABLE	__BIT(13)
#define		PROC_CTLS2_VMCS_SHADOWING	__BIT(14)
#define		PROC_CTLS2_ENCLS_EXITING	__BIT(15)
#define		PROC_CTLS2_RDSEED_EXITING	__BIT(16)
#define		PROC_CTLS2_PML_ENABLE		__BIT(17)
#define		PROC_CTLS2_EPT_VIOLATION	__BIT(18)
#define		PROC_CTLS2_CONCEAL_VMX_FROM_PT	__BIT(19)
#define		PROC_CTLS2_XSAVES_ENABLE	__BIT(20)
#define		PROC_CTLS2_MODE_BASED_EXEC_EPT	__BIT(22)
#define		PROC_CTLS2_SUBPAGE_PERMISSIONS	__BIT(23)
#define		PROC_CTLS2_PT_USES_GPA		__BIT(24)
#define		PROC_CTLS2_USE_TSC_SCALING	__BIT(25)
#define		PROC_CTLS2_WAIT_PAUSE_ENABLE	__BIT(26)
#define		PROC_CTLS2_ENCLV_EXITING	__BIT(28)
#define VMCS_PLE_GAP				0x00004020
#define VMCS_PLE_WINDOW				0x00004022
/* 32-bit read-only data fields */
#define VMCS_INSTRUCTION_ERROR			0x00004400
#define VMCS_EXIT_REASON			0x00004402
#define VMCS_EXIT_INTR_INFO			0x00004404
#define VMCS_EXIT_INTR_ERRCODE			0x00004406
#define VMCS_IDT_VECTORING_INFO			0x00004408
#define VMCS_IDT_VECTORING_ERROR		0x0000440A
#define VMCS_EXIT_INSTRUCTION_LENGTH		0x0000440C
#define VMCS_EXIT_INSTRUCTION_INFO		0x0000440E
/* 32-bit guest-state fields */
#define VMCS_GUEST_ES_LIMIT			0x00004800
#define VMCS_GUEST_CS_LIMIT			0x00004802
#define VMCS_GUEST_SS_LIMIT			0x00004804
#define VMCS_GUEST_DS_LIMIT			0x00004806
#define VMCS_GUEST_FS_LIMIT			0x00004808
#define VMCS_GUEST_GS_LIMIT			0x0000480A
#define VMCS_GUEST_LDTR_LIMIT			0x0000480C
#define VMCS_GUEST_TR_LIMIT			0x0000480E
#define VMCS_GUEST_GDTR_LIMIT			0x00004810
#define VMCS_GUEST_IDTR_LIMIT			0x00004812
#define VMCS_GUEST_ES_ACCESS_RIGHTS		0x00004814
#define VMCS_GUEST_CS_ACCESS_RIGHTS		0x00004816
#define VMCS_GUEST_SS_ACCESS_RIGHTS		0x00004818
#define VMCS_GUEST_DS_ACCESS_RIGHTS		0x0000481A
#define VMCS_GUEST_FS_ACCESS_RIGHTS		0x0000481C
#define VMCS_GUEST_GS_ACCESS_RIGHTS		0x0000481E
#define VMCS_GUEST_LDTR_ACCESS_RIGHTS		0x00004820
#define VMCS_GUEST_TR_ACCESS_RIGHTS		0x00004822
#define VMCS_GUEST_INTERRUPTIBILITY		0x00004824
#define		INT_STATE_STI			__BIT(0)
#define		INT_STATE_MOVSS			__BIT(1)
#define		INT_STATE_SMI			__BIT(2)
#define		INT_STATE_NMI			__BIT(3)
#define		INT_STATE_ENCLAVE		__BIT(4)
#define VMCS_GUEST_ACTIVITY			0x00004826
#define VMCS_GUEST_SMBASE			0x00004828
#define VMCS_GUEST_IA32_SYSENTER_CS		0x0000482A
#define VMCS_PREEMPTION_TIMER_VALUE		0x0000482E
/* 32-bit host state fields */
#define VMCS_HOST_IA32_SYSENTER_CS		0x00004C00
/* Natural-Width control fields */
#define VMCS_CR0_MASK				0x00006000
#define VMCS_CR4_MASK				0x00006002
#define VMCS_CR0_SHADOW				0x00006004
#define VMCS_CR4_SHADOW				0x00006006
#define VMCS_CR3_TARGET0			0x00006008
#define VMCS_CR3_TARGET1			0x0000600A
#define VMCS_CR3_TARGET2			0x0000600C
#define VMCS_CR3_TARGET3			0x0000600E
/* Natural-Width read-only fields */
#define VMCS_EXIT_QUALIFICATION			0x00006400
#define VMCS_IO_RCX				0x00006402
#define VMCS_IO_RSI				0x00006404
#define VMCS_IO_RDI				0x00006406
#define VMCS_IO_RIP				0x00006408
#define VMCS_GUEST_LINEAR_ADDRESS		0x0000640A
/* Natural-Width guest-state fields */
#define VMCS_GUEST_CR0				0x00006800
#define VMCS_GUEST_CR3				0x00006802
#define VMCS_GUEST_CR4				0x00006804
#define VMCS_GUEST_ES_BASE			0x00006806
#define VMCS_GUEST_CS_BASE			0x00006808
#define VMCS_GUEST_SS_BASE			0x0000680A
#define VMCS_GUEST_DS_BASE			0x0000680C
#define VMCS_GUEST_FS_BASE			0x0000680E
#define VMCS_GUEST_GS_BASE			0x00006810
#define VMCS_GUEST_LDTR_BASE			0x00006812
#define VMCS_GUEST_TR_BASE			0x00006814
#define VMCS_GUEST_GDTR_BASE			0x00006816
#define VMCS_GUEST_IDTR_BASE			0x00006818
#define VMCS_GUEST_DR7				0x0000681A
#define VMCS_GUEST_RSP				0x0000681C
#define VMCS_GUEST_RIP				0x0000681E
#define VMCS_GUEST_RFLAGS			0x00006820
#define VMCS_GUEST_PENDING_DBG_EXCEPTIONS	0x00006822
#define VMCS_GUEST_IA32_SYSENTER_ESP		0x00006824
#define VMCS_GUEST_IA32_SYSENTER_EIP		0x00006826
#define VMCS_GUEST_IA32_S_CET			0x00006828
#define VMCS_GUEST_SSP				0x0000682A
#define VMCS_GUEST_IA32_INTR_SSP_TABLE		0x0000682C
/* Natural-Width host-state fields */
#define VMCS_HOST_CR0				0x00006C00
#define VMCS_HOST_CR3				0x00006C02
#define VMCS_HOST_CR4				0x00006C04
#define VMCS_HOST_FS_BASE			0x00006C06
#define VMCS_HOST_GS_BASE			0x00006C08
#define VMCS_HOST_TR_BASE			0x00006C0A
#define VMCS_HOST_GDTR_BASE			0x00006C0C
#define VMCS_HOST_IDTR_BASE			0x00006C0E
#define VMCS_HOST_IA32_SYSENTER_ESP		0x00006C10
#define VMCS_HOST_IA32_SYSENTER_EIP		0x00006C12
#define VMCS_HOST_RSP				0x00006C14
#define VMCS_HOST_RIP				0x00006C16
#define VMCS_HOST_IA32_S_CET			0x00006C18
#define VMCS_HOST_SSP				0x00006C1A
#define VMCS_HOST_IA32_INTR_SSP_TABLE		0x00006C1C

/* VMX basic exit reasons. */
#define VMCS_EXITCODE_EXC_NMI			0
#define VMCS_EXITCODE_EXT_INT			1
#define VMCS_EXITCODE_SHUTDOWN			2
#define VMCS_EXITCODE_INIT			3
#define VMCS_EXITCODE_SIPI			4
#define VMCS_EXITCODE_SMI			5
#define VMCS_EXITCODE_OTHER_SMI			6
#define VMCS_EXITCODE_INT_WINDOW		7
#define VMCS_EXITCODE_NMI_WINDOW		8
#define VMCS_EXITCODE_TASK_SWITCH		9
#define VMCS_EXITCODE_CPUID			10
#define VMCS_EXITCODE_GETSEC			11
#define VMCS_EXITCODE_HLT			12
#define VMCS_EXITCODE_INVD			13
#define VMCS_EXITCODE_INVLPG			14
#define VMCS_EXITCODE_RDPMC			15
#define VMCS_EXITCODE_RDTSC			16
#define VMCS_EXITCODE_RSM			17
#define VMCS_EXITCODE_VMCALL			18
#define VMCS_EXITCODE_VMCLEAR			19
#define VMCS_EXITCODE_VMLAUNCH			20
#define VMCS_EXITCODE_VMPTRLD			21
#define VMCS_EXITCODE_VMPTRST			22
#define VMCS_EXITCODE_VMREAD			23
#define VMCS_EXITCODE_VMRESUME			24
#define VMCS_EXITCODE_VMWRITE			25
#define VMCS_EXITCODE_VMXOFF			26
#define VMCS_EXITCODE_VMXON			27
#define VMCS_EXITCODE_CR			28
#define VMCS_EXITCODE_DR			29
#define VMCS_EXITCODE_IO			30
#define VMCS_EXITCODE_RDMSR			31
#define VMCS_EXITCODE_WRMSR			32
#define VMCS_EXITCODE_FAIL_GUEST_INVALID	33
#define VMCS_EXITCODE_FAIL_MSR_INVALID		34
#define VMCS_EXITCODE_MWAIT			36
#define VMCS_EXITCODE_TRAP_FLAG			37
#define VMCS_EXITCODE_MONITOR			39
#define VMCS_EXITCODE_PAUSE			40
#define VMCS_EXITCODE_FAIL_MACHINE_CHECK	41
#define VMCS_EXITCODE_TPR_BELOW			43
#define VMCS_EXITCODE_APIC_ACCESS		44
#define VMCS_EXITCODE_VEOI			45
#define VMCS_EXITCODE_GDTR_IDTR			46
#define VMCS_EXITCODE_LDTR_TR			47
#define VMCS_EXITCODE_EPT_VIOLATION		48
#define VMCS_EXITCODE_EPT_MISCONFIG		49
#define VMCS_EXITCODE_INVEPT			50
#define VMCS_EXITCODE_RDTSCP			51
#define VMCS_EXITCODE_PREEMPT_TIMEOUT		52
#define VMCS_EXITCODE_INVVPID			53
#define VMCS_EXITCODE_WBINVD			54
#define VMCS_EXITCODE_XSETBV			55
#define VMCS_EXITCODE_APIC_WRITE		56
#define VMCS_EXITCODE_RDRAND			57
#define VMCS_EXITCODE_INVPCID			58
#define VMCS_EXITCODE_VMFUNC			59
#define VMCS_EXITCODE_ENCLS			60
#define VMCS_EXITCODE_RDSEED			61
#define VMCS_EXITCODE_PAGE_LOG_FULL		62
#define VMCS_EXITCODE_XSAVES			63
#define VMCS_EXITCODE_XRSTORS			64
#define VMCS_EXITCODE_SPP			66
#define VMCS_EXITCODE_UMWAIT			67
#define VMCS_EXITCODE_TPAUSE			68

/* -------------------------------------------------------------------------- */

static void vmm_vmx_vcpu_state_provide(struct vmm_vcpu *, uint64_t);
static void vmm_vmx_vcpu_setstate_all(struct vmm_vcpu *, uint64_t);
static void vmm_vmx_vcpu_getstate_all(struct vmm_vcpu *, uint64_t);

/*
 * These host values are static, they do not change at runtime and are the same
 * on all CPUs. We save them here because they are not saved in the VMCS.
 */
static struct {
	uint64_t xcr0;
	uint64_t star;
	uint64_t lstar;
	uint64_t cstar;
	uint64_t sfmask;
} vmm_vmx_global_hstate __cacheline_aligned;

#define VMM_VMX_MSRLIST_STAR		0
#define VMM_VMX_MSRLIST_LSTAR		1
#define VMM_VMX_MSRLIST_CSTAR		2
#define VMM_VMX_MSRLIST_SFMASK		3
#define VMM_VMX_MSRLIST_KERNELGSBASE	4
#define VMM_VMX_MSRLIST_EXIT_NMSR		5
#define VMM_VMX_MSRLIST_L1DFLUSH		5

/* On entry, we may do +1 to include L1DFLUSH. */
static size_t vmm_vmx_msrlist_entry_nmsr __read_mostly = VMM_VMX_MSRLIST_EXIT_NMSR;

struct vmxon {
	uint32_t ident;
#define VMXON_IDENT_REVISION	__BITS(30,0)

	uint8_t data[PAGE_SIZE - 4];
} __packed;

CTASSERT(sizeof(struct vmxon) == PAGE_SIZE);

struct vmxoncpu {
	vaddr_t va;
	paddr_t pa;
};

static struct vmxoncpu vmxoncpu[OS_MAXCPUS];

struct vmcs {
	uint32_t ident;
#define VMCS_IDENT_REVISION	__BITS(30,0)
#define VMCS_IDENT_SHADOW	__BIT(31)

	uint32_t abort;
	uint8_t data[PAGE_SIZE - 8];
} __packed;

CTASSERT(sizeof(struct vmcs) == PAGE_SIZE);

struct msr_entry {
	uint32_t msr;
	uint32_t rsvd;
	uint64_t val;
} __packed;

#define VPID_MAX	0xFFFF

/* VPID zero is reserved for the host. */
CTASSERT(VPID_MAX > 1);

static uint64_t vmm_vmx_tlb_flush_op __read_mostly;
static uint64_t vmm_vmx_ept_flush_op __read_mostly;
static uint64_t vmm_vmx_eptp_type __read_mostly;
static bool vmm_vmx_ept_has_ad __read_mostly;
static bool vmm_vmx_cpu_has_arch_cap __read_mostly;

static uint64_t vmm_vmx_pinbased_ctls __read_mostly;
static uint64_t vmm_vmx_procbased_ctls __read_mostly;
static uint64_t vmm_vmx_procbased_ctls2 __read_mostly;
static uint64_t vmm_vmx_entry_ctls __read_mostly;
static uint64_t vmm_vmx_exit_ctls __read_mostly;

static uint64_t vmm_vmx_cr0_fixed0 __read_mostly;
static uint64_t vmm_vmx_cr0_fixed1 __read_mostly;
static uint64_t vmm_vmx_cr4_fixed0 __read_mostly;
static uint64_t vmm_vmx_cr4_fixed1 __read_mostly;

#define VMM_VMX_PINBASED_CTLS_ONE	\
	(PIN_CTLS_INT_EXITING| \
	 PIN_CTLS_NMI_EXITING| \
	 PIN_CTLS_VIRTUAL_NMIS)

#define VMM_VMX_PINBASED_CTLS_ZERO	0

#define VMM_VMX_PROCBASED_CTLS_ONE	\
	(PROC_CTLS_USE_TSC_OFFSETTING| \
	 PROC_CTLS_HLT_EXITING| \
	 PROC_CTLS_MWAIT_EXITING | \
	 PROC_CTLS_RDPMC_EXITING | \
	 PROC_CTLS_RCR8_EXITING | \
	 PROC_CTLS_LCR8_EXITING | \
	 PROC_CTLS_UNCOND_IO_EXITING | /* no I/O bitmap */ \
	 PROC_CTLS_USE_MSR_BITMAPS | \
	 PROC_CTLS_MONITOR_EXITING | \
	 PROC_CTLS_ACTIVATE_CTLS2)

#define VMM_VMX_PROCBASED_CTLS_ZERO	\
	(PROC_CTLS_RCR3_EXITING| \
	 PROC_CTLS_LCR3_EXITING)

#define VMM_VMX_PROCBASED_CTLS2_ONE	\
	(PROC_CTLS2_ENABLE_EPT| \
	 PROC_CTLS2_ENABLE_VPID| \
	 PROC_CTLS2_UNRESTRICTED_GUEST)

#define VMM_VMX_PROCBASED_CTLS2_ZERO	0

#define VMM_VMX_ENTRY_CTLS_ONE	\
	(ENTRY_CTLS_LOAD_DEBUG_CONTROLS| \
	 ENTRY_CTLS_LOAD_EFER| \
	 ENTRY_CTLS_LOAD_PAT)

#define VMM_VMX_ENTRY_CTLS_ZERO	\
	(ENTRY_CTLS_SMM| \
	 ENTRY_CTLS_DISABLE_DUAL)

#define VMM_VMX_EXIT_CTLS_ONE	\
	(EXIT_CTLS_SAVE_DEBUG_CONTROLS| \
	 EXIT_CTLS_HOST_LONG_MODE| \
	 EXIT_CTLS_SAVE_PAT| \
	 EXIT_CTLS_LOAD_PAT| \
	 EXIT_CTLS_SAVE_EFER| \
	 EXIT_CTLS_LOAD_EFER)

#define VMM_VMX_EXIT_CTLS_ZERO	0

static uint8_t *vmm_vmx_asidmap __read_mostly;
static uint32_t vmm_vmx_maxasid __read_mostly;
static os_mtx_t vmm_vmx_asidlock __cacheline_aligned;

#define VMM_VMX_XCR0_MASK_DEFAULT	(XCR0_X87|XCR0_SSE)
static uint64_t vmm_vmx_xcr0_mask __read_mostly;

#define VMM_VMX_NCPUID_ENTRIES	64

#define VMCS_NPAGES	1
#define VMCS_SIZE	(VMCS_NPAGES * PAGE_SIZE)

#define MSRBM_NPAGES	1
#define MSRBM_SIZE	(MSRBM_NPAGES * PAGE_SIZE)

/* Guest/host CR0 mask: bits owned by the host */
#define CR0_STATIC_MASK \
	(CR0_ET | CR0_NW | CR0_CD)

/*
 * Guest real CR0 bits that must be handled specially:
 * - CR0_ET: hardwired to 1 in modern CPUs; must always be 1
 * - CR0_NE: proper FPU error handling; must always be 1
 * - CR0_CD, CR0_NW: cache control; must be forced to 0 for performance
 */
#define CR0_FORCE_ZERO \
	(CR0_NW | CR0_CD)
#define CR0_FORCE_ONE \
	(CR0_ET | CR0_NE)

#define CR4_VALID \
	(CR4_VME |			\
	 CR4_PVI |			\
	 CR4_TSD |			\
	 CR4_DE |			\
	 CR4_PSE |			\
	 CR4_PAE |			\
	 CR4_MCE |			\
	 CR4_PGE |			\
	 CR4_PCE |			\
	 CR4_OSFXSR |			\
	 CR4_OSXMMEXCPT |		\
	 CR4_UMIP |			\
	 /* CR4_LA57 excluded */	\
	 /* CR4_VMXE excluded */	\
	 /* CR4_SMXE excluded */	\
	 CR4_FSGSBASE |			\
	 CR4_PCIDE |			\
	 CR4_OSXSAVE |			\
	 CR4_SMEP |			\
	 CR4_SMAP			\
	 /* CR4_PKE excluded */		\
	 /* CR4_CET excluded */		\
	 /* CR4_PKS excluded */)
#define CR4_INVALID \
	(0xFFFFFFFFFFFFFFFFULL & ~CR4_VALID)

#define EFER_TLB_FLUSH \
	(EFER_NXE|EFER_LMA|EFER_LME)
#define CR0_TLB_FLUSH \
	(CR0_PG|CR0_WP|CR0_CD|CR0_NW)
#define CR4_TLB_FLUSH \
	(CR4_PSE|CR4_PAE|CR4_PGE|CR4_PCIDE|CR4_SMEP)

/* -------------------------------------------------------------------------- */

struct vmm_vmx_machdata {
	volatile uint64_t mach_htlb_gen;
	/* One guest TSC timeline is shared by every vCPU in the machine. */
	volatile uint64_t gtsc_offset;
	volatile uint64_t gtsc_generation;
	struct vmm_vmx_interrupt_machine *interrupt;
};

struct vmm_vmx_xsave {
	struct vmm_cpustate_fpu fpu;
	uint64_t xstate_bv;
	uint64_t xcomp_bv;
	uint8_t reserved0[8];
	uint8_t reserved[40];
};
CTASSERT(sizeof(struct vmm_vmx_xsave) == 512 + 64);

struct vmm_vmx_cpuid_filter {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
};
static const struct vmm_vmx_cpuid_filter vmm_vmx_cpuid_00000001 = {
	.eax = ~0,
	.ebx = ~0,
	.ecx =
	    CPUID_0_01_ECX_SSE3 |
	    CPUID_0_01_ECX_PCLMULQDQ |
	    /* CPUID_0_01_ECX_DTES64 excluded */
	    /* CPUID_0_01_ECX_MONITOR excluded */
	    /* CPUID_0_01_ECX_DS_CPL excluded */
	    /* CPUID_0_01_ECX_VMX excluded */
	    /* CPUID_0_01_ECX_SMX excluded */
	    /* CPUID_0_01_ECX_EIST excluded */
	    /* CPUID_0_01_ECX_TM2 excluded */
	    CPUID_0_01_ECX_SSSE3 |
	    /* CPUID_0_01_ECX_CNXTID excluded */
	    /* CPUID_0_01_ECX_SDBG excluded */
	    /* CPUID_0_01_ECX_FMA excluded */
	    CPUID_0_01_ECX_CX16 |
	    /* CPUID_0_01_ECX_XTPR excluded */
	    /* CPUID_0_01_ECX_PDCM excluded */
	    CPUID_0_01_ECX_PCID |
	    /* CPUID_0_01_ECX_DCA excluded */
	    CPUID_0_01_ECX_SSE41 |
	    CPUID_0_01_ECX_SSE42 |
	    /* CPUID_0_01_ECX_X2APIC excluded */
	    CPUID_0_01_ECX_MOVBE |
	    CPUID_0_01_ECX_POPCNT |
	    /* CPUID_0_01_ECX_TSC_DEADLINE excluded */
	    CPUID_0_01_ECX_AESNI |
	    CPUID_0_01_ECX_XSAVE |
	    CPUID_0_01_ECX_OSXSAVE |
	    /* CPUID_0_01_ECX_AVX excluded */
	    /* CPUID_0_01_ECX_F16C excluded */
	    CPUID_0_01_ECX_RDRAND,
	    /* CPUID_0_01_ECX_RAZ excluded */
	.edx =
	    CPUID_0_01_EDX_FPU |
	    CPUID_0_01_EDX_VME |
	    CPUID_0_01_EDX_DE |
	    CPUID_0_01_EDX_PSE |
	    CPUID_0_01_EDX_TSC |
	    CPUID_0_01_EDX_MSR |
	    CPUID_0_01_EDX_PAE |
	    /* CPUID_0_01_EDX_MCE excluded */
	    CPUID_0_01_EDX_CX8 |
	    CPUID_0_01_EDX_APIC |
	    CPUID_0_01_EDX_SEP |
	    /* CPUID_0_01_EDX_MTRR excluded */
	    CPUID_0_01_EDX_PGE |
	    /* CPUID_0_01_EDX_MCA excluded */
	    CPUID_0_01_EDX_CMOV |
	    CPUID_0_01_EDX_PAT |
	    /* CPUID_0_01_EDX_PSE36 excluded */
	    /* CPUID_0_01_EDX_PSN excluded */
	    CPUID_0_01_EDX_CLFSH |
	    /* CPUID_0_01_EDX_DS excluded */
	    /* CPUID_0_01_EDX_ACPI excluded */
	    CPUID_0_01_EDX_MMX |
	    CPUID_0_01_EDX_FXSR |
	    CPUID_0_01_EDX_SSE |
	    CPUID_0_01_EDX_SSE2 |
	    CPUID_0_01_EDX_SS |
	    CPUID_0_01_EDX_HTT |
	    /* CPUID_0_01_EDX_TM excluded */
	    CPUID_0_01_EDX_PBE
};

static const struct vmm_vmx_cpuid_filter vmm_vmx_cpuid_00000007 = {
	.eax = ~0,
	.ebx =
	    CPUID_0_07_EBX_FSGSBASE |
	    CPUID_0_07_EBX_TSC_ADJUST |
	    /* CPUID_0_07_EBX_SGX excluded */
	    CPUID_0_07_EBX_BMI1 |
	    /* CPUID_0_07_EBX_HLE excluded */
	    /* CPUID_0_07_EBX_AVX2 excluded */
	    CPUID_0_07_EBX_FDPEXONLY |
	    CPUID_0_07_EBX_SMEP |
	    CPUID_0_07_EBX_BMI2 |
	    CPUID_0_07_EBX_ERMS |
	    CPUID_0_07_EBX_INVPCID |
	    /* CPUID_0_07_EBX_RTM excluded */
	    /* CPUID_0_07_EBX_QM excluded */
	    CPUID_0_07_EBX_FPUCSDS |
	    /* CPUID_0_07_EBX_MPX excluded */
	    /* CPUID_0_07_EBX_PQE excluded */
	    /* CPUID_0_07_EBX_AVX512F excluded */
	    /* CPUID_0_07_EBX_AVX512DQ excluded */
	    CPUID_0_07_EBX_RDSEED |
	    CPUID_0_07_EBX_ADX |
	    CPUID_0_07_EBX_SMAP |
	    /* CPUID_0_07_EBX_AVX512_IFMA excluded */
	    CPUID_0_07_EBX_CLFLUSHOPT |
	    CPUID_0_07_EBX_CLWB |
	    /* CPUID_0_07_EBX_PT excluded */
	    /* CPUID_0_07_EBX_AVX512PF excluded */
	    /* CPUID_0_07_EBX_AVX512ER excluded */
	    /* CPUID_0_07_EBX_AVX512CD excluded */
	    CPUID_0_07_EBX_SHA,
	    /* CPUID_0_07_EBX_AVX512BW excluded */
	    /* CPUID_0_07_EBX_AVX512VL excluded */
	.ecx =
	    CPUID_0_07_ECX_PREFETCHWT1 |
	    /* CPUID_0_07_ECX_AVX512_VBMI excluded */
	    CPUID_0_07_ECX_UMIP |
	    /* CPUID_0_07_ECX_PKU excluded */
	    /* CPUID_0_07_ECX_OSPKE excluded */
	    /* CPUID_0_07_ECX_WAITPKG excluded */
	    /* CPUID_0_07_ECX_AVX512_VBMI2 excluded */
	    /* CPUID_0_07_ECX_CET_SS excluded */
	    CPUID_0_07_ECX_GFNI |
	    /* CPUID_0_07_ECX_VAES excluded */
	    /* CPUID_0_07_ECX_VPCLMULQDQ excluded */
	    /* CPUID_0_07_ECX_AVX512_VNNI excluded */
	    /* CPUID_0_07_ECX_AVX512_BITALG excluded */
	    /* CPUID_0_07_ECX_TME_EN excluded */
	    /* CPUID_0_07_ECX_AVX512_VPOPCNTDQ excluded */
	    /* CPUID_0_07_ECX_LA57 excluded */
	    /* CPUID_0_07_ECX_MAWAU excluded */
	    /* CPUID_0_07_ECX_RDPID excluded */
	    /* CPUID_0_07_ECX_KEY_LOCKER excluded */
	    /* CPUID_0_07_ECX_BUS_LOCK_DETECT excluded */
	    CPUID_0_07_ECX_CLDEMOTE |
	    CPUID_0_07_ECX_MOVDIRI |
	    CPUID_0_07_ECX_MOVDIR64B,
	    /* CPUID_0_07_ECX_ENQCMD excluded */
	    /* CPUID_0_07_ECX_SGXLC excluded */
	    /* CPUID_0_07_ECX_PKS excluded */
	.edx =
	    /* CPUID_0_07_EDX_SGX_KEYS excluded */
	    /* CPUID_0_07_EDX_AVX512_4VNNIW excluded */
	    /* CPUID_0_07_EDX_AVX512_4FMAPS excluded */
	    CPUID_0_07_EDX_FSREP_MOV |
	    /* CPUID_0_07_EDX_UINTR excluded */
	    /* CPUID_0_07_EDX_AVX512_VP2INTERSECT excluded */
	    /* CPUID_0_07_EDX_SRBDS_CTRL excluded */
	    CPUID_0_07_EDX_MD_CLEAR |
	    /* CPUID_0_07_EDX_RTM_ALWAYS_ABORT excluded */
	    /* CPUID_0_07_EDX_RTM_FORCE_ABORT excluded */
	    CPUID_0_07_EDX_SERIALIZE |
	    /* CPUID_0_07_EDX_HYBRID excluded */
	    /* CPUID_0_07_EDX_TSXLDTRK excluded */
	    /* CPUID_0_07_EDX_PCONFIG excluded */
	    /* CPUID_0_07_EDX_ARCH_LBRS excluded */
	    /* CPUID_0_07_EDX_CET_IBT excluded */
	    /* CPUID_0_07_EDX_AMX_BF16 excluded */
	    /* CPUID_0_07_EDX_AVX512_FP16 excluded */
	    /* CPUID_0_07_EDX_AMX_TILE excluded */
	    /* CPUID_0_07_EDX_AMX_INT8 excluded */
	    /* CPUID_0_07_EDX_IBRS excluded */
	    /* CPUID_0_07_EDX_STIBP excluded */
	    /* CPUID_0_07_EDX_L1D_FLUSH excluded */
	    CPUID_0_07_EDX_ARCH_CAP
	    /* CPUID_0_07_EDX_CORE_CAP excluded */
	    /* CPUID_0_07_EDX_SSBD excluded */
};

static const struct vmm_vmx_cpuid_filter vmm_vmx_cpuid_80000001 = {
	.eax = ~0,
	.ebx = ~0,
	.ecx =
	    CPUID_8_01_ECX_LAHF |
	    CPUID_8_01_ECX_CMPLEGACY |
	    /* CPUID_8_01_ECX_SVM excluded */
	    /* CPUID_8_01_ECX_EAPIC excluded */
	    CPUID_8_01_ECX_ALTMOVCR8 |
	    CPUID_8_01_ECX_ABM |
	    CPUID_8_01_ECX_SSE4A |
	    CPUID_8_01_ECX_MISALIGNSSE |
	    CPUID_8_01_ECX_3DNOWPF |
	    /* CPUID_8_01_ECX_OSVW excluded */
	    /* CPUID_8_01_ECX_IBS excluded */
	    CPUID_8_01_ECX_XOP |
	    /* CPUID_8_01_ECX_SKINIT excluded */
	    /* CPUID_8_01_ECX_WDT excluded */
	    /* CPUID_8_01_ECX_LWP excluded */
	    CPUID_8_01_ECX_FMA4 |
	    CPUID_8_01_ECX_TCE |
	    /* CPUID_8_01_ECX_NODEID excluded */
	    CPUID_8_01_ECX_TBM |
	    CPUID_8_01_ECX_TOPOEXT,
	    /* CPUID_8_01_ECX_PCEC excluded */
	    /* CPUID_8_01_ECX_PCENB excluded */
	    /* CPUID_8_01_ECX_DBE excluded */
	    /* CPUID_8_01_ECX_PERFTSC excluded */
	    /* CPUID_8_01_ECX_PERFEXTLLC excluded */
	    /* CPUID_8_01_ECX_MWAITX excluded */
	    /* CPUID_8_01_ECX_AddrMaskExt excluded */
	.edx =
	    CPUID_8_01_EDX_FPU |
	    CPUID_8_01_EDX_VME |
	    CPUID_8_01_EDX_DE |
	    CPUID_8_01_EDX_PSE |
	    CPUID_8_01_EDX_TSC |
	    CPUID_8_01_EDX_MSR |
	    CPUID_8_01_EDX_PAE |
	    /* CPUID_8_01_EDX_MCE excluded */
	    CPUID_8_01_EDX_CX8 |
	    CPUID_8_01_EDX_APIC |
	    CPUID_8_01_EDX_SYSCALL |
	    /* CPUID_8_01_EDX_MTRR excluded */
	    CPUID_8_01_EDX_PGE |
	    /* CPUID_8_01_EDX_MCA excluded */
	    CPUID_8_01_EDX_CMOV |
	    CPUID_8_01_EDX_PAT |
	    /* CPUID_8_01_EDX_PSE36 */
	    CPUID_8_01_EDX_XD |
	    CPUID_8_01_EDX_MMXEXT |
	    CPUID_8_01_EDX_MMX |
	    CPUID_8_01_EDX_FXSR |
	    CPUID_8_01_EDX_FFXSR |
	    CPUID_8_01_EDX_PAGE1GB |
	    /* CPUID_8_01_EDX_RDTSCP excluded */
	    CPUID_8_01_EDX_LM |
	    CPUID_8_01_EDX_3DNOWEXT |
	    CPUID_8_01_EDX_3DNOW
};

static const struct vmm_vmx_cpuid_filter vmm_vmx_cpuid_80000007 = {
	.eax = 0,
	.ebx = 0,
	.ecx = 0,
	.edx =
	    /* CPUID_8_07_EDX_TS excluded */
	    /* CPUID_8_07_EDX_FID excluded */
	    /* CPUID_8_07_EDX_VID excluded */
	    /* CPUID_8_07_EDX_TTP excluded */
	    /* CPUID_8_07_EDX_TM excluded */
	    /* CPUID_8_07_EDX_100MHzSteps excluded */
	    /* CPUID_8_07_EDX_HwPstate excluded */
	    CPUID_8_07_EDX_TscInvariant,
	    /* CPUID_8_07_EDX_CPB excluded */
	    /* CPUID_8_07_EDX_EffFreqRO excluded */
	    /* CPUID_8_07_EDX_ProcFeedbackIntf excluded */
	    /* CPUID_8_07_EDX_ProcPowerReport excluded */
};

static const struct vmm_vmx_cpuid_filter vmm_vmx_cpuid_80000008 = {
	.eax = ~0,
	.ebx =
	    CPUID_8_08_EBX_CLZERO |
	    /* CPUID_8_08_EBX_InstRetCntMsr excluded */
	    CPUID_8_08_EBX_RstrFpErrPtrs |
	    /* CPUID_8_08_EBX_INVLPGB excluded */
	    /* CPUID_8_08_EBX_RDPRU excluded */
	    /* CPUID_8_08_EBX_MCOMMIT excluded */
	    CPUID_8_08_EBX_WBNOINVD |
	    /* CPUID_8_08_EBX_IBPB excluded */
	    /* CPUID_8_08_EBX_INT_WBINVD excluded */
	    /* CPUID_8_08_EBX_IBRS excluded */
	    CPUID_8_08_EBX_EferLmsleUnsupp |
	    /* CPUID_8_08_EBX_INVLPGBnestedPg excluded */
	    /* CPUID_8_08_EBX_STIBP excluded */
	    /* CPUID_8_08_EBX_IBRS_ALWAYSON excluded */
	    /* CPUID_8_08_EBX_STIBP_ALWAYSON excluded */
	    /* CPUID_8_08_EBX_PREFER_IBRS excluded */
	    /* CPUID_8_08_EBX_SSBD excluded */
	    /* CPUID_8_08_EBX_VIRT_SSBD excluded */
	    CPUID_8_08_EBX_SsbdNotRequired |
	    /* CPUID_8_08_EBX_CPPC excluded */
	    /* CPUID_8_08_EBX_PSFD excluded */
	    CPUID_8_08_EBX_BTC_NO,
	    /* CPUID_8_08_EBX_IBPB_RET excluded */
	.ecx = 0,
	.edx = 0
};

static bool
vmm_vmx_pat_validate(uint64_t val)
{
	uint8_t *pat = (uint8_t *)&val;
	size_t i;

	for (i = 0; i < 8; i++) {
		if (__predict_false(pat[i] & ~__BITS(2,0)))
			return false;
		if (__predict_false(pat[i] == 2 || pat[i] == 3))
			return false;
	}

	return true;
}

static uint32_t
vmm_vmx_xsave_size(uint64_t xcr0 __unused)
{
	uint32_t size;

	/*
	 * x87/SSE are in the legacy region, and the size of that region is
	 * constant regardless of whether SSE is enabled.
	 */

	size = 512; /* x87 + SSE */
	size += 64; /* XSAVE header */

	return size;
}

struct vmm_vmx_cpudata {
	/* General. */
	uint64_t asid;
	bool gtlb_want_flush;
	bool gtsc_want_update;
	bool htlb_force_flush;
	uint64_t vcpu_htlb_gen;
	os_cpuset_t *htlb_want_flush;
	int hcpu_last;
	volatile int running_cpu;

	/* VMCS. */
	struct vmcs *vmcs;
	paddr_t vmcs_pa;
	size_t vmcs_refcnt;
	os_cpu_t *vmcs_cpu;
	bool vmcs_launched;

	/* MSR bitmap. */
	uint8_t *msrbm;
	paddr_t msrbm_pa;

	/* Percpu host state, absent from VMCS. */
	struct {
		uint64_t kernelgsbase;
#ifdef __DragonFly__
		uint64_t drs[VMM_X64_DR_COUNT];
#endif
	} hstate;

	/* Intr state. */
	bool int_window_exit;
	bool nmi_window_exit;
	bool evt_pending;
	struct vmm_vmx_interrupt_vcpu *interrupt;

	/* Guest state. */
	struct msr_entry *gmsr;
	paddr_t gmsr_pa;
	uint64_t gmsr_misc_enable;
	uint64_t gcr2;
	uint64_t gcr8;
	uint64_t gxcr0;
	uint64_t gprs[VMM_X64_GPR_COUNT];
	uint64_t drs[VMM_X64_DR_COUNT];
	uint64_t gtsc_offset;
	uint64_t gtsc_match;
	uint64_t gtsc_generation;
	uint64_t gtsc_adjust;
	struct vmm_vmx_xsave gxsave __aligned(64);

	/* Exact frontend CPUID template. */
	size_t cpuid_entry_count;
	struct vmm_cpuid_entry cpuid_entries[VMM_VMX_NCPUID_ENTRIES];
};

static const struct {
	uint64_t selector;
	uint64_t attrib;
	uint64_t limit;
	uint64_t base;
} vmm_vmx_guest_segs[VMM_X64_SEG_COUNT] = {
	[VMM_X64_SEG_ES] = {
		VMCS_GUEST_ES_SELECTOR,
		VMCS_GUEST_ES_ACCESS_RIGHTS,
		VMCS_GUEST_ES_LIMIT,
		VMCS_GUEST_ES_BASE
	},
	[VMM_X64_SEG_CS] = {
		VMCS_GUEST_CS_SELECTOR,
		VMCS_GUEST_CS_ACCESS_RIGHTS,
		VMCS_GUEST_CS_LIMIT,
		VMCS_GUEST_CS_BASE
	},
	[VMM_X64_SEG_SS] = {
		VMCS_GUEST_SS_SELECTOR,
		VMCS_GUEST_SS_ACCESS_RIGHTS,
		VMCS_GUEST_SS_LIMIT,
		VMCS_GUEST_SS_BASE
	},
	[VMM_X64_SEG_DS] = {
		VMCS_GUEST_DS_SELECTOR,
		VMCS_GUEST_DS_ACCESS_RIGHTS,
		VMCS_GUEST_DS_LIMIT,
		VMCS_GUEST_DS_BASE
	},
	[VMM_X64_SEG_FS] = {
		VMCS_GUEST_FS_SELECTOR,
		VMCS_GUEST_FS_ACCESS_RIGHTS,
		VMCS_GUEST_FS_LIMIT,
		VMCS_GUEST_FS_BASE
	},
	[VMM_X64_SEG_GS] = {
		VMCS_GUEST_GS_SELECTOR,
		VMCS_GUEST_GS_ACCESS_RIGHTS,
		VMCS_GUEST_GS_LIMIT,
		VMCS_GUEST_GS_BASE
	},
	[VMM_X64_SEG_GDT] = {
		0, /* doesn't exist */
		0, /* doesn't exist */
		VMCS_GUEST_GDTR_LIMIT,
		VMCS_GUEST_GDTR_BASE
	},
	[VMM_X64_SEG_IDT] = {
		0, /* doesn't exist */
		0, /* doesn't exist */
		VMCS_GUEST_IDTR_LIMIT,
		VMCS_GUEST_IDTR_BASE
	},
	[VMM_X64_SEG_LDT] = {
		VMCS_GUEST_LDTR_SELECTOR,
		VMCS_GUEST_LDTR_ACCESS_RIGHTS,
		VMCS_GUEST_LDTR_LIMIT,
		VMCS_GUEST_LDTR_BASE
	},
	[VMM_X64_SEG_TR] = {
		VMCS_GUEST_TR_SELECTOR,
		VMCS_GUEST_TR_ACCESS_RIGHTS,
		VMCS_GUEST_TR_LIMIT,
		VMCS_GUEST_TR_BASE
	}
};

/* -------------------------------------------------------------------------- */

static uint64_t
vmm_vmx_get_revision(void)
{
	uint64_t msr;

	msr = rdmsr(MSR_IA32_VMX_BASIC);
	msr &= IA32_VMX_BASIC_IDENT;

	return msr;
}

static
OS_IPI_FUNC(vmm_vmx_vmclear_ipi)
{
	paddr_t vmcs_pa = (paddr_t)arg;
	vmm_vmx_vmclear(&vmcs_pa);
}

static void
vmm_vmx_vmclear_remote(os_cpu_t *cpu, paddr_t vmcs_pa)
{
	int bound;

	OS_ASSERT(os_preempt_disabled());

	/*
	 * TODO: OSify curlwp_bind().
	 */

	bound = curlwp_bind();
	os_preempt_enable();

	os_ipi_unicast(cpu, vmm_vmx_vmclear_ipi, (void *)vmcs_pa);

	os_preempt_disable();
	curlwp_bindx(bound);
}

static void
vmm_vmx_vmcs_enter(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	os_cpu_t *vmcs_cpu;

	cpudata->vmcs_refcnt++;
	if (cpudata->vmcs_refcnt > 1) {
		OS_ASSERT(os_preempt_disabled());
		OS_ASSERT(vmm_vmx_vmptrst() == cpudata->vmcs_pa);
		return;
	}

	vmcs_cpu = cpudata->vmcs_cpu;
	cpudata->vmcs_cpu = (void *)0x00FFFFFFFFFFFFFF; /* clobber */

	os_preempt_disable();

	if (vmcs_cpu == NULL) {
		/* This VMCS is loaded for the first time. */
		vmm_vmx_vmclear(&cpudata->vmcs_pa);
		cpudata->vmcs_launched = false;
	} else if (vmcs_cpu != os_curcpu()) {
		/* This VMCS is active on a remote CPU. */
		vmm_vmx_vmclear_remote(vmcs_cpu, cpudata->vmcs_pa);
		cpudata->vmcs_launched = false;
	} else {
		/* This VMCS is active on curcpu, nothing to do. */
	}

	vmm_vmx_vmptrld(&cpudata->vmcs_pa);
}

static void
vmm_vmx_vmcs_leave(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	OS_ASSERT(os_preempt_disabled());
	OS_ASSERT(vmm_vmx_vmptrst() == cpudata->vmcs_pa);
	OS_ASSERT(cpudata->vmcs_refcnt > 0);
	cpudata->vmcs_refcnt--;

	if (cpudata->vmcs_refcnt > 0) {
		return;
	}

	cpudata->vmcs_cpu = os_curcpu();
	os_preempt_enable();
}

static void
vmm_vmx_vmcs_destroy(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	OS_ASSERT(os_preempt_disabled());
	OS_ASSERT(vmm_vmx_vmptrst() == cpudata->vmcs_pa);
	OS_ASSERT(cpudata->vmcs_refcnt == 1);
	cpudata->vmcs_refcnt--;

	vmm_vmx_vmclear(&cpudata->vmcs_pa);
	os_preempt_enable();
}

/* -------------------------------------------------------------------------- */

static void
vmm_vmx_event_waitexit_enable(struct vmm_vcpu *vcpu, bool nmi)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t ctls1;

	ctls1 = vmm_vmx_vmread(VMCS_PROCBASED_CTLS);

	if (nmi) {
		// XXX INT_STATE_NMI?
		ctls1 |= PROC_CTLS_NMI_WINDOW_EXITING;
		cpudata->nmi_window_exit = true;
	} else {
		ctls1 |= PROC_CTLS_INT_WINDOW_EXITING;
		cpudata->int_window_exit = true;
	}

	vmm_vmx_vmwrite(VMCS_PROCBASED_CTLS, ctls1);
}

static void
vmm_vmx_event_waitexit_disable(struct vmm_vcpu *vcpu, bool nmi)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t ctls1;

	ctls1 = vmm_vmx_vmread(VMCS_PROCBASED_CTLS);

	if (nmi) {
		ctls1 &= ~PROC_CTLS_NMI_WINDOW_EXITING;
		cpudata->nmi_window_exit = false;
	} else {
		ctls1 &= ~PROC_CTLS_INT_WINDOW_EXITING;
		cpudata->int_window_exit = false;
	}

	vmm_vmx_vmwrite(VMCS_PROCBASED_CTLS, ctls1);
}

static inline bool
vmm_vmx_excp_has_rf(uint8_t vector)
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
vmm_vmx_excp_has_error(uint8_t vector)
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
vmm_vmx_vcpu_inject(struct vmm_vcpu *vcpu,
    const struct vmm_cpuevent *event)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	int type = 0, err = 0, ret = EINVAL;
	uint64_t rflags, info, error;
	u_int evtype;
	uint8_t vector;

	evtype = event->type;
	vector = event->vector;
	error = event->error;
	__insn_barrier();

	vmm_vmx_vmcs_enter(vcpu);

	switch (evtype) {
	case VMM_CPUEVENT_EXCP:
		if (vector == 2 || vector >= 32)
			goto out;
		if (vector == 3 || vector == 0)
			goto out;
		if (vmm_vmx_excp_has_rf(vector)) {
			rflags = vmm_vmx_vmread(VMCS_GUEST_RFLAGS);
			vmm_vmx_vmwrite(VMCS_GUEST_RFLAGS, rflags | PSL_RF);
		}
		type = INTR_TYPE_HW_EXC;
		err = vmm_vmx_excp_has_error(vector);
		break;
	case VMM_CPUEVENT_INTR:
		type = INTR_TYPE_EXT_INT;
		if (vector == 2) {
			type = INTR_TYPE_NMI;
			vmm_vmx_event_waitexit_enable(vcpu, true);
		}
		err = 0;
		break;
	default:
		goto out;
	}

	info =
	    __SHIFTIN((uint64_t)vector, INTR_INFO_VECTOR) |
	    __SHIFTIN((uint64_t)type, INTR_INFO_TYPE) |
	    __SHIFTIN((uint64_t)err, INTR_INFO_ERROR) |
	    __SHIFTIN((uint64_t)1, INTR_INFO_VALID);
	vmm_vmx_vmwrite(VMCS_ENTRY_INTR_INFO, info);
	vmm_vmx_vmwrite(VMCS_ENTRY_EXCEPTION_ERROR, error);

	cpudata->evt_pending = true;
	ret = 0;

out:
	vmm_vmx_vmcs_leave(vcpu);
	return ret;
}

static void
vmm_vmx_inject_ud(struct vmm_vcpu *vcpu)
{
	struct vmm_cpuevent event;
	int ret __diagused;

	event.type = VMM_CPUEVENT_EXCP;
	event.vector = 6;
	event.error = 0;

	ret = vmm_vmx_vcpu_inject(vcpu, &event);
	OS_ASSERT(ret == 0);
}

static void
vmm_vmx_inject_gp(struct vmm_vcpu *vcpu)
{
	struct vmm_cpuevent event;
	int ret __diagused;

	event.type = VMM_CPUEVENT_EXCP;
	event.vector = 13;
	event.error = 0;

	ret = vmm_vmx_vcpu_inject(vcpu, &event);
	OS_ASSERT(ret == 0);
}

static inline int
vmm_vmx_vcpu_event_commit(struct vmm_vcpu *vcpu)
{
	struct vmm_cpuevent event;

	if (__predict_true(!vcpu->event_pending)) {
		return 0;
	}
	event = vcpu->event;
	vcpu->event_pending = 0;
	return vmm_vmx_vcpu_inject(vcpu, &event);
}

int
vmm_vmx_vcpu_inject_interrupt(struct vmm_vcpu *vcpu, uint8_t vector)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	struct vmm_cpuevent event = {
		.type = VMM_CPUEVENT_INTR,
		.vector = vector,
	};

	if (cpudata->evt_pending)
		return EBUSY;
	return vmm_vmx_vcpu_inject(vcpu, &event);
}

bool
vmm_vmx_vcpu_interrupt_allowed(struct vmm_vcpu *vcpu)
{
	uint64_t intstate;
	uint64_t rflags;
	bool allowed;

	vmm_vmx_vmcs_enter(vcpu);
	rflags = vmm_vmx_vmread(VMCS_GUEST_RFLAGS);
	intstate = vmm_vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
	allowed = (rflags & PSL_I) != 0 &&
	    (intstate & (INT_STATE_STI | INT_STATE_MOVSS)) == 0;
	vmm_vmx_vmcs_leave(vcpu);
	return allowed;
}

void
vmm_vmx_vcpu_request_interrupt_window(struct vmm_vcpu *vcpu)
{
	vmm_vmx_vmcs_enter(vcpu);
	vmm_vmx_event_waitexit_enable(vcpu, false);
	vmm_vmx_vmcs_leave(vcpu);
}

void
vmm_vmx_vcpu_set_rvi(struct vmm_vcpu *vcpu, uint8_t vector)
{
	uint64_t status;

	vmm_vmx_vmcs_enter(vcpu);
	status = vmm_vmx_vmread(VMCS_GUEST_INTR_STATUS);
	status &= ~0xffULL;
	status |= vector;
	vmm_vmx_vmwrite(VMCS_GUEST_INTR_STATUS, status);
	vmm_vmx_vmcs_leave(vcpu);
}

static inline void
vmm_vmx_inkernel_advance(void)
{
	uint64_t rip, inslen, intstate, rflags;

	/*
	 * Maybe we should also apply single-stepping and debug exceptions.
	 * Matters for guest-ring3, because it can execute 'cpuid' under a
	 * debugger.
	 */

	inslen = vmm_vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmm_vmx_vmread(VMCS_GUEST_RIP);
	vmm_vmx_vmwrite(VMCS_GUEST_RIP, rip + inslen);

	rflags = vmm_vmx_vmread(VMCS_GUEST_RFLAGS);
	vmm_vmx_vmwrite(VMCS_GUEST_RFLAGS, rflags & ~PSL_RF);

	intstate = vmm_vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
	vmm_vmx_vmwrite(VMCS_GUEST_INTERRUPTIBILITY,
	    intstate & ~(INT_STATE_STI|INT_STATE_MOVSS));
}

static void
vmm_vmx_exit_invalid(struct vmm_cpuexit *exit, uint64_t code)
{
	exit->u.inv.hwcode = code;
	exit->reason = VMM_CPUEXIT_INVALID;
}

static void
vmm_vmx_exit_exc_nmi(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	uint64_t qual;

	qual = vmm_vmx_vmread(VMCS_EXIT_INTR_INFO);

	if ((qual & INTR_INFO_VALID) == 0) {
		goto error;
	}
	if (__SHIFTOUT(qual, INTR_INFO_TYPE) != INTR_TYPE_NMI) {
		goto error;
	}

	exit->reason = VMM_CPUEXIT_NONE;
	return;

error:
	vmm_vmx_exit_invalid(exit, VMCS_EXITCODE_EXC_NMI);
}

#define VMM_VMX_CPUID_MAX_BASIC		0x16
#define VMM_VMX_CPUID_MAX_HYPERVISOR	0x40000000
#define VMM_VMX_CPUID_MAX_EXTENDED		0x80000008
static uint32_t vmm_vmx_cpuid_max_basic __read_mostly;
static uint32_t vmm_vmx_cpuid_max_extended __read_mostly;

static void
vmm_vmx_inkernel_exec_cpuid(struct vmm_vmx_cpudata *cpudata, uint32_t eax, uint32_t ecx)
{
	cpuid_desc_t descs;

	x86_get_cpuid2(eax, ecx, &descs);
	cpudata->gprs[VMM_X64_GPR_RAX] = descs.eax;
	cpudata->gprs[VMM_X64_GPR_RBX] = descs.ebx;
	cpudata->gprs[VMM_X64_GPR_RCX] = descs.ecx;
	cpudata->gprs[VMM_X64_GPR_RDX] = descs.edx;
}

static void
vmm_vmx_inkernel_handle_cpuid(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    uint32_t eax, uint32_t ecx)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	unsigned int ncpus;
	uint32_t clevel;
	uint64_t cr4;

	if (eax < 0x40000000) {
		if (__predict_false(eax > vmm_vmx_cpuid_max_basic)) {
			eax = vmm_vmx_cpuid_max_basic;
			vmm_vmx_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	} else if (eax < 0x80000000) {
		if (__predict_false(eax > VMM_VMX_CPUID_MAX_HYPERVISOR)) {
			eax = vmm_vmx_cpuid_max_basic;
			vmm_vmx_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	} else {
		if (__predict_false(eax > vmm_vmx_cpuid_max_extended)) {
			eax = vmm_vmx_cpuid_max_basic;
			vmm_vmx_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	}

	switch (eax) {
	case 0x00000000:
		cpudata->gprs[VMM_X64_GPR_RAX] = vmm_vmx_cpuid_max_basic;
		break;
	case 0x00000001:
		cpudata->gprs[VMM_X64_GPR_RAX] &= vmm_vmx_cpuid_00000001.eax;

		cpudata->gprs[VMM_X64_GPR_RBX] &= ~CPUID_0_01_EBX_LOCAL_APIC_ID;
		cpudata->gprs[VMM_X64_GPR_RBX] |= __SHIFTIN(vcpu->id,
		    CPUID_0_01_EBX_LOCAL_APIC_ID);

		ncpus = os_atomic_load_uint(&mach->vcpu_count);
		cpudata->gprs[VMM_X64_GPR_RBX] &= ~CPUID_0_01_EBX_HTT_CORES;
		cpudata->gprs[VMM_X64_GPR_RBX] |= __SHIFTIN(ncpus,
		    CPUID_0_01_EBX_HTT_CORES);

		cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_vmx_cpuid_00000001.ecx;
		cpudata->gprs[VMM_X64_GPR_RCX] |= CPUID_0_01_ECX_RAZ;
		if (!(vmm_vmx_procbased_ctls2 & PROC_CTLS2_INVPCID_ENABLE)) {
			cpudata->gprs[VMM_X64_GPR_RCX] &= ~CPUID_0_01_ECX_PCID;
		}

		cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_vmx_cpuid_00000001.edx;

		/* CPUID_0_01_ECX_OSXSAVE depends on CR4. */
		cr4 = vmm_vmx_vmread(VMCS_GUEST_CR4);
		if (!(cr4 & CR4_OSXSAVE)) {
			cpudata->gprs[VMM_X64_GPR_RCX] &= ~CPUID_0_01_ECX_OSXSAVE;
		}
		break;
	case 0x00000002:
		break;
	case 0x00000003:
		cpudata->gprs[VMM_X64_GPR_RAX] = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000004: /* Deterministic Cache Parameters */
		ncpus = os_atomic_load_uint(&mach->vcpu_count);
		clevel = __SHIFTOUT(cpudata->gprs[VMM_X64_GPR_RAX],
		    CPUID_0_04_EAX_CACHELEVEL);

		cpudata->gprs[VMM_X64_GPR_RAX] &= ~CPUID_0_04_EAX_SHARING;
		if (clevel >= 3) {
			/* L3 and above: all CPUs. */
			cpudata->gprs[VMM_X64_GPR_RAX] |=
			    __SHIFTIN(ncpus - 1, CPUID_0_04_EAX_SHARING);
		} else {
			/* L2 and below: one LP per CPU. */
			cpudata->gprs[VMM_X64_GPR_RAX] |=
			    __SHIFTIN(0, CPUID_0_04_EAX_SHARING);
		}

		cpudata->gprs[VMM_X64_GPR_RAX] &= ~CPUID_0_04_EAX_CORE_P_PKG;
		cpudata->gprs[VMM_X64_GPR_RAX] |=
		    __SHIFTIN(ncpus - 1, CPUID_0_04_EAX_CORE_P_PKG);
		break;
	case 0x00000005: /* MONITOR/MWAIT */
	case 0x00000006: /* Thermal and Power Management */
		cpudata->gprs[VMM_X64_GPR_RAX] = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000007: /* Structured Extended Feature Flags Enumeration */
		switch (ecx) {
		case 0:
			cpudata->gprs[VMM_X64_GPR_RAX] = 0;
			cpudata->gprs[VMM_X64_GPR_RBX] &= vmm_vmx_cpuid_00000007.ebx;
			cpudata->gprs[VMM_X64_GPR_RBX] |=
			    CPUID_0_07_EBX_TSC_ADJUST;
			cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_vmx_cpuid_00000007.ecx;
			cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_vmx_cpuid_00000007.edx;
			if (!(vmm_vmx_procbased_ctls2 & PROC_CTLS2_INVPCID_ENABLE)) {
				cpudata->gprs[VMM_X64_GPR_RBX] &= ~CPUID_0_07_EBX_INVPCID;
			}
			if (!vmm_vmx_cpu_has_arch_cap) {
				cpudata->gprs[VMM_X64_GPR_RDX] &= ~CPUID_0_07_EDX_ARCH_CAP;
			}
			break;
		default:
			cpudata->gprs[VMM_X64_GPR_RAX] = 0;
			cpudata->gprs[VMM_X64_GPR_RBX] = 0;
			cpudata->gprs[VMM_X64_GPR_RCX] = 0;
			cpudata->gprs[VMM_X64_GPR_RDX] = 0;
			break;
		}
		break;
	case 0x00000008: /* Empty */
	case 0x00000009: /* Direct Cache Access Information */
		cpudata->gprs[VMM_X64_GPR_RAX] = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x0000000A: /* Architectural Performance Monitoring */
		cpudata->gprs[VMM_X64_GPR_RAX] = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x0000000B: /* Extended Topology Enumeration */
		switch (ecx) {
		case 0: /* Threads */
			cpudata->gprs[VMM_X64_GPR_RAX] = 0;
			cpudata->gprs[VMM_X64_GPR_RBX] = 0;
			cpudata->gprs[VMM_X64_GPR_RCX] =
			    __SHIFTIN(ecx, CPUID_0_0B_ECX_LVLNUM) |
			    __SHIFTIN(CPUID_0_0B_ECX_LVLTYPE_SMT, CPUID_0_0B_ECX_LVLTYPE);
			cpudata->gprs[VMM_X64_GPR_RDX] = vcpu->id;
			break;
		case 1: /* Cores */
			ncpus = os_atomic_load_uint(&mach->vcpu_count);
			cpudata->gprs[VMM_X64_GPR_RAX] = ilog2(ncpus);
			cpudata->gprs[VMM_X64_GPR_RBX] = ncpus;
			cpudata->gprs[VMM_X64_GPR_RCX] =
			    __SHIFTIN(ecx, CPUID_0_0B_ECX_LVLNUM) |
			    __SHIFTIN(CPUID_0_0B_ECX_LVLTYPE_CORE, CPUID_0_0B_ECX_LVLTYPE);
			cpudata->gprs[VMM_X64_GPR_RDX] = vcpu->id;
			break;
		default:
			cpudata->gprs[VMM_X64_GPR_RAX] = 0;
			cpudata->gprs[VMM_X64_GPR_RBX] = 0;
			cpudata->gprs[VMM_X64_GPR_RCX] = 0; /* LVLTYPE_INVAL */
			cpudata->gprs[VMM_X64_GPR_RDX] = 0;
			break;
		}
		break;
	case 0x0000000C: /* Empty */
		cpudata->gprs[VMM_X64_GPR_RAX] = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x0000000D: /* Processor Extended State Enumeration */
		if (vmm_vmx_xcr0_mask == 0) {
			break;
		}
		switch (ecx) {
		case 0:
			/* Supported XCR0 bits. */
			cpudata->gprs[VMM_X64_GPR_RAX] = vmm_vmx_xcr0_mask & 0xFFFFFFFF;
			cpudata->gprs[VMM_X64_GPR_RDX] = vmm_vmx_xcr0_mask >> 32;
			/* XSAVE size for currently enabled XCR0 features. */
			cpudata->gprs[VMM_X64_GPR_RBX] = vmm_vmx_xsave_size(cpudata->gxcr0);
			/* XSAVE size for all supported XCR0 features. */
			cpudata->gprs[VMM_X64_GPR_RCX] = vmm_vmx_xsave_size(vmm_vmx_xcr0_mask);
			break;
		case 1:
			cpudata->gprs[VMM_X64_GPR_RAX] &=
			    (CPUID_0_0D_ECX1_EAX_XSAVEOPT |
			     CPUID_0_0D_ECX1_EAX_XGETBV1);
			cpudata->gprs[VMM_X64_GPR_RBX] = 0;
			cpudata->gprs[VMM_X64_GPR_RCX] = 0;
			cpudata->gprs[VMM_X64_GPR_RDX] = 0;
			break;
		default:
			cpudata->gprs[VMM_X64_GPR_RAX] = 0;
			cpudata->gprs[VMM_X64_GPR_RBX] = 0;
			cpudata->gprs[VMM_X64_GPR_RCX] = 0;
			cpudata->gprs[VMM_X64_GPR_RDX] = 0;
			break;
		}
		break;
	case 0x0000000E: /* Empty */
	case 0x0000000F: /* Intel RDT Monitoring Enumeration */
	case 0x00000010: /* Intel RDT Allocation Enumeration */
		cpudata->gprs[VMM_X64_GPR_RAX] = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000011: /* Empty */
	case 0x00000012: /* Intel SGX Capability Enumeration */
	case 0x00000013: /* Empty */
	case 0x00000014: /* Intel Processor Trace Enumeration */
		cpudata->gprs[VMM_X64_GPR_RAX] = 0;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000015: /* TSC and Nominal Core Crystal Clock Information */
	case 0x00000016: /* Processor Frequency Information */
		break;

	case 0x40000000: /* Hypervisor Information */
		cpudata->gprs[VMM_X64_GPR_RAX] = VMM_VMX_CPUID_MAX_HYPERVISOR;
		cpudata->gprs[VMM_X64_GPR_RBX] = 0;
		cpudata->gprs[VMM_X64_GPR_RCX] = 0;
		cpudata->gprs[VMM_X64_GPR_RDX] = 0;
		memcpy(&cpudata->gprs[VMM_X64_GPR_RBX], "___ ", 4);
	memcpy(&cpudata->gprs[VMM_X64_GPR_RCX], "VMM ", 4);
		memcpy(&cpudata->gprs[VMM_X64_GPR_RDX], " ___", 4);
		break;

	case 0x80000000:
		cpudata->gprs[VMM_X64_GPR_RAX] = vmm_vmx_cpuid_max_extended;
		break;
	case 0x80000001:
		cpudata->gprs[VMM_X64_GPR_RAX] &= vmm_vmx_cpuid_80000001.eax;
		cpudata->gprs[VMM_X64_GPR_RBX] &= vmm_vmx_cpuid_80000001.ebx;
		cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_vmx_cpuid_80000001.ecx;
		cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_vmx_cpuid_80000001.edx;
		break;
	case 0x80000002: /* Processor Brand String */
	case 0x80000003: /* Processor Brand String */
	case 0x80000004: /* Processor Brand String */
	case 0x80000005: /* Reserved Zero */
	case 0x80000006: /* Cache Information */
		break;
	case 0x80000007: /* TSC Information */
		cpudata->gprs[VMM_X64_GPR_RAX] &= vmm_vmx_cpuid_80000007.eax;
		cpudata->gprs[VMM_X64_GPR_RBX] &= vmm_vmx_cpuid_80000007.ebx;
		cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_vmx_cpuid_80000007.ecx;
		cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_vmx_cpuid_80000007.edx;
		break;
	case 0x80000008: /* Address Sizes */
		cpudata->gprs[VMM_X64_GPR_RAX] &= vmm_vmx_cpuid_80000008.eax;
		cpudata->gprs[VMM_X64_GPR_RBX] &= vmm_vmx_cpuid_80000008.ebx;
		cpudata->gprs[VMM_X64_GPR_RCX] &= vmm_vmx_cpuid_80000008.ecx;
		cpudata->gprs[VMM_X64_GPR_RDX] &= vmm_vmx_cpuid_80000008.edx;
		break;

	default:
		break;
	}
}

static void
vmm_vmx_supported_cpuid_entry(struct vmm_cpuid_entry *entry)
{
	cpuid_desc_t descs;

	x86_get_cpuid2(entry->leaf, entry->subleaf, &descs);
	entry->eax = descs.eax;
	entry->ebx = descs.ebx;
	entry->ecx = descs.ecx;
	entry->edx = descs.edx;

	switch (entry->leaf) {
	case 0x00000000:
		entry->eax = vmm_vmx_cpuid_max_basic;
		break;
	case 0x00000001:
		entry->eax &= vmm_vmx_cpuid_00000001.eax;
		entry->ebx &= ~(CPUID_0_01_EBX_LOCAL_APIC_ID |
		    CPUID_0_01_EBX_HTT_CORES);
		entry->ecx &= vmm_vmx_cpuid_00000001.ecx;
		entry->ecx |= CPUID_0_01_ECX_RAZ;
		entry->edx &= vmm_vmx_cpuid_00000001.edx;
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
		entry->ebx &= vmm_vmx_cpuid_00000007.ebx;
		entry->ebx |= CPUID_0_07_EBX_TSC_ADJUST;
		entry->ecx &= vmm_vmx_cpuid_00000007.ecx;
		entry->edx &= vmm_vmx_cpuid_00000007.edx;
		break;
	case 0x0000000D:
		if (vmm_vmx_xcr0_mask == 0)
			break;
		if (entry->subleaf == 0) {
			entry->eax = vmm_vmx_xcr0_mask & 0xFFFFFFFF;
			entry->edx = vmm_vmx_xcr0_mask >> 32;
			entry->ebx = vmm_vmx_xsave_size(vmm_vmx_xcr0_mask);
			entry->ecx = vmm_vmx_xsave_size(vmm_vmx_xcr0_mask);
		} else {
			entry->eax &= CPUID_0_0D_ECX1_EAX_XSAVEOPT |
			    CPUID_0_0D_ECX1_EAX_XSAVEC |
			    CPUID_0_0D_ECX1_EAX_XGETBV;
			entry->ebx = vmm_vmx_xsave_size(vmm_vmx_xcr0_mask);
			entry->ecx = 0;
			entry->edx = 0;
		}
		break;
	case 0x40000000:
		entry->eax = VMM_VMX_CPUID_MAX_HYPERVISOR;
		entry->ebx = 0;
		entry->ecx = 0;
		entry->edx = 0;
		memcpy(&entry->ebx, "___ ", 4);
		memcpy(&entry->ecx, "VMM ", 4);
		memcpy(&entry->edx, " ___", 4);
		break;
	case 0x80000000:
		entry->eax = vmm_vmx_cpuid_max_extended;
		break;
	case 0x80000001:
		entry->eax &= vmm_vmx_cpuid_80000001.eax;
		entry->ebx &= vmm_vmx_cpuid_80000001.ebx;
		entry->ecx &= vmm_vmx_cpuid_80000001.ecx;
		entry->edx &= vmm_vmx_cpuid_80000001.edx;
		break;
	case 0x80000007:
		entry->eax &= vmm_vmx_cpuid_80000007.eax;
		entry->ebx &= vmm_vmx_cpuid_80000007.ebx;
		entry->ecx &= vmm_vmx_cpuid_80000007.ecx;
		entry->edx &= vmm_vmx_cpuid_80000007.edx;
		break;
	case 0x80000008:
		entry->eax &= vmm_vmx_cpuid_80000008.eax;
		entry->ebx &= vmm_vmx_cpuid_80000008.ebx;
		entry->ecx = 0;
		entry->edx &= vmm_vmx_cpuid_80000008.edx;
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

/*
 * A frontend may supply its own CPU template, but it must not advertise an
 * architectural feature for which this backend has no corresponding state or
 * emulation.  In particular, QEMU templates can enable MTRR even though the
 * supported-CPUID query deliberately hides it.
 */
static void
vmm_vmx_filter_configured_cpuid_entry(struct vmm_cpuid_entry *entry)
{
	struct vmm_cpuid_entry supported;

	supported = *entry;
	vmm_vmx_supported_cpuid_entry(&supported);

	switch (entry->leaf) {
	case 0x00000001:
		entry->ecx &= supported.ecx;
		entry->edx &= supported.edx;
		break;
	case 0x00000007:
		if (entry->subleaf == 0) {
			entry->eax = supported.eax;
			entry->ebx &= supported.ebx;
			entry->ecx &= supported.ecx;
			entry->edx &= supported.edx;
		}
		break;
	case 0x80000001:
		entry->ecx &= supported.ecx;
		entry->edx &= supported.edx;
		break;
	case 0x80000007:
		entry->edx &= supported.edx;
		break;
	case 0x80000008:
		entry->ebx &= supported.ebx;
		break;
	default:
		break;
	}
}

static size_t
vmm_vmx_supported_cpuid_count(void)
{
	return (size_t)vmm_vmx_cpuid_max_basic + 1 +
	    (vmm_vmx_cpuid_max_basic >= 0x0000000D ? 1 : 0) + 1 +
	    (size_t)(vmm_vmx_cpuid_max_extended - 0x80000000) + 1;
}

int
vmm_vmx_get_supported_cpuid(struct vmm_cpuid_entry *entries,
	size_t *entry_count)
{
	uint32_t leaf;
	size_t count;
	size_t index;

	if (entry_count == NULL)
		return EINVAL;
	count = vmm_vmx_supported_cpuid_count();
	if (entries == NULL) {
		*entry_count = count;
		return 0;
	}
	if (*entry_count < count) {
		*entry_count = count;
		return E2BIG;
	}

	index = 0;
	for (leaf = 0; leaf <= vmm_vmx_cpuid_max_basic; ++leaf) {
		entries[index].leaf = leaf;
		entries[index].subleaf = 0;
		entries[index].flags =
		    leaf == 0x00000007 || leaf == 0x0000000D ?
		    VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF : 0;
		vmm_vmx_supported_cpuid_entry(&entries[index]);
		++index;
		if (leaf == 0x0000000D) {
			entries[index].leaf = leaf;
			entries[index].subleaf = 1;
			entries[index].flags =
			    VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF;
			vmm_vmx_supported_cpuid_entry(&entries[index]);
			++index;
		}
	}
	entries[index].leaf = 0x40000000;
	entries[index].subleaf = 0;
	entries[index].flags = 0;
	vmm_vmx_supported_cpuid_entry(&entries[index]);
	++index;
	for (leaf = 0x80000000; leaf <= vmm_vmx_cpuid_max_extended; ++leaf) {
		entries[index].leaf = leaf;
		entries[index].subleaf = 0;
		entries[index].flags = 0;
		vmm_vmx_supported_cpuid_entry(&entries[index]);
		++index;
	}
	KKASSERT(index == count);
	*entry_count = count;
	return 0;
}

static void
vmm_vmx_exit_insn(struct vmm_cpuexit *exit, uint64_t reason)
{
	uint64_t inslen, rip;

	inslen = vmm_vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmm_vmx_vmread(VMCS_GUEST_RIP);
	exit->u.insn.npc = rip + inslen;
	exit->reason = reason;
}

static void
vmm_vmx_exit_cpuid(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	const struct vmm_cpuid_entry *entry;
	const struct vmm_cpuid_entry *leaf_entry;
	uint32_t eax, ecx;
	size_t i;

	eax = (cpudata->gprs[VMM_X64_GPR_RAX] & 0xFFFFFFFF);
	ecx = (cpudata->gprs[VMM_X64_GPR_RCX] & 0xFFFFFFFF);
	vmm_vmx_inkernel_exec_cpuid(cpudata, eax, ecx);
	vmm_vmx_inkernel_handle_cpuid(mach, vcpu, eax, ecx);

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
		cpudata->gprs[VMM_X64_GPR_RAX] = entry->eax;
		cpudata->gprs[VMM_X64_GPR_RBX] = entry->ebx;
		cpudata->gprs[VMM_X64_GPR_RCX] = entry->ecx;
		cpudata->gprs[VMM_X64_GPR_RDX] = entry->edx;
	}

	/* CPUID topology is a property of the live VMM machine. */
	if (eax == 0x00000001) {
		cpudata->gprs[VMM_X64_GPR_RBX] &=
		    ~(CPUID_0_01_EBX_LOCAL_APIC_ID | CPUID_0_01_EBX_HTT_CORES);
		cpudata->gprs[VMM_X64_GPR_RBX] |= __SHIFTIN(vcpu->id,
		    CPUID_0_01_EBX_LOCAL_APIC_ID);
		lwkt_gettoken(&mach->token);
		cpudata->gprs[VMM_X64_GPR_RBX] |= __SHIFTIN(mach->vcpu_count,
		    CPUID_0_01_EBX_HTT_CORES);
		lwkt_reltoken(&mach->token);
	}

	vmm_vmx_inkernel_advance();
	exit->reason = VMM_CPUEXIT_NONE;
}

static void
vmm_vmx_exit_hlt(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t rflags;

	if (cpudata->int_window_exit) {
		rflags = vmm_vmx_vmread(VMCS_GUEST_RFLAGS);
		if (rflags & PSL_I) {
			vmm_vmx_event_waitexit_disable(vcpu, false);
		}
	}

	vmm_vmx_inkernel_advance();
	exit->reason = VMM_CPUEXIT_HALTED;
}

#define VMM_VMX_QUAL_CR_NUM		__BITS(3,0)
#define VMM_VMX_QUAL_CR_TYPE	__BITS(5,4)
#define		CR_TYPE_WRITE	0
#define		CR_TYPE_READ	1
#define		CR_TYPE_CLTS	2
#define		CR_TYPE_LMSW	3
#define VMM_VMX_QUAL_CR_LMSW_OPMEM	__BIT(6)
#define VMM_VMX_QUAL_CR_GPR		__BITS(11,8)
#define VMM_VMX_QUAL_CR_LMSW_SRC	__BIT(31,16)

static inline int
vmm_vmx_check_cr(uint64_t crval, uint64_t fixed0, uint64_t fixed1)
{
	/* Bits set to 1 in fixed0 are fixed to 1. */
	if ((crval & fixed0) != fixed0) {
		return -1;
	}
	/* Bits set to 0 in fixed1 are fixed to 0. */
	if (crval & ~fixed1) {
		return -1;
	}
	return 0;
}

static int
vmm_vmx_inkernel_handle_cr0(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    uint64_t qual)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t type, gpr, oldcr0, realcr0, fakecr0;
	uint64_t efer, ctls1;

	type = __SHIFTOUT(qual, VMM_VMX_QUAL_CR_TYPE);
	if (type != CR_TYPE_WRITE) {
		return -1;
	}

	gpr = __SHIFTOUT(qual, VMM_VMX_QUAL_CR_GPR);
	OS_ASSERT(gpr < 16);

	if (gpr == VMM_X64_GPR_RSP) {
		fakecr0 = vmm_vmx_vmread(VMCS_GUEST_RSP);
	} else {
		fakecr0 = cpudata->gprs[gpr];
	}
	fakecr0 |= CR0_ET; /* Force ET=1 for consistency. */

	realcr0 = (fakecr0 & ~CR0_FORCE_ZERO) | CR0_FORCE_ONE;
	if (vmm_vmx_check_cr(realcr0, vmm_vmx_cr0_fixed0, vmm_vmx_cr0_fixed1) == -1) {
		return -1;
	}

	/*
	 * XXX Handle 32bit PAE paging, need to set PDPTEs, fetched manually
	 * from CR3.
	 */

	if (realcr0 & CR0_PG) {
		ctls1 = vmm_vmx_vmread(VMCS_ENTRY_CTLS);
		efer = vmm_vmx_vmread(VMCS_GUEST_IA32_EFER);
		if (efer & EFER_LME) {
			ctls1 |= ENTRY_CTLS_LONG_MODE;
			efer |= EFER_LMA;
		} else {
			ctls1 &= ~ENTRY_CTLS_LONG_MODE;
			efer &= ~EFER_LMA;
		}
		vmm_vmx_vmwrite(VMCS_GUEST_IA32_EFER, efer);
		vmm_vmx_vmwrite(VMCS_ENTRY_CTLS, ctls1);
	}

	oldcr0 = (vmm_vmx_vmread(VMCS_CR0_SHADOW) & CR0_STATIC_MASK) |
	    (vmm_vmx_vmread(VMCS_GUEST_CR0) & ~CR0_STATIC_MASK);
	if ((oldcr0 ^ fakecr0) & CR0_TLB_FLUSH) {
		cpudata->gtlb_want_flush = true;
	}

	vmm_vmx_vmwrite(VMCS_CR0_SHADOW, fakecr0);
	vmm_vmx_vmwrite(VMCS_GUEST_CR0, realcr0);
	vmm_vmx_inkernel_advance();
	return 0;
}

static int
vmm_vmx_inkernel_handle_cr4(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    uint64_t qual)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t type, gpr, oldcr4, cr4;

	type = __SHIFTOUT(qual, VMM_VMX_QUAL_CR_TYPE);
	if (type != CR_TYPE_WRITE) {
		return -1;
	}

	gpr = __SHIFTOUT(qual, VMM_VMX_QUAL_CR_GPR);
	OS_ASSERT(gpr < 16);

	if (gpr == VMM_X64_GPR_RSP) {
		gpr = vmm_vmx_vmread(VMCS_GUEST_RSP);
	} else {
		gpr = cpudata->gprs[gpr];
	}

	if (gpr & CR4_INVALID) {
		return -1;
	}
	cr4 = gpr | CR4_VMXE;
	if (vmm_vmx_check_cr(cr4, vmm_vmx_cr4_fixed0, vmm_vmx_cr4_fixed1) == -1) {
		return -1;
	}

	oldcr4 = vmm_vmx_vmread(VMCS_GUEST_CR4);
	if ((oldcr4 ^ gpr) & CR4_TLB_FLUSH) {
		cpudata->gtlb_want_flush = true;
	}

	vmm_vmx_vmwrite(VMCS_GUEST_CR4, cr4);
	vmm_vmx_inkernel_advance();
	return 0;
}

static int
vmm_vmx_inkernel_handle_cr8(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    uint64_t qual)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t type, gpr;
	bool write;

	type = __SHIFTOUT(qual, VMM_VMX_QUAL_CR_TYPE);
	if (type == CR_TYPE_WRITE) {
		write = true;
	} else if (type == CR_TYPE_READ) {
		write = false;
	} else {
		return -1;
	}

	gpr = __SHIFTOUT(qual, VMM_VMX_QUAL_CR_GPR);
	OS_ASSERT(gpr < 16);

	if (write) {
		if (gpr == VMM_X64_GPR_RSP) {
			cpudata->gcr8 = vmm_vmx_vmread(VMCS_GUEST_RSP);
		} else {
			cpudata->gcr8 = cpudata->gprs[gpr];
		}
	} else {
		if (gpr == VMM_X64_GPR_RSP) {
			vmm_vmx_vmwrite(VMCS_GUEST_RSP, cpudata->gcr8);
		} else {
			cpudata->gprs[gpr] = cpudata->gcr8;
		}
	}

	vmm_vmx_inkernel_advance();
	return 0;
}

static void
vmm_vmx_exit_cr(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	uint64_t qual;
	int ret;

	exit->reason = VMM_CPUEXIT_NONE;

	qual = vmm_vmx_vmread(VMCS_EXIT_QUALIFICATION);

	switch (__SHIFTOUT(qual, VMM_VMX_QUAL_CR_NUM)) {
	case 0:
		ret = vmm_vmx_inkernel_handle_cr0(mach, vcpu, qual);
		break;
	case 4:
		ret = vmm_vmx_inkernel_handle_cr4(mach, vcpu, qual);
		break;
	case 8:
		ret = vmm_vmx_inkernel_handle_cr8(mach, vcpu, qual);
		break;
	default:
		ret = -1;
		break;
	}

	if (ret == -1) {
		vmm_vmx_inject_gp(vcpu);
	}
}

#define VMM_VMX_QUAL_IO_SIZE	__BITS(2,0)
#define		IO_SIZE_8	0
#define		IO_SIZE_16	1
#define		IO_SIZE_32	3
#define VMM_VMX_QUAL_IO_IN		__BIT(3)
#define VMM_VMX_QUAL_IO_STR		__BIT(4)
#define VMM_VMX_QUAL_IO_REP		__BIT(5)
#define VMM_VMX_QUAL_IO_DX		__BIT(6)
#define VMM_VMX_QUAL_IO_PORT	__BITS(31,16)

#define VMM_VMX_INFO_IO_ADRSIZE	__BITS(9,7)
#define		IO_ADRSIZE_16	0
#define		IO_ADRSIZE_32	1
#define		IO_ADRSIZE_64	2
#define VMM_VMX_INFO_IO_SEG		__BITS(17,15)

static void
vmm_vmx_exit_io(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	uint64_t qual, info, inslen, rip;

	qual = vmm_vmx_vmread(VMCS_EXIT_QUALIFICATION);
	info = vmm_vmx_vmread(VMCS_EXIT_INSTRUCTION_INFO);

	exit->reason = VMM_CPUEXIT_IO;

	exit->u.io.in = (qual & VMM_VMX_QUAL_IO_IN) != 0;
	exit->u.io.port = __SHIFTOUT(qual, VMM_VMX_QUAL_IO_PORT);

	OS_ASSERT(__SHIFTOUT(info, VMM_VMX_INFO_IO_SEG) < 6);
	exit->u.io.seg = __SHIFTOUT(info, VMM_VMX_INFO_IO_SEG);

	if (__SHIFTOUT(info, VMM_VMX_INFO_IO_ADRSIZE) == IO_ADRSIZE_64) {
		exit->u.io.address_size = 8;
	} else if (__SHIFTOUT(info, VMM_VMX_INFO_IO_ADRSIZE) == IO_ADRSIZE_32) {
		exit->u.io.address_size = 4;
	} else if (__SHIFTOUT(info, VMM_VMX_INFO_IO_ADRSIZE) == IO_ADRSIZE_16) {
		exit->u.io.address_size = 2;
	}

	if (__SHIFTOUT(qual, VMM_VMX_QUAL_IO_SIZE) == IO_SIZE_32) {
		exit->u.io.operand_size = 4;
	} else if (__SHIFTOUT(qual, VMM_VMX_QUAL_IO_SIZE) == IO_SIZE_16) {
		exit->u.io.operand_size = 2;
	} else if (__SHIFTOUT(qual, VMM_VMX_QUAL_IO_SIZE) == IO_SIZE_8) {
		exit->u.io.operand_size = 1;
	}

	exit->u.io.rep = (qual & VMM_VMX_QUAL_IO_REP) != 0;
	exit->u.io.str = (qual & VMM_VMX_QUAL_IO_STR) != 0;

	if (exit->u.io.in && exit->u.io.str) {
		exit->u.io.seg = VMM_X64_SEG_ES;
	}

	inslen = vmm_vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmm_vmx_vmread(VMCS_GUEST_RIP);
	exit->u.io.npc = rip + inslen;

	vmm_vmx_vcpu_state_provide(vcpu,
	    VMM_X64_STATE_GPRS | VMM_X64_STATE_SEGS |
	    VMM_X64_STATE_CRS | VMM_X64_STATE_MSRS);
}

static const uint64_t msr_ignore_list[] = {
	MSR_IA32_BIOS_SIGN_ID,
	MSR_IA32_PLATFORM_ID
};

static bool
vmm_vmx_inkernel_handle_msr(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t val;
	uint32_t msr;
	bool write;
	int error;
	size_t i;

	write = exit->reason == VMM_CPUEXIT_WRMSR;
	msr = write ? exit->u.wrmsr.msr : exit->u.rdmsr.msr;
	val = write ? exit->u.wrmsr.val : 0;
	if (cpudata->interrupt != NULL &&
	    vmm_vmx_interrupt_ops->vcpu_msr != NULL) {
		error = vmm_vmx_interrupt_ops->vcpu_msr(cpudata->interrupt,
		    write, msr, &val);
		if (error == 0) {
			if (!write) {
				cpudata->gprs[VMM_X64_GPR_RAX] = val & 0xffffffffU;
				cpudata->gprs[VMM_X64_GPR_RDX] = val >> 32;
			}
			goto handled;
		}
		if (error != ENOENT)
			goto error;
	}

	if (exit->reason == VMM_CPUEXIT_RDMSR) {
		if (exit->u.rdmsr.msr == MSR_TSC_ADJUST) {
			val = cpudata->gtsc_adjust;
			cpudata->gprs[VMM_X64_GPR_RAX] = val & 0xFFFFFFFF;
			cpudata->gprs[VMM_X64_GPR_RDX] = val >> 32;
			goto handled;
		}
		if (exit->u.rdmsr.msr == MSR_CR_PAT) {
			val = vmm_vmx_vmread(VMCS_GUEST_IA32_PAT);
			cpudata->gprs[VMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
			cpudata->gprs[VMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
		if (exit->u.rdmsr.msr == MSR_IA32_MISC_ENABLE) {
			val = cpudata->gmsr_misc_enable;
			cpudata->gprs[VMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
			cpudata->gprs[VMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
		if (exit->u.rdmsr.msr == MSR_IA32_ARCH_CAPABILITIES) {
			if (!vmm_vmx_cpu_has_arch_cap) {
				goto error;
			}
			val = rdmsr(MSR_IA32_ARCH_CAPABILITIES);
			val &= (IA32_ARCH_RDCL_NO |
			    IA32_ARCH_SSB_NO |
			    IA32_ARCH_MDS_NO |
			    IA32_ARCH_TAA_NO |
			    IA32_ARCH_SBDR_SSDP_NO |
			    IA32_ARCH_FBSDP_NO |
			    IA32_ARCH_PSDP_NO |
			    IA32_ARCH_BHI_NO |
			    IA32_ARCH_PBRSB_NO |
			    IA32_ARCH_GDS_NO |
			    IA32_ARCH_RFDS_NO);
			cpudata->gprs[VMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
			cpudata->gprs[VMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
		for (i = 0; i < __arraycount(msr_ignore_list); i++) {
			if (msr_ignore_list[i] != exit->u.rdmsr.msr)
				continue;
			val = 0;
			cpudata->gprs[VMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
			cpudata->gprs[VMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
	} else {
		if (exit->u.wrmsr.msr == MSR_TSC) {
			val = exit->u.wrmsr.val - rdtsc();
			cpudata->gtsc_adjust += val - cpudata->gtsc_offset;
			cpudata->gtsc_offset = val;
			cpudata->gtsc_want_update = true;
			goto handled;
		}
		if (exit->u.wrmsr.msr == MSR_TSC_ADJUST) {
			cpudata->gtsc_offset += exit->u.wrmsr.val -
			    cpudata->gtsc_adjust;
			cpudata->gtsc_adjust = exit->u.wrmsr.val;
			cpudata->gtsc_want_update = true;
			goto handled;
		}
		if (exit->u.wrmsr.msr == MSR_CR_PAT) {
			val = exit->u.wrmsr.val;
			if (__predict_false(!vmm_vmx_pat_validate(val))) {
				goto error;
			}
			vmm_vmx_vmwrite(VMCS_GUEST_IA32_PAT, val);
			goto handled;
		}
		if (exit->u.wrmsr.msr == MSR_IA32_MISC_ENABLE) {
			/* Don't care. */
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
	vmm_vmx_inkernel_advance();
	return true;

error:
	vmm_vmx_inject_gp(vcpu);
	return true;
}

static void
vmm_vmx_exit_rdmsr(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t inslen, rip;

	exit->reason = VMM_CPUEXIT_RDMSR;
	exit->u.rdmsr.msr = (cpudata->gprs[VMM_X64_GPR_RCX] & 0xFFFFFFFF);

	if (vmm_vmx_inkernel_handle_msr(mach, vcpu, exit)) {
		exit->reason = VMM_CPUEXIT_NONE;
		return;
	}

	inslen = vmm_vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmm_vmx_vmread(VMCS_GUEST_RIP);
	exit->u.rdmsr.npc = rip + inslen;

	vmm_vmx_vcpu_state_provide(vcpu, VMM_X64_STATE_GPRS);
}

static void
vmm_vmx_exit_wrmsr(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t rdx, rax, inslen, rip;

	rdx = cpudata->gprs[VMM_X64_GPR_RDX];
	rax = cpudata->gprs[VMM_X64_GPR_RAX];

	exit->reason = VMM_CPUEXIT_WRMSR;
	exit->u.wrmsr.msr = (cpudata->gprs[VMM_X64_GPR_RCX] & 0xFFFFFFFF);
	exit->u.wrmsr.val = (rdx << 32) | (rax & 0xFFFFFFFF);

	if (vmm_vmx_inkernel_handle_msr(mach, vcpu, exit)) {
		exit->reason = VMM_CPUEXIT_NONE;
		return;
	}

	inslen = vmm_vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmm_vmx_vmread(VMCS_GUEST_RIP);
	exit->u.wrmsr.npc = rip + inslen;

	vmm_vmx_vcpu_state_provide(vcpu, VMM_X64_STATE_GPRS);
}

static void
vmm_vmx_exit_xsetbv(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct vmm_cpuexit *exit)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint64_t val;

	exit->reason = VMM_CPUEXIT_NONE;

	val = (cpudata->gprs[VMM_X64_GPR_RDX] << 32) |
	    (cpudata->gprs[VMM_X64_GPR_RAX] & 0xFFFFFFFF);

	if (__predict_false(cpudata->gprs[VMM_X64_GPR_RCX] != 0)) {
		goto error;
	} else if (__predict_false((val & ~vmm_vmx_xcr0_mask) != 0)) {
		goto error;
	} else if (__predict_false((val & XCR0_X87) == 0)) {
		goto error;
	}

	cpudata->gxcr0 = val;

	vmm_vmx_inkernel_advance();
	return;

error:
	vmm_vmx_inject_gp(vcpu);
}

#define VMM_VMX_EPT_VIOLATION_READ		__BIT(0)
#define VMM_VMX_EPT_VIOLATION_WRITE		__BIT(1)
#define VMM_VMX_EPT_VIOLATION_EXECUTE	__BIT(2)

static void
vmm_vmx_exit_epf(struct vmm_vcpu *vcpu, struct vmm_cpuexit *exit)
{
	uint64_t perm;
	gpaddr_t gpa;

	gpa = vmm_vmx_vmread(VMCS_GUEST_PHYSICAL_ADDRESS);

	exit->reason = VMM_CPUEXIT_MEMORY;
	bzero(&exit->u.mem, sizeof(exit->u.mem));
	perm = vmm_vmx_vmread(VMCS_EXIT_QUALIFICATION);
	if (perm & VMM_VMX_EPT_VIOLATION_WRITE)
		exit->u.mem.prot = VM_PROT_WRITE;
	else if (perm & VMM_VMX_EPT_VIOLATION_EXECUTE)
		exit->u.mem.prot = VM_PROT_EXECUTE;
	else
		exit->u.mem.prot = VM_PROT_READ;
	exit->u.mem.gpa = gpa;
}

/* -------------------------------------------------------------------------- */

static void
vmm_vmx_vcpu_guest_fpu_enter(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	x86_curthread_save_fpu();

	x86_restore_fpu(&cpudata->gxsave, vmm_vmx_xcr0_mask);
	if (vmm_vmx_xcr0_mask != 0) {
		x86_set_xcr(0, cpudata->gxcr0);
	}
}

static void
vmm_vmx_vcpu_guest_fpu_leave(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	if (vmm_vmx_xcr0_mask != 0) {
		x86_set_xcr(0, vmm_vmx_global_hstate.xcr0);
	}
	x86_save_fpu(&cpudata->gxsave, vmm_vmx_xcr0_mask);

	x86_curthread_restore_fpu();
}

static void
vmm_vmx_vcpu_guest_dbregs_enter(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	x86_curthread_save_dbregs(cpudata->hstate.drs);

	x86_set_dr7(0);

	x86_set_dr0(cpudata->drs[VMM_X64_DR_DR0]);
	x86_set_dr1(cpudata->drs[VMM_X64_DR_DR1]);
	x86_set_dr2(cpudata->drs[VMM_X64_DR_DR2]);
	x86_set_dr3(cpudata->drs[VMM_X64_DR_DR3]);
	x86_set_dr6(cpudata->drs[VMM_X64_DR_DR6]);
}

static void
vmm_vmx_vcpu_guest_dbregs_leave(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	cpudata->drs[VMM_X64_DR_DR0] = x86_get_dr0();
	cpudata->drs[VMM_X64_DR_DR1] = x86_get_dr1();
	cpudata->drs[VMM_X64_DR_DR2] = x86_get_dr2();
	cpudata->drs[VMM_X64_DR_DR3] = x86_get_dr3();
	cpudata->drs[VMM_X64_DR_DR6] = x86_get_dr6();

	x86_curthread_restore_dbregs(cpudata->hstate.drs);
}

static void
vmm_vmx_vcpu_guest_misc_enter(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	/* This gets restored automatically by the CPU. */
	vmm_vmx_vmwrite(VMCS_HOST_IDTR_BASE, (uint64_t)os_curcpu_idt());
	vmm_vmx_vmwrite(VMCS_HOST_FS_BASE, rdmsr(MSR_FSBASE));
	vmm_vmx_vmwrite(VMCS_HOST_CR3, x86_get_cr3());
	vmm_vmx_vmwrite(VMCS_HOST_CR4, x86_get_cr4());

	/* Save the percpu host state. */
	cpudata->hstate.kernelgsbase = rdmsr(MSR_KERNELGSBASE);
}

static void
vmm_vmx_vcpu_guest_misc_leave(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	/* Restore the global host state. */
	wrmsr(MSR_STAR, vmm_vmx_global_hstate.star);
	wrmsr(MSR_LSTAR, vmm_vmx_global_hstate.lstar);
	wrmsr(MSR_CSTAR, vmm_vmx_global_hstate.cstar);
	wrmsr(MSR_SFMASK, vmm_vmx_global_hstate.sfmask);

	/* Restore the percpu host state. */
	wrmsr(MSR_KERNELGSBASE, cpudata->hstate.kernelgsbase);
}

/* -------------------------------------------------------------------------- */

#define VMM_VMX_INVVPID_ADDRESS		0
#define VMM_VMX_INVVPID_CONTEXT		1
#define VMM_VMX_INVVPID_ALL			2
#define VMM_VMX_INVVPID_CONTEXT_NOGLOBAL	3

#define VMM_VMX_INVEPT_CONTEXT		1
#define VMM_VMX_INVEPT_ALL			2

static inline void
vmm_vmx_gtlb_catchup(struct vmm_vcpu *vcpu, int hcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	if (cpudata->hcpu_last != hcpu) {
		cpudata->gtlb_want_flush = true;
	}
}

static inline void
vmm_vmx_htlb_catchup(struct vmm_vcpu *vcpu, int hcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	struct ept_desc ept_desc;

	if (__predict_true(!os_cpuset_isset(cpudata->htlb_want_flush, hcpu))) {
		return;
	}

	ept_desc.eptp = vmm_vmx_vmread(VMCS_EPTP);
	ept_desc.mbz = 0;
	vmm_vmx_invept(vmm_vmx_ept_flush_op, &ept_desc);
	os_cpuset_clear(cpudata->htlb_want_flush, hcpu);
}

static inline uint64_t
vmm_vmx_htlb_flush(struct vmm_machine *mach, struct vmm_vmx_cpudata *cpudata)
{
	struct ept_desc ept_desc;
	uint64_t machgen;

	clear_xinvltlb();
	machgen = vmspace_pmap(mach->vmspace)->pm_invgen;
	if (__predict_true(machgen == cpudata->vcpu_htlb_gen &&
	    !cpudata->htlb_force_flush)) {
		return machgen;
	}

	os_cpuset_setrunning(cpudata->htlb_want_flush);

	ept_desc.eptp = vmm_vmx_vmread(VMCS_EPTP);
	ept_desc.mbz = 0;
	vmm_vmx_invept(vmm_vmx_ept_flush_op, &ept_desc);

	return machgen;
}

static inline void
vmm_vmx_htlb_flush_ack(struct vmm_vmx_cpudata *cpudata, uint64_t machgen)
{
	cpudata->vcpu_htlb_gen = machgen;
	os_cpuset_clear(cpudata->htlb_want_flush, os_curcpu_number());
	cpudata->htlb_force_flush = false;
}

static inline void
vmm_vmx_exit_evt(struct vmm_vmx_cpudata *cpudata)
{
	uint64_t info, err, inslen;

	cpudata->evt_pending = false;

	info = vmm_vmx_vmread(VMCS_IDT_VECTORING_INFO);
	if (__predict_true((info & INTR_INFO_VALID) == 0)) {
		return;
	}
	err = vmm_vmx_vmread(VMCS_IDT_VECTORING_ERROR);

	vmm_vmx_vmwrite(VMCS_ENTRY_INTR_INFO, info);
	vmm_vmx_vmwrite(VMCS_ENTRY_EXCEPTION_ERROR, err);

	switch (__SHIFTOUT(info, INTR_INFO_TYPE)) {
	case INTR_TYPE_SW_INT:
	case INTR_TYPE_PRIV_SW_EXC:
	case INTR_TYPE_SW_EXC:
		inslen = vmm_vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
		vmm_vmx_vmwrite(VMCS_ENTRY_INSTRUCTION_LENGTH, inslen);
	}

	cpudata->evt_pending = true;
}

int
vmm_vmx_vcpu_run(struct vmm_vcpu *vcpu, struct vmm_cpuexit **reason)
{
	struct vmm_machine *mach = vcpu->machine;
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	struct vmm_vmx_machdata *machdata = mach->backend_state;
	struct vmm_cpuexit *exit = &vcpu->exit;
	struct vpid_desc vpid_desc;
	uint64_t exitcode;
	uint64_t intstate;
	uint64_t machgen;
	uint64_t tsc_generation;
	int hcpu, ret;
	int error = 0;
	bool launched;

restart:
	vmm_vmx_vmcs_enter(vcpu);

	hcpu = os_curcpu_number();
	launched = cpudata->vmcs_launched;

	vmm_vmx_gtlb_catchup(vcpu, hcpu);
	vmm_vmx_htlb_catchup(vcpu, hcpu);

	if (cpudata->hcpu_last != hcpu) {
		vmm_vmx_vmwrite(VMCS_HOST_TR_SELECTOR, os_curcpu_tss_sel());
		vmm_vmx_vmwrite(VMCS_HOST_TR_BASE, (uint64_t)os_curcpu_tss());
		vmm_vmx_vmwrite(VMCS_HOST_GDTR_BASE, (uint64_t)os_curcpu_gdt());
		vmm_vmx_vmwrite(VMCS_HOST_GS_BASE, rdmsr(MSR_GSBASE));
		cpudata->gtsc_want_update = true;
		cpudata->hcpu_last = hcpu;

#ifdef __DragonFly__
		/*
		 * XXX: We aren't tracking overloaded CPUs (multiple vCPUs
		 *      scheduled on the same physical CPU) yet so there are
		 *      currently no calls to pmap_del_cpu().
		 */
		pmap_add_cpu(mach->vmspace, hcpu);
#endif
	}

	vmm_vmx_vcpu_guest_dbregs_enter(vcpu);
	vmm_vmx_vcpu_guest_misc_enter(vcpu);

	while (1) {
		if (cpudata->interrupt != NULL &&
		    vmm_vmx_interrupt_ops->vcpu_prepare != NULL) {
			error = vmm_vmx_interrupt_ops->vcpu_prepare(
			    cpudata->interrupt);
			if (error == EAGAIN || error == EINPROGRESS) {
				if (error == EINPROGRESS)
					vmm_vmx_vcpu_setstate_all(vcpu,
					    VMM_X64_STATE_ALL);
				cpudata->vmcs_launched = launched;
				vmm_vmx_vcpu_guest_misc_leave(vcpu);
				vmm_vmx_vcpu_guest_dbregs_leave(vcpu);
				vmm_vmx_vmcs_leave(vcpu);
				if (error == EAGAIN) {
					error = tsleep(vcpu, PINTERLOCKED | PCATCH,
					    "vmmisip", 0);
					if (error != 0)
						return error;
					if (atomic_load_acq_int(&vcpu->kick_pending) != 0) {
						/*
						 * The VMCS and guest host-state swaps have already
						 * been undone above.  Return directly so the common
						 * vCPU layer can expose this as EINTR.
						 */
						exit->reason = VMM_CPUEXIT_NONE;
						*reason = exit;
						return 0;
					}
				}
				goto restart;
			}
			if (error != 0)
				break;
		}
		if (cpudata->gtlb_want_flush) {
			vpid_desc.vpid = cpudata->asid;
			vpid_desc.addr = 0;
			vmm_vmx_invvpid(vmm_vmx_tlb_flush_op, &vpid_desc);
			cpudata->gtlb_want_flush = false;
		}

		tsc_generation = atomic_load_acq_64(&machdata->gtsc_generation);
		if (cpudata->gtsc_generation != tsc_generation) {
			cpudata->gtsc_offset = atomic_load_acq_64(
			    &machdata->gtsc_offset) + cpudata->gtsc_adjust;
			cpudata->gtsc_generation = tsc_generation;
			cpudata->gtsc_want_update = true;
		}
		if (__predict_false(cpudata->gtsc_want_update)) {
			vmm_vmx_vmwrite(VMCS_TSC_OFFSET, cpudata->gtsc_offset);
			cpudata->gtsc_want_update = false;
		}

		vmm_vmx_cli();
		vmm_vmx_vcpu_guest_fpu_enter(vcpu);
		machgen = vmm_vmx_htlb_flush(mach, cpudata);

#ifdef __DragonFly__
		/*
		 * Check for pending host events (e.g., interrupt, AST)
		 * to make the state safe to VM Entry.  This check must
		 * be done after the cli to avoid gd_reqflags pending
		 * races.
		 *
		 * Emulators may assume that event injection succeeds, but
		 * we have to return to process these events.  To deal with
		 * this, use ERESTART mechanics.
		 */
		if (__predict_false(mycpu->gd_reqflags & RQF_HVM_MASK)) {
			/* INVEPT executed, so ack hTLB flush. */
			vmm_vmx_htlb_flush_ack(cpudata, machgen);
			vmm_vmx_vcpu_guest_fpu_leave(vcpu);
			vmm_vmx_sti();
			exit->reason = VMM_CPUEXIT_NONE;
			vmm_stat_vcpu_run_restart_preentry(
			    mycpu->gd_reqflags & RQF_HVM_MASK);
			error = ERESTART;
			break;
		}

		/*
		 * Only commit event requests when we are absolutely
		 * sure that we can issue the vmlaunch/vmresume.
		 */
		if (__predict_false(vmm_vmx_vcpu_event_commit(vcpu) != 0)) {
			/* INVEPT executed, so ack hTLB flush. */
			vmm_vmx_htlb_flush_ack(cpudata, machgen);
			vmm_vmx_vcpu_guest_fpu_leave(vcpu);
			vmm_vmx_sti();
			exit->reason = VMM_CPUEXIT_NONE;
			error = EINVAL;
			break;
		}
#endif

		if (cpudata->interrupt != NULL &&
		    vmm_vmx_interrupt_ops->vcpu_enter != NULL)
			vmm_vmx_interrupt_ops->vcpu_enter(cpudata->interrupt);
		x86_set_cr2(cpudata->gcr2);
		atomic_store_rel_int(&cpudata->running_cpu, hcpu);
		if (launched) {
			ret = vmm_vmx_vmresume(cpudata->gprs);
		} else {
			ret = vmm_vmx_vmlaunch(cpudata->gprs);
		}
		atomic_store_rel_int(&cpudata->running_cpu, -1);
		if (cpudata->interrupt != NULL &&
		    vmm_vmx_interrupt_ops->vcpu_leave != NULL)
			vmm_vmx_interrupt_ops->vcpu_leave(cpudata->interrupt);
		vmm_stat_vmexit();
		cpudata->gcr2 = x86_get_cr2();
		vmm_vmx_htlb_flush_ack(cpudata, machgen);
		vmm_vmx_vcpu_guest_fpu_leave(vcpu);
		vmm_vmx_sti();

		if (__predict_false(ret != 0)) {
			vmm_vmx_exit_invalid(exit, -1);
			break;
		}
		vmm_vmx_exit_evt(cpudata);
		if (cpudata->interrupt != NULL &&
		    vmm_vmx_interrupt_ops->vcpu_event_result != NULL)
			vmm_vmx_interrupt_ops->vcpu_event_result(cpudata->interrupt,
			    cpudata->evt_pending);

		launched = true;
		cpudata->vmcs_launched = true;

		exitcode = vmm_vmx_vmread(VMCS_EXIT_REASON);
		exitcode &= __BITS(15,0);

		switch (exitcode) {
		case VMCS_EXITCODE_EXC_NMI:
			vmm_vmx_exit_exc_nmi(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_EXT_INT:
			exit->reason = VMM_CPUEXIT_NONE;
			break;
		case VMCS_EXITCODE_CPUID:
			vmm_vmx_exit_cpuid(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_HLT:
			vmm_vmx_exit_hlt(mach, vcpu, exit);
			if (cpudata->interrupt != NULL &&
			    vmm_vmx_interrupt_ops->vcpu_exit != NULL &&
			    vmm_vmx_interrupt_ops->vcpu_exit(cpudata->interrupt,
			    exitcode, 0, 0))
				exit->reason = VMM_CPUEXIT_NONE;
			break;
		case VMCS_EXITCODE_CR:
			vmm_vmx_exit_cr(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_IO:
			vmm_vmx_exit_io(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_RDMSR:
			vmm_vmx_exit_rdmsr(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_WRMSR:
			vmm_vmx_exit_wrmsr(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_SHUTDOWN:
			exit->reason = VMM_CPUEXIT_SHUTDOWN;
			break;
		case VMCS_EXITCODE_MONITOR:
			vmm_vmx_exit_insn(exit, VMM_CPUEXIT_MONITOR);
			break;
		case VMCS_EXITCODE_MWAIT:
			vmm_vmx_exit_insn(exit, VMM_CPUEXIT_MWAIT);
			break;
		case VMCS_EXITCODE_XSETBV:
			vmm_vmx_exit_xsetbv(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_RDPMC:
		case VMCS_EXITCODE_RDTSCP:
		case VMCS_EXITCODE_INVVPID:
		case VMCS_EXITCODE_INVEPT:
		case VMCS_EXITCODE_VMCALL:
		case VMCS_EXITCODE_VMCLEAR:
		case VMCS_EXITCODE_VMLAUNCH:
		case VMCS_EXITCODE_VMPTRLD:
		case VMCS_EXITCODE_VMPTRST:
		case VMCS_EXITCODE_VMREAD:
		case VMCS_EXITCODE_VMRESUME:
		case VMCS_EXITCODE_VMWRITE:
		case VMCS_EXITCODE_VMXOFF:
		case VMCS_EXITCODE_VMXON:
			vmm_vmx_inject_ud(vcpu);
			exit->reason = VMM_CPUEXIT_NONE;
			break;
		case VMCS_EXITCODE_EPT_VIOLATION:
			vmm_vmx_exit_epf(vcpu, exit);
			break;
		case VMCS_EXITCODE_APIC_ACCESS:
		case VMCS_EXITCODE_VEOI:
		case VMCS_EXITCODE_APIC_WRITE:
			if (cpudata->interrupt != NULL &&
			    vmm_vmx_interrupt_ops->vcpu_exit != NULL &&
			    vmm_vmx_interrupt_ops->vcpu_exit(cpudata->interrupt,
			    exitcode, vmm_vmx_vmread(VMCS_EXIT_QUALIFICATION), 0)) {
				exit->reason = VMM_CPUEXIT_NONE;
			} else {
				vmm_vmx_exit_invalid(exit, exitcode);
			}
			break;
		case VMCS_EXITCODE_INT_WINDOW:
			vmm_vmx_event_waitexit_disable(vcpu, false);
			exit->reason = cpudata->interrupt != NULL &&
			    vmm_vmx_interrupt_ops->vintr_internal ?
			    VMM_CPUEXIT_NONE : VMM_CPUEXIT_INT_READY;
			break;
		case VMCS_EXITCODE_NMI_WINDOW:
			vmm_vmx_event_waitexit_disable(vcpu, true);
			exit->reason = cpudata->interrupt != NULL &&
			    vmm_vmx_interrupt_ops->vcpu_exit != NULL &&
			    vmm_vmx_interrupt_ops->vcpu_exit(cpudata->interrupt,
			    exitcode, 0, 0) ? VMM_CPUEXIT_NONE :
			    VMM_CPUEXIT_NMI_READY;
			break;
		default:
			vmm_vmx_exit_invalid(exit, exitcode);
			break;
		}

		/* A concurrent kick must return through vmm_vcpu_run(). */
		if (exit->reason == VMM_CPUEXIT_NONE &&
		    atomic_load_acq_int(&vcpu->kick_pending) != 0) {
			break;
		}

		/* Preserve an architectural exit for the generic VMM dispatcher. */
		if (exit->reason != VMM_CPUEXIT_NONE) {
			break;
		}

		/* If no reason to return to userland, keep rolling. */
		if (os_return_needed()) {
			vmm_stat_vcpu_run_restart_postexit(
			    mycpu->gd_reqflags & RQF_HVM_MASK);
			error = ERESTART;
			break;
		}
	}

	cpudata->vmcs_launched = launched;

	vmm_vmx_vcpu_guest_misc_leave(vcpu);
	vmm_vmx_vcpu_guest_dbregs_leave(vcpu);

	exit->exitstate.rflags = vmm_vmx_vmread(VMCS_GUEST_RFLAGS);
	exit->exitstate.cr8 = cpudata->gcr8;
	intstate = vmm_vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
	exit->exitstate.int_shadow =
	    (intstate & (INT_STATE_STI|INT_STATE_MOVSS)) != 0;
	exit->exitstate.int_window_exiting = cpudata->int_window_exit;
	exit->exitstate.nmi_window_exiting = cpudata->nmi_window_exit;
	exit->exitstate.evt_pending = cpudata->evt_pending;

	vmm_vmx_vmcs_leave(vcpu);

	if (error == 0)
		*reason = exit;
	return error;
}

void
vmm_vmx_vcpu_memory_mapping_changed(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	cpudata->htlb_force_flush = true;
}

static void
vmm_vmx_kick_ipiq(void *arg)
{
	(void)arg;
}

void
vmm_vmx_vcpu_kick(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	int cpu;

	cpu = atomic_load_acq_int(&cpudata->running_cpu);
	if (cpu >= 0 && cpu != os_curcpu_number())
		lwkt_send_ipiq_bycpu(cpu, vmm_vmx_kick_ipiq, vcpu);
}

/* -------------------------------------------------------------------------- */

static void
vmm_vmx_vcpu_msr_allow(uint8_t *bitmap, uint64_t msr, bool read, bool write)
{
	uint64_t byte;
	uint8_t bitoff;

	if (msr < 0x00002000) {
		/* Range 1 */
		byte = ((msr - 0x00000000) / 8) + 0;
	} else if (msr >= 0xC0000000 && msr < 0xC0002000) {
		/* Range 2 */
		byte = ((msr - 0xC0000000) / 8) + 1024;
	} else {
		panic("%s: wrong range", __func__);
	}

	bitoff = (msr & 0x7);

	if (read) {
		bitmap[byte] &= ~__BIT(bitoff);
	}
	if (write) {
		bitmap[2048 + byte] &= ~__BIT(bitoff);
	}
}

#define VMM_VMX_SEG_ATTRIB_TYPE		__BITS(3,0)
#define VMM_VMX_SEG_ATTRIB_S		__BIT(4)
#define VMM_VMX_SEG_ATTRIB_DPL		__BITS(6,5)
#define VMM_VMX_SEG_ATTRIB_P		__BIT(7)
#define VMM_VMX_SEG_ATTRIB_AVL		__BIT(12)
#define VMM_VMX_SEG_ATTRIB_L		__BIT(13)
#define VMM_VMX_SEG_ATTRIB_DEF		__BIT(14)
#define VMM_VMX_SEG_ATTRIB_G		__BIT(15)
#define VMM_VMX_SEG_ATTRIB_UNUSABLE		__BIT(16)

static void
vmm_vmx_vcpu_setstate_seg(const struct vmm_segment *segs, int idx)
{
	uint64_t attrib;

	attrib =
	    __SHIFTIN(segs[idx].attrib.type, VMM_VMX_SEG_ATTRIB_TYPE) |
	    __SHIFTIN(segs[idx].attrib.s, VMM_VMX_SEG_ATTRIB_S) |
	    __SHIFTIN(segs[idx].attrib.dpl, VMM_VMX_SEG_ATTRIB_DPL) |
	    __SHIFTIN(segs[idx].attrib.p, VMM_VMX_SEG_ATTRIB_P) |
	    __SHIFTIN(segs[idx].attrib.avl, VMM_VMX_SEG_ATTRIB_AVL) |
	    __SHIFTIN(segs[idx].attrib.l, VMM_VMX_SEG_ATTRIB_L) |
	    __SHIFTIN(segs[idx].attrib.def, VMM_VMX_SEG_ATTRIB_DEF) |
	    __SHIFTIN(segs[idx].attrib.g, VMM_VMX_SEG_ATTRIB_G) |
	    (!segs[idx].attrib.p ? VMM_VMX_SEG_ATTRIB_UNUSABLE : 0);

	if (idx != VMM_X64_SEG_GDT && idx != VMM_X64_SEG_IDT) {
		vmm_vmx_vmwrite(vmm_vmx_guest_segs[idx].selector, segs[idx].selector);
		vmm_vmx_vmwrite(vmm_vmx_guest_segs[idx].attrib, attrib);
	}
	vmm_vmx_vmwrite(vmm_vmx_guest_segs[idx].limit, segs[idx].limit);
	vmm_vmx_vmwrite(vmm_vmx_guest_segs[idx].base, segs[idx].base);
}

static void
vmm_vmx_vcpu_getstate_seg(struct vmm_segment *segs, int idx)
{
	uint64_t selector = 0, attrib = 0, base, limit;

	if (idx != VMM_X64_SEG_GDT && idx != VMM_X64_SEG_IDT) {
		selector = vmm_vmx_vmread(vmm_vmx_guest_segs[idx].selector);
		attrib = vmm_vmx_vmread(vmm_vmx_guest_segs[idx].attrib);
	}
	limit = vmm_vmx_vmread(vmm_vmx_guest_segs[idx].limit);
	base = vmm_vmx_vmread(vmm_vmx_guest_segs[idx].base);

	segs[idx].selector = selector;
	segs[idx].limit = limit;
	segs[idx].base = base;
	segs[idx].attrib.type = __SHIFTOUT(attrib, VMM_VMX_SEG_ATTRIB_TYPE);
	segs[idx].attrib.s = __SHIFTOUT(attrib, VMM_VMX_SEG_ATTRIB_S);
	segs[idx].attrib.dpl = __SHIFTOUT(attrib, VMM_VMX_SEG_ATTRIB_DPL);
	segs[idx].attrib.p = __SHIFTOUT(attrib, VMM_VMX_SEG_ATTRIB_P);
	segs[idx].attrib.avl = __SHIFTOUT(attrib, VMM_VMX_SEG_ATTRIB_AVL);
	segs[idx].attrib.l = __SHIFTOUT(attrib, VMM_VMX_SEG_ATTRIB_L);
	segs[idx].attrib.def = __SHIFTOUT(attrib, VMM_VMX_SEG_ATTRIB_DEF);
	segs[idx].attrib.g = __SHIFTOUT(attrib, VMM_VMX_SEG_ATTRIB_G);
	if (attrib & VMM_VMX_SEG_ATTRIB_UNUSABLE) {
		segs[idx].attrib.p = 0;
	}
}

static inline bool
vmm_vmx_state_gtlb_flush(const struct vmm_cpustate *state, uint64_t flags)
{
	uint64_t cr0, cr3, cr4, efer;

	if (flags & VMM_X64_STATE_CRS) {
		cr0 = vmm_vmx_vmread(VMCS_GUEST_CR0);
		if ((cr0 ^ state->crs[VMM_X64_CR_CR0]) & CR0_TLB_FLUSH) {
			return true;
		}
		cr3 = vmm_vmx_vmread(VMCS_GUEST_CR3);
		if (cr3 != state->crs[VMM_X64_CR_CR3]) {
			return true;
		}
		cr4 = vmm_vmx_vmread(VMCS_GUEST_CR4);
		if ((cr4 ^ state->crs[VMM_X64_CR_CR4]) & CR4_TLB_FLUSH) {
			return true;
		}
	}

	if (flags & VMM_X64_STATE_MSRS) {
		efer = vmm_vmx_vmread(VMCS_GUEST_IA32_EFER);
		if ((efer ^
		     state->msrs[VMM_X64_MSR_EFER]) & EFER_TLB_FLUSH) {
			return true;
		}
	}

	return false;
}

static void
vmm_vmx_vcpu_setstate_all(struct vmm_vcpu *vcpu, uint64_t flags)
{
	const struct vmm_cpustate *state = vcpu->state;
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	struct msr_entry *gmsr = cpudata->gmsr;
	struct vmm_cpustate_fpu *fpustate;
	uint64_t ctls1, intstate;
	int error;

	vmm_vmx_vmcs_enter(vcpu);

	if (vmm_vmx_state_gtlb_flush(state, flags)) {
		cpudata->gtlb_want_flush = true;
	}

	if (flags & VMM_X64_STATE_SEGS) {
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_CS);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_DS);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_ES);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_FS);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_GS);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_SS);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_GDT);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_IDT);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_LDT);
		vmm_vmx_vcpu_setstate_seg(state->segs, VMM_X64_SEG_TR);
	}

	CTASSERT(sizeof(cpudata->gprs) == sizeof(state->gprs));
	if (flags & VMM_X64_STATE_GPRS) {
		memcpy(cpudata->gprs, state->gprs, sizeof(state->gprs));

		vmm_vmx_vmwrite(VMCS_GUEST_RIP, state->gprs[VMM_X64_GPR_RIP]);
		vmm_vmx_vmwrite(VMCS_GUEST_RSP, state->gprs[VMM_X64_GPR_RSP]);
		vmm_vmx_vmwrite(VMCS_GUEST_RFLAGS, state->gprs[VMM_X64_GPR_RFLAGS]);
	}

	if (flags & VMM_X64_STATE_CRS) {
		vmm_vmx_vmwrite(VMCS_CR0_SHADOW,
		    (state->crs[VMM_X64_CR_CR0] & CR0_STATIC_MASK) |
		    CR0_ET);
		vmm_vmx_vmwrite(VMCS_GUEST_CR0,
		    (state->crs[VMM_X64_CR_CR0] & ~CR0_FORCE_ZERO) |
		    CR0_FORCE_ONE);

		cpudata->gcr2 = state->crs[VMM_X64_CR_CR2];

		/* XXX We are not handling PDPTE here. */
		vmm_vmx_vmwrite(VMCS_GUEST_CR3, state->crs[VMM_X64_CR_CR3]);

		/* CR4_VMXE is mandatory. */
		vmm_vmx_vmwrite(VMCS_GUEST_CR4,
		    (state->crs[VMM_X64_CR_CR4] & CR4_VALID) | CR4_VMXE);

		cpudata->gcr8 = state->crs[VMM_X64_CR_CR8];

		if (vmm_vmx_xcr0_mask != 0) {
			/* Clear illegal XCR0 bits, set mandatory X87 bit. */
			cpudata->gxcr0 = state->crs[VMM_X64_CR_XCR0];
			cpudata->gxcr0 &= vmm_vmx_xcr0_mask;
			cpudata->gxcr0 |= XCR0_X87;
		}
	}

	CTASSERT(sizeof(cpudata->drs) == sizeof(state->drs));
	if (flags & VMM_X64_STATE_DRS) {
		memcpy(cpudata->drs, state->drs, sizeof(state->drs));

		cpudata->drs[VMM_X64_DR_DR6] &= 0xFFFFFFFF;
		vmm_vmx_vmwrite(VMCS_GUEST_DR7, cpudata->drs[VMM_X64_DR_DR7]);
	}

	if (flags & VMM_X64_STATE_MSRS) {
		gmsr[VMM_VMX_MSRLIST_STAR].val =
		    state->msrs[VMM_X64_MSR_STAR];
		gmsr[VMM_VMX_MSRLIST_LSTAR].val =
		    state->msrs[VMM_X64_MSR_LSTAR];
		gmsr[VMM_VMX_MSRLIST_CSTAR].val =
		    state->msrs[VMM_X64_MSR_CSTAR];
		gmsr[VMM_VMX_MSRLIST_SFMASK].val =
		    state->msrs[VMM_X64_MSR_SFMASK];
		gmsr[VMM_VMX_MSRLIST_KERNELGSBASE].val =
		    state->msrs[VMM_X64_MSR_KERNELGSBASE];

		vmm_vmx_vmwrite(VMCS_GUEST_IA32_EFER,
		    state->msrs[VMM_X64_MSR_EFER]);
		vmm_vmx_vmwrite(VMCS_GUEST_IA32_PAT,
		    state->msrs[VMM_X64_MSR_PAT]);
		vmm_vmx_vmwrite(VMCS_GUEST_IA32_SYSENTER_CS,
		    state->msrs[VMM_X64_MSR_SYSENTER_CS]);
		vmm_vmx_vmwrite(VMCS_GUEST_IA32_SYSENTER_ESP,
		    state->msrs[VMM_X64_MSR_SYSENTER_ESP]);
		vmm_vmx_vmwrite(VMCS_GUEST_IA32_SYSENTER_EIP,
		    state->msrs[VMM_X64_MSR_SYSENTER_EIP]);

		/*
		 * The emulator might not want to set the TSC, because doing so
		 * would destroy TSC MP-synchronization across CPUs. Try to
		 * figure out what the emulator meant to do.
		 *
		 * If it's writing the last TSC value we reported via getstate,
		 * assume that the emulator does not want to write to the TSC.
		 */
		if (state->msrs[VMM_X64_MSR_TSC] != cpudata->gtsc_match &&
		    state->msrs[VMM_X64_MSR_TSC] != 0) {
			error = vmm_machine_set_tsc(vcpu->machine,
			    state->msrs[VMM_X64_MSR_TSC]);
			KKASSERT(error == 0);
		}

		/* ENTRY_CTLS_LONG_MODE must match EFER_LMA. */
		ctls1 = vmm_vmx_vmread(VMCS_ENTRY_CTLS);
		if (state->msrs[VMM_X64_MSR_EFER] & EFER_LMA) {
			ctls1 |= ENTRY_CTLS_LONG_MODE;
		} else {
			ctls1 &= ~ENTRY_CTLS_LONG_MODE;
		}
		vmm_vmx_vmwrite(VMCS_ENTRY_CTLS, ctls1);
	}

	if (flags & VMM_X64_STATE_INTR) {
		intstate = vmm_vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
		intstate &= ~(INT_STATE_STI|INT_STATE_MOVSS);
		if (state->intr.int_shadow) {
			intstate |= INT_STATE_MOVSS;
		}
		vmm_vmx_vmwrite(VMCS_GUEST_INTERRUPTIBILITY, intstate);

	}

	CTASSERT(sizeof(cpudata->gxsave.fpu) == sizeof(state->fpu));
	if (flags & VMM_X64_STATE_FPU) {
		memcpy(&cpudata->gxsave.fpu, &state->fpu, sizeof(state->fpu));

		fpustate = (struct vmm_cpustate_fpu *)&cpudata->gxsave.fpu;
		fpustate->fx_mxcsr_mask &= x86_fpu_mxcsr_mask;
		fpustate->fx_mxcsr &= fpustate->fx_mxcsr_mask;

		if (vmm_vmx_xcr0_mask != 0) {
			/* Reset XSTATE_BV, to force a reload. */
			cpudata->gxsave.xstate_bv = vmm_vmx_xcr0_mask;
		}
	}

	vmm_vmx_vmcs_leave(vcpu);

}

static void
vmm_vmx_vcpu_getstate_all(struct vmm_vcpu *vcpu, uint64_t flags)
{
	struct vmm_cpustate *state = vcpu->state;
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	struct msr_entry *gmsr = cpudata->gmsr;
	uint64_t intstate;

	vmm_vmx_vmcs_enter(vcpu);

	if (flags & VMM_X64_STATE_SEGS) {
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_CS);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_DS);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_ES);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_FS);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_GS);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_SS);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_GDT);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_IDT);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_LDT);
		vmm_vmx_vcpu_getstate_seg(state->segs, VMM_X64_SEG_TR);
	}

	CTASSERT(sizeof(cpudata->gprs) == sizeof(state->gprs));
	if (flags & VMM_X64_STATE_GPRS) {
		memcpy(state->gprs, cpudata->gprs, sizeof(state->gprs));

		state->gprs[VMM_X64_GPR_RIP] = vmm_vmx_vmread(VMCS_GUEST_RIP);
		state->gprs[VMM_X64_GPR_RSP] = vmm_vmx_vmread(VMCS_GUEST_RSP);
		state->gprs[VMM_X64_GPR_RFLAGS] = vmm_vmx_vmread(VMCS_GUEST_RFLAGS);
	}

	if (flags & VMM_X64_STATE_CRS) {
		state->crs[VMM_X64_CR_CR0] =
		    (vmm_vmx_vmread(VMCS_CR0_SHADOW) & CR0_STATIC_MASK) |
		    (vmm_vmx_vmread(VMCS_GUEST_CR0) & ~CR0_STATIC_MASK);
		state->crs[VMM_X64_CR_CR2] = cpudata->gcr2;
		state->crs[VMM_X64_CR_CR3] = vmm_vmx_vmread(VMCS_GUEST_CR3);
		state->crs[VMM_X64_CR_CR4] = vmm_vmx_vmread(VMCS_GUEST_CR4);
		state->crs[VMM_X64_CR_CR8] = cpudata->gcr8;
		state->crs[VMM_X64_CR_XCR0] = cpudata->gxcr0;

		/* Hide VMXE. */
		state->crs[VMM_X64_CR_CR4] &= ~CR4_VMXE;
	}

	CTASSERT(sizeof(cpudata->drs) == sizeof(state->drs));
	if (flags & VMM_X64_STATE_DRS) {
		memcpy(state->drs, cpudata->drs, sizeof(state->drs));

		state->drs[VMM_X64_DR_DR7] = vmm_vmx_vmread(VMCS_GUEST_DR7);
	}

	if (flags & VMM_X64_STATE_MSRS) {
		state->msrs[VMM_X64_MSR_STAR] =
		    gmsr[VMM_VMX_MSRLIST_STAR].val;
		state->msrs[VMM_X64_MSR_LSTAR] =
		    gmsr[VMM_VMX_MSRLIST_LSTAR].val;
		state->msrs[VMM_X64_MSR_CSTAR] =
		    gmsr[VMM_VMX_MSRLIST_CSTAR].val;
		state->msrs[VMM_X64_MSR_SFMASK] =
		    gmsr[VMM_VMX_MSRLIST_SFMASK].val;
		state->msrs[VMM_X64_MSR_KERNELGSBASE] =
		    gmsr[VMM_VMX_MSRLIST_KERNELGSBASE].val;
		state->msrs[VMM_X64_MSR_EFER] =
		    vmm_vmx_vmread(VMCS_GUEST_IA32_EFER);
		state->msrs[VMM_X64_MSR_PAT] =
		    vmm_vmx_vmread(VMCS_GUEST_IA32_PAT);
		state->msrs[VMM_X64_MSR_SYSENTER_CS] =
		    vmm_vmx_vmread(VMCS_GUEST_IA32_SYSENTER_CS);
		state->msrs[VMM_X64_MSR_SYSENTER_ESP] =
		    vmm_vmx_vmread(VMCS_GUEST_IA32_SYSENTER_ESP);
		state->msrs[VMM_X64_MSR_SYSENTER_EIP] =
		    vmm_vmx_vmread(VMCS_GUEST_IA32_SYSENTER_EIP);
		state->msrs[VMM_X64_MSR_TSC] = rdtsc() + cpudata->gtsc_offset;

		/* Save reported TSC value for later setstate check. */
		cpudata->gtsc_match = state->msrs[VMM_X64_MSR_TSC];
	}

	if (flags & VMM_X64_STATE_INTR) {
		intstate = vmm_vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
		state->intr.int_shadow =
		    (intstate & (INT_STATE_STI|INT_STATE_MOVSS)) != 0;
	}

	CTASSERT(sizeof(cpudata->gxsave.fpu) == sizeof(state->fpu));
	if (flags & VMM_X64_STATE_FPU) {
		memcpy(&state->fpu, &cpudata->gxsave.fpu, sizeof(state->fpu));
	}

	vmm_vmx_vmcs_leave(vcpu);

}

static void
vmm_vmx_vcpu_state_provide(struct vmm_vcpu *vcpu, uint64_t flags)
{
	vmm_vmx_vcpu_getstate_all(vcpu, flags);
}

void
vmm_vmx_vcpu_getstate(struct vmm_vcpu *vcpu)
{
	vmm_vmx_vcpu_getstate_all(vcpu, VMM_X64_STATE_ALL);
}

void
vmm_vmx_vcpu_setstate(struct vmm_vcpu *vcpu)
{
	vmm_vmx_vcpu_setstate_all(vcpu, VMM_X64_STATE_ALL);
}

/* -------------------------------------------------------------------------- */

static void
vmm_vmx_asid_alloc(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	size_t i, oct, bit;

	os_mtx_lock(&vmm_vmx_asidlock);

	for (i = 0; i < vmm_vmx_maxasid; i++) {
		oct = i / 8;
		bit = i % 8;

		if (vmm_vmx_asidmap[oct] & __BIT(bit)) {
			continue;
		}

		cpudata->asid = i;

		vmm_vmx_asidmap[oct] |= __BIT(bit);
		vmm_vmx_vmwrite(VMCS_VPID, i);
		os_mtx_unlock(&vmm_vmx_asidlock);
		return;
	}

	os_mtx_unlock(&vmm_vmx_asidlock);

	panic("%s: impossible", __func__);
}

static void
vmm_vmx_asid_free(struct vmm_vcpu *vcpu)
{
	size_t oct, bit;
	uint64_t asid;

	asid = vmm_vmx_vmread(VMCS_VPID);

	oct = asid / 8;
	bit = asid % 8;

	os_mtx_lock(&vmm_vmx_asidlock);
	vmm_vmx_asidmap[oct] &= ~__BIT(bit);
	os_mtx_unlock(&vmm_vmx_asidlock);
}

static void
vmm_vmx_vcpu_init(struct vmm_vcpu *vcpu)
{
	struct vmm_machine *mach = vcpu->machine;
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	struct vmcs *vmcs = cpudata->vmcs;
	struct msr_entry *gmsr = cpudata->gmsr;
	uint64_t rev, eptp;

	rev = vmm_vmx_get_revision();

	memset(vmcs, 0, VMCS_SIZE);
	vmcs->ident = __SHIFTIN(rev, VMCS_IDENT_REVISION);
	vmcs->abort = 0;

	vmm_vmx_vmcs_enter(vcpu);

	/* No link pointer. */
	vmm_vmx_vmwrite(VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFFULL);

	/* Install the CTLSs. */
	vmm_vmx_vmwrite(VMCS_PINBASED_CTLS, vmm_vmx_pinbased_ctls);
	vmm_vmx_vmwrite(VMCS_PROCBASED_CTLS, vmm_vmx_procbased_ctls);
	vmm_vmx_vmwrite(VMCS_PROCBASED_CTLS2, vmm_vmx_procbased_ctls2);
	vmm_vmx_vmwrite(VMCS_ENTRY_CTLS, vmm_vmx_entry_ctls);
	vmm_vmx_vmwrite(VMCS_EXIT_CTLS, vmm_vmx_exit_ctls);

	/* Allow direct access to certain MSRs. */
	memset(cpudata->msrbm, 0xFF, MSRBM_SIZE);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_EFER, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_STAR, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_LSTAR, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_CSTAR, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_SFMASK, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_KERNELGSBASE, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_CS, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_ESP, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_EIP, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_FSBASE, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_GSBASE, true, true);
	vmm_vmx_vcpu_msr_allow(cpudata->msrbm, MSR_TSC, true, false);
	vmm_vmx_vmwrite(VMCS_MSR_BITMAP, (uint64_t)cpudata->msrbm_pa);

	/*
	 * List of Guest MSRs loaded on VMENTRY, saved on VMEXIT. This
	 * includes the L1D_FLUSH MSR, to mitigate L1TF.
	 */
	gmsr[VMM_VMX_MSRLIST_STAR].msr = MSR_STAR;
	gmsr[VMM_VMX_MSRLIST_STAR].val = 0;
	gmsr[VMM_VMX_MSRLIST_LSTAR].msr = MSR_LSTAR;
	gmsr[VMM_VMX_MSRLIST_LSTAR].val = 0;
	gmsr[VMM_VMX_MSRLIST_CSTAR].msr = MSR_CSTAR;
	gmsr[VMM_VMX_MSRLIST_CSTAR].val = 0;
	gmsr[VMM_VMX_MSRLIST_SFMASK].msr = MSR_SFMASK;
	gmsr[VMM_VMX_MSRLIST_SFMASK].val = 0;
	gmsr[VMM_VMX_MSRLIST_KERNELGSBASE].msr = MSR_KERNELGSBASE;
	gmsr[VMM_VMX_MSRLIST_KERNELGSBASE].val = 0;
	gmsr[VMM_VMX_MSRLIST_L1DFLUSH].msr = MSR_IA32_FLUSH_CMD;
	gmsr[VMM_VMX_MSRLIST_L1DFLUSH].val = IA32_FLUSH_CMD_L1D_FLUSH;
	vmm_vmx_vmwrite(VMCS_ENTRY_MSR_LOAD_ADDRESS, cpudata->gmsr_pa);
	vmm_vmx_vmwrite(VMCS_EXIT_MSR_STORE_ADDRESS, cpudata->gmsr_pa);
	vmm_vmx_vmwrite(VMCS_ENTRY_MSR_LOAD_COUNT, vmm_vmx_msrlist_entry_nmsr);
	vmm_vmx_vmwrite(VMCS_EXIT_MSR_STORE_COUNT, VMM_VMX_MSRLIST_EXIT_NMSR);

	/* Set the CR0 mask. Any change of these bits causes a VMEXIT. */
	vmm_vmx_vmwrite(VMCS_CR0_MASK, CR0_STATIC_MASK);

	/* Force unsupported CR4 fields to zero. */
	vmm_vmx_vmwrite(VMCS_CR4_MASK, CR4_INVALID);
	vmm_vmx_vmwrite(VMCS_CR4_SHADOW, 0);

	/* Set the Host state for resuming. */
	vmm_vmx_vmwrite(VMCS_HOST_RIP, (uint64_t)(uintptr_t)vmm_vmx_resume_rip);
	vmm_vmx_vmwrite(VMCS_HOST_CS_SELECTOR, GSEL(GCODE_SEL, SEL_KPL));
	vmm_vmx_vmwrite(VMCS_HOST_SS_SELECTOR, GSEL(GDATA_SEL, SEL_KPL));
	vmm_vmx_vmwrite(VMCS_HOST_DS_SELECTOR, GSEL(GDATA_SEL, SEL_KPL));
	vmm_vmx_vmwrite(VMCS_HOST_ES_SELECTOR, GSEL(GDATA_SEL, SEL_KPL));
	vmm_vmx_vmwrite(VMCS_HOST_FS_SELECTOR, 0);
	vmm_vmx_vmwrite(VMCS_HOST_GS_SELECTOR, 0);
	vmm_vmx_vmwrite(VMCS_HOST_IA32_SYSENTER_CS, 0);
	vmm_vmx_vmwrite(VMCS_HOST_IA32_SYSENTER_ESP, 0);
	vmm_vmx_vmwrite(VMCS_HOST_IA32_SYSENTER_EIP, 0);
	vmm_vmx_vmwrite(VMCS_HOST_IA32_PAT, rdmsr(MSR_CR_PAT));
	vmm_vmx_vmwrite(VMCS_HOST_IA32_EFER, rdmsr(MSR_EFER));
	vmm_vmx_vmwrite(VMCS_HOST_CR0, x86_get_cr0() & ~CR0_TS);

	/* Generate ASID. */
	vmm_vmx_asid_alloc(vcpu);

	/* Enable Extended Paging, 4-Level. */
	eptp =
	    __SHIFTIN(vmm_vmx_eptp_type, EPTP_TYPE) |
	    __SHIFTIN(4-1, EPTP_WALKLEN) |
	    (vmm_vmx_ept_has_ad ? EPTP_FLAGS_AD : 0) |
	    os_vmspace_pdirpa(mach->vmspace);
	vmm_vmx_vmwrite(VMCS_EPTP, eptp);

	/* Init IA32_MISC_ENABLE. */
	cpudata->gmsr_misc_enable = rdmsr(MSR_IA32_MISC_ENABLE);
	cpudata->gmsr_misc_enable &=
	    ~(IA32_MISC_PERFMON_EN|IA32_MISC_EISST_EN|IA32_MISC_MWAIT_EN);
	cpudata->gmsr_misc_enable |=
	    (IA32_MISC_BTS_UNAVAIL|IA32_MISC_PEBS_UNAVAIL);

	/* Init XSAVE header. */
	cpudata->gxsave.xstate_bv = vmm_vmx_xcr0_mask;
	cpudata->gxsave.xcomp_bv = 0;

	vmm_vmx_vmcs_leave(vcpu);
}

int
vmm_vmx_vcpu_create(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata;
	struct vmm_vmx_machdata *machdata;
	struct vmm_vmx_interrupt_config interrupt_config;
	int error;

	/* Allocate the VMX cpudata. */
	cpudata = (struct vmm_vmx_cpudata *)os_pagemem_zalloc(sizeof(*cpudata));
	if (cpudata == NULL)
		return ENOMEM;

	vcpu->backend = cpudata;
	cpudata->hcpu_last = -1;
	cpudata->running_cpu = -1;

	/* VMCS */
	error = os_contigpa_zalloc(&cpudata->vmcs_pa,
	    (vaddr_t *)&cpudata->vmcs, VMCS_NPAGES);
	if (error)
		goto error;

	/* MSR Bitmap */
	error = os_contigpa_zalloc(&cpudata->msrbm_pa,
	    (vaddr_t *)&cpudata->msrbm, MSRBM_NPAGES);
	if (error)
		goto error;

	/* Guest MSR List */
	error = os_contigpa_zalloc(&cpudata->gmsr_pa,
	    (vaddr_t *)&cpudata->gmsr, 1);
	if (error)
		goto error;

	os_cpuset_init(&cpudata->htlb_want_flush);

	/* Init the VCPU info. */
	vmm_vmx_vcpu_init(vcpu);

	machdata = vcpu->machine->backend_state;
	if (machdata != NULL && machdata->interrupt != NULL) {
		error = vmm_vmx_interrupt_ops->vcpu_create(machdata->interrupt,
		    vcpu, &cpudata->interrupt, &interrupt_config);
		if (error != 0) {
			vmm_vmx_vcpu_destroy(vcpu);
			return error;
		}
		if (interrupt_config.enabled) {
			vmm_vmx_vmcs_enter(vcpu);
			vmm_vmx_vmwrite(VMCS_VIRTUAL_APIC,
			    interrupt_config.virtual_apic_page);
			vmm_vmx_vmwrite(VMCS_APIC_ACCESS,
			    interrupt_config.apic_access_page);
			vmm_vmx_vmwrite(VMCS_TPR_THRESHOLD, 0);
			vmm_vmx_vmwrite(VMCS_PROCBASED_CTLS,
			    vmm_vmx_vmread(VMCS_PROCBASED_CTLS) |
			    PROC_CTLS_USE_TPR_SHADOW);
			vmm_vmx_vmwrite(VMCS_PROCBASED_CTLS2,
			    vmm_vmx_vmread(VMCS_PROCBASED_CTLS2) |
			    PROC_CTLS2_VIRT_APIC_ACCESSES |
			    PROC_CTLS2_APIC_REG_VIRT |
			    PROC_CTLS2_VIRT_INT_DELIVERY);
			vmm_vmx_vmwrite(VMCS_EOI_EXIT0,
			    interrupt_config.eoi_exit[0]);
			vmm_vmx_vmwrite(VMCS_EOI_EXIT1,
			    interrupt_config.eoi_exit[1]);
			vmm_vmx_vmwrite(VMCS_EOI_EXIT2,
			    interrupt_config.eoi_exit[2]);
			vmm_vmx_vmwrite(VMCS_EOI_EXIT3,
			    interrupt_config.eoi_exit[3]);
			vmm_vmx_vmcs_leave(vcpu);
		}
	}

	return 0;

error:
	if (cpudata->vmcs_pa) {
		os_contigpa_free(cpudata->vmcs_pa, (vaddr_t)cpudata->vmcs,
		    VMCS_NPAGES);
	}
	if (cpudata->msrbm_pa) {
		os_contigpa_free(cpudata->msrbm_pa, (vaddr_t)cpudata->msrbm,
		    MSRBM_NPAGES);
	}
	if (cpudata->gmsr_pa) {
		os_contigpa_free(cpudata->gmsr_pa, (vaddr_t)cpudata->gmsr, 1);
	}
	os_pagemem_free(cpudata, sizeof(*cpudata));
	return error;
}

void
vmm_vmx_vcpu_destroy(struct vmm_vcpu *vcpu)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	if (cpudata->interrupt != NULL) {
		vmm_vmx_interrupt_ops->vcpu_destroy(cpudata->interrupt);
		cpudata->interrupt = NULL;
	}

	vmm_vmx_vmcs_enter(vcpu);
	vmm_vmx_asid_free(vcpu);
	vmm_vmx_vmcs_destroy(vcpu);

	os_cpuset_destroy(cpudata->htlb_want_flush);

	os_contigpa_free(cpudata->vmcs_pa, (vaddr_t)cpudata->vmcs,
	    VMCS_NPAGES);
	os_contigpa_free(cpudata->msrbm_pa, (vaddr_t)cpudata->msrbm,
	    MSRBM_NPAGES);
	os_contigpa_free(cpudata->gmsr_pa, (vaddr_t)cpudata->gmsr,
	    1);
	os_pagemem_free(cpudata, sizeof(*cpudata));
	vcpu->backend = NULL;
}

/* -------------------------------------------------------------------------- */

int
vmm_vmx_vcpu_set_cpuid(struct vmm_vcpu *vcpu,
    const struct vmm_cpuid_entry *entries, size_t entry_count)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	size_t i;
	size_t j;

	if (entry_count > VMM_VMX_NCPUID_ENTRIES)
		return E2BIG;
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
		memcpy(cpudata->cpuid_entries, entries,
		    entry_count * sizeof(*entries));
	for (i = 0; i < entry_count; ++i)
		vmm_vmx_filter_configured_cpuid_entry(
		    &cpudata->cpuid_entries[i]);
	cpudata->cpuid_entry_count = entry_count;
	return 0;
}

int
vmm_vmx_vcpu_get_lapic(struct vmm_vcpu *vcpu, void *registers, size_t size)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	if (cpudata->interrupt == NULL)
		return ENOTSUP;
	return vmm_vmx_interrupt_ops->vcpu_get_lapic(cpudata->interrupt,
	    registers, size);
}

int
vmm_vmx_vcpu_set_lapic(struct vmm_vcpu *vcpu, const void *registers,
    size_t size)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	if (cpudata->interrupt == NULL)
		return ENOTSUP;
	return vmm_vmx_interrupt_ops->vcpu_set_lapic(cpudata->interrupt,
	    registers, size);
}

int
vmm_vmx_vcpu_io(struct vmm_vcpu *vcpu, const struct vmm_cpuexit_io *exit)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;

	if (!vcpu->machine->irqchip || cpudata->interrupt == NULL ||
	    vmm_vmx_interrupt_ops->vcpu_io == NULL)
		return ENOENT;
	return vmm_vmx_interrupt_ops->vcpu_io(vcpu, exit);
}

int
vmm_vmx_vcpu_mmio(struct vmm_vcpu *vcpu, uint64_t address, size_t size,
    bool write, uint64_t *value)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	uint32_t value32;
	int error;

	/* The xAPIC and IOAPIC register ABI admits aligned 32-bit accesses only. */
	if (!vcpu->machine->irqchip || cpudata->interrupt == NULL ||
	    size != sizeof(value32) || value == NULL)
		return ENOENT;
	value32 = *value;
	error = vmm_vmx_interrupt_ops->vcpu_mmio(cpudata->interrupt, address,
	    write, &value32);
	if (error != 0)
		return ENOENT;
	*value = value32;
	return 0;
}

/* -------------------------------------------------------------------------- */

#ifdef __NetBSD__
static void
vmm_vmx_tlb_flush(struct pmap *pm)
{
	struct vmm_machine *mach = os_pmap_mach(pm);
	struct vmm_vmx_machdata *machdata = mach->backend_state;

	os_atomic_inc_64(&machdata->mach_htlb_gen);

	/*
	 * Send a dummy IPI to each CPU. The IPIs cause #VMEXITs. Afterwards the
	 * VCPU loops will see that their 'vcpu_htlb_gen' is out of sync, and
	 * will each flush their own TLB.
	 */
	os_ipi_kickall();
}
#endif

int
vmm_vmx_machine_create(struct vmm_machine *mach)
{
	struct pmap *pmap = os_vmspace_pmap(mach->vmspace);
	struct vmm_vmx_machdata *machdata;

	machdata = os_mem_zalloc(sizeof(*machdata));
	if (machdata == NULL)
		return ENOMEM;

	/* Transform into an EPT pmap. */
#if defined(__NetBSD__)
	pmap_ept_transform(pmap);
	os_pmap_mach(pmap) = (void *)mach;
	pmap->pm_tlb_flush = vmm_vmx_tlb_flush;
#elif defined(__DragonFly__)
	pmap_ept_transform(pmap, vmm_vmx_ept_has_ad ? 0 : PMAP_EMULATE_AD_BITS);
#endif

	mach->backend_state = machdata;

	/* Start with an hTLB flush everywhere. */
	machdata->mach_htlb_gen = 1;
	return 0;
}

int
vmm_vmx_machine_set_tsc(struct vmm_machine *mach, uint64_t value)
{
	struct vmm_vmx_machdata *machdata = mach->backend_state;

	if (machdata == NULL)
		return ENXIO;
	atomic_store_rel_64(&machdata->gtsc_offset, value - rdtsc());
	atomic_fetchadd_64(&machdata->gtsc_generation, 1);
	return 0;
}

int
vmm_vmx_vcpu_get_tsc(struct vmm_vcpu *vcpu, uint64_t *value)
{
	struct vmm_vmx_cpudata *cpudata = vcpu->backend;
	struct vmm_vmx_machdata *machdata = vcpu->machine->backend_state;
	uint64_t generation;

	if (machdata == NULL)
		return ENXIO;
	generation = atomic_load_acq_64(&machdata->gtsc_generation);
	if (cpudata->gtsc_generation != generation) {
		cpudata->gtsc_offset = atomic_load_acq_64(&machdata->gtsc_offset) +
		    cpudata->gtsc_adjust;
		cpudata->gtsc_generation = generation;
		cpudata->gtsc_want_update = true;
	}
	*value = rdtsc() + cpudata->gtsc_offset;
	return 0;
}

int
vmm_vmx_machine_create_irqchip(struct vmm_machine *mach)
{
	struct vmm_vmx_machdata *machdata = mach->backend_state;
	int error;

	if (machdata == NULL || vmm_vmx_interrupt_ops == NULL)
		return ENXIO;
	if (machdata->interrupt != NULL)
		return EALREADY;
	error = vmm_vmx_interrupt_ops->machine_create(mach,
	    &machdata->interrupt);
	if (error != 0)
		return error;
	error = vmm_vmx_interrupt_ops->machine_enable(machdata->interrupt);
	if (error == 0)
		return 0;
	vmm_vmx_interrupt_ops->machine_destroy(machdata->interrupt);
	machdata->interrupt = NULL;
	return error;
}

bool
vmm_vmx_irqchip_available(void)
{
	return vmm_vmx_interrupt_ops != NULL;
}

int
vmm_vmx_irq_raise_msi(struct vmm_machine *mach, uint64_t address,
    uint32_t data)
{
	struct vmm_vmx_machdata *machdata = mach->backend_state;

	if (machdata == NULL || machdata->interrupt == NULL)
		return ENXIO;
	return vmm_vmx_interrupt_ops->irq_raise_msi(machdata->interrupt,
	    address, data);
}

int
vmm_vmx_machine_set_irq(struct vmm_machine *mach, uint32_t gsi, bool level)
{
	struct vmm_vmx_machdata *machdata = mach->backend_state;

	if (machdata == NULL || machdata->interrupt == NULL)
		return ENXIO;
	return vmm_vmx_interrupt_ops->irq_set(machdata->interrupt, gsi, level);
}

int
vmm_vmx_machine_raise_legacy(struct vmm_machine *mach, uint8_t vector)
{
	struct vmm_vmx_machdata *machdata = mach->backend_state;

	if (machdata == NULL || machdata->interrupt == NULL)
		return ENXIO;
	return vmm_vmx_interrupt_ops->irq_raise_legacy(machdata->interrupt,
	    vector);
}

int
vmm_vmx_machine_get_ioapic(struct vmm_machine *mach,
	struct vmm_ioapic_state *state)
{
	struct vmm_vmx_machdata *machdata = mach->backend_state;

	if (machdata == NULL || machdata->interrupt == NULL)
		return ENXIO;
	return vmm_vmx_interrupt_ops->machine_get_ioapic(machdata->interrupt,
	    state);
}

int
vmm_vmx_machine_set_ioapic(struct vmm_machine *mach,
	const struct vmm_ioapic_state *state)
{
	struct vmm_vmx_machdata *machdata = mach->backend_state;

	if (machdata == NULL || machdata->interrupt == NULL)
		return ENXIO;
	return vmm_vmx_interrupt_ops->machine_set_ioapic(machdata->interrupt,
	    state);
}

void
vmm_vmx_machine_destroy(struct vmm_machine *mach)
{
	struct vmm_vmx_machdata *machdata = mach->backend_state;

	if (machdata == NULL)
		return;
#ifdef __DragonFly__
	/* Remove host pmap membership before removing the APIC access mapping. */
	pmap_del_all_cpus(mach->vmspace);
#endif
	if (machdata->interrupt != NULL)
		vmm_vmx_interrupt_ops->machine_destroy(machdata->interrupt);
	os_mem_free(machdata, sizeof(*machdata));
	mach->backend_state = NULL;
}

/* -------------------------------------------------------------------------- */

#define CTLS_ONE_ALLOWED(msrval, bitoff) \
	((msrval & __BIT(32 + bitoff)) != 0)
#define CTLS_ZERO_ALLOWED(msrval, bitoff) \
	((msrval & __BIT(bitoff)) == 0)

static int
vmm_vmx_check_ctls(uint64_t msr_ctls, uint64_t msr_true_ctls, uint64_t set_one)
{
	uint64_t basic, val, true_val;
	bool has_true;
	size_t i;

	basic = rdmsr(MSR_IA32_VMX_BASIC);
	has_true = (basic & IA32_VMX_BASIC_TRUE_CTLS) != 0;

	val = rdmsr(msr_ctls);
	if (has_true) {
		true_val = rdmsr(msr_true_ctls);
	} else {
		true_val = val;
	}

	for (i = 0; i < 32; i++) {
		if (!(set_one & __BIT(i))) {
			continue;
		}
		if (!CTLS_ONE_ALLOWED(true_val, i)) {
			return -1;
		}
	}

	return 0;
}

bool
vmm_vmx_apicv_available(void)
{
	if (vmm_vmx_check_ctls(MSR_IA32_VMX_PROCBASED_CTLS,
	    MSR_IA32_VMX_TRUE_PROCBASED_CTLS,
	    PROC_CTLS_USE_TPR_SHADOW | PROC_CTLS_ACTIVATE_CTLS2) != 0) {
		return false;
	}
	return vmm_vmx_check_ctls(MSR_IA32_VMX_PROCBASED_CTLS2,
	    MSR_IA32_VMX_PROCBASED_CTLS2,
	    PROC_CTLS2_VIRT_APIC_ACCESSES | PROC_CTLS2_APIC_REG_VIRT |
	    PROC_CTLS2_VIRT_INT_DELIVERY) == 0;
}

static int
vmm_vmx_init_ctls(uint64_t msr_ctls, uint64_t msr_true_ctls,
    uint64_t set_one, uint64_t set_zero, uint64_t *res)
{
	uint64_t basic, val, true_val;
	bool one_allowed, zero_allowed, has_true;
	size_t i;

	basic = rdmsr(MSR_IA32_VMX_BASIC);
	has_true = (basic & IA32_VMX_BASIC_TRUE_CTLS) != 0;

	val = rdmsr(msr_ctls);
	if (has_true) {
		true_val = rdmsr(msr_true_ctls);
	} else {
		true_val = val;
	}

	for (i = 0; i < 32; i++) {
		one_allowed = CTLS_ONE_ALLOWED(true_val, i);
		zero_allowed = CTLS_ZERO_ALLOWED(true_val, i);

		if (zero_allowed && !one_allowed) {
			if (set_one & __BIT(i))
				return -1;
			*res &= ~__BIT(i);
		} else if (one_allowed && !zero_allowed) {
			if (set_zero & __BIT(i))
				return -1;
			*res |= __BIT(i);
		} else {
			if (set_zero & __BIT(i)) {
				*res &= ~__BIT(i);
			} else if (set_one & __BIT(i)) {
				*res |= __BIT(i);
			} else if (!has_true) {
				*res &= ~__BIT(i);
			} else if (CTLS_ZERO_ALLOWED(val, i)) {
				*res &= ~__BIT(i);
			} else if (CTLS_ONE_ALLOWED(val, i)) {
				*res |= __BIT(i);
			} else {
				return -1;
			}
		}
	}

	return 0;
}

bool
vmm_vmx_ident(void)
{
	cpuid_desc_t descs;
	uint64_t msr;
	int ret;

	x86_get_cpuid(0x00000001, &descs);
	if (!(descs.ecx & CPUID_0_01_ECX_VMX)) {
		return false;
	}

	msr = rdmsr(MSR_IA32_FEATURE_CONTROL);
	if ((msr & IA32_FEATURE_CONTROL_LOCK) != 0 &&
	    (msr & IA32_FEATURE_CONTROL_OUT_SMX) == 0) {
		os_printf("vmm: VMX disabled in BIOS\n");
		return false;
	}

	msr = rdmsr(MSR_IA32_VMX_BASIC);
	if ((msr & IA32_VMX_BASIC_IO_REPORT) == 0) {
		os_printf("vmm: I/O reporting not supported\n");
		return false;
	}
	if (__SHIFTOUT(msr, IA32_VMX_BASIC_MEM_TYPE) != MEM_TYPE_WB) {
		os_printf("vmm: WB memory not supported\n");
		return false;
	}

	/* PG and PE are reported, even if Unrestricted Guests is supported. */
	vmm_vmx_cr0_fixed0 = rdmsr(MSR_IA32_VMX_CR0_FIXED0) & ~(CR0_PG|CR0_PE);
	vmm_vmx_cr0_fixed1 = rdmsr(MSR_IA32_VMX_CR0_FIXED1) | (CR0_PG|CR0_PE);
	ret = vmm_vmx_check_cr(x86_get_cr0(), vmm_vmx_cr0_fixed0, vmm_vmx_cr0_fixed1);
	if (ret == -1) {
		os_printf("vmm: CR0 requirements not satisfied\n");
		return false;
	}

	vmm_vmx_cr4_fixed0 = rdmsr(MSR_IA32_VMX_CR4_FIXED0);
	vmm_vmx_cr4_fixed1 = rdmsr(MSR_IA32_VMX_CR4_FIXED1);
	ret = vmm_vmx_check_cr(x86_get_cr4() | CR4_VMXE, vmm_vmx_cr4_fixed0,
	    vmm_vmx_cr4_fixed1);
	if (ret == -1) {
		os_printf("vmm: CR4 requirements not satisfied\n");
		return false;
	}

	/* Init the CTLSs right now, and check for errors. */
	ret = vmm_vmx_init_ctls(
	    MSR_IA32_VMX_PINBASED_CTLS, MSR_IA32_VMX_TRUE_PINBASED_CTLS,
	    VMM_VMX_PINBASED_CTLS_ONE, VMM_VMX_PINBASED_CTLS_ZERO,
	    &vmm_vmx_pinbased_ctls);
	if (ret == -1) {
		os_printf("vmm: pin-based-ctls requirements not satisfied\n");
		return false;
	}
	ret = vmm_vmx_init_ctls(
	    MSR_IA32_VMX_PROCBASED_CTLS, MSR_IA32_VMX_TRUE_PROCBASED_CTLS,
	    VMM_VMX_PROCBASED_CTLS_ONE, VMM_VMX_PROCBASED_CTLS_ZERO,
	    &vmm_vmx_procbased_ctls);
	if (ret == -1) {
		os_printf("vmm: proc-based-ctls requirements not satisfied\n");
		return false;
	}
	ret = vmm_vmx_init_ctls(
	    MSR_IA32_VMX_PROCBASED_CTLS2, MSR_IA32_VMX_PROCBASED_CTLS2,
	    VMM_VMX_PROCBASED_CTLS2_ONE, VMM_VMX_PROCBASED_CTLS2_ZERO,
	    &vmm_vmx_procbased_ctls2);
	if (ret == -1) {
		os_printf("vmm: proc-based-ctls2 requirements not satisfied\n");
		return false;
	}
	ret = vmm_vmx_check_ctls(
	    MSR_IA32_VMX_PROCBASED_CTLS2, MSR_IA32_VMX_PROCBASED_CTLS2,
	    PROC_CTLS2_INVPCID_ENABLE);
	if (ret != -1) {
		vmm_vmx_procbased_ctls2 |= PROC_CTLS2_INVPCID_ENABLE;
	}
	ret = vmm_vmx_init_ctls(
	    MSR_IA32_VMX_ENTRY_CTLS, MSR_IA32_VMX_TRUE_ENTRY_CTLS,
	    VMM_VMX_ENTRY_CTLS_ONE, VMM_VMX_ENTRY_CTLS_ZERO,
	    &vmm_vmx_entry_ctls);
	if (ret == -1) {
		os_printf("vmm: entry-ctls requirements not satisfied\n");
		return false;
	}
	ret = vmm_vmx_init_ctls(
	    MSR_IA32_VMX_EXIT_CTLS, MSR_IA32_VMX_TRUE_EXIT_CTLS,
	    VMM_VMX_EXIT_CTLS_ONE, VMM_VMX_EXIT_CTLS_ZERO,
	    &vmm_vmx_exit_ctls);
	if (ret == -1) {
		os_printf("vmm: exit-ctls requirements not satisfied\n");
		return false;
	}

	msr = rdmsr(MSR_IA32_VMX_EPT_VPID_CAP);
	if ((msr & IA32_VMX_EPT_VPID_WALKLENGTH_4) == 0) {
		os_printf("vmm: 4-level page tree not supported\n");
		return false;
	}
	if ((msr & IA32_VMX_EPT_VPID_INVEPT) == 0) {
		os_printf("vmm: INVEPT not supported\n");
		return false;
	}
	if ((msr & IA32_VMX_EPT_VPID_INVVPID) == 0) {
		os_printf("vmm: INVVPID not supported\n");
		return false;
	}
	if ((msr & IA32_VMX_EPT_VPID_FLAGS_AD) != 0) {
		vmm_vmx_ept_has_ad = true;
	} else {
		vmm_vmx_ept_has_ad = false;
	}
#ifdef __NetBSD__
	pmap_ept_has_ad = vmm_vmx_ept_has_ad;
#endif
	if (!(msr & IA32_VMX_EPT_VPID_UC) && !(msr & IA32_VMX_EPT_VPID_WB)) {
		os_printf("vmm: EPT UC/WB memory types not supported\n");
		return false;
	}

	vmm_vmx_cpu_has_arch_cap = false;
	x86_get_cpuid(0x00000000, &descs);
	if (descs.eax >= 7) {
		x86_get_cpuid2(0x00000007, 0, &descs);
		if (descs.edx & CPUID_0_07_EDX_ARCH_CAP) {
			vmm_vmx_cpu_has_arch_cap = true;
		}
	}

	return true;
}

static void
vmm_vmx_init_asid(uint32_t maxasid)
{
	size_t allocsz;

	os_mtx_init(&vmm_vmx_asidlock);

	vmm_vmx_maxasid = maxasid;
	allocsz = roundup(maxasid, 8) / 8;
	vmm_vmx_asidmap = os_mem_zalloc(allocsz);

	/* ASID 0 is reserved for the host. */
	vmm_vmx_asidmap[0] |= __BIT(0);
}

static
OS_IPI_FUNC(vmm_vmx_change_cpu)
{
	bool enable = arg != NULL;
	uint64_t msr, cr4;

	if (enable) {
		msr = rdmsr(MSR_IA32_FEATURE_CONTROL);
		if ((msr & IA32_FEATURE_CONTROL_LOCK) == 0) {
			/* Lock now, with VMX-outside-SMX enabled. */
			wrmsr(MSR_IA32_FEATURE_CONTROL, msr |
			    IA32_FEATURE_CONTROL_LOCK |
			    IA32_FEATURE_CONTROL_OUT_SMX);
		}
	}

	if (!enable) {
		vmm_vmx_vmxoff();
	}

	cr4 = x86_get_cr4();
	if (enable) {
		cr4 |= CR4_VMXE;
	} else {
		cr4 &= ~CR4_VMXE;
	}
	x86_set_cr4(cr4);

	if (enable) {
		vmm_vmx_vmxon(&vmxoncpu[os_curcpu_number()].pa);
	}
}

static void
vmm_vmx_init_l1tf(void)
{
	cpuid_desc_t descs;
	uint64_t msr;

	x86_get_cpuid(0x00000000, &descs);
	if (descs.eax < 7) {
		return;
	}

	x86_get_cpuid2(0x00000007, 0, &descs);

	if (descs.edx & CPUID_0_07_EDX_ARCH_CAP) {
		msr = rdmsr(MSR_IA32_ARCH_CAPABILITIES);
		if (msr & IA32_ARCH_SKIP_L1DFL_VMENTRY) {
			/* No mitigation needed. */
			return;
		}
	}

	if (descs.edx & CPUID_0_07_EDX_L1D_FLUSH) {
		/* Enable hardware mitigation. */
		vmm_vmx_msrlist_entry_nmsr += 1;
	}
}

int
vmm_vmx_init(void)
{
	struct vmxon *vmxon;
	uint32_t revision;
	cpuid_desc_t descs;
	os_cpu_t *cpu;
	uint64_t msr;
	paddr_t pa;
	vaddr_t va;
	int error;

	/* Init the ASID bitmap (VPID). */
	vmm_vmx_init_asid(VPID_MAX);

	/* Init the XCR0 mask. */
	vmm_vmx_xcr0_mask = VMM_VMX_XCR0_MASK_DEFAULT & x86_xsave_features;

	/* Init the max basic CPUID leaf. */
	x86_get_cpuid(0x00000000, &descs);
	vmm_vmx_cpuid_max_basic = uimin(descs.eax, VMM_VMX_CPUID_MAX_BASIC);

	/* Init the max extended CPUID leaf. */
	x86_get_cpuid(0x80000000, &descs);
	vmm_vmx_cpuid_max_extended = uimin(descs.eax, VMM_VMX_CPUID_MAX_EXTENDED);

	/* Init the TLB flush op, the EPT flush op and the EPTP type. */
	msr = rdmsr(MSR_IA32_VMX_EPT_VPID_CAP);
	if ((msr & IA32_VMX_EPT_VPID_INVVPID_CONTEXT) != 0) {
		vmm_vmx_tlb_flush_op = VMM_VMX_INVVPID_CONTEXT;
	} else {
		vmm_vmx_tlb_flush_op = VMM_VMX_INVVPID_ALL;
	}
	if ((msr & IA32_VMX_EPT_VPID_INVEPT_CONTEXT) != 0) {
		vmm_vmx_ept_flush_op = VMM_VMX_INVEPT_CONTEXT;
	} else {
		vmm_vmx_ept_flush_op = VMM_VMX_INVEPT_ALL;
	}
	if ((msr & IA32_VMX_EPT_VPID_WB) != 0) {
		vmm_vmx_eptp_type = EPTP_TYPE_WB;
	} else {
		vmm_vmx_eptp_type = EPTP_TYPE_UC;
	}

	/* Init the L1TF mitigation. */
	vmm_vmx_init_l1tf();

	/* Init the global host state. */
	if (vmm_vmx_xcr0_mask != 0) {
		vmm_vmx_global_hstate.xcr0 = x86_get_xcr(0);
	}
	vmm_vmx_global_hstate.star = rdmsr(MSR_STAR);
	vmm_vmx_global_hstate.lstar = rdmsr(MSR_LSTAR);
	vmm_vmx_global_hstate.cstar = rdmsr(MSR_CSTAR);
	vmm_vmx_global_hstate.sfmask = rdmsr(MSR_SFMASK);

	memset(vmxoncpu, 0, sizeof(vmxoncpu));
	revision = vmm_vmx_get_revision();

	OS_CPU_FOREACH(cpu) {
		error = os_contigpa_zalloc(&pa, &va, 1);
		if (error) {
			panic("%s: out of memory", __func__);
		}
		vmxoncpu[os_cpu_number(cpu)].pa = pa;
		vmxoncpu[os_cpu_number(cpu)].va = va;

		vmxon = (struct vmxon *)vmxoncpu[os_cpu_number(cpu)].va;
		vmxon->ident = __SHIFTIN(revision, VMXON_IDENT_REVISION);
	}

	os_ipi_broadcast(vmm_vmx_change_cpu, (void *)true);
	return 0;
}

static void
vmm_vmx_fini_asid(void)
{
	size_t allocsz;

	allocsz = roundup(vmm_vmx_maxasid, 8) / 8;
	os_mem_free(vmm_vmx_asidmap, allocsz);

	os_mtx_destroy(&vmm_vmx_asidlock);
}

void
vmm_vmx_fini(void)
{
	size_t i;

	os_ipi_broadcast(vmm_vmx_change_cpu, (void *)false);

	for (i = 0; i < OS_MAXCPUS; i++) {
		if (vmxoncpu[i].pa != 0)
			os_contigpa_free(vmxoncpu[i].pa, vmxoncpu[i].va, 1);
	}

	vmm_vmx_fini_asid();
}

int
vmm_vmx_capability(struct vmm_x64_capability *capability)
{
	if (capability == NULL)
		return EINVAL;

	capability->xcr0_mask = vmm_vmx_xcr0_mask;
	capability->mxcsr_mask = x86_fpu_mxcsr_mask;
	return 0;
}
