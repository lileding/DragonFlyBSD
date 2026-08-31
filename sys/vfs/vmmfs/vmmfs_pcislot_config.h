/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI synchronous device-register responder.
 */
#ifndef VMMFS_PCISLOT_CONFIG_H
#define VMMFS_PCISLOT_CONFIG_H

#include <sys/event.h>
#include <sys/queue.h>

#include "vmmfs_node.h"
#include <sys/types.h>

#include <sys/vmmfs.h>

struct file;
struct vmm_cpuexit;
struct vmm_cpustate;
struct vmmfs_pcislot;
struct vmmfs_pcislot_resource;
struct vmmfs_pcislot_config_request;
struct vmmfs_vcpu_thread;
struct vnode;
struct vop_ops;

TAILQ_HEAD(vmmfs_pcislot_config_request_queue,
	    vmmfs_pcislot_config_request);

struct vmmfs_pcislot_config {
	struct vmmfs_node node;
	struct lwkt_token token;
	struct kqinfo kq;
	struct file *responder;
	struct vmmfs_pcislot_config_request_queue requests;
	uint64_t generation;
	uint64_t next_sequence;
	bool opening;
	bool powered;
	bool closed;
};

int vmmfs_pcislot_config_init(struct vmmfs_mount *, struct vmmfs_branch *,
	struct vmmfs_pcislot_config *, struct vnode **);
void vmmfs_pcislot_config_revoke(struct vmmfs_pcislot_config *);
void vmmfs_pcislot_config_descriptor_changed(struct vmmfs_pcislot_config *,
	uint64_t, bool);
void vmmfs_pcislot_config_power_on(struct vmmfs_pcislot_config *, uint64_t);
void vmmfs_pcislot_config_power_off(struct vmmfs_pcislot_config *);
int vmmfs_pcislot_config_memory(struct vmmfs_pcislot_config *,
	struct vmmfs_vcpu_thread *, struct vmmfs_pcislot_resource *,
	const struct vmm_cpuexit *);
int vmmfs_pcislot_config_io(struct vmmfs_pcislot_config *,
	struct vmmfs_vcpu_thread *, struct vmmfs_pcislot_resource *,
	struct vmm_cpustate *, const struct vmm_cpuexit *);

#endif /* VMMFS_PCISLOT_CONFIG_H */
