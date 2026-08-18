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

#include <dev/virtual/vmm/vmm.h>

#include "vmmfs_events.h"
#include "vmmfs_loader.h"
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

struct vmmfs_machine_spec {
	struct vmmfs_vcpu_spec vcpu;
	struct vmmfs_memory_spec memory;
	struct vmmfs_loader_spec loader;
};

struct vmmfs_machine {
	RB_ENTRY(vmmfs_machine) entry;
	struct vmmfs_root *root;
	struct vnode *vnode;
	ino_t inode;
	char name[NAME_MAX + 1];
	struct lwkt_token token;
	vmm_machine_t machine;
	struct vmmfs_machine_spec spec;
	struct vmmfs_vcpu vcpu;
	struct vmmfs_memory memory;
	struct vmmfs_loader loader;
	struct vmmfs_stopped stopped;
	struct vmmfs_pciroot pciroot;
	struct vmmfs_platform_x64 platform;
	struct vmmfs_rtc rtc;
	struct vmmfs_serialroot serialroot;
	struct vmmfs_events events;
};

struct vmmfs_machine_tree;
RB_PROTOTYPE(vmmfs_machine_tree, vmmfs_machine, entry,
	vmmfs_machine_compare);

extern struct vop_ops vmmfs_machine_vops;

int vmmfs_machine_compare(struct vmmfs_machine *, struct vmmfs_machine *);
struct vmmfs_machine *vmmfs_machine_create(struct vmmfs_root *,
	const char *, size_t);
int vmmfs_machine_destroy(struct vmmfs_machine *);
void vmmfs_machine_free(struct vmmfs_machine *);

/* Synchronously forces a warm reset using the caller's credentials. */
int vmmfs_machine_reset(struct vmmfs_machine *, struct ucred *);

#endif /* VMMFS_MACHINE_H */
