/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI root directory object.
 */
#ifndef VMMFS_PCIROOT_H
#define VMMFS_PCIROOT_H

#include <sys/tree.h>
#include <sys/types.h>

struct vmmfs_machine;
struct vmmfs_pcislot;
struct vnode;
struct vop_ops;

RB_HEAD(vmmfs_pcislot_tree, vmmfs_pcislot);

struct vmmfs_pciroot {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
	struct vmmfs_pcislot_tree slots;
};

extern struct vop_ops vmmfs_pciroot_vops;

int vmmfs_pciroot_create(struct vmmfs_machine *, struct vmmfs_pciroot *);
int vmmfs_pciroot_destroy(struct vmmfs_pciroot *);

#endif /* VMMFS_PCIROOT_H */
