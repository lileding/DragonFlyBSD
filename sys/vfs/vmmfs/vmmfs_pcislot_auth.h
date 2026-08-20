/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI descriptor authorization capability.
 */
#ifndef VMMFS_PCISLOT_AUTH_H
#define VMMFS_PCISLOT_AUTH_H

#include <sys/types.h>

struct vmmfs_pcislot;

struct vmmfs_pcislot_auth {
	struct vmmfs_pcislot *slot;
	ino_t slot_inode;
	uint64_t generation;
	volatile u_int valid;
	volatile u_int references;
};

int vmmfs_pcislot_auth_create(struct vmmfs_pcislot *, uint64_t,
	struct vmmfs_pcislot_auth **);
void vmmfs_pcislot_auth_revoke(struct vmmfs_pcislot_auth *);
int vmmfs_pcislot_auth_check(struct vmmfs_pcislot *);

#endif /* VMMFS_PCISLOT_AUTH_H */
