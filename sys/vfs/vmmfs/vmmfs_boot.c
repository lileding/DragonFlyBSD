/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Persistent boot entry.  Every open returns a private launch file.
 */
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/systm.h>
#include <machine/atomic.h>
#include "vmmfs.h"
#include "vmmfs_boot.h"
#include "vmmfs_launch.h"
#include "vmmfs_machine.h"
#include "vmmfs_root.h"

#define VMMFS_BOOT_MODE 0600
static uint32_t vmmfs_boot_dev_serial;
static int vmmfs_boot_open(struct vop_open_args *);
static void vmmfs_boot_drop(struct vmmfs_node *);

static int
vmmfs_boot_deactivate(struct vmmfs_node *node)
{
	(void)node;
	return (0);
}

struct vop_ops vmmfs_boot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_boot_open,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_pathconf = vop_stdpathconf,
};

static struct dev_ops vmmfs_boot_dev_ops = {
	{ "vmmfs_boot", 0, D_MPSAFE },
};

int
vmmfs_boot_init(struct vmmfs_node *parent,
	struct vmmfs_boot *boot,
	struct vnode **vnodep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	uint32_t serial;
	int error;

	if (parent == NULL || boot == NULL || vnodep == NULL)
		return (EINVAL);
	machine = (struct vmmfs_machine *)parent;
	root = parent->mount->root_vnode->v_data;
	*vnodep = NULL;
	bzero(boot, sizeof(*boot));
	boot->node.parent = parent;
	boot->node.mount = parent->mount;
	boot->node.dead = false;
	boot->node.references = 1;
	lockinit(&boot->node.lock, "vmmfsnode", 0, 0);
	boot->node.deactivate = vmmfs_boot_deactivate;
	boot->node.drop = vmmfs_boot_drop;
	vmmfs_node_hold(parent);
	boot->node.inode = vmmfs_root_allocate_inode(root);
	boot->node.mode = VMMFS_BOOT_MODE;
	boot->node.size = machine->memory.size;
	serial = atomic_fetchadd_int(&vmmfs_boot_dev_serial, 1);
	boot->dev = make_only_dev(&vmmfs_boot_dev_ops, serial, UID_ROOT,
		GID_WHEEL, VMMFS_BOOT_MODE, "vmmfs_boot%d", serial);
	if (boot->dev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	boot->dev->si_drv1 = boot;
	error = vmmfs_vnode_create_cdev(parent->mount->mount,
	    &parent->mount->boot_vops, boot->dev, &boot->node, vnodep);
	if (error == 0)
		return (0);

fail:
	if (boot->dev != NULL) {
		boot->dev->si_drv1 = NULL;
		destroy_only_dev(boot->dev);
		boot->dev = NULL;
	}
	vmmfs_node_put(&boot->node);
	return (error);
}

static void
vmmfs_boot_drop(struct vmmfs_node *node)
{
	struct vmmfs_boot *boot = (struct vmmfs_boot *)node;

	if (boot->dev != NULL) {
		boot->dev->si_drv1 = NULL;
		destroy_only_dev(boot->dev);
	}
}

static int
vmmfs_boot_open(struct vop_open_args *ap)
{
	struct vmmfs_boot *boot = ap->a_vp->v_data;
	struct vmmfs_machine *machine = (struct vmmfs_machine *)boot->node.parent;
	struct vnode *vnode;
	struct file *file, *original;
	int error, abort_error;

	if (ap->a_fpp == NULL || (ap->a_mode & FWRITE) == 0)
		return (EACCES);
	/* machine_boot owns admission and the single launch identity. */
	error = VMMFS_WORK(machine, vmmfs_machine_boot(machine, &vnode));
	if (error != 0)
		return (error);
	error = vmmfs_launch_open(vnode, ap->a_cred, &file);
	if (error != 0) {
		abort_error = vmmfs_machine_abort(vnode->v_data);
		if (abort_error != 0)
			error = abort_error;
		vrele(vnode);
		return (error);
	}
	vrele(vnode);
	/*
	 * vn_open permits replacing the provisional file.  It releases the
	 * fixed entry vnode itself; this file owns only the private launch vnode.
	 */
	original = *ap->a_fpp;
	*ap->a_fpp = file;
	original->f_data = NULL;
	original->f_ops = &badfileops;
	fdrop(original);
	return (0);
}
