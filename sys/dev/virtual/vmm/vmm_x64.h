/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm x86-64 public CPU API.
 */
#ifndef VMM_X64_H
#define VMM_X64_H

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
	uint16_t	attrib;
	uint32_t	limit;
	uint64_t	base;
} __packed;

/*
 * Caller-owned architectural state for one x86-64 vCPU.  The VMM updates this
 * object across VM entry and exit; callers must serialize changes with run().
 */
struct vmm_cpustate {
	uint32_t	id;
	uint32_t	flags;
	uint64_t	runnable;
	uint64_t	gpr[VMM_X64_GPR_COUNT];
	uint64_t	cr[VMM_X64_CR_COUNT];
	uint64_t	msr[VMM_X64_MSR_COUNT];
	struct vmm_segment seg[VMM_X64_SEG_COUNT];
	uint64_t	intr_flags;
} __packed;

/*
 * One architectural VM exit.  code, info1, and info2 are raw hardware fields;
 * the caller interprets them through the selected architecture backend.  rip
 * and instruction bytes are a stable snapshot at VM exit.
 */
struct vmm_cpuexit {
	uint64_t	code;
	uint64_t	info1;
	uint64_t	info2;
	uint64_t	rip;
	uint8_t		inst_len;
	uint8_t		inst_bytes[15];
};

#endif /* VMM_X64_H */
