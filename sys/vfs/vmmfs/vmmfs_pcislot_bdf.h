/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot BDF information node.
 */
#ifndef VMMFS_PCISLOT_BDF_H
#define VMMFS_PCISLOT_BDF_H

#include <sys/types.h>

struct vmmfs_pcislot;
struct vnode;
struct vop_ops;

struct vmmfs_pcislot_bdf {
	struct vmmfs_pcislot *slot;
	struct vnode *vnode;
	ino_t inode;
};

extern struct vop_ops vmmfs_pcislot_bdf_vops;

int vmmfs_pcislot_bdf_create(struct vmmfs_pcislot *,
	struct vmmfs_pcislot_bdf *);
int vmmfs_pcislot_bdf_destroy(struct vmmfs_pcislot_bdf *);

#endif /* VMMFS_PCISLOT_BDF_H */
