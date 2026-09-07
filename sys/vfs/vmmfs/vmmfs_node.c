/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Thin VFS ownership helper for one vmmfs object vnode.
 */
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/namecache.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_node.h"

void
vmmfs_node_hold(struct vmmfs_node *node)
{
	KKASSERT(node != NULL);
	KKASSERT(atomic_load_acq_int(&node->references) != 0);
	atomic_add_int(&node->references, 1);
}



off_t
vmmfs_node_decimal_size(uint64_t value)
{
	off_t size;

	size = 2; /* one digit plus the trailing newline */
	while (value >= 10) {
		value /= 10;
		++size;
	}
	return (size);
}

int
vmmfs_node_access(struct vop_access_args *ap)
{
	struct vmmfs_node *node;
	int error;

	if (ap == NULL)
		return (EINVAL);
	node = ap->a_vp->v_data;
	if (node == NULL)
		return (ENOENT);
	error = vop_helper_access(ap, 0, 0, node->mode, 0);
	return (error);
}

int
vmmfs_node_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_node *node;
	struct vattr *vattr;

	if (ap == NULL)
		return (EINVAL);
	node = ap->a_vp->v_data;
	if (node == NULL)
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = ap->a_vp->v_type;
	vattr->va_mode = node->mode;
	vattr->va_nlink = ap->a_vp->v_type == VDIR ? 2 : 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = node->inode;
	vattr->va_size = node->size;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = node->size;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

int
vmmfs_node_getattr_lite(struct vop_getattr_lite_args *ap)
{
	struct vmmfs_node *node;
	struct vattr_lite *vattr;

	if (ap == NULL)
		return (EINVAL);
	node = ap->a_vp->v_data;
	if (node == NULL)
		return (ENOENT);
	vattr = ap->a_lvap;
	vattr->va_type = ap->a_vp->v_type;
	vattr->va_mode = node->mode;
	vattr->va_nlink = ap->a_vp->v_type == VDIR ? 2 : 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_size = node->size;
	vattr->va_flags = 0;
	return (0);
}

int
vmmfs_node_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_node *node;

	if (ap == NULL)
		return (0);
	node = ap->a_vp->v_data;
	if (node != NULL && node->dead && node->drop != NULL)
		(void)vrecycle(ap->a_vp);
	return (0);
}

int
vmmfs_node_open(struct vop_open_args *ap)
{
	struct vmmfs_node *node;
	int error;

	if (ap == NULL)
		return (EINVAL);
	node = ap->a_vp->v_data;
	if (node == NULL)
		return (ENOENT);
	error = vop_stdopen(ap);
	return (error);
}

int
vmmfs_node_read(struct vop_read_args *ap)
{
	struct vmmfs_node *node;
	char *buffer;
	size_t length;
	off_t offset;
	int error;

	if (ap == NULL)
		return (EINVAL);
	node = ap->a_vp->v_data;
	if (node == NULL)
		return (ENOENT);
	if (ap->a_uio->uio_offset < 0)
		return (EINVAL);
	if (node->load == NULL)
		return (0);
	KKASSERT(node->load_limit != 0);
	buffer = kmalloc(node->load_limit, M_VMMFS, M_WAITOK);
	error = VMMFS_CALL(node, load, buffer, node->load_limit, &length);
	if (error == 0 && length > node->load_limit)
		error = EOVERFLOW;
	if (error == 0) {
		offset = ap->a_uio->uio_offset;
		if ((size_t)offset < length)
			error = uiomove(buffer + offset, length - (size_t)offset,
			    ap->a_uio);
	}
	kfree(buffer, M_VMMFS);
	return (error);
}

int
vmmfs_node_setattr(struct vop_setattr_args *ap)
{
	struct vmmfs_node *node;
	struct vattr *vattr;
	int error;

	if (ap == NULL)
		return (EINVAL);
	node = ap->a_vp->v_data;
	if (node == NULL)
		return (ENOENT);
	vattr = ap->a_vap;
	if (vattr->va_mode != (mode_t)VNOVAL ||
	    vattr->va_uid != (uid_t)VNOVAL ||
	    vattr->va_gid != (gid_t)VNOVAL ||
	    vattr->va_flags != VNOVAL ||
	    vattr->va_atime.tv_sec != VNOVAL ||
	    vattr->va_mtime.tv_sec != VNOVAL)
		error = EOPNOTSUPP;
	else if (vattr->va_size == (off_t)VNOVAL)
		error = 0;
	else if (node->store == NULL)
		error = EROFS;
	else
		/* O_TRUNC prepares a control write; only store commits data. */
		error = vattr->va_size == 0 ? 0 : EINVAL;
	return (error);
}

int
vmmfs_node_write(struct vop_write_args *ap)
{
	struct vmmfs_node *node;
	char *buffer;
	size_t length;
	int error;

	if (ap == NULL)
		return (EINVAL);
	node = ap->a_vp->v_data;
	if (node == NULL)
		return (ENOENT);
	if (node->store == NULL)
		return (EROFS);
	if (ap->a_uio->uio_offset != 0 ||
	    (size_t)ap->a_uio->uio_resid > node->store_limit)
		return (EINVAL);
	length = (size_t)ap->a_uio->uio_resid;
	buffer = length == 0 ? NULL : kmalloc(length, M_VMMFS, M_WAITOK);
	if (length != 0) {
		error = uiomove(buffer, length, ap->a_uio);
		if (error != 0) {
			kfree(buffer, M_VMMFS);
			return (error);
		}
	}
	error = VMMFS_CALL(node, store, buffer, length);
	if (buffer != NULL)
		kfree(buffer, M_VMMFS);
	return (error);
}

void
vmmfs_node_put(struct vmmfs_node *node)
{
	struct vmmfs_node *parent;
	void (*drop)(struct vmmfs_node *);

	KKASSERT(node != NULL);
	KKASSERT(atomic_load_acq_int(&node->references) != 0);
	if (atomic_fetchadd_int(&node->references, -1) != 1)
		return;
	parent = node->parent;
	drop = node->drop;
	KKASSERT(drop != NULL);
	node->drop = NULL;
	lockuninit(&node->lock);
	lwkt_token_uninit(&node->token);
	drop(node);
	if (parent != NULL)
		vmmfs_node_put(parent);
}



static int
vmmfs_node_get_vnode(struct vmmfs_node *node, struct vnode **vnodep)
{
	struct vnode *vnode = node->vnode;

	if (vnode == NULL)
		return (ENOENT);
	vhold(vnode);
	*vnodep = vnode;
	return (0);
}

int
vmmfs_node_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_node *node = ap->a_dvp->v_data;
	struct vmmfs_node *parent = node->parent;
	struct vnode *vnode;
	int error;

	error = VMMFS_WORK(parent, vmmfs_node_get_vnode(parent, &vnode));
	if (error != 0)
		return (error);
	error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
	vdrop(vnode);
	if (error != 0)
		return (error);
	*ap->a_vpp = vnode;
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_vnode_create_regular(struct mount *mount, struct vop_ops **vops,
	enum vtype type, struct vmmfs_node *node, struct vnode **vnodep)
{
	struct vnode *vnode;
	int error;

	if (mount == NULL || vops == NULL || node == NULL || vnodep == NULL)
		return (EINVAL);
	error = getnewvnode(VT_SYNTH, mount, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = node;
	node->vnode = vnode;
	vnode->v_ops = vops;
	vnode->v_type = type;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	*vnodep = vnode;
	return (0);
}

int
vmmfs_vnode_create_cdev(struct mount *mount, struct vop_ops **vops,
	struct cdev *dev, struct vmmfs_node *node, struct vnode **vnodep)
{
	struct vnode *vnode;
	int error;

	if (mount == NULL || vops == NULL || dev == NULL || node == NULL ||
	    vnodep == NULL)
		return (EINVAL);
	error = getspecialvnode(VT_SYNTH, mount, vops, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = node;
	vnode->v_ops = vops;
	vnode->v_type = VCHR;
	error = v_associate_rdev(vnode, dev);
	if (error != 0) {
		vnode->v_data = NULL;
		vnode->v_type = VBAD;
		vx_downgrade(vnode);
		vn_unlock(vnode);
		vrele(vnode);
		return (error);
	}
	vnode->v_umajor = dev->si_umajor;
	vnode->v_uminor = dev->si_uminor;
	node->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	*vnodep = vnode;
	return (0);
}

void
vmmfs_vnode_discard(struct vnode *vnode)
{
	struct vmmfs_node *node;

	if (vnode == NULL)
		return;
	vx_get(vnode);
	node = vnode->v_data;
	(void)lockmgr(&node->lock, LK_EXCLUSIVE);
	lwkt_gettoken(&node->token);
	node->vnode = NULL;
	vnode->v_data = NULL;
	lwkt_reltoken(&node->token);
	(void)lockmgr(&node->lock, LK_RELEASE);
	vnode->v_type = VBAD;
	vx_put(vnode);
	vrele(vnode);
}

int
vmmfs_vnode_deactivate(struct vnode *vnode)
{
	struct vmmfs_node *node;
	int error;

	if (vnode == NULL)
		return (0);
	node = vnode->v_data;
	vref(vnode);
	error = lockmgr(&node->lock, LK_EXCLUSIVE);
	if (error != 0) {
		vrele(vnode);
		return (error);
	}
	lwkt_gettoken(&node->token);
	if (node->dead) {
		lwkt_reltoken(&node->token);
		(void)lockmgr(&node->lock, LK_RELEASE);
		vrele(vnode);
		/* A closed gate does not prove that the first callback completed. */
		return (EBUSY);
	}
	node->dead = true;
	lwkt_reltoken(&node->token);
	(void)lockmgr(&node->lock, LK_RELEASE);

	error = node->deactivate != NULL ? node->deactivate(node) : 0;
	if (error != 0) {
		/* Reopening cannot overlap a shared work invocation. */
		(void)lockmgr(&node->lock, LK_EXCLUSIVE);
		lwkt_gettoken(&node->token);
		node->dead = false;
		lwkt_reltoken(&node->token);
		(void)lockmgr(&node->lock, LK_RELEASE);
	} else {
		(void)fdrevoke(vnode, DTYPE_VNODE, proc0.p_ucred);
		cache_inval_vp(vnode, CINV_CHILDREN);
	}
	vrele(vnode);
	return (error);
}



int
vmmfs_node_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vnode;
	struct vmmfs_node *node;

	vnode = ap->a_vp;
	node = vnode->v_data;
	if (node != NULL) {
		(void)lockmgr(&node->lock, LK_EXCLUSIVE);
		lwkt_gettoken(&node->token);
		node->vnode = NULL;
		vnode->v_data = NULL;
		lwkt_reltoken(&node->token);
		(void)lockmgr(&node->lock, LK_RELEASE);
		vmmfs_node_put(node);
	}
	return (0);
}
