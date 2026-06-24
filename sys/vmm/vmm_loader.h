/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The loader object: the desired path to an executable run at start.
 * FS presentation: vmmfs_loader.c.
 *
 * Types (size_t) come from the includer.
 */
#ifndef VMM_LOADER_H
#define VMM_LOADER_H

#define VMM_LOADER_MAX	256

#define VMM_X64_NGPR	18
#define VMM_X64_NCR	6
#define VMM_X64_NMSR	11
#define VMM_X64_NSEG	10
#define VMM_GPA_RANGE_MAX 32

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
#define VMM_X64_CR_CR3		3
#define VMM_X64_CR_CR4		4
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

struct vmm_gpa_range {
	uint64_t	start;
	uint64_t	size;
	uint32_t	type;
	uint32_t	flags;
} __packed;

struct vmm_launch {
	uint64_t imm_mem_size;
	struct vmm_x64_vcpu_state imm_vcpu0;
	struct vmm_gpa_range imm_ranges[VMM_GPA_RANGE_MAX];
	uint32_t imm_range_count;
};

struct vmm_loader {
	/*
	 * Lock map:
	 * mut_path and mut_len are protected by the parent vmm_machine's
	 * token_lifecycle.  vmm_loader_run() receives a stable start-time
	 * snapshot only after vmm_machine has accepted a start worker.
	 */
	char		mut_path[VMM_LOADER_MAX];
	size_t		mut_len;		/* 0 = unset */
};

/* Parse + store the trimmed path.  1 = updated, 0 = reject. */
int	vmm_loader_parse(struct vmm_loader *l, const char *buf, size_t len);
/* Path + trailing newline (file contents). */
size_t	vmm_loader_format(const struct vmm_loader *l, char *out, size_t cap);
/* Path without trailing newline (for start-time resolution). */
size_t	vmm_loader_path(const struct vmm_loader *l, char *out, size_t cap);
int	vmm_loader_is_set(const struct vmm_loader *l);

struct ucred;
struct vmm_mem;
typedef int vmm_loader_cancel_fn(void *arg);
int	vmm_loader_run(struct vmm_loader *loader, struct vmm_mem *mem,
	    struct ucred *cred, struct vmm_launch *launch,
	    vmm_loader_cancel_fn *cancel, void *cancel_arg);

#endif /* VMM_LOADER_H */
