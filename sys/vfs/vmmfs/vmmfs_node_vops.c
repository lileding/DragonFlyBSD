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

#include "vmmfs_node.h"

static int
vmmfs_node_vop_branch(struct vnode *vnode, struct vmmfs_node **branchp)
{
	struct vmmfs_node *node;

	if (vnode == NULL || branchp == NULL)
		return (EINVAL);
	if (vnode->v_type != VDIR)
		return (ENOTDIR);
	node = vnode->v_data;
	if (node == NULL)
		return (ENXIO);
	*branchp = node;
	return (0);
}

int
vmmfs_node_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_node *node;
	struct vmmfs_node_item item;
	struct uio *uio;
	off_t offset;
	ino_t parent_inode;
	uint64_t index;
	int error;
	int stop;

	if (ap == NULL)
		return (EINVAL);
	error = vmmfs_node_vop_branch(ap->a_vp, &node);
	if (error != 0)
		return (error);
	if (node->read_item == NULL)
		return (EOPNOTSUPP);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	parent_inode = node->parent == NULL ? node->inode :
	    node->parent->inode;
	offset = uio->uio_offset;
	error = 0;
	stop = 0;
	if (offset == 0) {
		stop = vop_write_dirent(&error, uio, node->inode, DT_DIR,
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
		error = VMMFS_CALL(node, read_item, index, &item);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		KKASSERT(item.vnode != NULL);
		vrele(item.vnode);
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
vmmfs_node_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_node *node;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	if (ap == NULL)
		return (EINVAL);
	error = vmmfs_node_vop_branch(ap->a_dvp, &node);
	if (error != 0)
		return (error);
	if (node->get_item == NULL)
		return (EOPNOTSUPP);
	ncp = ap->a_nch->ncp;
	error = VMMFS_CALL(node, get_item, ncp->nc_name, ncp->nc_nlen, &vnode);
	if (error != 0) {
		cache_setvp(ap->a_nch, NULL);
		return (error);
	}
	cache_setvp(ap->a_nch, vnode);
	vrele(vnode);
	return (0);
}

int
vmmfs_node_nmkdir(struct vop_nmkdir_args *ap)
{
	struct vmmfs_node *node;
	struct namecache *ncp;
	struct vnode *vnode;
	int error, cleanup_error;

	if (ap == NULL)
		return (EINVAL);
	error = vmmfs_node_vop_branch(ap->a_dvp, &node);
	if (error != 0)
		return (error);
	if (node->create_item == NULL || node->remove_item == NULL)
		return (EOPNOTSUPP);
	if (ap->a_vap->va_type != VDIR)
		return (EINVAL);
	ncp = ap->a_nch->ncp;
	error = VMMFS_CALL(node, create_item, ap->a_dvp->v_mount, ncp->nc_name, ncp->nc_nlen, &vnode);
	if (error != 0)
		return (error);
	if (vnode == NULL)
		return (ENOMEM);
	error = vn_lock(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		/*
		 * The child is already discoverable.  If another operation has
		 * made it busy, retain the registry entry for normal resolution.
		 */
		if (vmmfs_node_deactivate(vnode->v_data)) {
			cleanup_error = VMMFS_CALL(node, remove_item,
			    ncp->nc_name, ncp->nc_nlen);
			if (cleanup_error != 0 && cleanup_error != ENOENT)
				kprintf("vmmfs: mkdir rollback: %d\n", cleanup_error);
		} else {
			vrele(vnode); /* Veto retains the create_item reference. */
		}
		return (error);
	}
	*ap->a_vpp = vnode;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
	return (0);
}

int
vmmfs_node_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vmmfs_node *node;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	if (ap == NULL)
		return (EINVAL);
	error = vmmfs_node_vop_branch(ap->a_dvp, &node);
	if (error != 0)
		return (error);
	if (node->remove_item == NULL)
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
	if (!vmmfs_node_deactivate(vnode->v_data)) {
		vrele(vnode);
		return (EBUSY);
	}
	error = VMMFS_CALL(node, remove_item, ncp->nc_name, ncp->nc_nlen);
	if (error == 0)
		cache_unlink(ap->a_nch);
	return (error);
}
