/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs - DragonFlyBSD system VMM control filesystem.
 *
 * M1: mountable VFS exposing machines/.
 * M2: machine import via `ln -s <config-dir> machines/<name>` (vop_nsymlink,
 *     an atomic import that reads/validates vcpu/mem/loader in the caller's
 *     context) and removal via `rmdir machines/<name>` (vop_nrmdir).  Each
 *     machine is a directory whose vcpu/mem/loader[/stopped] files reflect the
 *     imported desired state.  The loader is not yet executed (that is M3).
 *
 * The low-level VFS plumbing and the kernel file I/O live here in C; the Rust
 * side owns the config-value parsing/validation (vmmfs_parse_vcpu/mem).
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

extern int	vmmfs_rust_init(void);
extern void	vmmfs_rust_fini(void);
extern int	vmmfs_parse_vcpu(const uint8_t *buf, size_t len, uint32_t *out);
extern int	vmmfs_parse_mem(const uint8_t *buf, size_t len, uint64_t *out);

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs mount structures");

/*
 * Informational vnode tag.  vmmfs has no dedicated VT_ enum slot yet; VT_UNUSED7
 * is a reserved-unused value already present in the running kernel, so it is
 * safe to use as a purely informational v_tag.
 */
#define VMMFS_VTAG		VT_UNUSED7

#define VMMFS_ROOT_INO		1
#define VMMFS_MACHINES_INO	2
#define VMMFS_MACHINE_INO_BASE	3
#define VMMFS_MACHINE_INO_STRIDE 8	/* inos reserved per machine */

#define VMMFS_DIR_MODE		0555
#define VMMFS_FILE_MODE		0444
#define VMMFS_MAX_MACHINES	64
#define VMMFS_NAME_MAX		63

enum vmmfs_ntype {
	VMMFS_NROOT,
	VMMFS_NMACHINES,
	VMMFS_NMACHINE,
	VMMFS_NCONFIG,
};

/* Config files presented under a machine directory. */
enum vmmfs_cfg {
	VMMFS_CFG_VCPU,
	VMMFS_CFG_MEM,
	VMMFS_CFG_LOADER,
	VMMFS_CFG_STOPPED,
	VMMFS_NCFG,
};

static const char *const vmmfs_cfg_name[VMMFS_NCFG] = {
	[VMMFS_CFG_VCPU] =	"vcpu",
	[VMMFS_CFG_MEM] =	"mem",
	[VMMFS_CFG_LOADER] =	"loader",
	[VMMFS_CFG_STOPPED] =	"stopped",
};

struct vmmfs_machine;

struct vmmfs_node {
	enum vmmfs_ntype	vn_type;
	enum vmmfs_cfg		vn_cfg;		/* valid for VMMFS_NCONFIG */
	ino_t			vn_ino;
	mode_t			vn_mode;
	struct vmmfs_node      *vn_parent;	/* NULL for the root */
	struct vmmfs_machine   *vn_machine;	/* owning machine, or NULL */
	struct vnode	       *vn_vnode;	/* cached vnode, NULL if unbound */
	struct lock		vn_interlock;	/* guards vn_vnode binding */
};

/*
 * An imported machine.  M2 reflects only the desired state read at import.
 * Slots live in the mount and are permanent for the mount's lifetime; in_use
 * marks occupancy.
 */
struct vmmfs_machine {
	int			in_use;
	char			name[VMMFS_NAME_MAX + 1];
	uint32_t		vcpu;
	uint64_t		mem;
	int			stopped;
	uint64_t		loader_size;	/* captured at import */
	mode_t			loader_mode;
	struct vmmfs_node	node;		/* the machine directory */
	struct vmmfs_node	cfg[VMMFS_NCFG];/* vcpu/mem/loader/stopped files */
};

struct vmmfs_mount {
	struct mount	       *vm_mp;
	struct vmmfs_node	vm_root;
	struct vmmfs_node	vm_machines;
	struct lock		vm_lock;	/* protects the machine registry */
	struct vmmfs_machine	vm_mach[VMMFS_MAX_MACHINES];
};

#define VFS_TO_VMMFS(mp)	((struct vmmfs_mount *)((mp)->mnt_data))
#define VP_TO_VMMFS(vp)		((struct vmmfs_node *)((vp)->v_data))

static int	vmmfs_statfs(struct mount *mp, struct statfs *sbp,
		    struct ucred *cred);
static int	vmmfs_cfg_format(struct vmmfs_node *node, char *buf,
		    size_t bufsize);

static struct vop_ops vmmfs_vnode_vops;

/* --------------------------------------------------------------------- */

static void
vmmfs_node_init(struct vmmfs_node *node, enum vmmfs_ntype type, ino_t ino,
    struct vmmfs_node *parent, struct vmmfs_machine *machine,
    enum vmmfs_cfg cfg)
{
	node->vn_type = type;
	node->vn_cfg = cfg;
	node->vn_ino = ino;
	node->vn_mode = (type == VMMFS_NCONFIG) ? VMMFS_FILE_MODE :
	    VMMFS_DIR_MODE;
	node->vn_parent = parent;
	node->vn_machine = machine;
	node->vn_vnode = NULL;
	lockinit(&node->vn_interlock, "vmmfs node", 0, 0);
}

static void
vmmfs_node_uninit(struct vmmfs_node *node)
{
	lockuninit(&node->vn_interlock);
}

static ino_t
vmmfs_parent_ino(struct vmmfs_node *node)
{
	return (node->vn_parent != NULL) ? node->vn_parent->vn_ino :
	    node->vn_ino;
}

static int
vmmfs_cfg_present(struct vmmfs_machine *m, enum vmmfs_cfg cfg)
{
	if (cfg == VMMFS_CFG_STOPPED)
		return m->stopped;
	return 1;
}

/*
 * Bind a vnode to the given node, caching it.  Mirrors the interlocked
 * tmpfs_alloc_vp() normal path; vx_downgrade() after getnewvnode() is
 * mandatory so vflush() does not trip the v_spin assertion on unmount.
 */
static int
vmmfs_alloc_vp(struct mount *mp, struct vmmfs_node *node, int lkflag,
    struct vnode **vpp)
{
	struct vnode *vp;
	enum vtype vtype;
	int error = 0;

	vtype = (node->vn_type == VMMFS_NCONFIG) ? VREG : VDIR;

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
/* Reading the config directory in the caller's context.                 */

/*
 * Open <dir>[/<name>] for reading in the caller's namespace/credentials and
 * return the opened, held, *unlocked* vnode (caller must vn_close()).
 */
static int
vmmfs_open_path(const char *dir, const char *name, struct ucred *cred,
    struct vnode **vpp)
{
	struct nlookupdata nd;
	char *path;
	size_t dlen, nlen;
	int error;

	dlen = strlen(dir);
	if (name != NULL) {
		nlen = strlen(name);
		path = kmalloc(dlen + 1 + nlen + 1, M_VMMFS, M_WAITOK);
		bcopy(dir, path, dlen);
		path[dlen] = '/';
		bcopy(name, path + dlen + 1, nlen);
		path[dlen + 1 + nlen] = '\0';
	} else {
		path = kmalloc(dlen + 1, M_VMMFS, M_WAITOK);
		bcopy(dir, path, dlen + 1);
	}

	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW | NLC_LOCKVP);
	if (error == 0)
		error = vn_open(&nd, NULL, FREAD, 0);
	if (error == 0) {
		*vpp = nd.nl_open_vp;
		nd.nl_open_vp = NULL;
	}
	nlookup_done(&nd);
	kfree(path, M_VMMFS);
	if (error == 0)
		vn_unlock(*vpp);
	return error;
}

static int
vmmfs_read_file(const char *dir, const char *name, struct ucred *cred,
    char *buf, size_t bufsize, size_t *outlen)
{
	struct vnode *vp;
	int error, resid;

	error = vmmfs_open_path(dir, name, cred, &vp);
	if (error)
		return error;
	if (vp->v_type != VREG) {
		vn_close(vp, FREAD, NULL);
		return EINVAL;
	}
	error = vn_rdwr(UIO_READ, vp, buf, (int)bufsize, 0, UIO_SYSSPACE, 0,
	    cred, &resid);
	vn_close(vp, FREAD, NULL);
	if (error)
		return error;
	*outlen = bufsize - resid;
	return 0;
}

/*
 * Read and validate the minimal config directory.  Returns 0 and fills the
 * out parameters, or an errno: ENOENT if <dir> itself is missing, EINVAL if a
 * required file is missing or malformed, EACCES if loader is not executable.
 */
static int
vmmfs_read_config(const char *dir, struct ucred *cred, uint32_t *vcpu,
    uint64_t *mem, int *stopped, uint64_t *loader_size, mode_t *loader_mode)
{
	struct vnode *vp;
	char buf[64];
	size_t len;
	int error;

	/* The config directory itself must exist and be a directory. */
	error = vmmfs_open_path(dir, NULL, cred, &vp);
	if (error)
		return error;
	if (vp->v_type != VDIR) {
		vn_close(vp, FREAD, NULL);
		return ENOTDIR;
	}
	vn_close(vp, FREAD, NULL);

	error = vmmfs_read_file(dir, "vcpu", cred, buf, sizeof(buf), &len);
	if (error)
		return (error == ENOENT) ? EINVAL : error;
	if (vmmfs_parse_vcpu((const uint8_t *)buf, len, vcpu) != 0)
		return EINVAL;

	error = vmmfs_read_file(dir, "mem", cred, buf, sizeof(buf), &len);
	if (error)
		return (error == ENOENT) ? EINVAL : error;
	if (vmmfs_parse_mem((const uint8_t *)buf, len, mem) != 0)
		return EINVAL;

	/* loader must exist, be a regular file, and be executable. */
	error = vmmfs_open_path(dir, "loader", cred, &vp);
	if (error)
		return (error == ENOENT) ? EINVAL : error;
	if (vp->v_type != VREG) {
		vn_close(vp, FREAD, NULL);
		return EINVAL;
	}
	vn_lock(vp, LK_SHARED | LK_RETRY);
	{
		struct vattr va;

		error = VOP_GETATTR(vp, &va);
		/*
		 * Require at least one execute bit (a file with none is not
		 * executable by anyone, including root), then the caller's own
		 * execute permission.
		 */
		if (error == 0 && (va.va_mode & 0111) == 0)
			error = EACCES;
		if (error == 0)
			error = VOP_ACCESS(vp, VEXEC, cred);
		if (error == 0) {
			*loader_size = va.va_size;
			*loader_mode = va.va_mode;
		}
	}
	vn_unlock(vp);
	vn_close(vp, FREAD, NULL);
	if (error)
		return error;

	/* stopped is optional: its presence means "do not auto-start". */
	error = vmmfs_open_path(dir, "stopped", cred, &vp);
	if (error == 0) {
		*stopped = 1;
		vn_close(vp, FREAD, NULL);
	} else {
		*stopped = 0;
	}
	return 0;
}

/* --------------------------------------------------------------------- */
/* Machine registry (guarded by vm_lock).                                */

static struct vmmfs_machine *
vmmfs_find_machine(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	int i;

	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];

		if (m->in_use && (int)strlen(m->name) == nlen &&
		    bcmp(m->name, name, nlen) == 0)
			return m;
	}
	return NULL;
}

static struct vmmfs_machine *
vmmfs_alloc_slot(struct vmmfs_mount *vmp)
{
	int i;

	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];

		/* Free slot with no lingering cached vnode. */
		if (!m->in_use && m->node.vn_vnode == NULL)
			return m;
	}
	return NULL;
}

/* --------------------------------------------------------------------- */

static int
vmmfs_nresolve(struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	struct vmmfs_node *child = NULL;
	struct vnode *vp = NULL;
	int error;

	if (dnode->vn_type == VMMFS_NROOT) {
		if (ncp->nc_nlen == 8 && bcmp(ncp->nc_name, "machines", 8) == 0)
			child = &vmp->vm_machines;
	} else if (dnode->vn_type == VMMFS_NMACHINES) {
		struct vmmfs_machine *m;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
		if (m != NULL)
			child = &m->node;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
	} else if (dnode->vn_type == VMMFS_NMACHINE) {
		struct vmmfs_machine *m = dnode->vn_machine;
		int i;

		for (i = 0; i < VMMFS_NCFG; i++) {
			if (!vmmfs_cfg_present(m, i))
				continue;
			if ((int)strlen(vmmfs_cfg_name[i]) == ncp->nc_nlen &&
			    bcmp(vmmfs_cfg_name[i], ncp->nc_name,
			    ncp->nc_nlen) == 0) {
				child = &m->cfg[i];
				break;
			}
		}
	}

	if (child == NULL) {
		cache_setvp(ap->a_nch, NULL);
		return ENOENT;
	}

	error = vmmfs_alloc_vp(dvp->v_mount, child, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error)
		return error;

	vn_unlock(vp);
	cache_setvp(ap->a_nch, vp);
	vrele(vp);
	return 0;
}

static int
vmmfs_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
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

/*
 * `ln -s <config-dir> machines/<name>` is an atomic machine import, not a
 * symlink creation.  We read and validate the config in the caller's context,
 * register the machine, and reflect it as a directory.
 */
static int
vmmfs_nsymlink(struct vop_nsymlink_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	struct vmmfs_machine *m;
	struct vnode *vp;
	uint32_t vcpu;
	uint64_t mem, loader_size;
	mode_t loader_mode;
	int stopped, error;

	/* Machines are created only under machines/. */
	if (dnode->vn_type != VMMFS_NMACHINES)
		return EINVAL;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen > VMMFS_NAME_MAX)
		return ENAMETOOLONG;

	/* Read + validate the config in the caller's context (no vm_lock). */
	error = vmmfs_read_config(ap->a_target, ap->a_cred, &vcpu, &mem,
	    &stopped, &loader_size, &loader_mode);
	if (error)
		return error;

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	m = vmmfs_alloc_slot(vmp);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return ENOSPC;
	}
	bcopy(ncp->nc_name, m->name, ncp->nc_nlen);
	m->name[ncp->nc_nlen] = '\0';
	m->vcpu = vcpu;
	m->mem = mem;
	m->stopped = stopped;
	m->loader_size = loader_size;
	m->loader_mode = loader_mode;
	m->in_use = 1;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	error = vmmfs_alloc_vp(dvp->v_mount, &m->node, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error) {
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		m->in_use = 0;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return error;
	}

	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/*
 * `rmdir machines/<name>` removes the machine.  M2 has no running state, so a
 * machine is always removable; the EBUSY-when-running rule arrives with the
 * lifecycle milestone.
 */
static int
vmmfs_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	if (dnode->vn_type != VMMFS_NMACHINES)
		return EINVAL;

	/*
	 * Acquire the target vnode through the namecache so the final vrele
	 * drives the inactive/reclaim sequence that clears node->vn_vnode
	 * (mirrors tmpfs_nremove).
	 */
	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	m->in_use = 0;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	cache_unlink(ap->a_nch);
	vrele(vp);
	return 0;
}

static int
vmmfs_open(struct vop_open_args *ap)
{
	return vop_stdopen(ap);
}

static int
vmmfs_close(struct vop_close_args *ap)
{
	return vop_stdclose(ap);
}

static int
vmmfs_access(struct vop_access_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	/* Writability is expressed per node via the mode (dirs 0555, files 0444). */
	return vop_helper_access(ap, 0, 0, node->vn_mode, 0);
}

static int
vmmfs_getattr(struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);
	int is_file = (node->vn_type == VMMFS_NCONFIG);
	char buf[64];

	vap->va_type = is_file ? VREG : VDIR;
	vap->va_mode = node->vn_mode;
	vap->va_nlink = is_file ? 1 :
	    ((node->vn_type == VMMFS_NROOT) ? 3 : 2);
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = node->vn_ino;
	vap->va_size = is_file ? vmmfs_cfg_format(node, buf, sizeof(buf)) : 0;
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
 * Format a config file's normalized content.  Returns the length (as
 * ksnprintf would, excluding the NUL).  stopped is an empty file whose mere
 * presence signals the stopped desired-state.
 */
static int
vmmfs_cfg_format(struct vmmfs_node *node, char *buf, size_t bufsize)
{
	struct vmmfs_machine *m = node->vn_machine;

	switch (node->vn_cfg) {
	case VMMFS_CFG_VCPU:
		return ksnprintf(buf, bufsize, "%u\n", m->vcpu);
	case VMMFS_CFG_MEM:
		return ksnprintf(buf, bufsize, "%ju\n", (uintmax_t)m->mem);
	case VMMFS_CFG_LOADER:
		return ksnprintf(buf, bufsize, "%ju %04o\n",
		    (uintmax_t)m->loader_size,
		    (unsigned)(m->loader_mode & 07777));
	case VMMFS_CFG_STOPPED:
	default:
		return 0;
	}
}

static int
vmmfs_read(struct vop_read_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);
	char buf[64];
	int len;
	off_t off;

	if (vp->v_type != VREG || node->vn_type != VMMFS_NCONFIG)
		return EINVAL;
	if (uio->uio_offset < 0)
		return EINVAL;

	len = vmmfs_cfg_format(node, buf, sizeof(buf));
	if (len > (int)sizeof(buf))
		len = (int)sizeof(buf);
	off = uio->uio_offset;
	if (off >= len)
		return 0;
	return uiomove(buf + off, (size_t)(len - off), uio);
}

static int
vmmfs_readdir(struct vop_readdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);
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
		if (off == 2) {
			struct vmmfs_node *mn =
			    &VFS_TO_VMMFS(vp->v_mount)->vm_machines;

			r = vop_write_dirent(&error, uio, mn->vn_ino, DT_DIR, 8,
			    "machines");
			if (r) {
				full = 1;
				goto done;
			}
			off = 3;
		}
	} else if (node->vn_type == VMMFS_NMACHINES) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int i;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		for (i = (int)off - 2; i < VMMFS_MAX_MACHINES; i++) {
			struct vmmfs_machine *m = &vmp->vm_mach[i];

			if (!m->in_use)
				continue;
			r = vop_write_dirent(&error, uio, m->node.vn_ino,
			    DT_DIR, (uint16_t)strlen(m->name), m->name);
			if (r) {
				off = 2 + i;
				full = 1;
				break;
			}
			off = 2 + i + 1;
		}
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		if (!full && off < 2 + VMMFS_MAX_MACHINES)
			off = 2 + VMMFS_MAX_MACHINES;
	} else if (node->vn_type == VMMFS_NMACHINE) {
		struct vmmfs_machine *m = node->vn_machine;
		int i;

		for (i = (int)off - 2; i < VMMFS_NCFG; i++) {
			if (!vmmfs_cfg_present(m, i))
				continue;
			r = vop_write_dirent(&error, uio, m->cfg[i].vn_ino,
			    DT_REG, (uint16_t)strlen(vmmfs_cfg_name[i]),
			    vmmfs_cfg_name[i]);
			if (r) {
				off = 2 + i;
				full = 1;
				break;
			}
			off = 2 + i + 1;
		}
		if (!full && off < 2 + VMMFS_NCFG)
			off = 2 + VMMFS_NCFG;
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

static int
vmmfs_inactive(struct vop_inactive_args *ap)
{
	/* All nodes are permanent (registry-embedded); keep the vnode cached. */
	return 0;
}

static int
vmmfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	KKASSERT(node->vn_vnode == vp);
	node->vn_vnode = NULL;
	vp->v_data = NULL;
	lockmgr(&node->vn_interlock, LK_RELEASE);

	return 0;
}

static int
vmmfs_print(struct vop_print_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	kprintf("\tvmmfs_node %p ino %ju type %d\n", node,
	    (uintmax_t)(node != NULL ? node->vn_ino : 0),
	    node != NULL ? (int)node->vn_type : -1);
	return 0;
}

static struct vop_ops vmmfs_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_nresolve =		vmmfs_nresolve,
	.vop_nlookupdotdot =	vmmfs_nlookupdotdot,
	.vop_nsymlink =		vmmfs_nsymlink,
	.vop_nrmdir =		vmmfs_nrmdir,
	.vop_open =		vmmfs_open,
	.vop_close =		vmmfs_close,
	.vop_access =		vmmfs_access,
	.vop_getattr =		vmmfs_getattr,
	.vop_read =		vmmfs_read,
	.vop_readdir =		vmmfs_readdir,
	.vop_inactive =		vmmfs_inactive,
	.vop_reclaim =		vmmfs_reclaim,
	.vop_print =		vmmfs_print,
};

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
		m->in_use = 0;
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
		int j;

		for (j = 0; j < VMMFS_NCFG; j++)
			vmmfs_node_uninit(&vmp->vm_mach[i].cfg[j]);
		vmmfs_node_uninit(&vmp->vm_mach[i].node);
	}
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
	return vmmfs_rust_init();
}

static int
vmmfs_vfs_uninit(struct vfsconf *conf)
{
	vmmfs_rust_fini();
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

VFS_SET(vmmfs_vfsops, vmmfs, VFCF_MPSAFE);
MODULE_VERSION(vmmfs, 1);
