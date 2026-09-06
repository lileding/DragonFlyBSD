/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot directory object.
 */
#ifndef VMMFS_PCISLOT_H
#define VMMFS_PCISLOT_H

#include <sys/tree.h>
#include <sys/types.h>

#include "vmmfs_node.h"
#include "vmmfs_pciroot.h"
#include "vmmfs_pcislot_descriptor.h"
#include "vmmfs_pcislot_auth.h"
#include "vmmfs_pcislot_config.h"
#include "vmmfs_pcislot_events.h"

struct vmmfs_mount;
struct vmmfs_pcislot_resources;
struct vnode;
struct vop_ops;

#define VMMFS_PCISLOT_CONFIG_SIZE 4096
struct vmmfs_pcislot_type0 {
	uint8_t bytes[VMMFS_PCISLOT_CONFIG_SIZE];
	uint64_t bar_address[VMMFS_PCISLOT_MAX_BARS];
	uint64_t rom_address;
	uint32_t intx_gsi;
	uint8_t bar_probe[VMMFS_PCISLOT_MAX_BARS];
	uint16_t cap_offset[VMMFS_PCISLOT_MAX_CAPS];
	uint16_t ecap_offset[VMMFS_PCISLOT_MAX_ECAPS];
	bool rom_probe;
	bool powered;
	bool bus_master_enabled;
};

struct vmmfs_pcislot {
	struct vmmfs_node node;
	/* Registry ownership; protected by the parent node token. */
	struct vmmfs_pciroot_slot *entry;
	uint16_t bdf;
	/* Owned until registry detach; protected by the machine node token. */
	bool topology_reference;
	struct vmmfs_pcislot_descriptor descriptor;
	struct vmmfs_pcislot_config config;
	struct vmmfs_pcislot_events events;
	struct vmmfs_pcislot_type0 type0;
	/* Current powered generation; protected by the slot token. */
	struct vmmfs_pcislot_resources *resources;
	struct vnode *descriptor_vnode;
	struct vnode *config_vnode;
	struct vnode *events_vnode;
};

int vmmfs_pcislot_create(struct vmmfs_node *,
	uint16_t, struct vnode **);
int vmmfs_pcislot_power_on(struct vmmfs_pcislot *, vmm_machine_t);
void vmmfs_pcislot_power_off(struct vmmfs_pcislot *);
int vmmfs_pcislot_reset(struct vmmfs_pcislot *);
int vmmfs_pcislot_type0_config_read(struct vmmfs_pcislot *, vmm_vcpu_t,
	uint16_t, enum vmm_io_width, uint32_t *);
int vmmfs_pcislot_type0_config_write(struct vmmfs_pcislot *, vmm_vcpu_t,
	uint16_t, enum vmm_io_width, uint32_t);

#endif /* VMMFS_PCISLOT_H */
