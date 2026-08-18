/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI root directory object.
 */
#ifndef VMMFS_PCIROOT_H
#define VMMFS_PCIROOT_H

#include <sys/tree.h>
#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct vmmfs_pcislot;
struct vnode;
struct vop_ops;

RB_HEAD(vmmfs_pcislot_tree, vmmfs_pcislot);

struct vmmfs_pciroot {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
	uint32_t bdf_mask;
	struct vmmfs_pcislot_tree slots;
	vmm_machine_t runtime_machine;
	uint32_t config_address;
	vmm_io_t config_address_read;
	vmm_io_t config_address_write;
	vmm_io_t config_data_read;
	vmm_io_t config_data_write;
};

extern struct vop_ops vmmfs_pciroot_vops;

int vmmfs_pciroot_create(struct vmmfs_machine *, struct vmmfs_pciroot *);
int vmmfs_pciroot_destroy(struct vmmfs_pciroot *);
int vmmfs_pciroot_start(struct vmmfs_pciroot *, vmm_machine_t);
int vmmfs_pciroot_stop(struct vmmfs_pciroot *);

#endif /* VMMFS_PCIROOT_H */
