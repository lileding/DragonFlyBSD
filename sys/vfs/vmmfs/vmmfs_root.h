/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs mount root object.
 */
#ifndef VMMFS_ROOT_H
#define VMMFS_ROOT_H

#include <sys/types.h>

struct mount;
struct vmmfs_mount;
struct vmmfs_branch;
struct vmmfs_machine;
struct vnode;
struct vmmfs_root;

#define VMMFS_ROOT_INO 1

int vmmfs_root_create(struct mount *, struct vmmfs_root **);
int vmmfs_root_destroy(struct vmmfs_root *);
struct vmmfs_branch *vmmfs_root_branch(struct vmmfs_root *);
struct vmmfs_mount *vmmfs_root_state(struct vmmfs_root *);
ino_t vmmfs_root_allocate_inode(struct vmmfs_root *);
/* Returns a held vnode; caller releases it with vdrop(). */
struct vnode *vmmfs_root_vnode(struct vmmfs_root *);
bool vmmfs_root_empty(struct vmmfs_root *);
void vmmfs_root_machine_dropped(struct vmmfs_root *);
struct vnode *vmmfs_root_machine_vnode(struct vmmfs_root *,
	struct vmmfs_machine *);
void vmmfs_root_invalidate_machine(struct vmmfs_root *,
	struct vmmfs_machine *);

#endif /* VMMFS_ROOT_H */
