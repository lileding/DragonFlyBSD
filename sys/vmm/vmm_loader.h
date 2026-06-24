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

struct vmm_loader {
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
	    struct ucred *cred, vmm_loader_cancel_fn *cancel, void *cancel_arg);

#endif /* VMM_LOADER_H */
