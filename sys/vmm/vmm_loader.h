/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The loader config object: the desired path to an executable (often a sh
 * script) run at start.  Pure value + logic, no kernel/VFS deps -- reusable
 * vmm_ core.  FS presentation: vmmfs_loader.c.
 *
 * Types (size_t) come from the includer.
 */
#ifndef VMM_LOADER_H
#define VMM_LOADER_H

#define VMM_LOADER_MAX	256

struct vmm_loader {
	char		path[VMM_LOADER_MAX];
	size_t		len;		/* 0 = unset */
};

/* Parse + store the trimmed path.  1 = updated, 0 = reject. */
int	vmm_loader_parse(struct vmm_loader *l, const char *buf, size_t len);
/* Path + trailing newline (file contents). */
size_t	vmm_loader_format(const struct vmm_loader *l, char *out, size_t cap);
/* Path without trailing newline (for start-time resolution). */
size_t	vmm_loader_path(const struct vmm_loader *l, char *out, size_t cap);
int	vmm_loader_is_set(const struct vmm_loader *l);

#endif /* VMM_LOADER_H */
