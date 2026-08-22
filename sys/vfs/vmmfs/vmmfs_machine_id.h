/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine identity node.
 */
#ifndef VMMFS_MACHINE_ID_H
#define VMMFS_MACHINE_ID_H

#include <sys/types.h>

struct vmmfs_machine;
struct vnode;
struct vop_ops;

#define VMMFS_MACHINE_ID_LENGTH 6
#define VMMFS_MACHINE_INDEX_LENGTH 2
#define VMMFS_MACHINE_ID_MAX 999999U
#define VMMFS_MACHINE_INDEX_MAX 99U

struct vmmfs_machine_id {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
};

extern struct vop_ops vmmfs_machine_id_vops;

int vmmfs_machine_id_init(struct vmmfs_machine *,
	struct vmmfs_machine_id *);
int vmmfs_machine_id_fini(struct vmmfs_machine_id *);

#endif /* VMMFS_MACHINE_ID_H */
