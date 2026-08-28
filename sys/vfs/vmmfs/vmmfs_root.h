/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs mount root object.
 */
#ifndef VMMFS_ROOT_H
#define VMMFS_ROOT_H

#include <sys/param.h>
#include "vmmfs_node.h"

#include <sys/thread.h>
#include <sys/tree.h>
#include <sys/types.h>

struct mount;
struct vnode;
struct vmmfs_machine;

#define VMMFS_ROOT_INO 1

/* One stable snapshot of an item in a vmmfs object collection. */
struct vmmfs_item {
	uint64_t id;
	char name[NAME_MAX + 1];
	struct vmmfs_machine *machine;
};

RB_HEAD(vmmfs_machine_tree, vmmfs_machine);

/* One vmmfs mountpoint root and its machine namespace. */
struct vmmfs_root {
	struct vmmfs_node node;
	struct mount *mount;
	struct lwkt_token token;
	struct vmmfs_machine_tree machines;
	unsigned int machine_count;
};

extern struct vop_ops vmmfs_root_vops;

int vmmfs_root_create(struct mount *, struct vmmfs_root **);
int vmmfs_root_destroy(struct vmmfs_root *);

#endif /* VMMFS_ROOT_H */
