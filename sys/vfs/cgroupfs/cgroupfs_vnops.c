/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly cgroupfs: vnode operations.
 *
 * VOPs only adapt arguments; object semantics live in cgroupfs_group.c.
 * Directory vnodes carry a struct cgroupfs_group, regular file vnodes a
 * struct cgroupfs_file.  This is the mount's only vector, so namespace VOPs
 * dispatched through mnt_vn_use_ops need no trampoline.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include "cgroupfs.h"

#define CGROUPFS_DIRECTORY_MODE	0755

static void
cgroupfs_vnode_attr(struct vnode *vp, ino_t *inodep, mode_t *modep)
{
	if (vp->v_type == VDIR) {
		*inodep = cgroupfs_group_inode(vp->v_data);
		*modep = CGROUPFS_DIRECTORY_MODE;
	} else {
		cgroupfs_group_file_attr(vp->v_data, inodep, modep);
	}
}

static int
cgroupfs_vop_access(struct vop_access_args *ap)
{
	ino_t inode;
	mode_t mode;

	cgroupfs_vnode_attr(ap->a_vp, &inode, &mode);
	return (vop_helper_access(ap, 0, 0, mode, 0));
}

static int
cgroupfs_vop_getattr(struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	ino_t inode;
	mode_t mode;

	cgroupfs_vnode_attr(vp, &inode, &mode);
	VATTR_NULL(vap);
	vap->va_type = vp->v_type;
	vap->va_mode = mode;
	vap->va_nlink = vp->v_type == VDIR ? 2 : 1;
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = inode;
	/* Content is generated on read; report no size, as Linux does. */
	vap->va_size = 0;
	vap->va_blocksize = PAGE_SIZE;
	vap->va_bytes = 0;
	vap->va_flags = 0;
	vap->va_gen = 0;
	vap->va_filerev = 0;
	return (0);
}

static int
cgroupfs_vop_read(struct vop_read_args *ap)
{
	if (ap->a_vp->v_type == VDIR)
		return (EISDIR);
	return (cgroupfs_group_file_read(ap->a_vp->v_data, ap->a_uio));
}

static int
cgroupfs_vop_write(struct vop_write_args *ap)
{
	if (ap->a_vp->v_type == VDIR)
		return (EISDIR);
	return (cgroupfs_group_file_write(ap->a_vp->v_data, ap->a_uio));
}

/*
 * Only truncation to zero is accepted, on writable files: open(O_TRUNC)
 * precedes a control write, and the write alone commits.
 */
static int
cgroupfs_vop_setattr(struct vop_setattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	ino_t inode;
	mode_t mode;

	if (vap->va_mode != (mode_t)VNOVAL ||
	    vap->va_uid != (uid_t)VNOVAL ||
	    vap->va_gid != (gid_t)VNOVAL ||
	    vap->va_flags != VNOVAL ||
	    vap->va_atime.tv_sec != VNOVAL ||
	    vap->va_mtime.tv_sec != VNOVAL)
		return (EOPNOTSUPP);
	if (vap->va_size == (u_quad_t)VNOVAL)
		return (0);
	if (vp->v_type == VDIR)
		return (EISDIR);
	cgroupfs_vnode_attr(vp, &inode, &mode);
	if ((mode & S_IWUSR) == 0)
		return (EPERM);
	return (vap->va_size == 0 ? 0 : EINVAL);
}

static int
cgroupfs_vop_readdir(struct vop_readdir_args *ap)
{
	int eof;
	int error;

	if (ap->a_vp->v_type != VDIR)
		return (ENOTDIR);
	/* Directory cookies are only needed for NFS export. */
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	error = cgroupfs_group_readdir(ap->a_vp->v_data, ap->a_uio, &eof);
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = eof;
	return (error);
}

static int
cgroupfs_vop_nresolve(struct vop_nresolve_args *ap)
{
	struct namecache *ncp = ap->a_nch->ncp;
	struct vnode *vp;
	int error;

	error = cgroupfs_group_lookup(ap->a_dvp->v_data, ncp->nc_name,
	    ncp->nc_nlen, &vp);
	if (error == 0) {
		cache_setvp(ap->a_nch, vp);
		vrele(vp);
	} else if (error == ENOENT) {
		cache_setvp(ap->a_nch, NULL);
	}
	return (error);
}

static int
cgroupfs_vop_nmkdir(struct vop_nmkdir_args *ap)
{
	struct nchandle *nch = ap->a_nch;
	struct vnode *vp;
	int error;

	if (ap->a_vap->va_type != VDIR)
		return (EINVAL);
	error = cgroupfs_group_mkdir(ap->a_dvp->v_data, nch->ncp->nc_name,
	    nch->ncp->nc_nlen, &vp);
	if (error != 0)
		return (error);
	error = vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0) {
		vrele(vp);
		return (error);
	}
	cache_setunresolved(nch);
	cache_setvp(nch, vp);
	*ap->a_vpp = vp;
	return (0);
}

/*
 * The unlinked entry stays resolved so a working directory inside the
 * removed group still reaches our VOPs (and ENOENT) rather than an
 * unresolvable ncp.  The vnode is recycled once its last user lets go.
 */
static int
cgroupfs_vop_nrmdir(struct vop_nrmdir_args *ap)
{
	struct nchandle *nch = ap->a_nch;
	struct vnode *vp;
	int error;

	error = cgroupfs_group_rmdir(ap->a_dvp->v_data, nch->ncp->nc_name,
	    nch->ncp->nc_nlen, &vp);
	if (error != 0)
		return (error);
	cache_unlink(nch);
	cgroupfs_vnode_finalize(vp);
	vrele(vp);
	return (0);
}

static int
cgroupfs_vop_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	return (cgroupfs_group_lookup_parent(ap->a_dvp->v_data, ap->a_vpp));
}

static int
cgroupfs_vop_inactive(struct vop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;
	bool dead;

	if (vp->v_data == NULL)
		return (0);
	/* A removed group's vnodes are never looked up again. */
	if (vp->v_type == VDIR)
		dead = cgroupfs_group_is_dead(vp->v_data);
	else
		dead = cgroupfs_group_file_is_dead(vp->v_data);
	if (dead)
		vrecycle(vp);
	return (0);
}

static int
cgroupfs_vop_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	void *data = vp->v_data;

	vp->v_data = NULL;
	if (data == NULL)
		return (0);
	if (vp->v_type == VDIR)
		cgroupfs_group_reclaim(data);
	else
		cgroupfs_group_file_reclaim(data, vp);
	return (0);
}

/*
 * Namespace changes not implemented here must not fall through to the
 * default vector's vop_compat_* shims, which report misleading errors.
 */
struct vop_ops cgroupfs_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_access =		cgroupfs_vop_access,
	.vop_close =		vop_stdclose,
	.vop_getattr =		cgroupfs_vop_getattr,
	.vop_inactive =		cgroupfs_vop_inactive,
	.vop_ncreate =		(void *)vop_eopnotsupp,
	.vop_nlink =		(void *)vop_eopnotsupp,
	.vop_nlookupdotdot =	cgroupfs_vop_nlookupdotdot,
	.vop_nmkdir =		cgroupfs_vop_nmkdir,
	.vop_nmknod =		(void *)vop_eopnotsupp,
	.vop_nremove =		(void *)vop_eopnotsupp,
	.vop_nrename =		(void *)vop_eopnotsupp,
	.vop_nresolve =		cgroupfs_vop_nresolve,
	.vop_nrmdir =		cgroupfs_vop_nrmdir,
	.vop_nsymlink =		(void *)vop_eopnotsupp,
	.vop_nwhiteout =	(void *)vop_eopnotsupp,
	.vop_open =		vop_stdopen,
	.vop_pathconf =		vop_stdpathconf,
	.vop_read =		cgroupfs_vop_read,
	.vop_readdir =		cgroupfs_vop_readdir,
	.vop_reclaim =		cgroupfs_vop_reclaim,
	.vop_setattr =		cgroupfs_vop_setattr,
	.vop_write =		cgroupfs_vop_write,
};
