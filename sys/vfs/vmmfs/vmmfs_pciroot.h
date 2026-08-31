/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI root directory object.
 */
#ifndef VMMFS_PCIROOT_H
#define VMMFS_PCIROOT_H

#include "vmmfs_branch.h"

#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct vmmfs_pcislot;
struct vmmfs_vcpu_thread;
struct vnode;

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

struct vmmfs_pciroot_registry;

struct vmmfs_pciroot {
	struct vmmfs_branch branch;
	struct vmmfs_pciroot_registry *registry;
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

int vmmfs_pciroot_init(struct vmmfs_mount *, struct vmmfs_branch *,
	struct vmmfs_pciroot *, struct vnode **);
void vmmfs_pciroot_deactivate_slots(struct vmmfs_pciroot *);
int vmmfs_pciroot_start(struct vmmfs_pciroot *, vmm_machine_t);
int vmmfs_pciroot_reset(struct vmmfs_pciroot *);
int vmmfs_pciroot_stop(struct vmmfs_pciroot *);
int vmmfs_pciroot_memory(struct vmmfs_pciroot *, struct vmmfs_vcpu_thread *,
	const struct vmm_cpuexit *);
int vmmfs_pciroot_io(struct vmmfs_pciroot *, struct vmmfs_vcpu_thread *,
	struct vmm_cpustate *, const struct vmm_cpuexit *);

void vmmfs_pciroot_invalidate_slot(struct vmmfs_pciroot *,
	struct vmmfs_pcislot *);
#endif /* VMMFS_PCIROOT_H */
