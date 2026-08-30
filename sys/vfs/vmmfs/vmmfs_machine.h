/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine declaration object.
 */
#ifndef VMMFS_MACHINE_H
#define VMMFS_MACHINE_H

#include <sys/param.h>
#include <sys/thread.h>
#include <sys/tree.h>
#include <sys/types.h>

#include "vmmfs_branch.h"
#include <dev/virtual/vmm/vmm.h>

#include "vmmfs_events.h"
#include "vmmfs_boot.h"
#include "vmmfs_loader.h"
#include "vmmfs_machine_id.h"
#include "vmmfs_memory.h"
#include "vmmfs_pciroot.h"
#include "vmmfs_platform_x64.h"
#include "vmmfs_rtc.h"
#include "vmmfs_serialroot.h"
#include "vmmfs_stopped.h"
#include "vmmfs_vcpu.h"

struct mount;
struct vnode;
struct vop_ops;
struct vmmfs_root;
struct ucred;

struct vmmfs_machine {
	struct vmmfs_branch branch;
	ino_t inode;
	char name[NAME_MAX + 1];
	uint32_t id;
	struct vmmfs_machine_id id_node;
	bool root_counted;
	/* NULL is stopped.  A non-NULL instance owns the running topology. */
	vmm_machine_t machine;
	struct vmm_cpustate boot_state;
	struct vmmfs_vcpu vcpu;
	struct vmmfs_memory memory;
	struct vmmfs_loader loader;
	struct vmmfs_boot boot;
	struct vmmfs_stopped *stopped;
	struct vmmfs_pciroot pciroot;
	struct vmmfs_platform_x64 platform;
	struct vmmfs_rtc rtc;
	struct vmmfs_serialroot serialroot;
	struct vmmfs_events events;
	/* Machine-owned vnode references for its fixed namespace children. */
	struct vnode *id_vnode;
	struct vnode *vcpu_vnode;
	struct vnode *memory_vnode;
	struct vnode *loader_vnode;
	struct vnode *boot_vnode;
	struct vnode *stopped_vnode;
	struct vnode *pciroot_vnode;
	struct vnode *serialroot_vnode;
	struct vnode *events_vnode;
};

extern struct vop_ops vmmfs_machine_vops;

int vmmfs_machine_compare(struct vmmfs_machine *, struct vmmfs_machine *);
int vmmfs_machine_create(struct vmmfs_root *, const char *, size_t,
	struct vmmfs_machine **, struct vnode **);
/* Marks the machine and fixed children unavailable without revoking vnodes. */
void vmmfs_machine_deactivate_begin(struct vmmfs_machine *);
void vmmfs_machine_deactivate(struct vmmfs_machine *);
void vmmfs_machine_hold(struct vmmfs_machine *);
void vmmfs_machine_put(struct vmmfs_machine *);

/* Requests a warm reset without rerunning the loader. */
int vmmfs_machine_reset(struct vmmfs_machine *);

/* Requests terminal power-off from an external VOP or a guest runtime event. */
int vmmfs_machine_stop_request(struct vmmfs_machine *, const char *);

/* Starts a direct boot session and publishes its guest-memory mapping. */
int vmmfs_machine_boot_start(struct vmmfs_machine *);

/* Releases a direct boot whose final session fd closed before submission. */
int vmmfs_machine_boot_abort(struct vmmfs_machine *);

/* Consumes the one direct-boot BSP state submission and starts the vCPUs. */
int vmmfs_machine_boot_submit(struct vmmfs_machine *,
	const struct vmm_cpustate *);

/* The BSP invokes these after every other vCPU has reached its barrier. */
int vmmfs_machine_vcpu_reset(struct vmmfs_machine *);
void vmmfs_machine_vcpu_stopped(struct vmmfs_machine *);

#endif /* VMMFS_MACHINE_H */
