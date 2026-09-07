/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs mount root object.
 */
#ifndef VMMFS_ROOT_H
#define VMMFS_ROOT_H

#include <sys/types.h>

struct mount;
struct vmmfs_node;
struct vmmfs_root;

int vmmfs_root_create(struct mount *, struct vmmfs_node **);
ino_t vmmfs_root_allocate_inode(struct vmmfs_root *);
/* VFS unregister must wait for the remaining descendant references. */
int vmmfs_root_module_fini(void);

#endif /* VMMFS_ROOT_H */
