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

struct vmm_mem {
	/*
	 * Lock map:
	 * mut_bytes is protected by the parent vmm_machine's token_lifecycle.
	 * own_mut_backing is published/detached while holding that token, but
	 * vmm_mem_prepare() allocates and wires pages outside the token before
	 * publication, and vmm_mem_release_backing() unwires/deallocates after
	 * detach outside the token.
	 */
	uint64_t	mut_bytes;		/* 0 = unset */
	struct vmm_mem_backing *own_mut_backing;
};

/* Parse number[KkMmGg], > 0, large-page aligned.  1 = updated, 0 = reject. */
int	vmm_mem_parse(struct vmm_mem *m, const char *buf, size_t len);
size_t	vmm_mem_format(const struct vmm_mem *m, char *out, size_t cap);
int	vmm_mem_is_set(const struct vmm_mem *m);

struct vm_object;
int	vmm_mem_prepare(struct vmm_mem *m);
void	vmm_mem_release(struct vmm_mem *m);
struct vmm_mem_backing *vmm_mem_detach(struct vmm_mem *m);
void	vmm_mem_release_backing(struct vmm_mem_backing *b);
struct vm_object *vmm_mem_object(struct vmm_mem *m);
uint64_t vmm_mem_size(struct vmm_mem *m);
int	vmm_mem_gpa_pa(struct vmm_mem *m, uint64_t gpa, uint64_t *pa);

#endif /* VMM_MEM_H */
