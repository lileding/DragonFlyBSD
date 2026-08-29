/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot runtime resources.
 */
#ifndef VMMFS_PCISLOT_RESOURCE_H
#define VMMFS_PCISLOT_RESOURCE_H

#include <sys/event.h>
#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>
#include <sys/vmmfs.h>

#include "vmmfs_node.h"
#include "vmmfs_branch.h"

struct cdev;
struct vm_object;
struct vmspace;
struct vmmfs_machine;
struct vnode;
struct vop_ops;
struct vmm_cpuexit;
struct vmm_cpustate;
struct vmmfs_pcislot;
struct vmmfs_vcpu_thread;
struct vmmfs_pcislot_descriptor_value;

enum vmmfs_pcislot_resource_kind {
	VMMFS_PCISLOT_RESOURCE_BAR,
	VMMFS_PCISLOT_RESOURCE_PIO,
	VMMFS_PCISLOT_RESOURCE_ROM,
	VMMFS_PCISLOT_RESOURCE_DMA,
	VMMFS_PCISLOT_RESOURCE_KICK,
	VMMFS_PCISLOT_RESOURCE_INTX,
	VMMFS_PCISLOT_RESOURCE_MSI,
	VMMFS_PCISLOT_RESOURCE_MSIX,
};

struct vmmfs_pcislot_resources;

struct vmmfs_pcislot_resource_trap {
	uint64_t base;
	uint64_t size;
	vmm_io_t read_io;
	vmm_io_t write_io;
};

struct vmmfs_pcislot_resource {
	struct vmmfs_node node;
	struct vmmfs_pcislot_resources *resources;
	struct vmmfs_machine *machine;
	ino_t inode;
	enum vmmfs_pcislot_resource_kind kind;
	uint16_t index;
	uint16_t capability;
	uint16_t vector;
	struct lwkt_token token;
	struct kqinfo read_kq;
	struct vm_object *backing_object;
	struct vm_object *pager_object;
	struct vmspace *vmspace;
	struct cdev *dev;
	struct vmmfs_pcislot_resource_trap *traps;
	struct vmmfs_pci_kick *kicks;
	uint64_t gpa;
	uint64_t size;
	uint64_t mapping_size;
	size_t trap_count;
	size_t kick_head;
	size_t kick_count;
	size_t kick_capacity;
	uint64_t sequence;
	bool mapped;
	bool revoked;
	bool bus_master_enabled;
	bool intx_asserted;
};

struct vmmfs_pcislot_resources {
	struct vmmfs_branch branch;
	struct vmmfs_pcislot *slot;
	vmm_machine_t machine;
	uint64_t descriptor_generation;
	bool powered;
	bool destroying;
	size_t count;
	size_t initialized_count;
	struct vmmfs_pcislot_resource items[];
};

extern struct vop_ops vmmfs_pcislot_resource_vops;

int vmmfs_pcislot_resources_create(struct vmmfs_pcislot *,
	vmm_machine_t, const struct vmmfs_pcislot_descriptor_value *, uint64_t,
	struct vmmfs_pcislot_resources **);
void vmmfs_pcislot_resources_destroy(struct vmmfs_pcislot_resources *);
int vmmfs_pcislot_resources_rebind(struct vmmfs_pcislot_resources *,
	vmm_machine_t);
void vmmfs_pcislot_resources_unbind(struct vmmfs_pcislot_resources *);
struct vmmfs_pcislot_resource *vmmfs_pcislot_resources_find(
	struct vmmfs_pcislot_resources *, const char *, size_t);
int vmmfs_pcislot_resource_name(const struct vmmfs_pcislot_resource *,
	char *, size_t, size_t *);
int vmmfs_pcislot_resources_set_decode(struct vmmfs_pcislot_resources *,
	bool, bool, bool);
int vmmfs_pcislot_resources_bar_relocate(struct vmmfs_pcislot_resources *,
	unsigned int, uint64_t);
int vmmfs_pcislot_resources_rom_enable(struct vmmfs_pcislot_resources *,
	bool);
int vmmfs_pcislot_resources_rom_relocate(struct vmmfs_pcislot_resources *,
	uint64_t);
int vmmfs_pcislot_resources_msix_unmask(struct vmmfs_pcislot_resources *,
	unsigned int);
void vmmfs_pcislot_resources_trace_msix_control(
	struct vmmfs_pcislot_resources *, unsigned int);
int vmmfs_pcislot_resources_memory(struct vmmfs_pcislot_resources *,
	struct vmmfs_vcpu_thread *, const struct vmm_cpuexit *);
int vmmfs_pcislot_resources_io(struct vmmfs_pcislot_resources *,
	struct vmmfs_vcpu_thread *, struct vmm_cpustate *,
	const struct vmm_cpuexit *);

#endif /* VMMFS_PCISLOT_RESOURCE_H */
