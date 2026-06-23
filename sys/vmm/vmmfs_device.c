/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem presentation of a PCIe device: the NDEVICE file (a read-only
 * "<bdf>\n" line under a machine's or the host's devices/) and the NDEVLINK
 * symlink that indexes it under /vmm/devices/.  Wraps a vmm_device core; common
 * vops are shared from vmmfs.c.  vmm.ko only.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmm_device.h"
#include "vmmfs.h"
#include "vmmfs_device.h"
#include "vmmfs_node_if.h"

static int
vmmfs_device_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	char tmp[64];
	off_t size;

	size = vmm_device_format(&VMMFS_DEV_OF_NODE(node)->dev, tmp, sizeof(tmp));
	vmmfs_fill_attr(node, ap->a_vap, VREG, 1, size);
	return 0;
}

static int
vmmfs_device_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	struct uio *uio = ap->a_uio;
	char dbuf[64];
	int len;
	off_t off;

	if (uio->uio_offset < 0)
		return EINVAL;
	len = vmm_device_format(&VMMFS_DEV_OF_NODE(node)->dev, dbuf, sizeof(dbuf));
	off = uio->uio_offset;
	if (off >= len)
		return 0;
	return uiomove(dbuf + off, (size_t)(len - off), uio);
}

static kobj_method_t vmmfs_device_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_device_getattr),
	KOBJMETHOD(vmmfs_node_read,	vmmfs_device_read),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_open,	vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,	vmmnode_close),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
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
	len = vmmfs_devlink_target(vmp, VMMFS_DEV_OF_LINK(node), tmp, sizeof(tmp));
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	vmmfs_fill_attr(node, ap->a_vap, VLNK, 1, (len < 0) ? 0 : len);
	return 0;
}

static int
vmmfs_devlink_readlink(struct vmmfs_node *node, struct vop_readlink_args *ap)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(node->vn_vnode->v_mount);
	char buf[128];
	int len;

	lockmgr(&vmp->vm_lock, LK_SHARED);
	len = vmmfs_devlink_target(vmp, VMMFS_DEV_OF_LINK(node), buf, sizeof(buf));
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
