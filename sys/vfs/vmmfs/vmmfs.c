/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs filesystem module entry point.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include "vmmfs.h"

static int vmmfs_mount(struct mount *, char *, caddr_t, struct ucred *);
static int vmmfs_unmount(struct mount *, int);
static int vmmfs_statfs(struct mount *, struct statfs *, struct ucred *);
static int vmmfs_root_vfs(struct mount *, struct vnode **);

static struct vfsops vmmfs_vfsops = {
	.vfs_flags = 0,
	.vfs_mount = vmmfs_mount,
	.vfs_unmount = vmmfs_unmount,
	.vfs_root = vmmfs_root_vfs,
	.vfs_statfs = vmmfs_statfs,
};

static int
vmmfs_root_vfs(struct mount *mount, struct vnode **vnode)
{
	struct vmmfs_mount *state;
	struct vmmfs_root *root;
	struct vnode *vp;
	int error;

	state = (struct vmmfs_mount *)mount->mnt_data;
	if (state == NULL)
		return (ENXIO);
	root = state->root;
	if (root == NULL)
		return (ENXIO);

	lwkt_gettoken(&root->token);
	vp = root->vnode;
	lwkt_reltoken(&root->token);
	if (vp == NULL)
		return (ENOENT);
	vhold(vp);
	error = vget(vp, LK_EXCLUSIVE | LK_RETRY);
	vdrop(vp);
	if (error != 0)
		return (error);
	*vnode = vp;
	return (0);
}

static int
vmmfs_mount(struct mount *mount, char *path, caddr_t data,
	struct ucred *cred)
{
	struct vmmfs_mount *state;
	struct vmmfs_root *root;
	size_t size;
	int error;

	(void)data;
	if ((mount->mnt_flag & MNT_UPDATE) != 0)
		return (EOPNOTSUPP);

	state = kmalloc(sizeof(*state), M_VMMFS, M_WAITOK | M_ZERO);
	state->mount = mount;
	mount->mnt_flag |= MNT_LOCAL;
	mount->mnt_kern_flag |= MNTK_NOSTKMNT | MNTK_ALL_MPSAFE;
	mount->mnt_data = (qaddr_t)state;
	vfs_getnewfsid(mount);

	size = sizeof("vmmfs") - 1;
	bcopy("vmmfs", mount->mnt_stat.f_mntfromname, size);
	bzero(mount->mnt_stat.f_mntfromname + size, MNAMELEN - size);
	bzero(mount->mnt_stat.f_mntonname,
	    sizeof(mount->mnt_stat.f_mntonname));
	error = copyinstr(path, mount->mnt_stat.f_mntonname,
	    sizeof(mount->mnt_stat.f_mntonname) - 1, &size);
	if (error != 0) {
		mount->mnt_data = NULL;
		goto fail;
	}
	vfs_add_vnodeops(mount, &vmmfs_root_vops,
	    &mount->mnt_vn_norm_ops);
	state->root_vops = mount->mnt_vn_norm_ops;
	vfs_add_vnodeops(mount, &vmmfs_machine_vops,
	    &state->machine_vops);
	error = vmmfs_root_create(mount, &root);
	if (error != 0) {
		vfs_rm_vnodeops(mount, NULL, &state->machine_vops);
		vfs_rm_vnodeops(mount, NULL, &mount->mnt_vn_norm_ops);
		mount->mnt_data = NULL;
		goto fail;
	}
	state->root = root;
	return (vmmfs_statfs(mount, &mount->mnt_stat, cred));

fail:
	kfree(state, M_VMMFS);
	return (error);
}

static int
vmmfs_unmount(struct mount *mount, int flags)
{
	struct vmmfs_mount *state;
	struct vmmfs_root *root;
	struct vnode *root_vnode;
	int error;

	state = (struct vmmfs_mount *)mount->mnt_data;
	if (state == NULL)
		return (ENXIO);
	root = state->root;
	if (root == NULL)
		return (ENXIO);
	lwkt_gettoken(&root->token);
	if (!RB_EMPTY(&root->machines)) {
		lwkt_reltoken(&root->token);
		return (EBUSY);
	}
	root_vnode = root->vnode;
	root->vnode = NULL;
	lwkt_reltoken(&root->token);
	if (root_vnode != NULL)
		vrele(root_vnode);
	error = vflush(mount, 0, (flags & MNT_FORCE) ? FORCECLOSE : 0);
	if (error != 0)
		return (error);
	vfs_rm_vnodeops(mount, NULL, &state->machine_vops);
	state->root_vops = NULL;
	vfs_rm_vnodeops(mount, NULL, &mount->mnt_vn_norm_ops);
	mount->mnt_data = NULL;
	vmmfs_root_destroy(root);
	kfree(state, M_VMMFS);
	return (0);
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
		statfs->f_type = mount->mnt_vfc->vfc_typenum;
		bcopy(&mount->mnt_stat.f_fsid, &statfs->f_fsid,
		    sizeof(statfs->f_fsid));
		bcopy(mount->mnt_stat.f_mntfromname, statfs->f_mntfromname,
		    MNAMELEN);
	}
	return (0);
}

VFS_SET(vmmfs_vfsops, vmmfs, VFCF_SYNTHETIC | VFCF_MPSAFE);
MODULE_VERSION(vmmfs, 1);
