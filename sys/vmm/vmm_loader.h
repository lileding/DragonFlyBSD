/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The loader object: the desired path to an executable run at start.
 * FS presentation: vmmfs_loader.c.
 *
 * Types (uint64_t/size_t) come from the includer.
 */
#ifndef VMM_LOADER_H
#define VMM_LOADER_H

#define VMM_LOADER_MAX	256

struct vmm_loader {
	/*
	 * Lock map:
	 * mut_path and mut_len are protected by the parent vmm_machine's
	 * token_lifecycle.  vmm_machine copies a stable start-time path
	 * snapshot before calling vmm_loader_run().
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
/* Loader fd/mmap capability objects still hold vmm.ko pager callbacks. */
int	vmm_loader_busy(void);

struct ucred;
struct vm_object;
struct vmm_launch;
typedef int vmm_loader_cancel_fn(void *arg);
/*
 * On entry, *launch is cleared.  On success, it contains the validated launch
 * state produced by fd4; on failure, it remains empty.
 */
int	vmm_loader_run(const char *path, struct vm_object *mem_object,
	    uint64_t mem_size, struct ucred *cred, struct vmm_launch *launch,
	    vmm_loader_cancel_fn *cancel, void *cancel_arg);

#endif /* VMM_LOADER_H */
