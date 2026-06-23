/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The devices/ collections.  NDEVICES is a machine's (or the host's) devices/
 * directory: it lists the PCIe devices bound to that owner and supports mv
 * (rebind, desired state) and rm (unbind).  NDEVROOT is the flat /vmm/devices/
 * index of symlinks to every device's current owner.  Both draw from the
 * (stub) per-mount device pool.
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
#include "vmmfs.h"
#include "vmmfs_device.h"
#include "vmmfs_node_if.h"

/* ---- machines/<name>/devices/ and machines/host/devices/ (NDEVICES) ---- */

static int
vmmfs_devices_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;
	struct vmmfs_device *d;

	lockmgr(&vmp->vm_lock, LK_SHARED);
	d = vmmfs_find_device(vmp, dnode->vn_machine, ncp->nc_name, ncp->nc_nlen);
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
	off_t off;
	int full, error, i;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);
	lockmgr(&vmp->vm_lock, LK_SHARED);
	{
		struct vmmfs_device *d;
		int skip = (int)off - 2;

		i = 0;
		SLIST_FOREACH(d, &vmp->vm_devs, dv_link) {
			if (!vmm_device_owned_by(&d->dev,
			    VMMFS_CORE_MACHINE_OF(node->vn_machine)))
				continue;
			if (i++ < skip)
				continue;
			if (vop_write_dirent(&error, uio, d->node.vn_ino, DT_REG,
			    (uint16_t)strlen(d->dev.bdf), d->dev.bdf)) {
				full = 1;
				break;
			}
			off++;
		}
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

/*
 * `rm <name>/devices/<dev>` unbinds a device: a host device returns to the host
 * pool, a user backend is unloaded.  The host pool itself is fixed.
 */
static int
vmmfs_devices_nremove(struct vmmfs_node *dnode, struct vop_nremove_args *ap)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_dvp->v_mount);
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_device *d;
	struct vnode *vp;
	int error;

	if (dnode->vn_machine == NULL)
		return EPERM;

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	d = vmmfs_find_device(vmp, dnode->vn_machine, ncp->nc_name, ncp->nc_nlen);
	if (d == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	if (d->dev.is_host) {
		vmm_device_unbind(&d->dev);	/* back to host pool */
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		cache_unlink(ap->a_nch);
		vrele(vp);
		return 0;
	}
	/*
	 * A user backend is unloaded: drop it from the pool and free it.  (Real
	 * backends that can be held open will need the machine-style refcount.)
	 */
	SLIST_REMOVE(&vmp->vm_devs, d, vmmfs_device, dv_link);
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	cache_unlink(ap->a_nch);
	vrele(vp);
	vmmfs_node_uninit(&d->node);
	vmmfs_node_uninit(&d->link);
	kfree(d, M_VMMFS);
	return 0;
}

/*
 * `mv <devices>/<dev> <devices>/` rebinds a device to another owner.  Both ends
 * must be devices/ directories and the BDF name is preserved; cp is impossible
 * (devices/ rejects file creation).  Desired-state semantics.
 */
static int
vmmfs_devices_nrename(struct vmmfs_node *fdnode, struct vop_nrename_args *ap)
{
	struct namecache *fncp = ap->a_fnch->ncp;
	struct namecache *tncp = ap->a_tnch->ncp;
	struct vmmfs_node *tdnode = VP_TO_VMMFS(ap->a_tdvp);
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_fdvp->v_mount);
	struct vmmfs_device *d;

	if (!VMMFS_NODE_IS(fdnode, vmmfs_devices_class) ||
	    !VMMFS_NODE_IS(tdnode, vmmfs_devices_class))
		return EXDEV;
	if (fncp->nc_nlen != tncp->nc_nlen ||
	    bcmp(fncp->nc_name, tncp->nc_name, fncp->nc_nlen) != 0)
		return EINVAL;	/* a device keeps its BDF name */

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	d = vmmfs_find_device(vmp, fdnode->vn_machine, fncp->nc_name,
	    fncp->nc_nlen);
	if (d == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return ENOENT;
	}
	if (fdnode->vn_machine == tdnode->vn_machine) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return 0;	/* no-op rebind */
	}
	if (vmmfs_find_device(vmp, tdnode->vn_machine, tncp->nc_name,
	    tncp->nc_nlen) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	vmm_device_bind(&d->dev, VMMFS_CORE_MACHINE_OF(tdnode->vn_machine));
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	cache_rename(ap->a_fnch, ap->a_tnch);
	return 0;
}

static kobj_method_t vmmfs_devices_methods[] = {
	KOBJMETHOD(vmmfs_node_nresolve,		vmmfs_devices_nresolve),
	KOBJMETHOD(vmmfs_node_readdir,		vmmfs_devices_readdir),
	KOBJMETHOD(vmmfs_node_nremove,		vmmfs_devices_nremove),
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

/* ---- /vmm/devices/ symlink index (NDEVROOT) ---- */

static int
vmmfs_devroot_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;
	struct vmmfs_device *d;

	(void)dnode;
	lockmgr(&vmp->vm_lock, LK_SHARED);
	d = vmmfs_find_device_any(vmp, ncp->nc_name, ncp->nc_nlen);
	if (d != NULL)
		child = &d->link;
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmmfs_devroot_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_mount *vmp;
	off_t off;
	int full, error, i;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);
	lockmgr(&vmp->vm_lock, LK_SHARED);
	{
		struct vmmfs_device *d;
		int skip = (int)off - 2;

		i = 0;
		SLIST_FOREACH(d, &vmp->vm_devs, dv_link) {
			if (i++ < skip)
				continue;
			if (vop_write_dirent(&error, uio, d->link.vn_ino, DT_LNK,
			    (uint16_t)strlen(d->dev.bdf), d->dev.bdf)) {
				full = 1;
				break;
			}
			off++;
		}
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

static kobj_method_t vmmfs_devroot_methods[] = {
	KOBJMETHOD(vmmfs_node_nresolve,		vmmfs_devroot_nresolve),
	KOBJMETHOD(vmmfs_node_readdir,		vmmfs_devroot_readdir),
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
DEFINE_CLASS(vmmfs_devroot, vmmfs_devroot_methods, 0);
