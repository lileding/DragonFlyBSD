/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Typed views of the one VMMFS namespace-parent reference.
 */
#ifndef VMMFS_PARENT_H
#define VMMFS_PARENT_H

#include "vmmfs_node.h"

struct vmmfs_machine;
struct vmmfs_pciroot;
struct vmmfs_pcislot;
struct vmmfs_pcislot_resources;
struct vmmfs_serialroot;

static __inline struct vmmfs_node *
vmmfs_node_parent(const struct vmmfs_node *node)
{
	return (node == NULL ? NULL : node->parent);
}

#define vmmfs_pciroot_machine(pciroot) \
	((struct vmmfs_machine *)vmmfs_node_parent(&(pciroot)->node))
#define vmmfs_serialroot_machine(serialroot) \
	((struct vmmfs_machine *)vmmfs_node_parent(&(serialroot)->node))
#define vmmfs_pcislot_pciroot(slot) \
	((struct vmmfs_pciroot *)vmmfs_node_parent(&(slot)->node))
#define vmmfs_events_machine(events) \
	((struct vmmfs_machine *)vmmfs_node_parent(&(events)->node))
#define vmmfs_loader_machine(loader) \
	((struct vmmfs_machine *)vmmfs_node_parent(&(loader)->node))
#define vmmfs_machine_id_machine(identity) \
	((struct vmmfs_machine *)vmmfs_node_parent(&(identity)->node))
#define vmmfs_memory_machine(memory) \
	((struct vmmfs_machine *)vmmfs_node_parent(&(memory)->node))
#define vmmfs_vcpu_machine(vcpu) \
	((struct vmmfs_machine *)vmmfs_node_parent(&(vcpu)->node))
#define vmmfs_pcislot_config_slot(config) \
	((struct vmmfs_pcislot *)vmmfs_node_parent(&(config)->node))
#define vmmfs_pcislot_descriptor_slot(descriptor) \
	((struct vmmfs_pcislot *)vmmfs_node_parent(&(descriptor)->node))
#define vmmfs_pcislot_powered_slot(powered) \
	((struct vmmfs_pcislot *)vmmfs_node_parent(&(powered)->node))
#define vmmfs_pcislot_resource_resources(resource) \
	((struct vmmfs_pcislot_resources *)vmmfs_node_parent(&(resource)->node))
#define vmmfs_pcislot_resources_slot(resources) \
	((struct vmmfs_pcislot *)vmmfs_node_parent(&(resources)->node))

#endif /* VMMFS_PARENT_H */
