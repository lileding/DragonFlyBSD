/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs serial-port directory object.
 */
#ifndef VMMFS_SERIALROOT_H
#define VMMFS_SERIALROOT_H

#include "vmmfs_branch.h"

#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct vmmfs_serialroot_registry;
struct vnode;

struct vmmfs_serialroot {
	struct vmmfs_branch branch;
	ino_t inode;
	struct vmmfs_serialroot_registry *registry;
};

struct vmmfs_serialport_info {
	unsigned int number;
	uint16_t base;
	unsigned int gsi;
};

int vmmfs_serialroot_init(struct vmmfs_mount *, struct vmmfs_branch *,
	struct vmmfs_serialroot *, struct vnode **);
void vmmfs_serialroot_deactivate_ports(struct vmmfs_serialroot *);
int vmmfs_serialroot_start(struct vmmfs_serialroot *, vmm_machine_t);
int vmmfs_serialroot_stop(struct vmmfs_serialroot *);
size_t vmmfs_serialroot_port_count(struct vmmfs_serialroot *);
int vmmfs_serialroot_port_info(struct vmmfs_serialroot *, size_t,
	struct vmmfs_serialport_info *);

#endif /* VMMFS_SERIALROOT_H */
