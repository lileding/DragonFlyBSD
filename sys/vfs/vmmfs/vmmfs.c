/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs filesystem module entry point.
 *
 * The active filesystem implementation will be rebuilt here incrementally.
 * The previous implementation remains under legacy/ and is intentionally
 * excluded from this module build.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/mount.h>
#include <sys/module.h>
#include <sys/stat.h>
#include <sys/vnode.h>

static int
vmmfs_mount(struct mount *mount, char *path, caddr_t data,
    struct ucred *cred)
{

	(void)mount;
	(void)path;
	(void)data;
	(void)cred;
	return EOPNOTSUPP;
}

static int
vmmfs_unmount(struct mount *mount, int flags)
{

	(void)mount;
	(void)flags;
	return EOPNOTSUPP;
}

static int
vmmfs_root(struct mount *mount, struct vnode **vnode)
{

	(void)mount;
	(void)vnode;
	return EOPNOTSUPP;
}

static int
vmmfs_statfs(struct mount *mount, struct statfs *statfs,
    struct ucred *cred)
{

	(void)mount;
	(void)statfs;
	(void)cred;
	return EOPNOTSUPP;
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
