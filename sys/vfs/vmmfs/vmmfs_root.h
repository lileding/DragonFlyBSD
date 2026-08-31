/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs mount root object.
 */
#ifndef VMMFS_ROOT_H
#define VMMFS_ROOT_H

#include <sys/types.h>

struct mount;
struct vnode;
struct vmmfs_root;

int vmmfs_root_create(struct mount *, struct vnode **);
ino_t vmmfs_root_allocate_inode(struct vmmfs_root *);

#endif /* VMMFS_ROOT_H */
