/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm x86-64 public CPU API.
 */
#ifndef VMM_X64_H
#define VMM_X64_H

#define VMM_X64_GPR_COUNT	18
#define VMM_X64_CR_COUNT	6
#define VMM_X64_MSR_COUNT	11
#define VMM_X64_SEG_COUNT	10

struct vmm_segment {
	uint16_t	selector;
	uint16_t	attrib;
	uint32_t	limit;
	uint64_t	base;
} __packed;

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

struct vmm_cpuexit;

#endif /* VMM_X64_H */
