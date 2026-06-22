/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The host object: machines/host/, a permanent read-only directory whose only
 * child is devices/ (the host PCIe device pool).  Unlike user VMs it is never
 * created or removed.
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

static int
vmm_host_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;

	if (ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "devices", 7) == 0)
		child = &vmp->vm_host_devices;
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmm_host_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	off_t off;
	int full, error;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	if (off == 2) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);

		if (vop_write_dirent(&error, uio, vmp->vm_host_devices.vn_ino,
		    DT_DIR, 7, "devices")) {
			full = 1;
			goto out;
		}
		off = 3;
	}
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

static kobj_method_t vmm_host_methods[] = {
	KOBJMETHOD(vmm_node_nresolve,		vmm_host_nresolve),
	KOBJMETHOD(vmm_node_readdir,		vmm_host_readdir),
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
DEFINE_CLASS(vmm_host, vmm_host_methods, 0);
