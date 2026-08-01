/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs presentation of vPCIe consumers.  This layer only translates
 * directory operations into vmm_pcie calls; BDF allocation and attachment
 * lifetime are core-fabric responsibilities.
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
#include <sys/queue.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmm_pcie.h"
#include "vmmfs.h"
#include "vmmfs_device.h"
#include "vmmfs_node_if.h"

/* ---- <machine>/devices/ ---- */

static int
vmmfs_devices_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child;
	struct vmmfs_device *d;

	child = NULL;
	lockmgr(&vmp->vm_lock, LK_SHARED);
	d = vmmfs_find_device(vmp, dnode->vn_machine, ncp->nc_name,
	    ncp->nc_nlen);
	if (d != NULL)
		child = &d->node;
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmmfs_devices_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_mount *vmp;
	struct vmmfs_device *d;
	struct vmm_pcie_root *consumer;
	off_t off;
	int error;
	int full;
	int i;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);
	KKASSERT(node->vn_machine != NULL);
	consumer = &node->vn_machine->machine.own_mut_pcie_root;
	lockmgr(&vmp->vm_lock, LK_SHARED);
	i = 0;
	SLIST_FOREACH(d, &vmp->vm_device_views, dv_view_link) {
		if (!vmm_pcie_device_at_root(&d->own_mut_device, consumer))
			continue;
		if (i++ < (int)off - 2)
			continue;
		if (vop_write_dirent(&error, uio, d->node.vn_ino, DT_DIR,
		    (uint16_t)d->own_mut_device.imm_name_len,
		    d->own_mut_device.imm_name)) {
			full = 1;
			break;
		}
		off++;
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

/* A new provider belongs directly to this machine's PCIe root. */
static int
vmmfs_devices_nmkdir(struct vmmfs_node *dnode, struct vop_nmkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_device *d;
	struct vnode *vp;
	ino_t idx;
	int alloc_error;
	int error;

	if (dnode->vn_machine == NULL)
		return EPERM;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen > VMM_DEVICE_NAME_MAX)
		return ENAMETOOLONG;
	d = kmalloc(sizeof(*d), M_VMMFS, M_WAITOK | M_ZERO);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmp->vm_closing) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		kfree(d, M_VMMFS);
		return EBUSY;
	}
	error = vmm_pcie_device_create(&vmp->own_mut_pcie,
	    &dnode->vn_machine->machine.own_mut_pcie_root, ncp->nc_name,
	    ncp->nc_nlen, &d->own_mut_device);
	if (error == 0) {
		idx = (ino_t)vmp->vm_next_dev++;
		vmmfs_device_init(d, dnode, idx);
		SLIST_INSERT_HEAD(&vmp->vm_device_views, d, dv_view_link);
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (error != 0) {
		kfree(d, M_VMMFS);
		return error;
	}

	error = vmmfs_alloc_vp(dvp->v_mount, &d->node,
	    LK_EXCLUSIVE | LK_RETRY, &vp);
	if (error != 0) {
		alloc_error = error;
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		SLIST_REMOVE(&vmp->vm_device_views, d, vmmfs_device,
		    dv_view_link);
		error = vmm_pcie_device_destroy(&vmp->own_mut_pcie,
		    &d->own_mut_device);
		KKASSERT(error == 0);
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vmmfs_device_uninit(d);
		kfree(d, M_VMMFS);
		return alloc_error;
	}
	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/* A P1 function has no children and may be deleted only before registration. */
static int
vmmfs_devices_nrmdir(struct vmmfs_node *dnode, struct vop_nrmdir_args *ap)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_dvp->v_mount);
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_device *d;
	struct vnode *vp;
	int error;

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error != 0)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	d = vmmfs_find_device(vmp, dnode->vn_machine, ncp->nc_name,
	    ncp->nc_nlen);
	if (d == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	error = vmm_pcie_device_destroy(&vmp->own_mut_pcie,
	    &d->own_mut_device);
	if (error != 0) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return error;
	}
	SLIST_REMOVE(&vmp->vm_device_views, d, vmmfs_device, dv_view_link);
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	cache_inval_vp(vp, CINV_DESTROY | CINV_CHILDREN);
	vrele(vp);
	vmmfs_device_revoke(d);
	vmmfs_device_uninit(d);
	kfree(d, M_VMMFS);
	return 0;
}

/* mv is a consumer attachment change.  A function name is immutable. */
static int
vmmfs_devices_nrename(struct vmmfs_node *fdnode, struct vop_nrename_args *ap)
{
	struct namecache *fncp = ap->a_fnch->ncp;
	struct namecache *tncp = ap->a_tnch->ncp;
	struct vmmfs_node *tdnode = VP_TO_VMMFS(ap->a_tdvp);
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_fdvp->v_mount);
	struct vmmfs_device *d;
	struct vmm_pcie_root *consumer;
	int error;

	if (!VMMFS_NODE_IS(fdnode, vmmfs_devices_class) ||
	    !VMMFS_NODE_IS(tdnode, vmmfs_devices_class))
		return EXDEV;
	if (fncp->nc_nlen != tncp->nc_nlen ||
	    bcmp(fncp->nc_name, tncp->nc_name, fncp->nc_nlen) != 0)
		return EINVAL;

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	d = vmmfs_find_device(vmp, fdnode->vn_machine, fncp->nc_name,
	    fncp->nc_nlen);
	if (d == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return ENOENT;
	}
	KKASSERT(tdnode->vn_machine != NULL);
	consumer = &tdnode->vn_machine->machine.own_mut_pcie_root;
	error = vmm_pcie_device_move(&vmp->own_mut_pcie,
	    &d->own_mut_device, consumer);
	if (error == 0)
		d->node.vn_parent = tdnode;
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (error != 0)
		return error;
	cache_rename(ap->a_fnch, ap->a_tnch);
	return 0;
}

static kobj_method_t vmmfs_devices_methods[] = {
	KOBJMETHOD(vmmfs_node_nresolve,		vmmfs_devices_nresolve),
	KOBJMETHOD(vmmfs_node_readdir,		vmmfs_devices_readdir),
	KOBJMETHOD(vmmfs_node_nmkdir,		vmmfs_devices_nmkdir),
	KOBJMETHOD(vmmfs_node_nrmdir,		vmmfs_devices_nrmdir),
	KOBJMETHOD(vmmfs_node_nrename,		vmmfs_devices_nrename),
	KOBJMETHOD(vmmfs_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmmfs_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmmfs_node_access,		vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_open,		vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,		vmmnode_close),
	KOBJMETHOD(vmmfs_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_devices, vmmfs_devices_methods, 0);
