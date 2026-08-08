/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The filesystem identity of one machine.  This is deliberately empty of
 * VMM execution state; that state will be added only after the root namespace
 * has a correct vnode lifecycle.
 */
#ifndef VMMFS_MACHINE_H
#define VMMFS_MACHINE_H

#include <sys/param.h>
#include <sys/tree.h>

#define VMMFS_MACHINE_NAME_MAX 63
#define VMMFS_MACHINE_INO 2

struct mount;
struct vop_ops;
struct vnode;

struct vmmfs_machine;

struct vmmfs_machine_name {
	RB_ENTRY(vmmfs_machine_name) entry;
	struct vmmfs_machine *machine;
	char name[VMMFS_MACHINE_NAME_MAX + 1];
};

struct vmmfs_machine {
	struct mount *mount;
	struct vnode *parent;
	struct vnode *vnode;
	struct vmmfs_machine_name *name_entry;
};

RB_HEAD(vmmfs_machine_tree, vmmfs_machine_name);
int vmmfs_machine_name_cmp(struct vmmfs_machine_name *,
	struct vmmfs_machine_name *);
RB_PROTOTYPE(vmmfs_machine_tree, vmmfs_machine_name, entry,
	vmmfs_machine_name_cmp);

extern struct vop_ops vmmfs_machine_vops;

int vmmfs_machine_create(struct mount *, struct vnode *, struct vop_ops **,
	const char *, int, struct vmmfs_machine **);
void vmmfs_machine_free(struct vmmfs_machine *);

#endif /* VMMFS_MACHINE_H */
