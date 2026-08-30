/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs stopped declaration node.
 */
#ifndef VMMFS_STOPPED_H
#define VMMFS_STOPPED_H

#include "vmmfs_node.h"

#include <sys/types.h>

struct vmmfs_machine;
struct vnode;
struct vop_ops;

struct vmmfs_stopped {
	struct vmmfs_node node;
	ino_t inode;
};

extern struct vop_ops vmmfs_stopped_vops;

int vmmfs_stopped_create(struct vmmfs_machine *, struct vmmfs_stopped **,
	struct vnode **);

#endif /* VMMFS_STOPPED_H */
