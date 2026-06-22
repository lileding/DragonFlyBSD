/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs - DragonFlyBSD system VMM control filesystem.
 *
 * Milestone M1: mountable read-only skeleton.  The root directory exposes a
 * single "machines" subdirectory (empty for now).  Machine import and
 * lifecycle land in later milestones.
 *
 * The low-level VFS plumbing lives here in C (the kernel VFS/namecache ABI
 * boundary).  The Rust side currently owns load/unload logging and will grow
 * to own the vmmfs object model (machine registry, manifest parser, state
 * machines).
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
#include <sys/dirent.h>
#include <sys/stat.h>

extern int	vmmfs_rust_init(void);
extern void	vmmfs_rust_fini(void);

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs mount structures");

/*
 * Informational vnode tag.  vmmfs has no dedicated VT_ enum slot yet; VT_UNUSED7
 * is a reserved-unused value already present in the running kernel, so it is
 * safe to use as a purely informational v_tag.  Promote to a real VT_VMMFS once
 * vmmfs lands in the shared headers.
 */
#define VMMFS_VTAG		VT_UNUSED7

/* Fixed inode numbers for the M1 synthetic namespace. */
#define VMMFS_ROOT_INO		1
#define VMMFS_MACHINES_INO	2

#define VMMFS_DIR_MODE		0555

enum vmmfs_ntype {
	VMMFS_NROOT,
	VMMFS_NMACHINES,
};

/*
 * A synthetic vmmfs node.  M1 has exactly two, both directories, embedded in
 * the mount structure and permanent for the life of the mount.
 */
struct vmmfs_node {
	enum vmmfs_ntype	vn_type;
	ino_t			vn_ino;
	mode_t			vn_mode;
	struct vmmfs_node      *vn_parent;	/* NULL for the root */
	struct vnode	       *vn_vnode;	/* cached vnode, NULL if unbound */
	struct lock		vn_interlock;	/* guards vn_vnode binding */
};

struct vmmfs_mount {
	struct mount	       *vm_mp;
	struct vmmfs_node	vm_root;
	struct vmmfs_node	vm_machines;
};

#define VFS_TO_VMMFS(mp)	((struct vmmfs_mount *)((mp)->mnt_data))
#define VP_TO_VMMFS(vp)		((struct vmmfs_node *)((vp)->v_data))

static int	vmmfs_statfs(struct mount *mp, struct statfs *sbp,
		    struct ucred *cred);

static struct vop_ops vmmfs_vnode_vops;

/* --------------------------------------------------------------------- */

static void
vmmfs_node_init(struct vmmfs_node *node, enum vmmfs_ntype type, ino_t ino,
    struct vmmfs_node *parent)
{
	node->vn_type = type;
	node->vn_ino = ino;
	node->vn_mode = VMMFS_DIR_MODE;
	node->vn_parent = parent;
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

/*
 * Bind a vnode to the given node, caching it on the node.  Mirrors the
 * interlocked tmpfs_alloc_vp() normal path: a node maps to at most one vnode,
 * and concurrent lookups must converge on the same vnode.
 */
static int
vmmfs_alloc_vp(struct mount *mp, struct vmmfs_node *node, int lkflag,
    struct vnode **vpp)
{
	struct vnode *vp;
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
	vp->v_type = VDIR;
	node->vn_vnode = vp;
	lockmgr(&node->vn_interlock, LK_RELEASE);

	/*
	 * getnewvnode() returns a VX-locked vnode; downgrade it to an ordinary
	 * lockmgr lock (clearing the v_spin update state) so later vx_get()s
	 * during vflush()/reclaim do not trip the spinlock assertion.
	 */
	vx_downgrade(vp);

out:
	*vpp = vp;
	return error;
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

	/* M1: the only resolvable name is "machines" under the root. */
	if (dnode->vn_type == VMMFS_NROOT && ncp->nc_nlen == 8 &&
	    bcmp(ncp->nc_name, "machines", 8) == 0)
		child = &vmp->vm_machines;

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
	struct vnode *vp = ap->a_vp;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);

	if ((ap->a_mode & VWRITE) && (vp->v_mount->mnt_flag & MNT_RDONLY))
		return EROFS;

	return vop_helper_access(ap, 0, 0, node->vn_mode, 0);
}

static int
vmmfs_getattr(struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);

	vap->va_type = VDIR;
	vap->va_mode = node->vn_mode;
	vap->va_nlink = (node->vn_type == VMMFS_NROOT) ? 3 : 2;
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = node->vn_ino;
	vap->va_size = 0;
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

static int
vmmfs_readdir(struct vop_readdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);
	int maxoff;
	int error = 0;
	int r;
	off_t off;

	if (vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0)
		return EINVAL;

	/* M1: root lists ".", "..", "machines"; machines lists ".", "..". */
	maxoff = (node->vn_type == VMMFS_NROOT) ? 3 : 2;
	off = uio->uio_offset;

	if (off == 0) {
		r = vop_write_dirent(&error, uio, node->vn_ino, DT_DIR, 1, ".");
		if (r)
			goto done;
		off = 1;
	}
	if (off == 1) {
		r = vop_write_dirent(&error, uio, vmmfs_parent_ino(node),
		    DT_DIR, 2, "..");
		if (r)
			goto done;
		off = 2;
	}
	if (node->vn_type == VMMFS_NROOT && off == 2) {
		struct vmmfs_node *m = &VFS_TO_VMMFS(vp->v_mount)->vm_machines;

		r = vop_write_dirent(&error, uio, m->vn_ino, DT_DIR, 8,
		    "machines");
		if (r)
			goto done;
		off = 3;
	}

done:
	uio->uio_offset = off;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = (off >= maxoff);
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	return error;
}

static int
vmmfs_inactive(struct vop_inactive_args *ap)
{
	/* M1 nodes are permanent; keep the vnode cached, never recycle. */
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
	.vop_open =		vmmfs_open,
	.vop_close =		vmmfs_close,
	.vop_access =		vmmfs_access,
	.vop_getattr =		vmmfs_getattr,
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

	if (mp->mnt_flag & MNT_UPDATE)
		return EOPNOTSUPP;

	vmp = kmalloc(sizeof(*vmp), M_VMMFS, M_WAITOK | M_ZERO);
	vmp->vm_mp = mp;
	vmmfs_node_init(&vmp->vm_root, VMMFS_NROOT, VMMFS_ROOT_INO, NULL);
	vmmfs_node_init(&vmp->vm_machines, VMMFS_NMACHINES, VMMFS_MACHINES_INO,
	    &vmp->vm_root);

	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_flag |= MNT_RDONLY;
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

	vmmfs_node_uninit(&vmp->vm_machines);
	vmmfs_node_uninit(&vmp->vm_root);
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
	sbp->f_files = 2;
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
