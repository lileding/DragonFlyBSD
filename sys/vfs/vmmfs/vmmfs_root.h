/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef VMMFS_ROOT_H
#define VMMFS_ROOT_H

struct mount;
struct vop_ops;
struct vnode;

extern struct vop_ops vmmfs_root_vops;

int vmmfs_root_mount(struct mount *);
int vmmfs_root_unmount(struct mount *, int);
int vmmfs_root_vnode(struct mount *, struct vnode **);

#endif /* VMMFS_ROOT_H */
