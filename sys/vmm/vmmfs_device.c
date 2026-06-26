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


/* ---- per-mount device pool helpers ---- */

struct vmmfs_device *
vmmfs_find_device(struct vmmfs_mount *vmp, struct vmmfs_machine *owner,
    const char *name, int nlen)
{
	struct vmmfs_device *d;

	SLIST_FOREACH(d, &vmp->vm_devs, dv_link) {
		if (vmm_device_owned_by(&d->dev, VMMFS_CORE_MACHINE_OF(owner)) &&
		    vmm_device_bdf_eq(&d->dev, name, nlen))
			return d;
	}
	return NULL;
}

struct vmmfs_device *
vmmfs_find_device_any(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	struct vmmfs_device *d;

	SLIST_FOREACH(d, &vmp->vm_devs, dv_link) {
		if (vmm_device_bdf_eq(&d->dev, name, nlen))
			return d;
	}
	return NULL;
}

static struct vmmfs_device *
vmmfs_device_add(struct vmmfs_mount *vmp, const char *bdf, int is_host)
{
	struct vmmfs_device *d;
	ino_t idx = (ino_t)vmp->vm_next_dev++;

	d = kmalloc(sizeof(*d), M_VMMFS, M_WAITOK | M_ZERO);
	vmm_device_init(&d->dev, bdf, is_host);
	vmmfs_node_init(&d->node, &vmmfs_device_class, VREG, 0444,
	    VMMFS_DEV_INO_BASE + idx, &vmp->vm_host_devices, NULL);
	vmmfs_node_init(&d->link, &vmmfs_devlink_class, VLNK, 0777,
	    VMMFS_DEVLINK_INO_BASE + idx, &vmp->vm_devroot, NULL);
	SLIST_INSERT_HEAD(&vmp->vm_devs, d, dv_link);
	return d;
}

void
vmmfs_device_init_host_pool(struct vmmfs_mount *vmp)
{
	static const char *const stub_bdf[] = {
		"0000:00:02.0", "0000:00:03.0", "0000:00:04.0",
	};
	int i, n = (int)(sizeof(stub_bdf) / sizeof(stub_bdf[0]));

	for (i = 0; i < n; i++)
		(void)vmmfs_device_add(vmp, stub_bdf[i], 1);
}

void
vmmfs_device_unbind_owner_locked(struct vmmfs_mount *vmp,
    struct vmm_machine *owner, struct vmmfs_devlist *tofree)
{
	struct vmmfs_device *d, *nd;

	SLIST_FOREACH_MUTABLE(d, &vmp->vm_devs, dv_link, nd) {
		if (!vmm_device_owned_by(&d->dev, owner))
			continue;
		if (d->dev.is_host) {
			vmm_device_unbind(&d->dev);
		} else {
			SLIST_REMOVE(&vmp->vm_devs, d, vmmfs_device, dv_link);
			SLIST_INSERT_HEAD(tofree, d, dv_link);
		}
	}
}

void
vmmfs_device_free_list(struct vmmfs_devlist *list)
{
	struct vmmfs_device *d;

	while (!SLIST_EMPTY(list)) {
		d = SLIST_FIRST(list);
		SLIST_REMOVE_HEAD(list, dv_link);
		vmmfs_node_uninit(&d->node);
		vmmfs_node_uninit(&d->link);
		kfree(d, M_VMMFS);
	}
}

void
vmmfs_device_destroy_all(struct vmmfs_mount *vmp)
{
	struct vmmfs_device *d;

	while (!SLIST_EMPTY(&vmp->vm_devs)) {
		d = SLIST_FIRST(&vmp->vm_devs);
		SLIST_REMOVE_HEAD(&vmp->vm_devs, dv_link);
		vmmfs_node_uninit(&d->node);
		vmmfs_node_uninit(&d->link);
		kfree(d, M_VMMFS);
	}
}

static const char *
vmmfs_owner_name(struct vmm_machine *owner)
{
	return owner != NULL ? VMMFS_MACHINE_OF_CORE(owner)->name : "host";
}

int
vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
    char *buf, size_t bufsize)
{
	(void)vmp;
	return ksnprintf(buf, bufsize, "../machines/%s/devices/%s",
	    vmmfs_owner_name(d->dev.owner), d->dev.bdf);
}
