/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs stopped declaration node.
 */
#ifndef VMMFS_STOPPED_H
#define VMMFS_STOPPED_H

#include <sys/types.h>

struct vmmfs_machine;
struct vnode;
struct vop_ops;

struct vmmfs_stopped {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
};

extern struct vop_ops vmmfs_stopped_vops;

int vmmfs_stopped_init(struct vmmfs_machine *, struct vmmfs_stopped *);
int vmmfs_stopped_fini(struct vmmfs_stopped *);

#endif /* VMMFS_STOPPED_H */
