/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs command-layer objects.
 */
#ifndef VMMFS_H
#define VMMFS_H

#include <sys/dirent.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/thread.h>
#include <sys/tree.h>
#include <sys/types.h>

#include "../../dev/virtual/vmm/vmm.h"

struct mount;
struct vnode;
struct vop_ops;
struct vm_object;
struct vmspace;
struct vmmfs_machine;
struct vmmfs_pcidev;

MALLOC_DECLARE(M_VMMFS);

#define VMMFS_ROOT_INO 1

/* One stable snapshot of an item in a vmmfs object collection. */
struct vmmfs_item {
	uint64_t id;
	char name[NAME_MAX + 1];
	struct vmmfs_machine *machine;
};

/* The stopped node is present exactly when the declared target is stopped. */
struct vmmfs_stopped {
	bool present;
};

/* Machine-level declaration state. */
struct vmmfs_machine_spec {
	struct vmmfs_stopped stopped;
};

/* The declared vCPU count for a later cold start. */
struct vmmfs_vcpu_spec {
	uint32_t count;
};

/* The declared guest RAM size for a later cold start. */
struct vmmfs_memory_spec {
	uint64_t size;
};

/* The loader program used by a later cold start. */
struct vmmfs_loader_spec {
	char path[MAXPATHLEN];
};

struct vmmfs_pcislot_spec {
	RB_ENTRY(vmmfs_pcislot_spec) entry;
	char name[NAME_MAX + 1];
};

RB_HEAD(vmmfs_pcislot_spec_tree, vmmfs_pcislot_spec);

/* The declared PCIe slots for a later cold start. */
struct vmmfs_pciroot_spec {
	struct vmmfs_pcislot_spec_tree slots;
};

RB_HEAD(vmmfs_machine_tree, vmmfs_machine);

/* One vmmfs mountpoint root and its machine namespace. */
struct vmmfs_domain {
	struct mount *mount;
	struct vnode *vnode;
	struct vnode *(*as_vnode)(struct vmmfs_domain *);
	int (*read_item)(struct vmmfs_domain *, uint64_t,
	    struct vmmfs_item *);
	int (*create_item)(struct vmmfs_domain *, const char *, size_t,
	    struct vmmfs_machine **);
	int (*remove_item)(struct vmmfs_domain *, struct vmmfs_machine *);
	struct vop_ops *machine_vops;
	struct lwkt_token token;
	struct vmmfs_machine_tree machines;
	ino_t next_ino;
};

struct vmmfs_vcpu {
	struct vmmfs_machine *machine;
	struct vmmfs_vcpu_spec spec;
	vmm_vcpu_t *vcpus;
	uint32_t running_count;
};

struct vmmfs_memory {
	struct vmmfs_machine *machine;
	struct vmmfs_memory_spec spec;
	struct vm_object *object;
	struct vmspace *boot_vmspace;
	struct vmspace *run_vmspace;
};

struct vmmfs_loader {
	struct vmmfs_machine *machine;
	struct vmmfs_loader_spec spec;
};

struct vmmfs_pcislot {
	struct vmmfs_machine *machine;
	struct vmmfs_pcislot_spec spec;
	struct vmmfs_pcidev *device;
};

struct vmmfs_pciroot {
	struct vmmfs_machine *machine;
	struct vmmfs_pciroot_spec spec;
};

struct vmmfs_events {
	struct vmmfs_machine *machine;
};

struct vmmfs_status {
	struct vmmfs_machine *machine;
};

struct vmmfs_console {
	struct vmmfs_machine *machine;
};

/*
 * spec_token protects every declaration field, including stopped and the
 * child specs.  Runtime state is not read through these fields after a cold
 * start has captured its declaration snapshot.
 */
struct vmmfs_machine {
	RB_ENTRY(vmmfs_machine) entry;
	struct vmmfs_domain *domain;
	struct vnode *vnode;
	struct vnode *(*as_vnode)(struct vmmfs_machine *);
	ino_t inode;
	char name[NAME_MAX + 1];
	struct lwkt_token spec_token;
	struct vmmfs_machine_spec spec;
	vmm_machine_t machine;
	struct vmmfs_vcpu vcpu;
	struct vmmfs_memory memory;
	struct vmmfs_loader loader;
	struct vmmfs_pciroot pciroot;
	struct vmmfs_events events;
	struct vmmfs_status status;
	struct vmmfs_console console;
};

int vmmfs_machine_compare(struct vmmfs_machine *, struct vmmfs_machine *);
RB_PROTOTYPE(vmmfs_machine_tree, vmmfs_machine, entry,
	vmmfs_machine_compare);

extern struct vop_ops vmmfs_machine_vops;

int vmmfs_domain_create(struct mount *, struct vmmfs_domain **);
void vmmfs_domain_destroy(struct vmmfs_domain *);
struct vmmfs_machine *vmmfs_machine_create(struct vmmfs_domain *,
	const char *, size_t);
void vmmfs_machine_destroy(struct vmmfs_machine *);

#endif /* VMMFS_H */
