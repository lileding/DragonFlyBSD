/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One machine as presented by vmmfs.  This is the filesystem wrapper around
 * the executable VM object, struct vmm_machine.  The machines/ collection owns
 * the RB tree; this object owns the machine directory and lifecycle files.
 */
#ifndef VMMFS_MACHINE_H
#define VMMFS_MACHINE_H

#include "vmm_machine.h"
#include "vmmfs.h"

struct vmmfs_machine {
	RB_ENTRY(vmmfs_machine)	vm_link;
	struct vmmfs_mount	*vm_mount;
	char			name[VMMFS_NAME_MAX + 1];
	int			vm_refs;
	int			vm_in_tree;	/* guards a single RB_REMOVE */
	struct vmm_machine	machine;	/* executable VM core object */
	struct vmmfs_node	node;		/* the machine directory */
	struct vmmfs_node	n_vcpu, n_mem, n_loader, n_console;
	struct vmmfs_node	n_lease, n_events, n_status, n_stopped;
	struct vmmfs_node	vn_devices;	/* this machine's devices/ */
};

#define VMMFS_MACHINE_OF_CORE(m) \
	((struct vmmfs_machine *)((char *)(m) - __offsetof(struct vmmfs_machine, machine)))
#define VMMFS_CORE_MACHINE_OF(m)	((m) != NULL ? &(m)->machine : NULL)

int	vmmfs_machine_cmp(struct vmmfs_machine *a, struct vmmfs_machine *b);
RB_PROTOTYPE(vmmfs_machtree, vmmfs_machine, vm_link, vmmfs_machine_cmp);

struct vmmfs_machine *vmmfs_machine_create(struct vmmfs_mount *vmp,
	    const char *name, int nlen);
void	vmmfs_machine_ref(struct vmmfs_mount *vmp, struct vmmfs_machine *m);
void	vmmfs_machine_unref(struct vmmfs_mount *vmp, struct vmmfs_machine *m);
void	vmmfs_machine_mark_deleted(struct vmmfs_mount *vmp,
	    struct vmmfs_machine *m);

#endif /* VMMFS_MACHINE_H */
