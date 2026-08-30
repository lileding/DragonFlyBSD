/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs filesystem interfaces.
 */
#ifndef VMMFS_H
#define VMMFS_H

#include <sys/malloc.h>
#include <sys/types.h>
#include <sys/vmmfs.h>

struct mount;
struct vmmfs_root;
struct vop_ops;
struct vnode;

struct vmmfs_mount {
	struct mount *mount;
	ino_t root_inode;
	struct vnode *root_vnode;
	struct vop_ops *root_vops;
	struct vop_ops *machine_vops;
	struct vop_ops *machine_id_vops;
	struct vop_ops *vcpu_vops;
	struct vop_ops *memory_vops;
	struct vop_ops *loader_vops;
	struct vop_ops *boot_vops;
	struct vop_ops *stopped_vops;
	struct vop_ops *events_vops;
	struct vop_ops *serialroot_vops;
	struct vop_ops *serialport_vops;
	struct vop_ops *pciroot_vops;
	struct vop_ops *pcislot_vops;
	struct vop_ops *pcislot_descriptor_vops;
	struct vop_ops *pcislot_config_vops;
	struct vop_ops *pcislot_resource_vops;
	struct vop_ops *pcislot_events_vops;
};

MALLOC_DECLARE(M_VMMFS);

#endif /* VMMFS_H */
