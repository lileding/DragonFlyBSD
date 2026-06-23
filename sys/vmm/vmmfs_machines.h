/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The machines/ registry interface: the RB tree of user VMs keyed by name and
 * the per-machine lifecycle hooks the rest of the fs layer calls.  vmm.ko only.
 * Include after vmmfs.h (it uses struct vmmfs_machines / vmmfs_mount).
 */
#ifndef VMMFS_MACHINES_H
#define VMMFS_MACHINES_H

int	vmmfs_machines_cmp(struct vmmfs_machines *a, struct vmmfs_machines *b);
RB_PROTOTYPE(vmmfs_machtree, vmmfs_machines, vm_link, vmmfs_machines_cmp);

/* vnode lifetime: ref on bind, unref on reclaim; mark_deleted drops the tree
 * reference (rmdir / last lease close). */
void	vmmfs_machines_ref(struct vmmfs_mount *vmp, struct vmmfs_machines *m);
void	vmmfs_machines_unref(struct vmmfs_mount *vmp, struct vmmfs_machines *m);
void	vmmfs_machines_mark_deleted(struct vmmfs_mount *vmp,
	    struct vmmfs_machines *m);
/* Resolve + check the desired loader at start time (caller's cred). */
int	vmmfs_validate_loader(struct vmmfs_machines *m, struct ucred *cred);

#endif /* VMMFS_MACHINES_H */
