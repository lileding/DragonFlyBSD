/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine identity node.
 */
#ifndef VMMFS_MACHINE_ID_H
#define VMMFS_MACHINE_ID_H

#include "vmmfs_node.h"

#include <sys/types.h>

struct vmmfs_mount;
struct vmmfs_machine;
struct vnode;
struct vop_ops;

#define VMMFS_MACHINE_ID_MAX 999999U
#define VMMFS_MACHINE_INDEX_MAX 99U

struct vmmfs_machine_id {
	struct vmmfs_node node;
};

int vmmfs_machine_id_init(struct vmmfs_node *,
	struct vmmfs_machine_id *, struct vnode **);

#endif /* VMMFS_MACHINE_ID_H */
