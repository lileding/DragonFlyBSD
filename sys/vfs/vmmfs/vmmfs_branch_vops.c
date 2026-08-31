/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Common directory VOPs for VMMFS namespace branches.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/namecache.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs_branch.h"

static int
vmmfs_branch_vop_branch(struct vnode *vnode, struct vmmfs_branch **branchp)
{
	struct vmmfs_branch *branch;

	if (vnode == NULL || branchp == NULL)
		return (EINVAL);
	if (vnode->v_type != VDIR)
		return (ENOTDIR);
	branch = vnode->v_data;
	if (branch == NULL || branch->ops == NULL)
		return (ENXIO);
	*branchp = branch;
	return (0);
}

int
vmmfs_branch_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_branch *branch;
	struct vmmfs_branch_item item;
	struct uio *uio;
	off_t offset;
	ino_t parent_inode;
	uint64_t index;
	int error;
	int stop;

	if (ap == NULL)
		return (EINVAL);
	error = vmmfs_branch_vop_branch(ap->a_vp, &branch);
	if (error != 0)
		return (error);
	if (branch->ops->read_item == NULL)
		return (EOPNOTSUPP);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	parent_inode = branch->node.parent == NULL ? branch->node.inode :
	    branch->node.parent->node.inode;
	offset = uio->uio_offset;
	error = 0;
	stop = 0;
	if (offset == 0) {
		stop = vop_write_dirent(&error, uio, branch->node.inode, DT_DIR,
		    1, ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, parent_inode, DT_DIR, 2,
		    "..");
		if (!stop)
			offset = 2;
	}
	index = offset - 2;
	while (!stop) {
		error = branch->ops->read_item(branch, index, &item);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		KKASSERT(item.vnode != NULL);
		vdrop(item.vnode);
		stop = vop_write_dirent(&error, uio, item.inode, DT_DIR,
		    (uint16_t)strlen(item.name), item.name);
		if (!stop) {
			offset++;
			index++;
		}
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop && error == 0;
	return (error);
}

int
vmmfs_branch_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_branch *branch;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	if (ap == NULL)
		return (EINVAL);
	error = vmmfs_branch_vop_branch(ap->a_dvp, &branch);
	if (error != 0)
		return (error);
	if (branch->ops->get_item == NULL)
		return (EOPNOTSUPP);
	ncp = ap->a_nch->ncp;
	error = branch->ops->get_item(branch, ncp->nc_name, ncp->nc_nlen,
	    &vnode);
	if (error != 0) {
		cache_setvp(ap->a_nch, NULL);
		return (error);
	}
	error = vget(vnode, LK_EXCLUSIVE);
	vdrop(vnode);
	if (error != 0)
		return (error);
	vn_unlock(vnode);
	cache_setvp(ap->a_nch, vnode);
	vrele(vnode);
	return (0);
}

int
vmmfs_branch_nmkdir(struct vop_nmkdir_args *ap)
{
	struct vmmfs_branch *branch;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	if (ap == NULL || ap->a_vap == NULL)
		return (EINVAL);
	error = vmmfs_branch_vop_branch(ap->a_dvp, &branch);
	if (error != 0)
		return (error);
	if (branch->ops->create_item == NULL || branch->ops->remove_item == NULL)
		return (EOPNOTSUPP);
	if (ap->a_vap->va_type != VDIR)
		return (EINVAL);
	ncp = ap->a_nch->ncp;
	error = branch->ops->create_item(branch, ap->a_dvp->v_mount,
	    ncp->nc_name, ncp->nc_nlen, &vnode);
	if (error != 0)
		return (error);
	if (vnode == NULL)
		return (ENOMEM);
	error = vget(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		if (vmmfs_vnode_deactivate(vnode) != 0)
			panic("vmmfs_branch_nmkdir: created child cannot deactivate");
		branch->ops->remove_item(branch, ncp->nc_name, ncp->nc_nlen);
		return (error);
	}
	*ap->a_vpp = vnode;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
	return (0);
}

int
vmmfs_branch_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vmmfs_branch *branch;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	if (ap == NULL)
		return (EINVAL);
	error = vmmfs_branch_vop_branch(ap->a_dvp, &branch);
	if (error != 0)
		return (error);
	if (branch->ops->remove_item == NULL)
		return (EOPNOTSUPP);
	ncp = ap->a_nch->ncp;
	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vnode);
	if (error != 0)
		return (error);
	vn_unlock(vnode);
	if (vnode->v_type != VDIR) {
		vrele(vnode);
		return (ENOTDIR);
	}
	error = vmmfs_vnode_deactivate(vnode);
	if (error == 0) {
		branch->ops->remove_item(branch, ncp->nc_name, ncp->nc_nlen);
		cache_unlink(ap->a_nch);
	}
	vrele(vnode);
	return (error);
}
