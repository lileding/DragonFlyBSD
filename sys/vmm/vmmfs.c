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
 * Each C registry slot (struct vmmfs_machine) embeds a vmm_machine.
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
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs mount structures");


const char *const vmmfs_cfg_name[VMMFS_NCFG] = {
	[VMMFS_CFG_VCPU] =	"vcpu",
	[VMMFS_CFG_MEM] =	"mem",
	[VMMFS_CFG_LOADER] =	"loader",
	[VMMFS_CFG_LEASE] =	"lease",
	[VMMFS_CFG_EVENTS] =	"events",
	[VMMFS_CFG_CONSOLE] =	"console",
	[VMMFS_CFG_STATUS] =	"status.tar.gz",
	[VMMFS_CFG_STOPPED] =	"stopped",
};

/* The three desired-state registers use the per-open buffer + commit-on-close. */
int
vmmfs_cfg_is_register(enum vmmfs_cfg cfg)
{
	return cfg == VMMFS_CFG_VCPU || cfg == VMMFS_CFG_MEM ||
	    cfg == VMMFS_CFG_LOADER;
}

static mode_t
vmmfs_cfg_mode(enum vmmfs_cfg cfg)
{
	switch (cfg) {
	case VMMFS_CFG_VCPU:
	case VMMFS_CFG_MEM:
	case VMMFS_CFG_LOADER:
	case VMMFS_CFG_CONSOLE:
	case VMMFS_CFG_STOPPED:
		return 0644;
	case VMMFS_CFG_LEASE:
	case VMMFS_CFG_EVENTS:
	case VMMFS_CFG_STATUS:
	default:
		return 0444;
	}
}


static int	vmmfs_statfs(struct mount *mp, struct statfs *sbp,
		    struct ucred *cred);

/* --------------------------------------------------------------------- */

static void
vmmfs_node_init(struct vmmfs_node *node, enum vmmfs_ntype type, ino_t ino,
    struct vmmfs_node *parent, struct vmmfs_machine *machine,
    enum vmmfs_cfg cfg)
{
	node->vn_type = type;
	node->vn_cfg = cfg;
	node->vn_ino = ino;
	if (type == VMMFS_NCONFIG)
		node->vn_mode = vmmfs_cfg_mode(cfg);
	else if (type == VMMFS_NDEVICE)
		node->vn_mode = 0444;
	else if (type == VMMFS_NDEVLINK)
		node->vn_mode = 0777;
	else
		node->vn_mode = VMMFS_DIR_MODE;
	node->vn_owner = VMMFS_OWNER_HOST;
	node->vn_parent = parent;
	node->vn_machine = machine;
	node->vn_vnode = NULL;
	lockinit(&node->vn_interlock, "vmmfs node", 0, 0);
	SLIST_INIT(&node->vn_obufs);
	kobj_init((kobj_t)node, vmmfs_class_for(type, cfg));
}

/* Map a node type (and config kind) to its KOBJ class. */
kobj_class_t
vmmfs_class_for(enum vmmfs_ntype type, enum vmmfs_cfg cfg)
{
	(void)cfg;
	switch (type) {
	case VMMFS_NDEVICE:
		return &vmm_device_class;
	case VMMFS_NDEVLINK:
		return &vmm_devlink_class;
	case VMMFS_NMACHINES:
		return &vmm_machines_class;
	case VMMFS_NHOST:
		return &vmm_host_class;
	case VMMFS_NDEVICES:
		return &vmm_devices_class;
	case VMMFS_NDEVROOT:
		return &vmm_devroot_class;
	default:
		return &vmm_legacy_class;
	}
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
	int nlink = (node->vn_type == VMMFS_NROOT) ? 3 : 2;

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

static void
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

int
vmmfs_cfg_present(struct vmmfs_machine *m, enum vmmfs_cfg cfg)
{
	if (cfg == VMMFS_CFG_STOPPED)
		return vmm_machine_is_stopped(&m->state);
	return 1;
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
	enum vtype vtype;
	int error = 0;

	if (node->vn_type == VMMFS_NCONFIG || node->vn_type == VMMFS_NDEVICE)
		vtype = VREG;
	else if (node->vn_type == VMMFS_NDEVLINK)
		vtype = VLNK;
	else
		vtype = VDIR;

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

/* Read a register: the open buffer if it was written, else the current value. */
int
vmmfs_obuf_read(struct vmmfs_node *node, struct file *fp, struct uio *uio)
{
	struct vmmfs_openbuf *ob;
	uint8_t tmp[300];
	char *data;
	int len;
	off_t off;

	if (uio->uio_offset < 0)
		return EINVAL;
	ob = vmmfs_obuf_get(node, fp, 0);
	if (ob != NULL && ob->ob_written) {
		data = ob->ob_data;
		len = ob->ob_len;
	} else {
		len = (int)vmmfs_cfg_text(node, tmp, sizeof(tmp));
		data = (char *)tmp;
	}
	off = uio->uio_offset;
	if (off >= len)
		return 0;
	return uiomove(data + off, (size_t)(len - off), uio);
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

/* Commit (parse + update desired) on close, then free the buffer. */
static void
vmmfs_cfg_commit(struct vmmfs_node *node, const uint8_t *buf, size_t len);

void
vmmfs_obuf_commit_close(struct vmmfs_node *node, struct file *fp)
{
	struct vmmfs_openbuf *ob = NULL, *it;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	SLIST_FOREACH(it, &node->vn_obufs, ob_link) {
		if (it->ob_fp == fp) {
			ob = it;
			SLIST_REMOVE(&node->vn_obufs, it, vmmfs_openbuf,
			    ob_link);
			break;
		}
	}
	lockmgr(&node->vn_interlock, LK_RELEASE);
	if (ob == NULL)
		return;
	if (ob->ob_written)
		vmmfs_cfg_commit(node, (const uint8_t *)ob->ob_data,
		    (size_t)ob->ob_len);
	kfree(ob->ob_data, M_VMMFS);
	kfree(ob, M_VMMFS);
}

/* Current desired value of a register, serialized as text. */
size_t
vmmfs_cfg_text(struct vmmfs_node *node, uint8_t *buf, size_t cap)
{
	struct vmmfs_machine *m = node->vn_machine;

	switch (node->vn_cfg) {
	case VMMFS_CFG_VCPU:
		return vmm_machine_vcpu_text(&m->state, buf, cap);
	case VMMFS_CFG_MEM:
		return vmm_machine_mem_text(&m->state, buf, cap);
	case VMMFS_CFG_LOADER:
		return vmm_machine_loader_text(&m->state, buf, cap);
	default:
		return 0;
	}
}

static void
vmmfs_cfg_commit(struct vmmfs_node *node, const uint8_t *buf, size_t len)
{
	struct vmmfs_machine *m = node->vn_machine;

	switch (node->vn_cfg) {
	case VMMFS_CFG_VCPU:
		(void)vmm_machine_commit_vcpu(&m->state, buf, len);
		break;
	case VMMFS_CFG_MEM:
		(void)vmm_machine_commit_mem(&m->state, buf, len);
		break;
	case VMMFS_CFG_LOADER:
		(void)vmm_machine_commit_loader(&m->state, buf, len);
		break;
	default:
		break;
	}
}

/*
 * Validate the desired loader at start time: resolve the path in the caller's
 * context and require a regular, executable file.  No execution yet (vmm core).
 */
int
vmmfs_validate_loader(struct vmmfs_machine *m, struct ucred *cred)
{
	struct nlookupdata nd;
	struct vnode *vp = NULL;
	struct vattr va;
	char path[VMMFS_OBUF_MAX];
	size_t n;
	int error;

	n = vmm_machine_loader_path(&m->state, path, sizeof(path) - 1);
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
/* Device pool (guarded by vm_lock).                                     */

int
vmmfs_device_format(struct vmmfs_device *d, char *buf, size_t bufsize)
{
	return ksnprintf(buf, bufsize, "%s\n", d->bdf);
}

struct vmmfs_device *
vmmfs_find_device(struct vmmfs_mount *vmp, int owner, const char *name, int nlen)
{
	int i;

	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		struct vmmfs_device *d = &vmp->vm_dev[i];

		if (d->in_use && d->owner == owner &&
		    (int)strlen(d->bdf) == nlen && bcmp(d->bdf, name, nlen) == 0)
			return d;
	}
	return NULL;
}

/* Find any in_use device by bdf, regardless of owner (for the index). */
struct vmmfs_device *
vmmfs_find_device_any(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	int i;

	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		struct vmmfs_device *d = &vmp->vm_dev[i];

		if (d->in_use && (int)strlen(d->bdf) == nlen &&
		    bcmp(d->bdf, name, nlen) == 0)
			return d;
	}
	return NULL;
}

/* Owner display name: "host" or the owning machine's name. */
static const char *
vmmfs_owner_name(struct vmmfs_mount *vmp, int owner)
{
	if (owner == VMMFS_OWNER_HOST)
		return "host";
	if (owner >= 0 && owner < VMMFS_MAX_MACHINES &&
	    vmp->vm_mach[owner].in_use)
		return vmp->vm_mach[owner].name;
	return NULL;
}

/* Relative symlink target for a device in the /vmm/devices/ index. */
int
vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
    char *buf, size_t bufsize)
{
	const char *owner = vmmfs_owner_name(vmp, d->owner);

	if (owner == NULL)
		return -1;
	return ksnprintf(buf, bufsize, "../machines/%s/devices/%s", owner,
	    d->bdf);
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
	vmmfs_node_init(&vmp->vm_root, VMMFS_NROOT, VMMFS_ROOT_INO, NULL, NULL,
	    0);
	vmmfs_node_init(&vmp->vm_machines, VMMFS_NMACHINES, VMMFS_MACHINES_INO,
	    &vmp->vm_root, NULL, 0);
	vmmfs_node_init(&vmp->vm_host, VMMFS_NHOST, VMMFS_HOST_INO,
	    &vmp->vm_machines, NULL, 0);
	vmmfs_node_init(&vmp->vm_host_devices, VMMFS_NDEVICES,
	    VMMFS_HOST_DEV_INO, &vmp->vm_host, NULL, 0);
	vmp->vm_host_devices.vn_owner = VMMFS_OWNER_HOST;
	vmmfs_node_init(&vmp->vm_devroot, VMMFS_NDEVROOT, VMMFS_DEVROOT_INO,
	    &vmp->vm_root, NULL, 0);
	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];
		ino_t base = VMMFS_MACHINE_INO_BASE +
		    (ino_t)i * VMMFS_MACHINE_INO_STRIDE;
		int j;

		vmmfs_node_init(&m->node, VMMFS_NMACHINE, base,
		    &vmp->vm_machines, m, 0);
		for (j = 0; j < VMMFS_NCFG; j++)
			vmmfs_node_init(&m->cfg[j], VMMFS_NCONFIG, base + 1 + j,
			    &m->node, m, j);
		vmmfs_node_init(&m->vn_devices, VMMFS_NDEVICES,
		    base + VMMFS_MACHINE_DEV_OFF, &m->node, m, 0);
		m->vn_devices.vn_owner = i;
		m->in_use = 0;
	}
	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		struct vmmfs_device *d = &vmp->vm_dev[i];

		d->in_use = 0;
		d->owner = VMMFS_OWNER_HOST;
		d->is_host = 1;
		vmmfs_node_init(&d->node, VMMFS_NDEVICE,
		    VMMFS_DEV_INO_BASE + (ino_t)i, &vmp->vm_host_devices, NULL,
		    0);
		vmmfs_node_init(&d->link, VMMFS_NDEVLINK,
		    VMMFS_DEVLINK_INO_BASE + (ino_t)i, &vmp->vm_devroot, NULL,
		    0);
	}
	/* Stub host PCIe device pool: a few fixed BDFs, all owned by host. */
	{
		static const char *const stub_bdf[] = {
			"0000:00:02.0", "0000:00:03.0", "0000:00:04.0",
		};
		int n = (int)(sizeof(stub_bdf) / sizeof(stub_bdf[0]));

		for (i = 0; i < n && i < VMMFS_MAX_DEVICES; i++) {
			struct vmmfs_device *d = &vmp->vm_dev[i];

			d->in_use = 1;
			strlcpy(d->bdf, stub_bdf[i], sizeof(d->bdf));
		}
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
	int i;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	error = vflush(mp, 0, flags);
	if (error)
		return error;

	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];
		int j;

		for (j = 0; j < VMMFS_NCFG; j++)
			vmmfs_node_uninit(&m->cfg[j]);
		vmmfs_node_uninit(&m->vn_devices);
		vmmfs_node_uninit(&m->node);
	}
	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		vmmfs_node_uninit(&vmp->vm_dev[i].node);
		vmmfs_node_uninit(&vmp->vm_dev[i].link);
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
	sbp->f_files = VMMFS_MAX_MACHINES;
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
