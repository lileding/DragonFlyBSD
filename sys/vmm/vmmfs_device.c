/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem presentation of one vPCIe function.  vmm_pcie owns the function
 * registry, BDF allocation, and attachment.  vmmfs owns only directory and
 * symlink nodes that present that core object.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/uio.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmm_pcie.h"
#include "vmmfs.h"
#include "vmmfs_device.h"
#include "vmmfs_node_if.h"

static int
vmmfs_device_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	return vmmfs_nresolve_finish(ap->a_dvp, NULL, ap->a_nch);
}

static int
vmmfs_device_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	off_t off;
	int full;
	int error;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	return vmmfs_readdir_end(ap, off, full, error);
}

static kobj_method_t vmmfs_device_methods[] = {
	KOBJMETHOD(vmmfs_node_nresolve,		vmmfs_device_nresolve),
	KOBJMETHOD(vmmfs_node_readdir,		vmmfs_device_readdir),
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
DEFINE_CLASS(vmmfs_device, vmmfs_device_methods, 0);

static int
vmmfs_devlink_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(node->vn_vnode->v_mount);
	char tmp[128];
	int len;

	lockmgr(&vmp->vm_lock, LK_SHARED);
	len = vmmfs_devlink_target(vmp, VMMFS_DEV_OF_LINK(node), tmp,
	    sizeof(tmp));
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	vmmfs_fill_attr(node, ap->a_vap, VLNK, 1, len < 0 ? 0 : len);
	return 0;
}

static int
vmmfs_devlink_readlink(struct vmmfs_node *node, struct vop_readlink_args *ap)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(node->vn_vnode->v_mount);
	char buf[128];
	int len;

	lockmgr(&vmp->vm_lock, LK_SHARED);
	len = vmmfs_devlink_target(vmp, VMMFS_DEV_OF_LINK(node), buf,
	    sizeof(buf));
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (len < 0)
		return ENOENT;
	return uiomove(buf, (size_t)len, ap->a_uio);
}

static kobj_method_t vmmfs_devlink_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_devlink_getattr),
	KOBJMETHOD(vmmfs_node_readlink,	vmmfs_devlink_readlink),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_devlink, vmmfs_devlink_methods, 0);

struct vmmfs_device *
vmmfs_find_device(struct vmmfs_mount *vmp, struct vmmfs_machine *owner,
    const char *name, int nlen)
{
	struct vmm_pcie_root *consumer;
	struct vmm_device *device;

	consumer = owner != NULL ? &owner->machine.own_mut_pcie_root :
	    vmm_pcie_host_root(&vmp->own_mut_pcie);
	device = vmm_pcie_device_find(&vmp->own_mut_pcie, consumer, name, nlen);
	return device != NULL ? VMMFS_DEV_OF_CORE(device) : NULL;
}

struct vmmfs_device *
vmmfs_find_device_any(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	struct vmm_device *device;

	device = vmm_pcie_device_find_name(&vmp->own_mut_pcie, name, nlen);
	return device != NULL ? VMMFS_DEV_OF_CORE(device) : NULL;
}

void
vmmfs_device_return_owner_locked(struct vmmfs_mount *vmp,
    struct vmm_machine *owner)
{
	struct vmmfs_device *d;
	struct vmm_pcie_root *host;
	struct vmm_pcie_root *machine;
	int error;

	host = vmm_pcie_host_root(&vmp->own_mut_pcie);
	machine = &owner->own_mut_pcie_root;
	SLIST_FOREACH(d, &vmp->vm_device_views, dv_view_link) {
		if (!vmm_pcie_device_attached_to(&d->own_mut_device, machine))
			continue;
		error = vmm_pcie_device_move(&vmp->own_mut_pcie,
		    &d->own_mut_device, host);
		KKASSERT(error == 0);
		d->node.vn_parent = &vmp->vm_host_devices;
	}
}

void
vmmfs_device_destroy_all(struct vmmfs_mount *vmp)
{
	struct vmmfs_device *d;
	int error;

	while (!SLIST_EMPTY(&vmp->vm_device_views)) {
		d = SLIST_FIRST(&vmp->vm_device_views);
		SLIST_REMOVE_HEAD(&vmp->vm_device_views, dv_view_link);
		error = vmm_pcie_device_destroy(&vmp->own_mut_pcie,
		    &d->own_mut_device);
		KKASSERT(error == 0);
		vmmfs_node_uninit(&d->node);
		vmmfs_node_uninit(&d->link);
		kfree(d, M_VMMFS);
	}
}

int
vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
    char *buf, size_t bufsize)
{
	struct vmm_pcie_root *consumer;
	const char *owner;

	consumer = vmm_pcie_device_consumer(&d->own_mut_device);
	if (consumer == NULL)
		return -1;
	if (consumer == vmm_pcie_host_root(&vmp->own_mut_pcie)) {
		owner = "host";
	} else if (consumer->borrow_imm_machine != NULL) {
		owner = VMMFS_MACHINE_OF_CORE(consumer->borrow_imm_machine)->name;
	} else {
		return -1;
	}
	return ksnprintf(buf, bufsize, "../machines/%s/devices/%s", owner,
	    d->own_mut_device.imm_name);
}
