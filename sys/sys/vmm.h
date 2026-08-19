/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly VMM user ABI.
 *
 * This header defines the x86-64 architectural state exchanged through the
 * VMM loader data channel.  Loaders and vmm(4) must use the same installed
 * copy of this header.
 */
#ifndef _SYS_VMM_H_
#define _SYS_VMM_H_

#include <sys/types.h>

#ifndef _KERNEL
#ifndef CTASSERT
#define CTASSERT(expression) _Static_assert((expression), #expression)
#endif
#endif

#if !defined(__x86_64__)
#error "vmm user ABI is unavailable on this architecture"
#endif

enum vmm_io_width {
	VMM_IO_WIDTH_8 = 1,
	VMM_IO_WIDTH_16 = 2,
	VMM_IO_WIDTH_32 = 4,
	VMM_IO_WIDTH_64 = 8,
};

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
CTASSERT(sizeof(struct vmm_segment) == 16);

/* Architectural interrupt state. */
struct vmm_cpustate_intr {
	uint64_t int_shadow:1;
	uint64_t rsvd:63;
};
CTASSERT(sizeof(struct vmm_cpustate_intr) == 8);

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

/*
 * Caller-owned x86-64 architectural state written by a loader and consumed
 * by vmm(4).  Its layout is an ABI contract and must remain stable.
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
CTASSERT(sizeof(struct vmm_cpustate) == 1008);

#endif /* _SYS_VMM_H_ */
