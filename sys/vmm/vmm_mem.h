/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The mem object: desired guest memory size plus kernel backing object.
 * FS presentation: vmmfs_mem.c.
 *
 * Types (uint64_t/size_t) come from the includer.
 */
#ifndef VMM_MEM_H
#define VMM_MEM_H

/*
 * The current pc64 backend maps guest RAM through a DragonFly machine
 * vmspace, matching NVMM's address capacity.  Keep the cap at the config
 * boundary so start workers never accept a memory size the backend cannot map.
 */
#define VMM_MEM_ALIGN	(2ull * 1024 * 1024)
#define VMM_MEM_MAX	(127ull * 1024 * (1ull << 30))

struct vmm_mem {
	/*
	 * Lock map:
	 * mut_bytes is protected by the parent vmm_machine's token_lifecycle.
	 * own_mut_backing is published by vmm_mem_publish() and detached by
	 * vmm_mem_detach() while holding that token.  vmm_mem_prepare()
	 * creates the guest RAM vm_object and machine vmspace outside the
	 * token, and vmm_mem_release_backing() tears them down after detach
	 * outside the token.  Guest pages are allocated lazily by loader mmap
	 * faults or by vCPU nested-page-fault handling.
	 */
	uint64_t	mut_bytes;		/* 0 = unset */
	struct vmm_mem_backing *own_mut_backing;
};

/*
 * Parse number[KkMmGg], > 0, <= VMM_MEM_MAX, VMM_MEM_ALIGN aligned.
 * 1 = updated, 0 = reject.
 */
int	vmm_mem_parse(struct vmm_mem *m, const char *buf, size_t len);
size_t	vmm_mem_format(const struct vmm_mem *m, char *out, size_t cap);
int	vmm_mem_is_set(const struct vmm_mem *m);

struct vm_object;
struct vmspace;
int	vmm_mem_prepare(uint64_t bytes, struct vmm_mem_backing **backingp);
int	vmm_mem_publish(struct vmm_mem *m, struct vmm_mem_backing *backing);
struct vmm_mem_backing *vmm_mem_detach(struct vmm_mem *m);
void	vmm_mem_release_backing(struct vmm_mem_backing *b);
struct vm_object *vmm_mem_object(struct vmm_mem *m);
struct vmspace *vmm_mem_vmspace(struct vmm_mem *m);
uint64_t vmm_mem_size(struct vmm_mem *m);
int	vmm_mem_fault_gpa(struct vmm_mem *m, uint64_t gpa, int prot);

#endif /* VMM_MEM_H */
