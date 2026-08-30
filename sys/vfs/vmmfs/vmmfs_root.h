/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs mount root object.
 */
#ifndef VMMFS_ROOT_H
#define VMMFS_ROOT_H

#include <sys/param.h>
#include "vmmfs_branch.h"

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
	struct vnode *vnode;
};

/* The root, not the machine, owns the namespace vnode reference. */
struct vmmfs_root_machine {
	RB_ENTRY(vmmfs_root_machine) entry;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
};

RB_HEAD(vmmfs_machine_tree, vmmfs_root_machine);

int vmmfs_root_machine_compare(struct vmmfs_root_machine *,
	struct vmmfs_root_machine *);
RB_PROTOTYPE(vmmfs_machine_tree, vmmfs_root_machine, entry,
	vmmfs_root_machine_compare);
/* One vmmfs mountpoint root and its machine namespace. */
struct vmmfs_root {
	struct vmmfs_branch branch;
	struct mount *mount;
	struct vmmfs_machine_tree machines;
	unsigned int machine_count;
};

extern struct vop_ops vmmfs_root_vops;

int vmmfs_root_create(struct mount *, struct vmmfs_root **);
int vmmfs_root_destroy(struct vmmfs_root *);
/* Returns a held registry vnode; caller releases it with vdrop(). */
struct vnode *vmmfs_root_machine_vnode(struct vmmfs_root *,
	struct vmmfs_machine *);
void vmmfs_root_invalidate_machine(struct vmmfs_root *,
	struct vmmfs_machine *);

#endif /* VMMFS_ROOT_H */
