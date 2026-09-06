/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs stopped declaration node.
 */
#ifndef VMMFS_STOPPED_H
#define VMMFS_STOPPED_H

#include "vmmfs_node.h"

#include <sys/types.h>

struct vmmfs_mount;
struct vmmfs_machine;
struct vnode;
struct vop_ops;

struct vmmfs_stopped {
	struct vmmfs_node node;
};

int vmmfs_stopped_create(struct vmmfs_mount *, struct vmmfs_node *,
	struct vnode **);

#endif /* VMMFS_STOPPED_H */
