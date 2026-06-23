/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs - DragonFlyBSD system VMM control filesystem.
 *
 * A machine is created with `mkdir machines/<name>`: it always starts stopped
 * with empty config.  The config files vcpu/mem/loader behave like hardware
 * registers — open() yields a per-open buffer holding the current DESIRED value
 * as text; read/write/seek act on that buffer; only close() atomically parses
 * the whole buffer and updates the desired value (invalid input is a no-op, the
 * old value is kept).  A successful write does NOT mean the value updated; only
 * reading it back is authoritative.  `rm stopped` starts the machine after
 * validating the config is complete and the loader path is executable in the
 * caller's context; `echo apic|force > stopped` stops it; `rmdir` deletes a
 * stopped machine.  Every operation is atomic or idempotent — no intermediate
 * state.  The loader is not yet executed (that is "vmm core").
 *
 * Naming: vmmfs_* is the control plane (this file) -- the VFS/namecache
 * plumbing, per-open buffers, loader-path validation, and the machine registry.
 * vmm_* is the VMM core (vmm_machine.c) and owns what a machine IS: config
 * parsing, the desired-state registers, and the lifecycle/lease/event state
 * machine -- pure logic with no kernel deps, host-unit-tested (vmm_machine_test.c).
 * Each C registry slot (struct vmmfs_machines) embeds a vmm_machine.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
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
#include <sys/tree.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"
#include "vmmfs_machines.h"
#include "vmmfs_device.h"
#include "vmm_node_if.h"

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs mount structures");

static int	vmmfs_statfs(struct mount *mp, struct statfs *sbp,
		    struct ucred *cred);

/* --------------------------------------------------------------------- */

/*
 * Initialize a node in place.  The caller supplies the KOBJ class (behavior),
 * the vnode type, and the mode -- there is no type tag and no class lookup.
 */
void
vmmfs_node_init(struct vmmfs_node *node, kobj_class_t class, enum vtype vtype,
    mode_t mode, ino_t ino, struct vmmfs_node *parent,
    struct vmmfs_machines *machine)
{
	node->vn_vtype = vtype;
	node->vn_mode = mode;
	node->vn_ino = ino;
	node->vn_parent = parent;
	node->vn_machine = machine;
	node->vn_vnode = NULL;
	lockinit(&node->vn_interlock, "vmmfs node", 0, 0);
	SLIST_INIT(&node->vn_obufs);
	kobj_init((kobj_t)node, class);
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
	vmmfs_obuf_drain(node);
	lockuninit(&node->vn_interlock);
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
int
vmmfs_alloc_vp(struct mount *mp, struct vmmfs_node *node, int lkflag,
    struct vnode **vpp)
{
	struct vnode *vp;
	enum vtype vtype = node->vn_vtype;
	int error = 0;

loop:
	vp = NULL;
	if (node->vn_vnode == NULL) {
		error = getnewvnode(VMMFS_VTAG, mp, &vp, VLKTIMEOUT,
		    LK_CANRECURSE);
		if (error)
			goto out;
	}

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	if (node->vn_vnode != NULL) {
		struct vnode *ovp = node->vn_vnode;

		vhold(ovp);
		lockmgr(&node->vn_interlock, LK_RELEASE);
		if (vp != NULL) {
			vp->v_type = VBAD;
			vx_put(vp);
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
	lockmgr(&node->vn_interlock, LK_RELEASE);

	/* A machine node's first live vnode counts toward the machine's
	 * lifetime; reclaim drops it.  (vn_interlock released first to keep
	 * vm_lock un-nested.) */
	if (node->vn_machine != NULL)
		vmmfs_machine_ref(VFS_TO_VMMFS(mp), node->vn_machine);

	vx_downgrade(vp);

out:
	*vpp = vp;
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
    vmm_text_fn text)
{
	char tmp[300];
	off_t size;

	size = text(&node->vn_machine->state, tmp, sizeof(tmp));
	vmmfs_fill_attr(node, ap->a_vap, VREG, 1, size);
	return 0;
}

int
vmmfs_register_read(struct vmmfs_node *node, struct vop_read_args *ap,
    vmm_text_fn text)
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
		len = (int)text(&node->vn_machine->state, tmp, sizeof(tmp));
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
    vmm_commit_fn commit)
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
		if (ob->ob_written)
			(void)commit(&node->vn_machine->state, ob->ob_data,
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

/* ---- the filesystem root (/vmm): machines/ + devices/ ---- */

static int
vmm_root_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;

	(void)dnode;
	if (ncp->nc_nlen == 8 && bcmp(ncp->nc_name, "machines", 8) == 0)
		child = &vmp->vm_machines;
	else if (ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "devices", 7) == 0)
		child = &vmp->vm_devroot;
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmm_root_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_mount *vmp;
	off_t off;
	int full, error;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);
	if (off == 2) {
		if (vop_write_dirent(&error, uio, vmp->vm_machines.vn_ino, DT_DIR,
		    8, "machines")) {
			full = 1;
			goto out;
		}
		off = 3;
	}
	if (off == 3) {
		if (vop_write_dirent(&error, uio, vmp->vm_devroot.vn_ino, DT_DIR,
		    7, "devices")) {
			full = 1;
			goto out;
		}
		off = 4;
	}
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

static kobj_method_t vmm_root_methods[] = {
	KOBJMETHOD(vmm_node_nresolve,		vmm_root_nresolve),
	KOBJMETHOD(vmm_node_readdir,		vmm_root_readdir),
	KOBJMETHOD(vmm_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmm_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmm_node_access,		vmmnode_access),
	KOBJMETHOD(vmm_node_open,		vmmnode_open),
	KOBJMETHOD(vmm_node_close,		vmmnode_close),
	KOBJMETHOD(vmm_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_root, vmm_root_methods, 0);

/*
 * Validate the desired loader at start time: resolve the path in the caller's
 * context and require a regular, executable file.  No execution yet (vmm core).
 */
int
vmmfs_validate_loader(struct vmmfs_machines *m, struct ucred *cred)
{
	struct nlookupdata nd;
	struct vnode *vp = NULL;
	struct vattr va;
	char path[VMMFS_OBUF_MAX];
	size_t n;
	int error;

	n = vmm_loader_path(&m->state.loader, path, sizeof(path) - 1);
	if (n == 0)
		return EINVAL;
	path[n] = '\0';

	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW | NLC_LOCKVP);
	if (error == 0)
		error = vn_open(&nd, NULL, FREAD, 0);
	if (error == 0) {
		vp = nd.nl_open_vp;
		nd.nl_open_vp = NULL;
	}
	nlookup_done(&nd);
	if (error)
		return error;

	vn_unlock(vp);
	if (vp->v_type != VREG) {
		vn_close(vp, FREAD, NULL);
		return EACCES;
	}
	vn_lock(vp, LK_SHARED | LK_RETRY);
	error = VOP_GETATTR(vp, &va);
	if (error == 0 && (va.va_mode & 0111) == 0)
		error = EACCES;
	if (error == 0)
		error = VOP_ACCESS(vp, VEXEC, cred);
	vn_unlock(vp);
	vn_close(vp, FREAD, NULL);
	return error;
}

/* --------------------------------------------------------------------- */
/* Device pool (guarded by vm_lock).  Device formatting lives in the core
 * (vmm_device_format); these helpers are the fs-side pool lookups.       */

struct vmmfs_device *
vmmfs_find_device(struct vmmfs_mount *vmp, struct vmmfs_machines *owner,
    const char *name, int nlen)
{
	struct vmmfs_device *d;

	SLIST_FOREACH(d, &vmp->vm_devs, dv_link) {
		if (vmm_device_owned_by(&d->dev, VMMFS_STATE_OF(owner)) &&
		    vmm_device_bdf_eq(&d->dev, name, nlen))
			return d;
	}
	return NULL;
}

/* Find any device by bdf, regardless of owner (for the index). */
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

/* Allocate a device, wire up its nodes with fresh inos, and add it to the pool. */
static struct vmmfs_device *
vmmfs_device_add(struct vmmfs_mount *vmp, const char *bdf, int is_host)
{
	struct vmmfs_device *d;
	ino_t idx = (ino_t)vmp->vm_next_dev++;

	d = kmalloc(sizeof(*d), M_VMMFS, M_WAITOK | M_ZERO);
	vmm_device_init(&d->dev, bdf, is_host);
	if (is_host)
		vmm_host_add_device(&vmp->host);
	vmmfs_node_init(&d->node, &vmm_device_class, VREG, 0444,
	    VMMFS_DEV_INO_BASE + idx, &vmp->vm_host_devices, NULL);
	vmmfs_node_init(&d->link, &vmm_devlink_class, VLNK, 0777,
	    VMMFS_DEVLINK_INO_BASE + idx, &vmp->vm_devroot, NULL);
	SLIST_INSERT_HEAD(&vmp->vm_devs, d, dv_link);
	return d;
}

/* Owner display name: "host" or the owning machine's name (mapped from the
 * core VM pointer back to its fs slot). */
static const char *
vmmfs_owner_name(struct vmm_machine *owner)
{
	return owner != NULL ? VMMFS_MACHINES_OF_STATE(owner)->name : "host";
}

/* Relative symlink target for a device in the /vmm/devices/ index. */
int
vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
    char *buf, size_t bufsize)
{
	(void)vmp;
	return ksnprintf(buf, bufsize, "../machines/%s/devices/%s",
	    vmmfs_owner_name(d->dev.owner), d->dev.bdf);
}

/* --------------------------------------------------------------------- */


/* --------------------------------------------------------------------- */

static int
vmmfs_mount(struct mount *mp, char *path, caddr_t data, struct ucred *cred)
{
	struct vmmfs_mount *vmp;
	size_t size;
	int i;

	if (mp->mnt_flag & MNT_UPDATE)
		return EOPNOTSUPP;

	vmp = kmalloc(sizeof(*vmp), M_VMMFS, M_WAITOK | M_ZERO);
	vmp->vm_mp = mp;
	lockinit(&vmp->vm_lock, "vmmfs registry", 0, 0);
	vmmfs_node_init(&vmp->vm_root, &vmm_root_class, VDIR, VMMFS_DIR_MODE,
	    VMMFS_ROOT_INO, NULL, NULL);
	vmmfs_node_init(&vmp->vm_machines, &vmm_machines_class, VDIR,
	    VMMFS_DIR_MODE, VMMFS_MACHINES_INO, &vmp->vm_root, NULL);
	vmmfs_node_init(&vmp->vm_host, &vmm_host_class, VDIR, VMMFS_DIR_MODE,
	    VMMFS_HOST_INO, &vmp->vm_machines, NULL);
	vmmfs_node_init(&vmp->vm_host_devices, &vmm_devices_class, VDIR,
	    VMMFS_DIR_MODE, VMMFS_HOST_DEV_INO, &vmp->vm_host, NULL);
	vmmfs_node_init(&vmp->vm_devroot, &vmm_devroot_class, VDIR,
	    VMMFS_DIR_MODE, VMMFS_DEVROOT_INO, &vmp->vm_root, NULL);
	RB_INIT(&vmp->vm_machtree);
	vmp->vm_next_ino = VMMFS_MACHINE_INO_BASE;
	vmm_host_init(&vmp->host);
	SLIST_INIT(&vmp->vm_devs);
	vmp->vm_next_dev = 0;
	/* Stub host PCIe device pool: a few fixed BDFs, all owned by host. */
	{
		static const char *const stub_bdf[] = {
			"0000:00:02.0", "0000:00:03.0", "0000:00:04.0",
		};
		int n = (int)(sizeof(stub_bdf) / sizeof(stub_bdf[0]));

		for (i = 0; i < n; i++)
			(void)vmmfs_device_add(vmp, stub_bdf[i], 1);
	}

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
	return 0;
}

static int
vmmfs_unmount(struct mount *mp, int mntflags)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(mp);
	int flags = 0;
	int error;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	error = vflush(mp, 0, flags);
	if (error)
		return error;

	/*
	 * vflush reclaimed every vnode, so any rmdir'd-but-lease-held machine
	 * has already been freed (its last unref).  Free the live ones: drop
	 * each tree reference, which takes vm_refs to 0 and frees the struct.
	 */
	{
		struct vmmfs_machines *m;

		while ((m = RB_ROOT(&vmp->vm_machtree)) != NULL) {
			RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
			vmmfs_machine_unref(vmp, m);
		}
	}
	while (!SLIST_EMPTY(&vmp->vm_devs)) {
		struct vmmfs_device *d = SLIST_FIRST(&vmp->vm_devs);

		SLIST_REMOVE_HEAD(&vmp->vm_devs, dv_link);
		vmmfs_node_uninit(&d->node);
		vmmfs_node_uninit(&d->link);
		kfree(d, M_VMMFS);
	}
	vmmfs_node_uninit(&vmp->vm_devroot);
	vmmfs_node_uninit(&vmp->vm_host_devices);
	vmmfs_node_uninit(&vmp->vm_host);
	vmmfs_node_uninit(&vmp->vm_machines);
	vmmfs_node_uninit(&vmp->vm_root);
	lockuninit(&vmp->vm_lock);
	mp->mnt_data = NULL;
	kfree(vmp, M_VMMFS);
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
	kprintf("vmm: loaded\n");
	return 0;
}

static int
vmmfs_vfs_uninit(struct vfsconf *conf)
{
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
