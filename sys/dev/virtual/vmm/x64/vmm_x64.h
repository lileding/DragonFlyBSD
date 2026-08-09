/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm x86-64 public CPU API.
 */
#ifndef VMM_X64_H
#define VMM_X64_H

#include <sys/types.h>

/* General-purpose register indices. */
#define VMM_X64_GPR_RAX	0
#define VMM_X64_GPR_RCX	1
#define VMM_X64_GPR_RDX	2
#define VMM_X64_GPR_RBX	3
#define VMM_X64_GPR_RSP	4
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
#define VMM_X64_GPR_RIP	16
#define VMM_X64_GPR_RFLAGS	17
#define VMM_X64_GPR_COUNT	18

/* Control register indices. */
#define VMM_X64_CR_CR0		0
#define VMM_X64_CR_CR2		1
#define VMM_X64_CR_CR3		2
#define VMM_X64_CR_CR4		3
#define VMM_X64_CR_CR8		4
#define VMM_X64_CR_XCR0	5
#define VMM_X64_CR_COUNT	6

/* Debug register indices. */
#define VMM_X64_DR_DR0		0
#define VMM_X64_DR_DR1		1
#define VMM_X64_DR_DR2		2
#define VMM_X64_DR_DR3		3
#define VMM_X64_DR_DR6		4
#define VMM_X64_DR_DR7		5
#define VMM_X64_DR_COUNT	6

/* MSR state indices. */
#define VMM_X64_MSR_EFER		0
#define VMM_X64_MSR_STAR		1
#define VMM_X64_MSR_LSTAR		2
#define VMM_X64_MSR_CSTAR		3
#define VMM_X64_MSR_SFMASK		4
#define VMM_X64_MSR_KERNELGSBASE	5
#define VMM_X64_MSR_SYSENTER_CS	6
#define VMM_X64_MSR_SYSENTER_ESP	7
#define VMM_X64_MSR_SYSENTER_EIP	8
#define VMM_X64_MSR_PAT		9
#define VMM_X64_MSR_TSC		10
#define VMM_X64_MSR_COUNT		11

/* Segment state indices. */
#define VMM_X64_SEG_ES		0
#define VMM_X64_SEG_CS		1
#define VMM_X64_SEG_SS		2
#define VMM_X64_SEG_DS		3
#define VMM_X64_SEG_FS		4
#define VMM_X64_SEG_GS		5
#define VMM_X64_SEG_GDT	6
#define VMM_X64_SEG_IDT	7
#define VMM_X64_SEG_LDT	8
#define VMM_X64_SEG_TR		9
#define VMM_X64_SEG_COUNT	10

/* CPUID bits to clear and set after VMM applies its base CPU policy. */
struct vmm_cpuid_mask {
	uint32_t leaf;
	uint32_t clear_eax;
	uint32_t clear_ebx;
	uint32_t clear_ecx;
	uint32_t clear_edx;
	uint32_t set_eax;
	uint32_t set_ebx;
	uint32_t set_ecx;
	uint32_t set_edx;
};

/* Capabilities of the selected x86-64 backend. */
struct vmm_x64_capability {
	uint64_t xcr0_mask;
	uint32_t mxcsr_mask;
	uint32_t cpuid_mask_max;
};

/* One x86-64 segment descriptor image. */
struct vmm_segment {
	uint16_t	selector;
	struct {
		uint16_t type:4;
		uint16_t s:1;
		uint16_t dpl:2;
		uint16_t p:1;
		uint16_t avl:1;
		uint16_t l:1;
		uint16_t def:1;
		uint16_t g:1;
		uint16_t rsvd:4;
	} attrib;
	uint32_t	limit;
	uint64_t	base;
};

/* Architectural interrupt state. */
struct vmm_cpustate_intr {
	uint64_t int_shadow:1;
	uint64_t int_window_exiting:1;
	uint64_t nmi_window_exiting:1;
	uint64_t evt_pending:1;
	uint64_t rsvd:60;
};

/* FXSAVE-format x87 and SSE architectural state. */
union vmm_cpustate_fpu_addr {
	uint64_t fa_64;
	struct {
		uint32_t fa_off;
		uint16_t fa_seg;
		uint16_t fa_opcode;
	} fa_32;
};
CTASSERT(sizeof(union vmm_cpustate_fpu_addr) == 8);

struct vmm_cpustate_fpu_mmreg {
	uint64_t mm_significand;
	uint16_t mm_exp_sign;
	uint8_t mm_rsvd[6];
};
CTASSERT(sizeof(struct vmm_cpustate_fpu_mmreg) == 16);

struct vmm_cpustate_fpu_xmmreg {
	uint8_t xmm_bytes[16];
};
CTASSERT(sizeof(struct vmm_cpustate_fpu_xmmreg) == 16);

struct vmm_cpustate_fpu {
	uint16_t fx_cw;
	uint16_t fx_sw;
	uint8_t fx_tw;
	uint8_t fx_zero;
	uint16_t fx_opcode;
	union vmm_cpustate_fpu_addr fx_ip;
	union vmm_cpustate_fpu_addr fx_dp;
	uint32_t fx_mxcsr;
	uint32_t fx_mxcsr_mask;
	struct vmm_cpustate_fpu_mmreg fx_87_ac[8];
	struct vmm_cpustate_fpu_xmmreg fx_xmm[16];
	uint8_t fx_rsvd[96];
} __aligned(16);
CTASSERT(sizeof(struct vmm_cpustate_fpu) == 512);

#define VMM_X64_STATE_SEGS	0x01
#define VMM_X64_STATE_GPRS	0x02
#define VMM_X64_STATE_CRS	0x04
#define VMM_X64_STATE_DRS	0x08
#define VMM_X64_STATE_MSRS	0x10
#define VMM_X64_STATE_INTR	0x20
#define VMM_X64_STATE_FPU	0x40
#define VMM_X64_STATE_ALL	\
	(VMM_X64_STATE_SEGS | VMM_X64_STATE_GPRS | \
	 VMM_X64_STATE_CRS | VMM_X64_STATE_DRS | \
	 VMM_X64_STATE_MSRS | VMM_X64_STATE_INTR | \
	 VMM_X64_STATE_FPU)

/*
 * Caller-owned x86-64 architectural state.  VMM reads and updates this single
 * state image across vmm_vcpu_run(); callers must serialize concurrent access.
 */
struct vmm_cpustate {
	struct vmm_segment segs[VMM_X64_SEG_COUNT];
	uint64_t gprs[VMM_X64_GPR_COUNT];
	uint64_t crs[VMM_X64_CR_COUNT];
	uint64_t drs[VMM_X64_DR_COUNT];
	uint64_t msrs[VMM_X64_MSR_COUNT];
	struct vmm_cpustate_intr intr;
	struct vmm_cpustate_fpu fpu;
};

struct vmm_cpuexit_memory {
	int prot;
	uint64_t gpa;
	uint8_t inst_len;
	uint8_t inst_bytes[15];
};

struct vmm_cpuexit_io {
	bool in;
	uint16_t port;
	int8_t seg;
	uint8_t address_size;
	uint8_t operand_size;
	bool rep;
	bool str;
	uint64_t npc;
};

struct vmm_cpuexit_rdmsr {
	uint32_t msr;
	uint64_t npc;
};

struct vmm_cpuexit_wrmsr {
	uint32_t msr;
	uint64_t val;
	uint64_t npc;
};

struct vmm_cpuexit_insn {
	uint64_t npc;
};

struct vmm_cpuexit_invalid {
	uint64_t hwcode;
};

#define VMM_CPUEXIT_NONE		0x0000000000000000ULL
#define VMM_CPUEXIT_INVALID		0xFFFFFFFFFFFFFFFFULL
#define VMM_CPUEXIT_MEMORY		0x0000000000000001ULL
#define VMM_CPUEXIT_IO			0x0000000000000002ULL
#define VMM_CPUEXIT_SHUTDOWN		0x0000000000001000ULL
#define VMM_CPUEXIT_INT_READY		0x0000000000001001ULL
#define VMM_CPUEXIT_NMI_READY		0x0000000000001002ULL
#define VMM_CPUEXIT_HALTED		0x0000000000001003ULL
#define VMM_CPUEXIT_TPR_CHANGED		0x0000000000001004ULL
#define VMM_CPUEXIT_RDMSR		0x0000000000002000ULL
#define VMM_CPUEXIT_WRMSR		0x0000000000002001ULL
#define VMM_CPUEXIT_MONITOR		0x0000000000002002ULL
#define VMM_CPUEXIT_MWAIT		0x0000000000002003ULL
#define VMM_CPUEXIT_CPUID		0x0000000000002004ULL

/* x86 architectural event classes accepted by a backend. */
#define VMM_CPUEVENT_EXCP	0
#define VMM_CPUEVENT_INTR	1

/* One architectural event requested by the caller before a vCPU run. */
struct vmm_cpuevent {
	uint8_t type;
	uint8_t vector;
	uint64_t error;
};

/*
 * One x86-64 architectural VM exit.  reason selects the valid union member;
 * exitstate reports the interrupt-delivery state at the return boundary.
 */
struct vmm_cpuexit {
	uint64_t reason;
	union {
		struct vmm_cpuexit_memory mem;
		struct vmm_cpuexit_io io;
		struct vmm_cpuexit_rdmsr rdmsr;
		struct vmm_cpuexit_wrmsr wrmsr;
		struct vmm_cpuexit_insn insn;
		struct vmm_cpuexit_invalid inv;
	} u;
	struct {
		uint64_t rflags;
		uint64_t cr8;
		uint64_t int_shadow:1;
		uint64_t int_window_exiting:1;
		uint64_t nmi_window_exiting:1;
		uint64_t evt_pending:1;
		uint64_t rsvd:60;
	} exitstate;
};

#endif /* VMM_X64_H */
