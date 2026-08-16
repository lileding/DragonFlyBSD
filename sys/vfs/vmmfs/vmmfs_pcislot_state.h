/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot state information node.
 */
#ifndef VMMFS_PCISLOT_STATE_H
#define VMMFS_PCISLOT_STATE_H

#include <sys/types.h>

struct vmmfs_pcislot;
struct vnode;
struct vop_ops;

struct vmmfs_pcislot_state {
	struct vmmfs_pcislot *slot;
	struct vnode *vnode;
	ino_t inode;
};

extern struct vop_ops vmmfs_pcislot_state_vops;

int vmmfs_pcislot_state_create(struct vmmfs_pcislot *,
	struct vmmfs_pcislot_state *);
int vmmfs_pcislot_state_destroy(struct vmmfs_pcislot_state *);

#endif /* VMMFS_PCISLOT_STATE_H */
