/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs vnode operations: the vop_ops handlers + table.  The data model,
 * registry, vnode allocation, and per-open buffers live in vmmfs.c; vmmfs.h is
 * the shared interface.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/nlookup.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <sys/queue.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_node_if.h"

#define VMMFS_NODE_ENTER_OR_RETURN(node) do { \
	int error__ = vmmfs_node_enter(node); \
	if (error__ != 0) \
		return error__; \
} while (0)

#define VMMFS_NODE_LEAVE_RETURN(node, error) do { \
	vmmfs_node_leave(node); \
	return error; \
} while (0)

int
vmmnode_nlookupdotdot(struct vmmfs_node *dnode, struct vop_nlookupdotdot_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	int error;

	*vpp = NULL;

	error = VOP_ACCESS(dvp, VEXEC, ap->a_cred);
	if (error)
		return error;

	if (dnode->vn_parent != NULL) {
		error = vmmfs_alloc_vp(dvp->v_mount, dnode->vn_parent,
		    LK_EXCLUSIVE | LK_RETRY, vpp);
		if (*vpp != NULL)
			vn_unlock(*vpp);
	}

	return (*vpp == NULL) ? ENOENT : 0;
}

/* Generic open/close shared by every node that needs no per-open work. */
int
vmmnode_open(struct vmmfs_node *node, struct vop_open_args *ap)
{
	(void)node;
	return vop_stdopen(ap);
}

int
vmmnode_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	(void)node;
	return vop_stdclose(ap);
}

int
vmmnode_ioctl(struct vmmfs_node *node, struct vop_ioctl_args *ap)
{
	(void)node;
	return vop_stdioctl(ap);
}

int
vmmnode_kqfilter(struct vmmfs_node *node, struct vop_kqfilter_args *ap)
{
	(void)node;
	(void)ap;
	return EOPNOTSUPP;
}

int
vmmnode_access(struct vmmfs_node *node, struct vop_access_args *ap)
{

	return vop_helper_access(ap, 0, 0, node->vn_mode, 0);
}

/*
 * Accept no-op size changes (the O_TRUNC from `echo ... > file`) on writable
 * config files; everything else is read-only.
 */
int
vmmnode_setattr(struct vmmfs_node *node, struct vop_setattr_args *ap)
{

	if (node->vn_vtype == VREG && (node->vn_mode & 0200))
		return 0;
	return EPERM;
}

int
vmmnode_inactive(struct vmmfs_node *node, struct vop_inactive_args *ap)
{
	return 0;
}

int
vmmnode_reclaim(struct vmmfs_node *node, struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;

	/* No open fd can remain here, but free any stray buffers defensively. */
	vmmfs_obuf_drain(node);

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	KKASSERT(node->vn_vnode == vp);
	node->vn_vnode = NULL;
	vp->v_data = NULL;
	lockmgr(&node->vn_interlock, LK_RELEASE);
	if (vp->v_type == VCHR)
		v_release_rdev(vp);

	return 0;
}

int
vmmnode_print(struct vmmfs_node *node, struct vop_print_args *ap)
{

	kprintf("\tvmmfs_node %p ino %ju vtype %d\n", node,
	    (uintmax_t)(node != NULL ? node->vn_ino : 0),
	    node != NULL ? (int)node->vn_vtype : -1);
	return 0;
}

static void
vmmnode_revoke(struct vmmfs_node *node)
{
	(void)node;
}

/*
 * KOBJ dispatch.  Each vop_ops entry is a thin shim that resolves the node and
 * forwards to its class via the VMMFS_NODE_* methods; every node carries its own
 * object class (vmmfs_vcpu, vmmfs_machine, ...) bound at creation.
 */
static int
vmmfs_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_dvp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_NRESOLVE(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_dvp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_NLOOKUPDOTDOT(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_nmkdir(struct vop_nmkdir_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_dvp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_NMKDIR(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_ncreate(struct vop_ncreate_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_dvp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_NCREATE(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_nremove(struct vop_nremove_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_dvp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_NREMOVE(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_dvp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_NRMDIR(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_nrename(struct vop_nrename_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_fdvp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_NRENAME(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_readlink(struct vop_readlink_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_READLINK(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_open(struct vop_open_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_OPEN(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_close(struct vop_close_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	vmmfs_node_enter_close(node);
	error = VMMFS_NODE_CLOSE(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_access(struct vop_access_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_ACCESS(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_GETATTR(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_setattr(struct vop_setattr_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_SETATTR(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_read(struct vop_read_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_READ(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_write(struct vop_write_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_WRITE(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_ioctl(struct vop_ioctl_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	if (!VMMFS_NODE_IS(node, vmmfs_console_class))
		error = vop_stdioctl(ap);
	else
		error = VMMFS_NODE_IOCTL(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	if (!VMMFS_NODE_IS(node, vmmfs_console_class))
		error = EOPNOTSUPP;
	else
		error = VMMFS_NODE_KQFILTER(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	VMMFS_NODE_ENTER_OR_RETURN(node);
	error = VMMFS_NODE_READDIR(node, ap);
	VMMFS_NODE_LEAVE_RETURN(node, error);
}

static int
vmmfs_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	if (node == NULL)
		return 0;
	return VMMFS_NODE_INACTIVE(node, ap);
}

static int
vmmfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	if (node == NULL)
		return 0;
	return VMMFS_NODE_RECLAIM(node, ap);
}

static int
vmmfs_print(struct vop_print_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	if (node == NULL)
		return 0;
	return VMMFS_NODE_PRINT(node, ap);
}

/*
 * Fallback class: the common vops every node shares.  Each real node is created
 * with its own object class; this is only the safety net for a node created
 * without one (and the holder of the shared commons referenced above).
 */
static kobj_method_t vmmfs_base_methods[] = {
	KOBJMETHOD(vmmfs_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmmfs_node_open,		vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,		vmmnode_close),
	KOBJMETHOD(vmmfs_node_ioctl,		vmmnode_ioctl),
	KOBJMETHOD(vmmfs_node_kqfilter,		vmmnode_kqfilter),
	KOBJMETHOD(vmmfs_node_access,		vmmnode_access),
	KOBJMETHOD(vmmfs_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmmfs_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,		vmmnode_print),
	KOBJMETHOD(vmmfs_node_revoke,		vmmnode_revoke),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_base, vmmfs_base_methods, 0);

struct vop_ops vmmfs_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_nresolve =		vmmfs_nresolve,
	.vop_nlookupdotdot =	vmmfs_nlookupdotdot,
	.vop_nmkdir =		vmmfs_nmkdir,
	.vop_ncreate =		vmmfs_ncreate,
	.vop_nremove =		vmmfs_nremove,
	.vop_nrmdir =		vmmfs_nrmdir,
	.vop_nrename =		vmmfs_nrename,
	.vop_readlink =		vmmfs_readlink,
	.vop_open =		vmmfs_open,
	.vop_close =		vmmfs_close,
	.vop_access =		vmmfs_access,
	.vop_getattr =		vmmfs_getattr,
	.vop_setattr =		vmmfs_setattr,
	.vop_read =		vmmfs_read,
	.vop_write =		vmmfs_write,
	.vop_ioctl =		vmmfs_ioctl,
	.vop_kqfilter =		vmmfs_kqfilter,
	.vop_readdir =		vmmfs_readdir,
	.vop_inactive =		vmmfs_inactive,
	.vop_reclaim =		vmmfs_reclaim,
	.vop_print =		vmmfs_print,
};
