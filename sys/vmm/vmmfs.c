/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs - DragonFlyBSD system VMM control filesystem.
 *
 * A machine is created with `mkdir <name>`: it always starts stopped
 * with empty config.  The config files vcpu/mem/loader behave like hardware
 * registers — open() yields a per-open buffer holding the current DESIRED value
 * as text; read/write/seek act on that buffer; only close() atomically parses
 * the whole buffer and updates the desired value (invalid input is a no-op, the
 * old value is kept).  A successful write does NOT mean the value updated; only
 * reading it back is authoritative.  `rm stopped` only declares desired=running
 * and queues the vmm_machine worker; it never waits for the loader.  `echo
 * apic|force > stopped` declares desired=stopped and may cancel that worker;
 * `rmdir` deletes a stopped machine.
 *
 * Naming: vmmfs_* is the filesystem control plane.  This file owns the
 * filesystem root, mount/unmount, shared vnode helpers, and register buffers.
 * vmm_* is the VMM core (vmm_machine.c) and owns what a machine IS: config
 * parsing, the desired-state registers, and the lifecycle/lease/event state
 * machine.  Each filesystem machine wrapper embeds one struct vmm_machine.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kern_syscall.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/thread2.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/dirent.h>
#include <sys/filedesc.h>
#include <sys/uio.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/kobj.h>

#include "vmm_domain.h"
#include "vmm_dma.h"
#include "vmm_loader.h"
#include "vmm_machine.h"
#include "vmm_pcie_bar.h"
#include "vmm_pcie_user.h"
#include "vmm_vcpu.h"
#include "vmmfs.h"
#include "vmmfs_device.h"
#include "vmmfs_machine.h"
#include "vmmfs_node_if.h"

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs mount structures");
static struct lock vmmfs_mount_lock;
static int vmmfs_mount_count;
/* vfs_register() ignores vfs_init() errors; mount is the real admission point. */
static int vmmfs_backend_error;
static int vmmfs_initialized;

static void
vmmfs_mount_count_hold(void)
{
	lockmgr(&vmmfs_mount_lock, LK_EXCLUSIVE);
	vmmfs_mount_count++;
	lockmgr(&vmmfs_mount_lock, LK_RELEASE);
}

static void
vmmfs_mount_count_release(void)
{
	lockmgr(&vmmfs_mount_lock, LK_EXCLUSIVE);
	KKASSERT(vmmfs_mount_count > 0);
	vmmfs_mount_count--;
	lockmgr(&vmmfs_mount_lock, LK_RELEASE);
}

static int
vmmfs_mount_count_busy(void)
{
	int busy;

	lockmgr(&vmmfs_mount_lock, LK_SHARED);
	busy = (vmmfs_mount_count != 0);
	lockmgr(&vmmfs_mount_lock, LK_RELEASE);
	return busy;
}


static int	vmmfs_statfs(struct mount *mp, struct statfs *sbp,
		    struct ucred *cred);
static void	vmmfs_machine_reaper(void *arg);

int
vmmfs_machine_cmp(struct vmmfs_machine *a, struct vmmfs_machine *b)
{
	return strcmp(a->name, b->name);
}
RB_GENERATE(vmmfs_machtree, vmmfs_machine, vm_link, vmmfs_machine_cmp);

/* --------------------------------------------------------------------- */

/*
 * Initialize a node in place.  The caller supplies the KOBJ class (behavior),
 * the vnode type, and the mode -- there is no type tag and no class lookup.
 */
void
vmmfs_node_init(struct vmmfs_node *node, kobj_class_t class, enum vtype vtype,
    mode_t mode, ino_t ino, struct vmmfs_node *parent,
    struct vmmfs_machine *machine)
{
	node->vn_vtype = vtype;
	node->vn_mode = mode;
	node->vn_ino = ino;
	node->vn_parent = parent;
	node->vn_machine = machine;
	node->vn_vnode = NULL;
	lockinit(&node->vn_interlock, "vmmfs node", 0, 0);
	lwkt_token_init(&node->token_gate, "vmmfsgate");
	node->mut_revoking = 0;
	node->mut_active = 0;
	SLIST_INIT(&node->vn_obufs);
	kobj_init((kobj_t)node, class);
}

int
vmmfs_node_enter(struct vmmfs_node *node)
{
	int error;

	lwkt_gettoken(&node->token_gate);
	if (node->mut_revoking)
		error = ENXIO;
	else {
		node->mut_active++;
		error = 0;
	}
	lwkt_reltoken(&node->token_gate);
	return error;
}

void
vmmfs_node_enter_close(struct vmmfs_node *node)
{
	lwkt_gettoken(&node->token_gate);
	node->mut_active++;
	lwkt_reltoken(&node->token_gate);
}

void
vmmfs_node_leave(struct vmmfs_node *node)
{
	lwkt_gettoken(&node->token_gate);
	KKASSERT(node->mut_active != 0);
	if (--node->mut_active == 0)
		wakeup(node);
	lwkt_reltoken(&node->token_gate);
}

void
vmmfs_node_begin_revoke(struct vmmfs_node *node)
{
	lwkt_gettoken(&node->token_gate);
	node->mut_revoking = 1;
	lwkt_reltoken(&node->token_gate);
}

int
vmmfs_node_is_revoking(struct vmmfs_node *node)
{
	int revoking;

	lwkt_gettoken(&node->token_gate);
	revoking = node->mut_revoking;
	lwkt_reltoken(&node->token_gate);
	return revoking;
}

void
vmmfs_node_wait(struct vmmfs_node *node)
{
	lwkt_gettoken(&node->token_gate);
	while (node->mut_active != 0)
		tsleep(node, 0, "vmmfsgate", 0);
	lwkt_reltoken(&node->token_gate);
}

/* Fill the type-independent fields of a getattr result. */
void
vmmfs_fill_attr(struct vmmfs_node *node, struct vattr *vap, enum vtype type,
    int nlink, off_t size)
{
	vap->va_type = type;
	vap->va_mode = node->vn_mode;
	vap->va_nlink = nlink;
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = node->vn_vnode->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = node->vn_ino;
	vap->va_size = size;
	vap->va_blocksize = PAGE_SIZE;
	vap->va_atime.tv_sec = 0;
	vap->va_atime.tv_nsec = 0;
	vap->va_mtime = vap->va_atime;
	vap->va_ctime = vap->va_atime;
	vap->va_gen = 1;
	vap->va_flags = 0;
	vap->va_bytes = 0;
	vap->va_filerev = 0;
}

/* getattr for any directory node. */
int
vmmfs_dir_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	int nlink = (node->vn_parent == NULL) ? 3 : 2;	/* only root has no parent */

	vmmfs_fill_attr(node, ap->a_vap, VDIR, nlink, 0);
	return 0;
}

/*
 * Finish an nresolve: bind a vnode to the resolved child (or record a negative
 * cache entry).  Shared by every directory class.
 */
int
vmmfs_nresolve_finish(struct vnode *dvp, struct vmmfs_node *child,
    struct nchandle *nch)
{
	struct vnode *vp;
	int error;

	if (child == NULL) {
		cache_setvp(nch, NULL);
		return ENOENT;
	}
	if (vmmfs_node_is_revoking(child)) {
		cache_setvp(nch, NULL);
		return ENOENT;
	}
	error = vmmfs_alloc_vp(dvp->v_mount, child, LK_EXCLUSIVE | LK_RETRY, &vp);
	if (error)
		return error;
	vn_unlock(vp);
	cache_setvp(nch, vp);
	vrele(vp);
	return 0;
}

/*
 * Emit "." and "..".  On return *offp is the resume offset (>= 2 once both are
 * written) and *fullp is set if the uio filled.  Shared readdir prologue.
 */
int
vmmfs_readdir_dots(struct vop_readdir_args *ap, struct vmmfs_node *node,
    off_t *offp, int *fullp)
{
	struct uio *uio = ap->a_uio;
	int error = 0;
	off_t off;

	*offp = 0;
	*fullp = 0;
	if (ap->a_vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0)
		return EINVAL;

	off = uio->uio_offset;
	if (off == 0) {
		if (vop_write_dirent(&error, uio, node->vn_ino, DT_DIR, 1, ".")) {
			*fullp = 1;
			goto out;
		}
		off = 1;
	}
	if (off == 1) {
		if (vop_write_dirent(&error, uio, vmmfs_parent_ino(node),
		    DT_DIR, 2, "..")) {
			*fullp = 1;
			goto out;
		}
		off = 2;
	}
out:
	*offp = off;
	return error;
}

/* Record the resume offset, eof flag, and (empty) cookies; readdir epilogue. */
int
vmmfs_readdir_end(struct vop_readdir_args *ap, off_t off, int full, int error)
{
	ap->a_uio->uio_offset = off;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !full;
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	return error;
}

/* Free any lingering per-open buffers (teardown only; no commit). */
void
vmmfs_obuf_drain(struct vmmfs_node *node)
{
	struct vmmfs_openbuf *ob;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	while ((ob = SLIST_FIRST(&node->vn_obufs)) != NULL) {
		SLIST_REMOVE_HEAD(&node->vn_obufs, ob_link);
		kfree(ob->ob_data, M_VMMFS);
		kfree(ob, M_VMMFS);
	}
	lockmgr(&node->vn_interlock, LK_RELEASE);
}

void
vmmfs_node_uninit(struct vmmfs_node *node)
{
	vmmfs_node_wait(node);
	vmmfs_obuf_drain(node);
	lockuninit(&node->vn_interlock);
	lwkt_token_uninit(&node->token_gate);
}

/* Revoke all namecache aliases before the owning vmmfs object is released. */
void
vmmfs_node_revoke(struct vmmfs_node *node)
{
	struct vnode *vp;

	vmmfs_node_begin_revoke(node);
	VMMFS_NODE_REVOKE(node);

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	vp = node->vn_vnode;
	if (vp != NULL)
		vhold(vp);
	lockmgr(&node->vn_interlock, LK_RELEASE);
	if (vp == NULL)
		return;

	if (vget(vp, LK_EXCLUSIVE | LK_RETRY) != 0) {
		vdrop(vp);
		return;
	}
	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	if (node->vn_vnode != vp) {
		lockmgr(&node->vn_interlock, LK_RELEASE);
		vput(vp);
		vdrop(vp);
		return;
	}
	lockmgr(&node->vn_interlock, LK_RELEASE);
	vn_unlock(vp);

	(void)vrevoke(vp, proc0.p_ucred);
	vx_get(vp);
	vgone_vxlocked(vp);
	if (vp->v_mount == NULL)
		insmntque(vp, vfs_get_dummymount());
	vx_put(vp);
	vrele(vp);
	vdrop(vp);
	vmmfs_node_wait(node);
}

ino_t
vmmfs_parent_ino(struct vmmfs_node *node)
{
	return (node->vn_parent != NULL) ? node->vn_parent->vn_ino :
	    node->vn_ino;
}

/*
 * Bind a vnode to the given node, caching it.  Mirrors the interlocked
 * tmpfs_alloc_vp() normal path; vx_downgrade() after getnewvnode() is mandatory
 * so vflush() does not trip the v_spin assertion on unmount.
 */
static void
vmmfs_discard_new_vp(struct vnode *vp)
{

	/*
	 * getnewvnode() returned this vnode VX-locked and ref'd.
	 * Reclaim it now so a loadable vmmfs module never leaves cached
	 * vnodes pointing at a mount-local v_ops indirection after kldunload.
	 */
	vgone_vxlocked(vp);
	vx_put(vp);
}

int
vmmfs_alloc_vp(struct mount *mp, struct vmmfs_node *node, int lkflag,
    struct vnode **vpp)
{
	struct vnode *vp;
	enum vtype vtype = node->vn_vtype;
	int error = 0;

	if (vmmfs_node_is_revoking(node))
		return ENOENT;
	kprintf("vmm klog: alloc_vp begin node=%p ino=%ju type=%d vnode=%p\n",
	    node, (uintmax_t)node->vn_ino, vtype, node->vn_vnode);
loop:
	vp = NULL;
	if (node->vn_vnode == NULL) {
		kprintf("vmm klog: alloc_vp getnewvnode node=%p ino=%ju\n",
		    node, (uintmax_t)node->vn_ino);
		error = getnewvnode(VMMFS_VTAG, mp, &vp, VLKTIMEOUT,
		    LK_CANRECURSE);
		if (error) {
			kprintf("vmm klog: alloc_vp getnewvnode error=%d node=%p\n",
			    error, node);
			goto out;
		}
		/*
		 * The vnode is already visible on mp's vnode list.  VFS mount
		 * scans skip VNON, so mark the not-yet-bound vnode VBAD until
		 * v_data and the real type are installed.  The inactive/reclaim
		 * shims tolerate v_data == NULL for this half-initialized window.
		 */
		vp->v_type = VBAD;
	}

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	if (vmmfs_node_is_revoking(node)) {
		lockmgr(&node->vn_interlock, LK_RELEASE);
		if (vp != NULL)
			vmmfs_discard_new_vp(vp);
		return ENOENT;
	}
	if (node->vn_vnode != NULL) {
		struct vnode *ovp = node->vn_vnode;

		kprintf("vmm klog: alloc_vp existing node=%p ovp=%p\n",
		    node, ovp);
		vhold(ovp);
		lockmgr(&node->vn_interlock, LK_RELEASE);
		if (vp != NULL) {
			vmmfs_discard_new_vp(vp);
			vp = NULL;
		}
		if (vget(ovp, (lkflag & ~LK_RETRY) | LK_EXCLUSIVE) != 0) {
			vdrop(ovp);
			goto loop;
		}
		if (node->vn_vnode != ovp) {
			vput(ovp);
			vdrop(ovp);
			goto loop;
		}
		vdrop(ovp);
		vp = ovp;
		goto out;
	}

	if (vp == NULL) {
		lockmgr(&node->vn_interlock, LK_RELEASE);
		goto loop;
	}

	vp->v_data = node;
	vp->v_type = vtype;
	node->vn_vnode = vp;
	kprintf("vmm klog: alloc_vp bound node=%p vp=%p ino=%ju\n",
	    node, vp, (uintmax_t)node->vn_ino);
	lockmgr(&node->vn_interlock, LK_RELEASE);

	vx_downgrade(vp);
	kprintf("vmm klog: alloc_vp downgraded node=%p vp=%p\n", node, vp);

out:
	*vpp = vp;
	kprintf("vmm klog: alloc_vp end node=%p vp=%p error=%d\n",
	    node, vp, error);
	return error;
}

/* --------------------------------------------------------------------- */
/* Per-open config-register buffers (PCIe-register semantics).           */

static struct vmmfs_openbuf *
vmmfs_obuf_get(struct vmmfs_node *node, struct file *fp, int create)
{
	struct vmmfs_openbuf *ob;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	SLIST_FOREACH(ob, &node->vn_obufs, ob_link) {
		if (ob->ob_fp == fp) {
			lockmgr(&node->vn_interlock, LK_RELEASE);
			return ob;
		}
	}
	lockmgr(&node->vn_interlock, LK_RELEASE);
	if (!create)
		return NULL;

	ob = kmalloc(sizeof(*ob), M_VMMFS, M_WAITOK | M_ZERO);
	ob->ob_fp = fp;
	ob->ob_cap = 64;
	ob->ob_data = kmalloc(ob->ob_cap, M_VMMFS, M_WAITOK);
	ob->ob_len = 0;
	ob->ob_written = 0;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	{
		struct vmmfs_openbuf *ex;

		SLIST_FOREACH(ex, &node->vn_obufs, ob_link) {
			if (ex->ob_fp == fp) {
				lockmgr(&node->vn_interlock, LK_RELEASE);
				kfree(ob->ob_data, M_VMMFS);
				kfree(ob, M_VMMFS);
				return ex;
			}
		}
	}
	SLIST_INSERT_HEAD(&node->vn_obufs, ob, ob_link);
	lockmgr(&node->vn_interlock, LK_RELEASE);
	return ob;
}

/* Write into a register's open buffer (created on demand). */
int
vmmfs_obuf_write(struct vmmfs_node *node, struct file *fp, struct uio *uio)
{
	struct vmmfs_openbuf *ob;
	off_t off;
	size_t need, resid0, wrote;
	int error;

	if (uio->uio_offset < 0)
		return EINVAL;
	ob = vmmfs_obuf_get(node, fp, 1);
	if (ob == NULL)
		return ENOMEM;

	off = uio->uio_offset;
	need = (size_t)off + (size_t)uio->uio_resid;
	if (need > VMMFS_OBUF_MAX)
		return EFBIG;
	if ((int)need > ob->ob_cap) {
		int ncap = ob->ob_cap;
		char *nd;

		while (ncap < (int)need)
			ncap *= 2;
		nd = kmalloc(ncap, M_VMMFS, M_WAITOK);
		bcopy(ob->ob_data, nd, ob->ob_len);
		kfree(ob->ob_data, M_VMMFS);
		ob->ob_data = nd;
		ob->ob_cap = ncap;
	}
	if (off > ob->ob_len)
		bzero(ob->ob_data + ob->ob_len, (size_t)(off - ob->ob_len));

	resid0 = uio->uio_resid;
	error = uiomove(ob->ob_data + off, (size_t)uio->uio_resid, uio);
	if (error)
		return error;
	wrote = resid0 - uio->uio_resid;
	ob->ob_written = 1;
	if ((int)(off + wrote) > ob->ob_len)
		ob->ob_len = (int)(off + wrote);
	return 0;
}


/*
 * Shared register-file vops.  vcpu/mem/loader differ only in the text/commit
 * functions they pass; the open-buffer mechanism here is generic.
 */
int
vmmfs_register_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap,
    vmmfs_text_fn text)
{
	char tmp[300];
	off_t size;

	size = text(&node->vn_machine->machine, tmp, sizeof(tmp));
	vmmfs_fill_attr(node, ap->a_vap, VREG, 1, size);
	return 0;
}

int
vmmfs_register_read(struct vmmfs_node *node, struct vop_read_args *ap,
    vmmfs_text_fn text)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_openbuf *ob;
	char tmp[300];
	char *data;
	int len;
	off_t off;

	if (uio->uio_offset < 0)
		return EINVAL;
	ob = vmmfs_obuf_get(node, ap->a_fp, 0);
	if (ob != NULL && ob->ob_written) {
		data = ob->ob_data;
		len = ob->ob_len;
	} else {
		len = (int)text(&node->vn_machine->machine, tmp, sizeof(tmp));
		data = tmp;
	}
	off = uio->uio_offset;
	if (off >= len)
		return 0;
	return uiomove(data + off, (size_t)(len - off), uio);
}

int
vmmfs_register_write(struct vmmfs_node *node, struct vop_write_args *ap)
{
	return vmmfs_obuf_write(node, ap->a_fp, ap->a_uio);
}

int
vmmfs_register_open(struct vmmfs_node *node, struct vop_open_args *ap)
{
	(void)node;
	return vop_stdopen(ap);
}

int
vmmfs_register_close(struct vmmfs_node *node, struct vop_close_args *ap,
    vmmfs_commit_fn commit)
{
	struct vmmfs_openbuf *ob = NULL, *it;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	SLIST_FOREACH(it, &node->vn_obufs, ob_link) {
		if (it->ob_fp == ap->a_fp) {
			ob = it;
			SLIST_REMOVE(&node->vn_obufs, it, vmmfs_openbuf, ob_link);
			break;
		}
	}
	lockmgr(&node->vn_interlock, LK_RELEASE);
	if (ob != NULL) {
		if (ob->ob_written && !vmmfs_node_is_revoking(node))
			(void)commit(&node->vn_machine->machine, ob->ob_data,
			    (size_t)ob->ob_len);
		kfree(ob->ob_data, M_VMMFS);
		kfree(ob, M_VMMFS);
	}
	return vop_stdclose(ap);
}

/* getattr for a config file with no inline contents (size 0). */
int
vmmfs_zero_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	vmmfs_fill_attr(node, ap->a_vap, VREG, 1, 0);
	return 0;
}

/* read for a config file that has no readable contents yet (immediate EOF). */
int
vmmfs_zero_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	(void)node;
	(void)ap;
	return 0;
}

/* ---- the filesystem root: machine registry keyed by name ---- */

/* Caller holds vm_lock.  name need not be NUL-terminated. */
static struct vmmfs_machine *
vmmfs_root_find(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	struct vmmfs_machine *m;

	if (nlen < 0 || nlen > VMMFS_NAME_MAX)
		return NULL;
	m = RB_ROOT(&vmp->vm_machtree);
	while (m != NULL) {
		const char *mname = m->name;
		int mlen = strlen(mname);
		int cmp;

		cmp = strncmp(name, mname, (nlen < mlen) ? nlen : mlen);
		if (cmp == 0) {
			if (nlen < mlen)
				cmp = -1;
			else if (nlen > mlen)
				cmp = 1;
		}
		if (cmp < 0)
			m = RB_LEFT(m, vm_link);
		else if (cmp > 0)
			m = RB_RIGHT(m, vm_link);
		else
			return m;
	}
	return NULL;
}

static int
vmmfs_root_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vmmfs_node *child = NULL;

	(void)dnode;
	lockmgr(&vmp->vm_lock, LK_SHARED);
	m = vmmfs_root_find(vmp, ncp->nc_name, ncp->nc_nlen);
	if (m != NULL)
		child = &m->node;
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmmfs_root_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
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
		struct vmmfs_machine *m;
		int skip = (int)off - 2;

		i = 0;
		RB_FOREACH(m, vmmfs_machtree, &vmp->vm_machtree) {
			if (i++ < skip)
				continue;
			if (vop_write_dirent(&error, uio, m->node.vn_ino, DT_DIR,
			    (uint16_t)strlen(m->name), m->name)) {
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

static int
vmmfs_root_nmkdir(struct vmmfs_node *dnode, struct vop_nmkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vnode *vp;
	const char *name = ncp->nc_name;
	int name_len = ncp->nc_nlen;
	int create_leased = 0;
	int error;

	(void)dnode;
	if (name_len == 0 || name_len > VMMFS_NAME_MAX)
		return ENAMETOOLONG;
	if (name_len >= 7 && bcmp(name + name_len - 7, ".leased", 7) == 0) {
		if (name_len == 7)
			return EINVAL;
		name_len -= 7;
		create_leased = 1;
	}
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmp->vm_closing) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EBUSY;
	}
	vmp->vm_machine_count++;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	m = vmmfs_machine_create(vmp, name, name_len);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmmfs_root_find(vmp, name, name_len) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vmmfs_machine_free(m);
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		KKASSERT(vmp->vm_machine_count > 0);
		vmp->vm_machine_count--;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	if (create_leased) {
		error = vmmfs_machine_install_hidden_lease(m, ap->a_cred);
		if (error != 0) {
			lockmgr(&vmp->vm_lock, LK_RELEASE);
			vmmfs_machine_free(m);
			lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
			KKASSERT(vmp->vm_machine_count > 0);
			vmp->vm_machine_count--;
			lockmgr(&vmp->vm_lock, LK_RELEASE);
			return error;
		}
	}
	RB_INSERT(vmmfs_machtree, &vmp->vm_machtree, m);
	m->vm_in_tree = 1;
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (create_leased)
		cache_inval_vp(dvp, CINV_CHILDREN);

	vmm_debug_trace("root_mkdir inserted name=%s m=%p", m->name,
	    &m->machine);
	if (!vmm_debug_allow_nmkdir_vnode) {
		if (create_leased) {
			(void)kern_close(m->mut_hidden_lease_fd);
		} else {
			lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
			m->vm_in_tree = 0;
			RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
			lockmgr(&vmp->vm_lock, LK_RELEASE);
			vmmfs_machine_free(m);
			lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
			KKASSERT(vmp->vm_machine_count > 0);
			vmp->vm_machine_count--;
			lockmgr(&vmp->vm_lock, LK_RELEASE);
		}
		return EBUSY;
	}

	error = vmmfs_alloc_vp(dvp->v_mount, &m->node,
	    LK_EXCLUSIVE | LK_RETRY, &vp);
	if (error != 0) {
		if (create_leased) {
			(void)kern_close(m->mut_hidden_lease_fd);
		} else {
			lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
			m->vm_in_tree = 0;
			RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
			lockmgr(&vmp->vm_lock, LK_RELEASE);
			vmmfs_machine_free(m);
			lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
			KKASSERT(vmp->vm_machine_count > 0);
			vmp->vm_machine_count--;
			lockmgr(&vmp->vm_lock, LK_RELEASE);
		}
		return error;
	}
	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	if (!create_leased)
		cache_setvp(ap->a_nch, vp);
	return 0;
}

static int
vmmfs_root_nrmdir(struct vmmfs_node *dnode, struct vop_nrmdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	(void)dnode;
	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error != 0)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m = vmmfs_root_find(vmp, ncp->nc_name, ncp->nc_nlen);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	lwkt_gettoken(&m->machine.token_config);
	if (m->machine.mut_leased || !m->machine.mut_desired_stopped) {
		lwkt_reltoken(&m->machine.token_config);
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return EBUSY;
	}
	lwkt_reltoken(&m->machine.token_config);
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	error = vmm_machine_execute(&m->machine, vmm_machine_command_stop, NULL);
	if (error != 0) {
		vrele(vp);
		return error;
	}

	cache_inval_vp(vp, CINV_DESTROY | CINV_CHILDREN);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	error = vmmfs_device_destroy_owner_locked(vmp, &m->machine);
	if (error == 0) {
		KKASSERT(m->vm_in_tree != 0);
		m->vm_in_tree = 0;
		RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (error != 0) {
		vrele(vp);
		return error;
	}

	vrele(vp);
	error = lwkt_create(vmmfs_machine_reaper, m, NULL, NULL, 0, -1,
	    "vmmfsreap");
	if (error != 0)
		vmmfs_machine_reaper(m);
	return 0;
}

static kobj_method_t vmmfs_root_methods[] = {
	KOBJMETHOD(vmmfs_node_nresolve,		vmmfs_root_nresolve),
	KOBJMETHOD(vmmfs_node_readdir,		vmmfs_root_readdir),
	KOBJMETHOD(vmmfs_node_nmkdir,		vmmfs_root_nmkdir),
	KOBJMETHOD(vmmfs_node_nrmdir,		vmmfs_root_nrmdir),
	KOBJMETHOD(vmmfs_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmmfs_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmmfs_node_access,		vmmnode_access),
	KOBJMETHOD(vmmfs_node_open,		vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,		vmmnode_close),
	KOBJMETHOD(vmmfs_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_root, vmmfs_root_methods, 0);

static void
vmmfs_machine_reaper(void *arg)
{
	struct vmmfs_machine *m = arg;
	struct vmmfs_mount *vmp = m->vm_mount;
	int error;

	lwkt_gettoken(&m->machine.token_config);
	m->machine.mut_desired_stopped = 1;
	lwkt_reltoken(&m->machine.token_config);
	(void)vmm_machine_execute(&m->machine, vmm_machine_command_stop, NULL);

	vmm_machine_drain(&m->machine);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	error = vmmfs_device_destroy_owner_locked(vmp, &m->machine);
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (error != 0) {
		kprintf("vmm klog: machine_reaper device destroy failed m=%p error=%d\n",
		    m, error);
		return;
	}
	vmmfs_machine_free(m);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	KKASSERT(vmp->vm_machine_count > 0);
	vmp->vm_machine_count--;
	lockmgr(&vmp->vm_lock, LK_RELEASE);
}

void
vmmfs_machine_reclaim(struct vmmfs_machine *m)
{
	struct vmmfs_mount *vmp = m->vm_mount;
	int error;

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (m->vm_in_tree == 0) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return;
	}
	m->vm_in_tree = 0;
	RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	error = lwkt_create(vmmfs_machine_reaper, m, NULL, NULL, 0, -1,
	    "vmmfsreap");
	if (error != 0)
		kprintf("vmm klog: machine_reclaim lwkt_create failed m=%p error=%d\n",
		    m, error);
}

/* --------------------------------------------------------------------- */

static int
vmmfs_mount(struct mount *mp, char *path, caddr_t data, struct ucred *cred)
{
	struct vmmfs_mount *vmp;
	size_t size;

	if (!vmmfs_initialized)
		return vmmfs_backend_error != 0 ? vmmfs_backend_error : ENXIO;
	kprintf("vmm klog: mount begin mp=%p path=%p\n", mp, path);
	if (mp->mnt_flag & MNT_UPDATE)
		return EOPNOTSUPP;

	kprintf("vmm klog: mount kmalloc\n");
	vmp = kmalloc(sizeof(*vmp), M_VMMFS, M_WAITOK | M_ZERO);
	vmp->vm_mp = mp;
	kprintf("vmm klog: mount node init vmp=%p\n", vmp);
	lockinit(&vmp->vm_lock, "vmmfs registry", 0, 0);
	vmm_pcie_init(&vmp->own_mut_pcie);
	vmmfs_node_init(&vmp->vm_root, &vmmfs_root_class, VDIR, VMMFS_DIR_MODE,
	    VMMFS_ROOT_INO, NULL, NULL);
	RB_INIT(&vmp->vm_machtree);
	vmp->vm_next_ino = VMMFS_MACHINE_INO_BASE;
	SLIST_INIT(&vmp->vm_device_views);
	vmp->vm_next_dev = 0;

	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_ALL_MPSAFE;
	mp->mnt_kern_flag |= MNTK_NOMSYNC;
	mp->mnt_data = (qaddr_t)vmp;
	mp->mnt_iosize_max = MAXBSIZE;
	vfs_getnewfsid(mp);

	vfs_add_vnodeops(mp, &vmmfs_vnode_vops, &mp->mnt_vn_norm_ops);

	copystr("vmmfs", mp->mnt_stat.f_mntfromname, MNAMELEN - 1, &size);
	bzero(mp->mnt_stat.f_mntfromname + size, MNAMELEN - size);
	bzero(mp->mnt_stat.f_mntonname, sizeof(mp->mnt_stat.f_mntonname));
	copyinstr(path, mp->mnt_stat.f_mntonname,
	    sizeof(mp->mnt_stat.f_mntonname) - 1, &size);

	vmmfs_statfs(mp, &mp->mnt_stat, cred);
	vmmfs_mount_count_hold();
	kprintf("vmm klog: mount done mp=%p vmp=%p\n", mp, vmp);
	return 0;
}


static int
vmmfs_unmount(struct mount *mp, int mntflags)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(mp);
	int error;

	kprintf("vmm klog: unmount begin mp=%p vmp=%p flags=%d\n", mp, vmp,
	    mntflags);
	(void)mntflags;
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmp->vm_machine_count != 0) {
		kprintf("vmm klog: unmount busy vmp=%p count=%d\n", vmp,
		    vmp->vm_machine_count);
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EBUSY;
	}
	error = vmm_pcie_begin_shutdown(&vmp->own_mut_pcie);
	if (error != 0) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return error;
	}
	vmp->vm_closing = 1;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	error = vflush(mp, 0, 0);
	if (error) {
		kprintf("vmm klog: unmount vflush error=%d vmp=%p\n", error,
		    vmp);
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		vmp->vm_closing = 0;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vmm_pcie_cancel_shutdown(&vmp->own_mut_pcie);
		return error;
	}

	vmmfs_device_destroy_all(vmp);
	vmm_pcie_uninit(&vmp->own_mut_pcie);
	vmmfs_node_uninit(&vmp->vm_root);
	vmmfs_mount_count_release();
	lockuninit(&vmp->vm_lock);
	mp->mnt_data = NULL;
	kfree(vmp, M_VMMFS);
	kprintf("vmm klog: unmount done mp=%p\n", mp);
	return 0;
}

static int
vmmfs_root(struct mount *mp, struct vnode **vpp)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(mp);
	int error;

	error = vmmfs_alloc_vp(mp, &vmp->vm_root, LK_EXCLUSIVE | LK_RETRY, vpp);
	if (error)
		return error;
	(*vpp)->v_flag |= VROOT;
	(*vpp)->v_type = VDIR;
	return 0;
}

static int
vmmfs_statfs(struct mount *mp, struct statfs *sbp, struct ucred *cred)
{
	sbp->f_iosize = PAGE_SIZE;
	sbp->f_bsize = PAGE_SIZE;
	sbp->f_blocks = 1;
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = 0;	/* machines are created dynamically */
	sbp->f_ffree = 0;
	return 0;
}

static int
vmmfs_vfs_init(struct vfsconf *conf)
{
	int error;

	(void)conf;
	kprintf("vmm klog: vfs_init begin\n");
	vmmfs_backend_error = 0;
	vmmfs_initialized = 0;
	error = vmm_backend_probe();
	if (error != 0) {
		vmmfs_backend_error = error;
		kprintf("vmm: backend unavailable error=%d\n", error);
		return 0;
	}
	lockinit(&vmmfs_mount_lock, "vmmfs mounts", 0, 0);
	vmmfs_mount_count = 0;
	kprintf("vmm klog: domain init begin\n");
	error = vmm_domain_init();
	if (error) {
		kprintf("vmm klog: domain init error=%d\n", error);
		lockuninit(&vmmfs_mount_lock);
		vmm_backend_uninit();
		vmmfs_backend_error = error;
		return 0;
	}
	vmmfs_initialized = 1;
	kprintf("vmm klog: domain init done\n");
	kprintf("vmm: loaded\n");
	return 0;
}

static int
vmmfs_vfs_uninit(struct vfsconf *conf)
{
	(void)conf;
	kprintf("vmm klog: vfs_uninit begin\n");
	if (!vmmfs_initialized)
		return 0;
	if (vmmfs_mount_count_busy()) {
		kprintf("vmm klog: vfs_uninit busy\n");
		return EBUSY;
	}
	if (atomic_load_acq_int(&vmm_pcie_user_session_count) != 0) {
		kprintf("vmm klog: vfs_uninit vPCIe session count=%u\n",
		    vmm_pcie_user_session_count);
		return EBUSY;
	}
	if (vmm_loader_mmap_active()) {
		kprintf("vmm klog: vfs_uninit loader mmap active\n");
		return EBUSY;
	}
	if (vmm_pcie_bar_mmap_active()) {
		kprintf("vmm klog: vfs_uninit vPCIe BAR capability active\n");
		return EBUSY;
	}
	if (vmm_dma_mmap_active()) {
		kprintf("vmm klog: vfs_uninit vPCIe DMA capability active\n");
		return EBUSY;
	}
	vmm_domain_uninit();
	vmm_backend_uninit();
	lockuninit(&vmmfs_mount_lock);
	vmmfs_initialized = 0;
	kprintf("vmm: unloaded\n");
	return 0;
}

static struct vfsops vmmfs_vfsops = {
	.vfs_flags =		0,
	.vfs_mount =		vmmfs_mount,
	.vfs_unmount =		vmmfs_unmount,
	.vfs_root =		vmmfs_root,
	.vfs_statfs =		vmmfs_statfs,
	.vfs_init =		vmmfs_vfs_init,
	.vfs_uninit =		vmmfs_vfs_uninit,
};

VFS_SET(vmmfs_vfsops, vmm, VFCF_MPSAFE);
MODULE_VERSION(vmm, 1);
