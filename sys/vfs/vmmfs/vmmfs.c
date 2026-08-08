/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs filesystem module entry point.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/systm.h>

#include "vmmfs_machine.h"
#include "vmmfs_root.h"

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs mount structures");

static int vmmfs_statfs(struct mount *mount, struct statfs *statfs,
    struct ucred *cred);

static int
vmmfs_mount(struct mount *mount, char *path, caddr_t data,
    struct ucred *cred)
{
	size_t size;
	int error;

	(void)data;

	if ((mount->mnt_flag & MNT_UPDATE) != 0)
		return EOPNOTSUPP;

	vfs_add_vnodeops(mount, &vmmfs_root_vops, &mount->mnt_vn_norm_ops);
	error = vmmfs_root_mount(mount);
	if (error != 0) {
		vfs_rm_vnodeops(mount, NULL, &mount->mnt_vn_norm_ops);
		return error;
	}

	mount->mnt_flag |= MNT_LOCAL;
	mount->mnt_kern_flag |= MNTK_ALL_MPSAFE;
	mount->mnt_kern_flag |= MNTK_NOMSYNC;
	vfs_getnewfsid(mount);

	size = sizeof("vmmfs") - 1;
	bcopy("vmmfs", mount->mnt_stat.f_mntfromname, size);
	bzero(mount->mnt_stat.f_mntfromname + size,
	    MNAMELEN - size);
	bzero(mount->mnt_stat.f_mntonname,
	    sizeof(mount->mnt_stat.f_mntonname));
	error = copyinstr(path, mount->mnt_stat.f_mntonname,
	    sizeof(mount->mnt_stat.f_mntonname) - 1, &size);
	if (error != 0) {
		(void)vmmfs_root_unmount(mount, MNT_FORCE);
		vfs_rm_vnodeops(mount, NULL, &mount->mnt_vn_norm_ops);
		return error;
	}

	return vmmfs_statfs(mount, &mount->mnt_stat, cred);
}

static int
vmmfs_unmount(struct mount *mount, int flags)
{
	int error;

	error = vmmfs_root_unmount(mount, flags);
	if (error == 0)
		vfs_rm_vnodeops(mount, NULL, &mount->mnt_vn_norm_ops);
	return error;
}

static int
vmmfs_root(struct mount *mount, struct vnode **vnode)
{

	return vmmfs_root_vnode(mount, vnode);
}

static int
vmmfs_statfs(struct mount *mount, struct statfs *statfs,
    struct ucred *cred)
{

	(void)cred;
	statfs->f_bsize = PAGE_SIZE;
	statfs->f_iosize = PAGE_SIZE;
	statfs->f_blocks = 1;
	statfs->f_bfree = 0;
	statfs->f_bavail = 0;
	statfs->f_files = 1;
	statfs->f_ffree = 0;
	if (statfs != &mount->mnt_stat) {
		statfs->f_type = mount->mnt_stat.f_type;
		statfs->f_fsid = mount->mnt_stat.f_fsid;
		bcopy(mount->mnt_stat.f_mntfromname, statfs->f_mntfromname,
		    MNAMELEN);
	}
	return 0;
}

static struct vfsops vmmfs_vfsops = {
	.vfs_flags = 0,
	.vfs_mount = vmmfs_mount,
	.vfs_unmount = vmmfs_unmount,
	.vfs_root = vmmfs_root,
	.vfs_statfs = vmmfs_statfs,
};

VFS_SET(vmmfs_vfsops, vmmfs, VFCF_SYNTHETIC | VFCF_MPSAFE);
MODULE_VERSION(vmmfs, 1);
MODULE_DEPEND(vmmfs, vmm, 1, 1, 1);
