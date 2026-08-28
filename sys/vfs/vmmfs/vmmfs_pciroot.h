/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI root directory object.
 */
#ifndef VMMFS_PCIROOT_H
#define VMMFS_PCIROOT_H

#include "vmmfs_node.h"

#include <sys/tree.h>
#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct vmmfs_pcislot;
struct vmmfs_vcpu_thread;
struct vnode;
struct vop_ops;

/*
 * The sole VMMFS x64 platform ABI follows the Cloud Hypervisor layout:
 * BAR/MMIO is [0xc0000000, 0xe8000000), ECAM is [0xe8000000, 0xf8000000),
 * and [0xf8000000, 4 GiB) remains a non-RAM platform hole.
 */
#define VMMFS_PCI_MMIO_GPA 0xc0000000ULL
#define VMMFS_PCI_MMIO_END 0xe8000000ULL
#define VMMFS_PCI_ECAM_GPA 0xe8000000ULL
#define VMMFS_PCI_ECAM_SIZE 0x10000000ULL
#define VMMFS_PCI_ECAM_END (VMMFS_PCI_ECAM_GPA + VMMFS_PCI_ECAM_SIZE)
#define VMMFS_PCI_HOLE_END 0x100000000ULL
#define VMMFS_PCI_PIO_GPA 0x1000U
#define VMMFS_PCI_PIO_END 0xc000U

RB_HEAD(vmmfs_pcislot_tree, vmmfs_pcislot);

struct vmmfs_pciroot {
	struct vmmfs_node node;
	struct vmmfs_machine *machine;
	ino_t inode;
	bool node_reference;
	struct vmmfs_pcislot_tree slots;
	vmm_machine_t runtime_machine;
	uint64_t mmio_next;
	uint32_t pio_next;
	uint32_t config_address;
	vmm_io_t config_address_read;
	vmm_io_t config_address_write;
	vmm_io_t config_data_read;
	vmm_io_t config_data_write;
	vmm_io_t ecam_read;
	vmm_io_t ecam_write;
};

extern struct vop_ops vmmfs_pciroot_vops;

int vmmfs_pciroot_init(struct vmmfs_machine *, struct vmmfs_pciroot *);
void vmmfs_pciroot_fini(struct vmmfs_pciroot *);
void vmmfs_pciroot_release_vnodes(struct vmmfs_pciroot *);
int vmmfs_pciroot_start(struct vmmfs_pciroot *, vmm_machine_t);
int vmmfs_pciroot_reset(struct vmmfs_pciroot *);
int vmmfs_pciroot_stop(struct vmmfs_pciroot *);
int vmmfs_pciroot_memory(struct vmmfs_pciroot *, struct vmmfs_vcpu_thread *,
	const struct vmm_cpuexit *);
int vmmfs_pciroot_io(struct vmmfs_pciroot *, struct vmmfs_vcpu_thread *,
	struct vmm_cpustate *, const struct vmm_cpuexit *);

#endif /* VMMFS_PCIROOT_H */
