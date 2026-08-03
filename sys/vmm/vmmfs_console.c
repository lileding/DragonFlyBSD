/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem presentation of machines/<name>/console.  The node is a vmmfs
 * path to the machine-owned tty cdev; terminal behavior lives in vmm_console.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysmsg.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmm_console.h"
#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_node_if.h"

static cdev_t
vmmfs_console_dev(struct vmmfs_node *node)
{
	return vmm_console_dev(&node->vn_machine->machine.own_mut_console);
}

static int
vmmfs_console_associate(struct vmmfs_node *node, struct vnode *vp)
{
	cdev_t dev;
	int error;

	if (vp->v_rdev != NULL)
		return 0;
	dev = vmmfs_console_dev(node);
	if (dev == NULL)
		return ENXIO;
	error = v_associate_rdev(vp, dev);
	if (error)
		return error;
	vp->v_umajor = dev->si_umajor;
	vp->v_uminor = dev->si_uminor;
	return 0;
}

static int
vmmfs_console_open(struct vmmfs_node *node, struct vop_open_args *ap)
{
	struct vnode *vp = ap->a_vp;
	cdev_t dev;
	int error;

	error = vmmfs_console_associate(node, vp);
	if (error)
		return error;
	dev = vp->v_rdev;
	if (dev->si_iosize_max == 0)
		dev->si_iosize_max = min(MAXPHYS, 64 * 1024);
	/* A terminal has no file position. */
	/* cu forks concurrent readers and writers. */
	vsetflags(vp, VISTTY | VNOTSEEKABLE);
	vn_unlock(vp);
	error = dev_dopen(dev, ap->a_mode, S_IFCHR, ap->a_cred, ap->a_fpp,
	    vp);
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	if (error)
		return error;
	return vop_stdopen(ap);
}

static int
vmmfs_console_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	struct vnode *vp = ap->a_vp;
	cdev_t dev = vp->v_rdev;
	int error = 0;

	(void)node;
	if (dev != NULL && vp->v_opencount <= 1) {
		vn_unlock(vp);
		error = dev_dclose(dev, ap->a_fflag, S_IFCHR, ap->a_fp);
		vn_lock(vp, LK_SHARED | LK_RETRY);
	}
	if (vp->v_opencount > 0)
		vop_stdclose(ap);
	return error;
}

static int
vmmfs_console_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	int error;

	error = vmmfs_console_associate(node, ap->a_vp);
	if (error)
		return error;
	vmmfs_fill_attr(node, ap->a_vap, VCHR, 1, 0);
	return 0;
}

static int
vmmfs_console_setattr(struct vmmfs_node *node, struct vop_setattr_args *ap)
{
	if (ap->a_vap->va_size != VNOVAL)
		return 0;
	return vmmnode_setattr(node, ap);
}

static int
vmmfs_console_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	struct vnode *vp = ap->a_vp;
	cdev_t dev = vp->v_rdev;
	int error;

	(void)node;
	if (dev == NULL)
		return EBADF;
	if (ap->a_uio->uio_resid == 0)
		return 0;
	vn_unlock(vp);
	error = dev_dread(dev, ap->a_uio, ap->a_ioflag, ap->a_fp);
	vn_lock(vp, LK_SHARED | LK_RETRY);
	return error;
}

static int
vmmfs_console_write(struct vmmfs_node *node, struct vop_write_args *ap)
{
	struct vnode *vp = ap->a_vp;
	cdev_t dev = vp->v_rdev;
	int error;

	(void)node;
	if (dev == NULL)
		return EBADF;
	if (ap->a_uio->uio_resid == 0)
		return 0;
	vn_unlock(vp);
	error = dev_dwrite(dev, ap->a_uio, ap->a_ioflag, ap->a_fp);
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	return error;
}

static int
vmmfs_console_ioctl(struct vmmfs_node *node, struct vop_ioctl_args *ap)
{
	struct vnode *vp = ap->a_vp;
	cdev_t dev = vp->v_rdev;

	(void)node;
	if (dev == NULL)
		return EBADF;
	return dev_dioctl(dev, ap->a_command, ap->a_data, ap->a_fflag,
	    ap->a_cred, ap->a_sysmsg, NULL);
}

static int
vmmfs_console_kqfilter(struct vmmfs_node *node,
    struct vop_kqfilter_args *ap)
{
	struct vnode *vp = ap->a_vp;
	cdev_t dev = vp->v_rdev;

	(void)node;
	if (dev == NULL)
		return EBADF;
	return dev_dkqfilter(dev, ap->a_kn, NULL);
}

static void
vmmfs_console_revoke(struct vmmfs_node *node)
{
	cdev_t dev = vmmfs_console_dev(node);

	if (dev != NULL)
		dev_drevoke(dev);
}

static kobj_method_t vmmfs_console_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_console_getattr),
	KOBJMETHOD(vmmfs_node_read,	vmmfs_console_read),
	KOBJMETHOD(vmmfs_node_write,	vmmfs_console_write),
	KOBJMETHOD(vmmfs_node_ioctl,	vmmfs_console_ioctl),
	KOBJMETHOD(vmmfs_node_kqfilter,	vmmfs_console_kqfilter),
	KOBJMETHOD(vmmfs_node_revoke,	vmmfs_console_revoke),
	KOBJMETHOD(vmmfs_node_open,	vmmfs_console_open),
	KOBJMETHOD(vmmfs_node_close,	vmmfs_console_close),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmfs_console_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_console, vmmfs_console_methods, 0);
