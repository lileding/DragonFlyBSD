/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86 launch state produced by fd4 manifest loaders.
 *
 * Types (uint16_t/uint32_t/uint64_t/uint8_t/size_t) come from the includer.
 */
#ifndef VMM_LOADER_X86_H
#define VMM_LOADER_X86_H

#define VMM_X64_NGPR	18
#define VMM_X64_NCR	6
#define VMM_X64_NMSR	11
#define VMM_X64_NSEG	10
#define VMM_X64_MAX_VCPU 256
#define VMM_GPA_RANGE_MAX 32

/*
 * x86 architectural local-APIC MMIO hole.  This is loader ABI, not just an
 * SVM implementation detail: fd3 is sized as mem_size, but this GPA page is
 * platform MMIO rather than guest RAM.  Loaders must not place manifest GPA
 * ranges, page tables, descriptors, stacks, kernels, initramfs, or boot data
 * here.  OS loaders that publish a memory map, such as Linux E820, must mark
 * this page reserved whenever mem_size covers it.
 */
#define VMM_X86_LAPIC_MMIO_GPA	0xfee00000ULL
#define VMM_X86_LAPIC_MMIO_SIZE	0x1000ULL

#define VMM_GPA_RANGE_LOAD		1
#define VMM_GPA_RANGE_BOOT_PARAMS	2
#define VMM_GPA_RANGE_CMDLINE		3
#define VMM_GPA_RANGE_INITRAMFS		4
#define VMM_GPA_RANGE_PAGE_TABLE	5
#define VMM_GPA_RANGE_DESC_TABLE	6
#define VMM_GPA_RANGE_STACK		7
#define VMM_GPA_RANGE_BOOT_DATA		8
#define VMM_GPA_RANGE_GUEST_STACK	9

#define VMM_X64_XCR0_X87		(1ULL << 0)
#define VMM_X64_XCR0_SSE		(1ULL << 1)
#define VMM_X64_XCR0_AVX		(1ULL << 2)
#define VMM_X64_XCR0_BNDREGS		(1ULL << 3)
#define VMM_X64_XCR0_BNDCSR		(1ULL << 4)
#define VMM_X64_XCR0_OPMASK		(1ULL << 5)
#define VMM_X64_XCR0_ZMM_HI256		(1ULL << 6)
#define VMM_X64_XCR0_HI16_ZMM		(1ULL << 7)
#define VMM_X64_XCR0_PKRU		(1ULL << 9)
#define VMM_X64_XCR0_XTILE_CFG		(1ULL << 17)
#define VMM_X64_XCR0_XTILE_DATA		(1ULL << 18)
#define VMM_X64_XCR0_MPX		(VMM_X64_XCR0_BNDREGS | \
					    VMM_X64_XCR0_BNDCSR)
#define VMM_X64_XCR0_AVX512		(VMM_X64_XCR0_OPMASK | \
					    VMM_X64_XCR0_ZMM_HI256 | \
					    VMM_X64_XCR0_HI16_ZMM)
#define VMM_X64_XCR0_XTILE		(VMM_X64_XCR0_XTILE_CFG | \
					    VMM_X64_XCR0_XTILE_DATA)

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

#define VMM_X64_CR_CR0		0
#define VMM_X64_CR_CR2		1
#define VMM_X64_CR_CR3		2
#define VMM_X64_CR_CR4		3
#define VMM_X64_CR_CR8		4
#define VMM_X64_CR_XCR0	5

#define VMM_X64_MSR_EFER	0
#define VMM_X64_MSR_STAR	1
#define VMM_X64_MSR_LSTAR	2
#define VMM_X64_MSR_CSTAR	3
#define VMM_X64_MSR_SFMASK	4
#define VMM_X64_MSR_KERNELGSBASE 5
#define VMM_X64_MSR_SYSENTER_CS	6
#define VMM_X64_MSR_SYSENTER_ESP 7
#define VMM_X64_MSR_SYSENTER_EIP 8
#define VMM_X64_MSR_PAT		9
#define VMM_X64_MSR_TSC		10

#define VMM_X64_SEG_ES		0
#define VMM_X64_SEG_CS		1
#define VMM_X64_SEG_SS		2
#define VMM_X64_SEG_DS		3
#define VMM_X64_SEG_FS		4
#define VMM_X64_SEG_GS		5
#define VMM_X64_SEG_GDT		6
#define VMM_X64_SEG_IDT		7
#define VMM_X64_SEG_LDT		8
#define VMM_X64_SEG_TR		9

struct vmm_x64_seg_state {
	uint16_t	selector;
	uint16_t	attrib;
	uint32_t	limit;
	uint64_t	base;
} __packed;

struct vmm_x64_vcpu_state {
	uint32_t	vcpu_id;
	uint32_t	flags;
	uint64_t	runnable;
	uint64_t	gpr[VMM_X64_NGPR];
	uint64_t	cr[VMM_X64_NCR];
	uint64_t	msr[VMM_X64_NMSR];
	struct vmm_x64_seg_state seg[VMM_X64_NSEG];
	uint64_t	intr_flags;
} __packed;

/*
 * Immutable CPU topology for one machine run.  The BSP launch state remains
 * record-local; secondary CPUs start through the backend's reset/SIPI path.
 */
struct vmm_x64_cpu_topology {
	uint32_t	imm_vcpu_count;
	uint32_t	imm_apic_ids[VMM_X64_MAX_VCPU];
} __packed;

struct vmm_gpa_range {
	uint64_t	start;
	uint64_t	size;
	uint32_t	type;
	uint32_t	flags;
} __packed;

/*
 * Immutable guest TSC template.  A zero rate means host-native TSC frequency;
 * a nonzero rate requests hardware SVM TSC scaling to that frequency.
 */
struct vmm_x64_time_state {
	uint64_t	tsc_hz;
} __packed;

struct vmm_launch {
	uint64_t imm_mem_size;
	uint64_t imm_guest_tsc_hz;
	struct vmm_x64_vcpu_state imm_vcpu0;
	struct vmm_x64_cpu_topology imm_cpu_topology;
	struct vmm_gpa_range imm_ranges[VMM_GPA_RANGE_MAX];
	uint32_t imm_range_count;
};

int	vmm_loader_x86_manifest_load(uint64_t mem_size, const uint8_t *buf,
	    size_t cap, struct vmm_launch *launch);
int	vmm_loader_x86_xcr0_valid(uint64_t xcr0);
int	vmm_loader_x86_pat_valid(uint64_t pat);

#endif /* VMM_LOADER_X86_H */
