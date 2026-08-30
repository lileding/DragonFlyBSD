/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs serial-port directory object.
 */
#ifndef VMMFS_SERIALROOT_H
#define VMMFS_SERIALROOT_H

#include "vmmfs_branch.h"

#include <sys/tree.h>
#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct vmmfs_serialport;
struct vnode;
struct vop_ops;

struct vmmfs_serialroot_port {
	RB_ENTRY(vmmfs_serialroot_port) entry;
	struct vmmfs_serialport *port;
	struct vnode *vnode;
};

RB_HEAD(vmmfs_serialport_tree, vmmfs_serialroot_port);
RB_PROTOTYPE(vmmfs_serialport_tree, vmmfs_serialroot_port, entry,
	vmmfs_serialroot_port_compare);

struct vmmfs_serialroot {
	struct vmmfs_branch branch;
	ino_t inode;
	struct vmmfs_serialport_tree ports;
};

extern struct vop_ops vmmfs_serialroot_vops;

int vmmfs_serialroot_init(struct vmmfs_machine *, struct vmmfs_serialroot *, struct vnode **);
void vmmfs_serialroot_deactivate_begin(struct vmmfs_serialroot *);
void vmmfs_serialroot_deactivate_ports(struct vmmfs_serialroot *);
int vmmfs_serialroot_start(struct vmmfs_serialroot *, vmm_machine_t);
int vmmfs_serialroot_stop(struct vmmfs_serialroot *);

#endif /* VMMFS_SERIALROOT_H */
