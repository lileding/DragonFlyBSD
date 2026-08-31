/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot runtime resources.
 */
#ifndef VMMFS_PCISLOT_RESOURCE_H
#define VMMFS_PCISLOT_RESOURCE_H

#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>


struct vnode;
struct vmm_cpuexit;
struct vmm_cpustate;
struct vmmfs_pcislot;
struct vmmfs_pcislot_descriptor_value;
struct vmmfs_vcpu_thread;

struct vmmfs_pcislot_resource;
struct vmmfs_pcislot_resources;

int vmmfs_pcislot_resources_create(struct vmmfs_pcislot *,
	vmm_machine_t, const struct vmmfs_pcislot_descriptor_value *, uint64_t,
	struct vmmfs_pcislot_resources **);
void vmmfs_pcislot_resources_deactivate(struct vmmfs_pcislot_resources *);
int vmmfs_pcislot_resources_rebind(struct vmmfs_pcislot_resources *,
	vmm_machine_t);
void vmmfs_pcislot_resources_unbind(struct vmmfs_pcislot_resources *);
int vmmfs_pcislot_resources_lookup(struct vmmfs_pcislot_resources *,
	const char *, size_t, struct vnode **);
int vmmfs_pcislot_resources_read_item(struct vmmfs_pcislot_resources *,
	uint64_t, ino_t *, char *, size_t, size_t *);
int vmmfs_pcislot_resource_index(const struct vmmfs_pcislot_resource *,
	uint16_t *);
int vmmfs_pcislot_resource_gpa(const struct vmmfs_pcislot_resource *,
	uint64_t *);
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
int vmmfs_pcislot_resources_memory(struct vmmfs_pcislot_resources *,
	struct vmmfs_vcpu_thread *, const struct vmm_cpuexit *);
int vmmfs_pcislot_resources_io(struct vmmfs_pcislot_resources *,
	struct vmmfs_vcpu_thread *, struct vmm_cpustate *,
	const struct vmm_cpuexit *);

#endif /* VMMFS_PCISLOT_RESOURCE_H */
