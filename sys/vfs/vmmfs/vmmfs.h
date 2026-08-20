/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs filesystem interfaces.
 */
#ifndef VMMFS_H
#define VMMFS_H

#include <sys/malloc.h>
#include <sys/types.h>

#include "vmmfs_machine.h"
#include "vmmfs_root.h"

struct mount;
struct vop_ops;
struct vnode;

struct vmmfs_mount {
	struct mount *mount;
	volatile u_int next_inode;
	struct vmmfs_root *root;
	struct vop_ops *root_vops;
	struct vop_ops *machine_vops;
	struct vop_ops *vcpu_vops;
	struct vop_ops *memory_vops;
	struct vop_ops *loader_vops;
	struct vop_ops *stopped_vops;
	struct vop_ops *events_vops;
	struct vop_ops *serialroot_vops;
	struct vop_ops *serialport_vops;
	struct vop_ops *pciroot_vops;
	struct vop_ops *pcislot_vops;
	struct vop_ops *pcislot_bdf_vops;
	struct vop_ops *pcislot_descriptor_vops;
	struct vop_ops *pcislot_config_vops;
	struct vop_ops *pcislot_resource_vops;
	struct vop_ops *pcislot_events_vops;
};

MALLOC_DECLARE(M_VMMFS);

/*
 * Revoke a VMMFS vnode and leave a permanent mount for late close handling.
 * The caller transfers the object's vnode reference to this function.
 */
void vmmfs_vnode_revoke(struct vnode *);

#endif /* VMMFS_H */
