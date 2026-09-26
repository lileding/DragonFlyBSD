/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly cgroupfs: filesystem operations.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/spinlock2.h>
#include <sys/vnode.h>

#include "cgroupfs.h"

/* Inodes 0 and 1 are conventionally reserved; the root gets 2. */
#define CGROUPFS_ROOT_INODE	2

static int cgroupfs_mount(struct mount *, char *, caddr_t, struct ucred *);
static int cgroupfs_unmount(struct mount *, int);
static int cgroupfs_root(struct mount *, struct vnode **);
static int cgroupfs_statfs(struct mount *, struct statfs *, struct ucred *);

static int
cgroupfs_mount(struct mount *mp, char *path, caddr_t data, struct ucred *cred)
{
	struct cgroupfs_mount *kmp;
	size_t size;
	int error;

	(void)data;
	if (mp->mnt_flag & MNT_UPDATE)
		return (EOPNOTSUPP);

	kmp = kmalloc(sizeof(*kmp), M_CGROUPFS, M_WAITOK | M_ZERO);
	kmp->mount = mp;
	kmp->next_inode = CGROUPFS_ROOT_INODE;
	lockinit(&kmp->hierarchy_lock, "cgrphier", 0, 0);
	spin_init(&kmp->vnode_spin, "cgrpvn");

	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_NOSTKMNT | MNTK_ALL_MPSAFE;
	mp->mnt_data = (qaddr_t)kmp;
	vfs_getnewfsid(mp);

	size = sizeof("cgroupfs") - 1;
	bcopy("cgroupfs", mp->mnt_stat.f_mntfromname, size);
	bzero(mp->mnt_stat.f_mntfromname + size, MNAMELEN - size);
	bzero(mp->mnt_stat.f_mntonname, sizeof(mp->mnt_stat.f_mntonname));
	error = copyinstr(path, mp->mnt_stat.f_mntonname,
	    sizeof(mp->mnt_stat.f_mntonname) - 1, &size);
	if (error != 0)
		goto fail;

	/* Root vnode creation needs the vector in place. */
	vfs_add_vnodeops(mp, &cgroupfs_vnode_vops, &mp->mnt_vn_norm_ops);
	error = cgroupfs_group_create_root(kmp, &kmp->root);
	if (error != 0) {
		vfs_rm_vnodeops(mp, &cgroupfs_vnode_vops, &mp->mnt_vn_norm_ops);
		goto fail;
	}
	return (cgroupfs_statfs(mp, &mp->mnt_stat, cred));

fail:
	mp->mnt_data = NULL;
	spin_uninit(&kmp->vnode_spin);
	lockuninit(&kmp->hierarchy_lock);
	kfree(kmp, M_CGROUPFS);
	return (error);
}

static int
cgroupfs_unmount(struct mount *mp, int mntflags)
{
	struct cgroupfs_mount *kmp = (struct cgroupfs_mount *)mp->mnt_data;
	int error;

	error = cgroupfs_group_unmount_begin(kmp->root);
	if (error != 0)
		return (error);
	/*
	 * The root vnode's base reference is the only persistent one left;
	 * vflush() releases it on success, and every group is then freed by
	 * the reclaims it performs.
	 */
	error = vflush(mp, 1, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);
	if (error != 0) {
		cgroupfs_group_unmount_abort(kmp->root);
		return (error);
	}
	kmp->root = NULL;
	vfs_rm_vnodeops(mp, &cgroupfs_vnode_vops, &mp->mnt_vn_norm_ops);
	spin_uninit(&kmp->vnode_spin);
	lockuninit(&kmp->hierarchy_lock);
	kfree(kmp, M_CGROUPFS);
	mp->mnt_data = NULL;
	return (0);
}

static int
cgroupfs_root(struct mount *mp, struct vnode **vpp)
{
	struct cgroupfs_mount *kmp = (struct cgroupfs_mount *)mp->mnt_data;

	return (cgroupfs_group_root_vnode(kmp->root, vpp));
}

static int
cgroupfs_statfs(struct mount *mp, struct statfs *sbp, struct ucred *cred)
{
	(void)cred;
	sbp->f_bsize = PAGE_SIZE;
	sbp->f_iosize = PAGE_SIZE;
	sbp->f_blocks = 1;
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = 1;
	sbp->f_ffree = 0;
	if (sbp != &mp->mnt_stat) {
		sbp->f_type = mp->mnt_vfc->vfc_typenum;
		bcopy(&mp->mnt_stat.f_fsid, &sbp->f_fsid, sizeof(sbp->f_fsid));
		bcopy(mp->mnt_stat.f_mntfromname, sbp->f_mntfromname,
		    MNAMELEN);
	}
	return (0);
}

static struct vfsops cgroupfs_vfsops = {
	.vfs_flags =	0,
	.vfs_mount =	cgroupfs_mount,
	.vfs_unmount =	cgroupfs_unmount,
	.vfs_root =	cgroupfs_root,
	.vfs_statfs =	cgroupfs_statfs,
};

VFS_SET(cgroupfs_vfsops, cgroupfs, VFCF_SYNTHETIC | VFCF_MPSAFE);
MODULE_VERSION(cgroupfs, 1);
