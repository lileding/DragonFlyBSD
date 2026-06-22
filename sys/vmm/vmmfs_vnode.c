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
#include <sys/uio.h>
#include <sys/queue.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"
#include "vmm_node_if.h"

static int
vmmnode_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;

	if (dnode->vn_type == VMMFS_NROOT) {
		if (ncp->nc_nlen == 8 && bcmp(ncp->nc_name, "machines", 8) == 0)
			child = &vmp->vm_machines;
		else if (ncp->nc_nlen == 7 &&
		    bcmp(ncp->nc_name, "devices", 7) == 0)
			child = &vmp->vm_devroot;
	}

	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

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
vmmnode_access(struct vmmfs_node *node, struct vop_access_args *ap)
{

	return vop_helper_access(ap, 0, 0, node->vn_mode, 0);
}

static int
vmmnode_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	int is_file = (node->vn_type == VMMFS_NCONFIG ||
	    node->vn_type == VMMFS_NDEVICE);
	int is_link = (node->vn_type == VMMFS_NDEVLINK);
	uint8_t tmp[300];

	vap->va_type = is_link ? VLNK : (is_file ? VREG : VDIR);
	vap->va_mode = node->vn_mode;
	vap->va_nlink = (is_file || is_link) ? 1 :
	    ((node->vn_type == VMMFS_NROOT) ? 3 : 2);
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = node->vn_ino;
	if (node->vn_type == VMMFS_NCONFIG &&
	    vmmfs_cfg_is_register(node->vn_cfg)) {
		vap->va_size = vmmfs_cfg_text(node, tmp, sizeof(tmp));
	} else {
		vap->va_size = 0;
	}
	vap->va_blocksize = PAGE_SIZE;
	vap->va_atime.tv_sec = 0;
	vap->va_atime.tv_nsec = 0;
	vap->va_mtime = vap->va_atime;
	vap->va_ctime = vap->va_atime;
	vap->va_gen = 1;
	vap->va_flags = 0;
	vap->va_bytes = 0;
	vap->va_filerev = 0;

	return 0;
}

/*
 * Accept no-op size changes (the O_TRUNC from `echo ... > file`) on writable
 * config files; everything else is read-only.
 */
int
vmmnode_setattr(struct vmmfs_node *node, struct vop_setattr_args *ap)
{

	if (node->vn_type == VMMFS_NCONFIG && (node->vn_mode & 0200))
		return 0;
	return EPERM;
}

static int
vmmnode_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;

	if (vp->v_type != VREG)
		return EINVAL;

	if (node->vn_type != VMMFS_NCONFIG)
		return EINVAL;

	/*
	 * events is a stream: each read drains and consumes the queued event
	 * lines (shared one-shot cursor).  Non-blocking; blocking read and
	 * kqueue are a later step.
	 */
	if (node->vn_cfg == VMMFS_CFG_EVENTS) {
		uint8_t ebuf[256];
		size_t n;

		n = vmm_machine_read_events(&node->vn_machine->state, ebuf,
		    sizeof(ebuf));
		if (n == 0)
			return 0;
		return uiomove(ebuf, n, uio);
	}

	if (vmmfs_cfg_is_register(node->vn_cfg))
		return vmmfs_obuf_read(node, ap->a_fp, uio);

	/* console / status.tar.gz / lease / stopped: empty for now. */
	return 0;
}

static int
vmmnode_write(struct vmmfs_node *node, struct vop_write_args *ap)
{
	struct uio *uio = ap->a_uio;
	char buf[16];
	size_t take;
	int error, force;

	if (node->vn_type != VMMFS_NCONFIG)
		return EPERM;

	if (vmmfs_cfg_is_register(node->vn_cfg))
		return vmmfs_obuf_write(node, ap->a_fp, uio);

	if (node->vn_cfg == VMMFS_CFG_CONSOLE) {
		/* Console input is a stub: accept and discard. */
		while (uio->uio_resid > 0) {
			char dump[64];
			size_t d = (uio->uio_resid < (int)sizeof(dump)) ?
			    (size_t)uio->uio_resid : sizeof(dump);

			error = uiomove(dump, d, uio);
			if (error)
				return error;
		}
		return 0;
	}

	if (node->vn_cfg != VMMFS_CFG_STOPPED)
		return EPERM;

	/*
	 * Writing the stopped control file selects the stop method (apic|force)
	 * and (re)applies the stop.  Idempotent.
	 */
	take = (uio->uio_resid < (int)(sizeof(buf) - 1)) ?
	    (size_t)uio->uio_resid : sizeof(buf) - 1;
	error = uiomove(buf, take, uio);
	if (error)
		return error;
	buf[take] = '\0';
	force = (take >= 5 && strncmp(buf, "force", 5) == 0);

	while (uio->uio_resid > 0) {
		char dump[32];
		size_t d = (uio->uio_resid < (int)sizeof(dump)) ?
		    (size_t)uio->uio_resid : sizeof(dump);

		error = uiomove(dump, d, uio);
		if (error)
			return error;
	}

	vmm_machine_stop(&node->vn_machine->state, force);
	return 0;
}

static int
vmmnode_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	int error = 0;
	int r;
	int full = 0;
	off_t off;

	if (vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0)
		return EINVAL;

	off = uio->uio_offset;

	if (off == 0) {
		r = vop_write_dirent(&error, uio, node->vn_ino, DT_DIR, 1, ".");
		if (r) {
			full = 1;
			goto done;
		}
		off = 1;
	}
	if (off == 1) {
		r = vop_write_dirent(&error, uio, vmmfs_parent_ino(node),
		    DT_DIR, 2, "..");
		if (r) {
			full = 1;
			goto done;
		}
		off = 2;
	}

	if (node->vn_type == VMMFS_NROOT) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);

		if (off == 2) {
			r = vop_write_dirent(&error, uio,
			    vmp->vm_machines.vn_ino, DT_DIR, 8, "machines");
			if (r) {
				full = 1;
				goto done;
			}
			off = 3;
		}
		if (off == 3) {
			r = vop_write_dirent(&error, uio, vmp->vm_devroot.vn_ino,
			    DT_DIR, 7, "devices");
			if (r) {
				full = 1;
				goto done;
			}
			off = 4;
		}
	}

done:
	uio->uio_offset = off;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !full;
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	return error;
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

	return 0;
}

int
vmmnode_print(struct vmmfs_node *node, struct vop_print_args *ap)
{

	kprintf("\tvmmfs_node %p ino %ju type %d\n", node,
	    (uintmax_t)(node != NULL ? node->vn_ino : 0),
	    node != NULL ? (int)node->vn_type : -1);
	return 0;
}

/*
 * KOBJ dispatch.  Each vop_ops entry is a thin shim that resolves the node
 * and forwards to its class.  For now every node uses one catch-all class
 * (vmm_legacy) whose methods are the handlers above; step 2 splits these
 * into per-object classes.
 */
static int
vmmfs_nresolve(struct vop_nresolve_args *ap)
{
	return VMM_NODE_NRESOLVE(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	return VMM_NODE_NLOOKUPDOTDOT(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nmkdir(struct vop_nmkdir_args *ap)
{
	return VMM_NODE_NMKDIR(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_ncreate(struct vop_ncreate_args *ap)
{
	return VMM_NODE_NCREATE(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nremove(struct vop_nremove_args *ap)
{
	return VMM_NODE_NREMOVE(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nrmdir(struct vop_nrmdir_args *ap)
{
	return VMM_NODE_NRMDIR(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nrename(struct vop_nrename_args *ap)
{
	return VMM_NODE_NRENAME(VP_TO_VMMFS(ap->a_fdvp), ap);
}

static int
vmmfs_readlink(struct vop_readlink_args *ap)
{
	return VMM_NODE_READLINK(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_open(struct vop_open_args *ap)
{
	return VMM_NODE_OPEN(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_close(struct vop_close_args *ap)
{
	return VMM_NODE_CLOSE(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_access(struct vop_access_args *ap)
{
	return VMM_NODE_ACCESS(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_getattr(struct vop_getattr_args *ap)
{
	return VMM_NODE_GETATTR(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_setattr(struct vop_setattr_args *ap)
{
	return VMM_NODE_SETATTR(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_read(struct vop_read_args *ap)
{
	return VMM_NODE_READ(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_write(struct vop_write_args *ap)
{
	return VMM_NODE_WRITE(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_readdir(struct vop_readdir_args *ap)
{
	return VMM_NODE_READDIR(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_inactive(struct vop_inactive_args *ap)
{
	return VMM_NODE_INACTIVE(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_reclaim(struct vop_reclaim_args *ap)
{
	return VMM_NODE_RECLAIM(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_print(struct vop_print_args *ap)
{
	return VMM_NODE_PRINT(VP_TO_VMMFS(ap->a_vp), ap);
}

static kobj_method_t vmm_legacy_methods[] = {
	KOBJMETHOD(vmm_node_nresolve, vmmnode_nresolve),
	KOBJMETHOD(vmm_node_nlookupdotdot, vmmnode_nlookupdotdot),
	KOBJMETHOD(vmm_node_open, vmmnode_open),
	KOBJMETHOD(vmm_node_close, vmmnode_close),
	KOBJMETHOD(vmm_node_access, vmmnode_access),
	KOBJMETHOD(vmm_node_getattr, vmmnode_getattr),
	KOBJMETHOD(vmm_node_setattr, vmmnode_setattr),
	KOBJMETHOD(vmm_node_read, vmmnode_read),
	KOBJMETHOD(vmm_node_write, vmmnode_write),
	KOBJMETHOD(vmm_node_readdir, vmmnode_readdir),
	KOBJMETHOD(vmm_node_inactive, vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim, vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print, vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_legacy, vmm_legacy_methods, 0);

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
	.vop_readdir =		vmmfs_readdir,
	.vop_inactive =		vmmfs_inactive,
	.vop_reclaim =		vmmfs_reclaim,
	.vop_print =		vmmfs_print,
};
