/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs loader declaration object.
 */
#ifndef VMMFS_LOADER_H
#define VMMFS_LOADER_H

#include <sys/param.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct vmmfs_loader;
struct vmmfs_memory;
struct ucred;

struct vmmfs_loader_spec {
	/* A NUL-terminated, one-page shell script executed with fd 2 and fd 3. */
	char script[PAGE_SIZE];
};

extern struct vop_ops vmmfs_loader_vops;
int vmmfs_loader_create(struct vmmfs_machine *, struct vmmfs_loader *);
int vmmfs_loader_destroy(struct vmmfs_loader *);
int vmmfs_loader_init(void);
int vmmfs_loader_uninit(void);
int vmmfs_loader_run(struct vmmfs_loader *, struct vmmfs_memory *,
	struct ucred *, struct vmm_cpustate *);

struct vmmfs_loader {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
};

#endif /* VMMFS_LOADER_H */
