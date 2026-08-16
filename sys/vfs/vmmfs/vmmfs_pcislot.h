/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot directory object.
 */
#ifndef VMMFS_PCISLOT_H
#define VMMFS_PCISLOT_H

#include <sys/param.h>
#include <sys/tree.h>
#include <sys/types.h>

#include "vmmfs_pciroot.h"

struct vnode;
struct vop_ops;

struct vmmfs_pcislot {
	RB_ENTRY(vmmfs_pcislot) entry;
	struct vmmfs_pciroot *pciroot;
	struct vnode *vnode;
	ino_t inode;
	char name[NAME_MAX + 1];
};

RB_PROTOTYPE(vmmfs_pcislot_tree, vmmfs_pcislot, entry,
	vmmfs_pcislot_compare);

extern struct vop_ops vmmfs_pcislot_vops;

int vmmfs_pcislot_compare(struct vmmfs_pcislot *, struct vmmfs_pcislot *);
int vmmfs_pcislot_create(struct vmmfs_pciroot *, const char *, size_t,
	struct vmmfs_pcislot **);
int vmmfs_pcislot_destroy(struct vmmfs_pcislot *);

#endif /* VMMFS_PCISLOT_H */
