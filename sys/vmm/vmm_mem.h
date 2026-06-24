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
	uint64_t	bytes;		/* 0 = unset */
	struct vmm_mem_backing *backing;
};

/* Parse number[KkMmGg], > 0, large-page aligned.  1 = updated, 0 = reject. */
int	vmm_mem_parse(struct vmm_mem *m, const char *buf, size_t len);
size_t	vmm_mem_format(const struct vmm_mem *m, char *out, size_t cap);
int	vmm_mem_is_set(const struct vmm_mem *m);

struct vm_object;
int	vmm_mem_prepare(struct vmm_mem *m);
void	vmm_mem_release(struct vmm_mem *m);
struct vm_object *vmm_mem_object(struct vmm_mem *m);

#endif /* VMM_MEM_H */
