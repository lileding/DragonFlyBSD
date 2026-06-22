/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The machines/ collection: the registry of user VMs and the directory vops
 * that create (mkdir), remove (rmdir), resolve, and list them.  host is a
 * permanent reserved entry handled by vmm_host.c.  The registry is still a
 * fixed array; a later step replaces it with a dynamic RB tree.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"
#include "vmm_node_if.h"

/* ---- registry ---- */

static struct vmmfs_machine *
vmmfs_find_machine(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	int i;

	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];

		if (m->in_use && (int)strlen(m->name) == nlen &&
		    bcmp(m->name, name, nlen) == 0)
			return m;
	}
	return NULL;
}

/*
 * Find a reusable slot: not in_use and with no lingering cached vnode (so a
 * machine deleted while fds were open is not reused until reclaimed).
 */
static struct vmmfs_machine *
vmmfs_alloc_slot(struct vmmfs_mount *vmp)
{
	int i, j;

	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];
		int busy = 0;

		if (m->in_use || m->node.vn_vnode != NULL ||
		    m->vn_devices.vn_vnode != NULL)
			continue;
		for (j = 0; j < VMMFS_NCFG; j++) {
			if (m->cfg[j].vn_vnode != NULL) {
				busy = 1;
				break;
			}
		}
		if (busy)
			continue;
		return m;
	}
	return NULL;
}

/*
 * Mark a machine deleted (rmdir source 1, or the last lease close).  The slot
 * is reclaimed lazily (vmmfs_alloc_slot / unmount), so still-open fds keep
 * working until their vnodes are reclaimed.
 */
void
vmmfs_machine_mark_deleted(struct vmmfs_mount *vmp, struct vmmfs_machine *m)
{
	int idx = (int)(m - vmp->vm_mach);
	int i;

	vmm_machine_begin_delete(&m->state);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m->in_use = 0;
	/* Any device bound to this machine: host devices return to the host
	 * pool, user backends are unloaded. */
	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		struct vmmfs_device *d = &vmp->vm_dev[i];

		if (d->in_use && d->owner == idx) {
			if (d->is_host)
				d->owner = VMMFS_OWNER_HOST;
			else
				d->in_use = 0;
		}
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
}

/* ---- machines/ directory vops ---- */

static int
vmm_machines_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;

	(void)dnode;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0) {
		child = &vmp->vm_host;
	} else {
		struct vmmfs_machine *m;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
		if (m != NULL)
			child = &m->node;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
	}
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmm_machines_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_mount *vmp;
	off_t off;
	int full, error, i;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);

	/* host is always the first entry. */
	if (off == 2) {
		if (vop_write_dirent(&error, uio, vmp->vm_host.vn_ino, DT_DIR, 4,
		    "host")) {
			full = 1;
			goto out;
		}
		off = 3;
	}
	lockmgr(&vmp->vm_lock, LK_SHARED);
	for (i = (int)off - 3; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];

		if (!m->in_use)
			continue;
		if (vop_write_dirent(&error, uio, m->node.vn_ino, DT_DIR,
		    (uint16_t)strlen(m->name), m->name)) {
			off = 3 + i;
			full = 1;
			break;
		}
		off = 3 + i + 1;
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (!full && off < 3 + VMMFS_MAX_MACHINES)
		off = 3 + VMMFS_MAX_MACHINES;
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

/*
 * `mkdir machines/<name>` creates a machine: always stopped, empty config.
 * The user then writes vcpu/mem/loader and `rm stopped` to start.
 */
static int
vmm_machines_nmkdir(struct vmmfs_node *dnode, struct vop_nmkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	(void)dnode;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen > VMMFS_NAME_MAX)
		return ENAMETOOLONG;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EEXIST;	/* host is reserved */

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	m = vmmfs_alloc_slot(vmp);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return ENOSPC;
	}
	bcopy(ncp->nc_name, m->name, ncp->nc_nlen);
	m->name[ncp->nc_nlen] = '\0';
	vmm_machine_init(&m->state);
	m->in_use = 1;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	error = vmmfs_alloc_vp(dvp->v_mount, &m->node, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error) {
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		m->in_use = 0;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return error;
	}

	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/*
 * `rmdir machines/<name>` removes a stopped machine (source 1): it deletes
 * regardless of leases.  The slot is freed lazily so still-open fds keep
 * working until reclaimed.
 */
static int
vmm_machines_nrmdir(struct vmmfs_node *dnode, struct vop_nrmdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	(void)dnode;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EPERM;	/* host is not removable */

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	if (!vmm_machine_is_stopped(&m->state)) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return EBUSY;
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	vmmfs_machine_mark_deleted(vmp, m);
	cache_inval_vp(vp, CINV_DESTROY | CINV_CHILDREN);
	vrele(vp);
	return 0;
}

static kobj_method_t vmm_machines_methods[] = {
	KOBJMETHOD(vmm_node_nresolve,		vmm_machines_nresolve),
	KOBJMETHOD(vmm_node_readdir,		vmm_machines_readdir),
	KOBJMETHOD(vmm_node_nmkdir,		vmm_machines_nmkdir),
	KOBJMETHOD(vmm_node_nrmdir,		vmm_machines_nrmdir),
	KOBJMETHOD(vmm_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmm_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmm_node_access,		vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmm_node_open,		vmmnode_open),
	KOBJMETHOD(vmm_node_close,		vmmnode_close),
	KOBJMETHOD(vmm_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_machines, vmm_machines_methods, 0);
