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

#include "vmmfs_node.h"
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
#include "vmmfs_vcpu.h"

struct mount;
struct vnode;
struct vop_ops;
struct vmmfs_mount;
struct ucred;
struct vmmfs_launch;

struct vmmfs_machine {
	struct vmmfs_node node;
	struct lwkt_token token;
	char name[NAME_MAX + 1];
	uint32_t id;
	struct vmmfs_machine_id id_node;
	/*
	 * NULL is stopped.  A non-NULL instance owns the running topology.
	 * runtime_released marks the retry window before stopped is republished.
	 */
	vmm_machine_t machine;
	struct vmmfs_launch *launch;
	bool runtime_releasing;
	bool runtime_released;
	/* Admitted runtime work, or topology removal until registry detach. */
	u_int runtime_references;
	struct vmm_cpustate boot_state;
	struct vmmfs_vcpu vcpu;
	struct vmmfs_memory memory;
	struct vmmfs_loader loader;
	struct vmmfs_boot boot;
	struct vmmfs_pciroot pciroot;
	struct vmmfs_platform_x64 platform;
	struct vmmfs_rtc rtc;
	struct vmmfs_serialroot serialroot;
	struct vmmfs_events events;
	/* Owns the stopped node through its ordinary vnode reference. */
	struct vmmfs_stopped *stopped;
};

int vmmfs_machine_create(struct vmmfs_node *,
	const char *, size_t, struct vmmfs_machine **);

/* Requests a warm reset without rerunning the loader. */
int vmmfs_machine_reset(struct vmmfs_machine *);

/* Requests terminal power-off from an external VOP or a guest runtime event. */
int vmmfs_machine_request_stop(struct vmmfs_machine *, const char *);

/* Atomically admits a private launch and prepares its platform. */
int vmmfs_machine_boot(struct vmmfs_machine *, struct vmmfs_launch **);
/* Only the current launch may start or abort the admitted runtime. */
int vmmfs_machine_run(struct vmmfs_launch *);
int vmmfs_machine_abort(struct vmmfs_launch *);

/* The BSP invokes these after every other vCPU has reached its barrier. */
int vmmfs_machine_vcpu_reset(struct vmmfs_machine *);
void vmmfs_machine_vcpu_stopped(struct vmmfs_machine *);

#endif /* VMMFS_MACHINE_H */
