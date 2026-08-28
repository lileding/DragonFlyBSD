/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Thin VFS ownership helper for one vmmfs object vnode.
 */
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include "vmmfs_node.h"

int
vmmfs_node_init(struct vmmfs_node *node, struct mount *mount,
	struct vop_ops **vops, enum vtype type, void *data)
{
	struct vnode *vnode;
	int error;

	if (node == NULL || mount == NULL || vops == NULL || data == NULL)
		return (EINVAL);
	if (node->vnode != NULL)
		return (EBUSY);
	error = getnewvnode(VT_SYNTH, mount, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = data;
	vnode->v_ops = vops;
	vnode->v_type = type;
	node->vnode = vnode;
	node->published = false;
	node->base_reference = true;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_node_init_cdev(struct vmmfs_node *node, struct mount *mount,
	struct vop_ops **vops, struct cdev *dev, void *data)
{
	struct vnode *vnode;
	int error;

	if (node == NULL || mount == NULL || vops == NULL || dev == NULL ||
	    data == NULL)
		return (EINVAL);
	if (node->vnode != NULL)
		return (EBUSY);
	error = getspecialvnode(VT_SYNTH, mount, vops, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = data;
	vnode->v_ops = vops;
	vnode->v_type = VCHR;
	node->vnode = vnode;
	node->published = false;
	node->base_reference = true;
	error = v_associate_rdev(vnode, dev);
	if (error != 0) {
		vx_downgrade(vnode);
		vn_unlock(vnode);
		vmmfs_node_abort(node);
		return (error);
	}
	vnode->v_umajor = dev->si_umajor;
	vnode->v_uminor = dev->si_uminor;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

void
vmmfs_node_abort(struct vmmfs_node *node)
{
	struct vnode *vnode;

	if (node == NULL || node->vnode == NULL)
		return;
	vnode = node->vnode;
	KKASSERT(!node->published);
	KKASSERT(node->base_reference);
	node->vnode = NULL;
	node->published = false;
	node->base_reference = false;
	vx_get(vnode);
	vnode->v_data = NULL;
	vnode->v_type = VBAD;
	vx_put(vnode);
	/* Drop the base reference that vmmfs_node_init() retained. */
	vrele(vnode);
}

void
vmmfs_node_publish(struct vmmfs_node *node)
{

	KKASSERT(node != NULL);
	KKASSERT(node->vnode != NULL);
	KKASSERT(!node->published);
	KKASSERT(node->base_reference);
	node->published = true;
}

void
vmmfs_node_unpublish(struct vmmfs_node *node)
{
	struct vnode *vnode;

	if (node == NULL || node->vnode == NULL || !node->base_reference)
		return;
	if (!node->published)
		return;
	vnode = node->vnode;
	node->published = false;
	node->base_reference = false;
	cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	vfinalize(vnode);
	/* Drop the base reference that keeps a published object alive. */
	vrele(vnode);
}

void
vmmfs_node_release_transient(struct vmmfs_node *node)
{
	struct vnode *vnode;

	if (node == NULL || node->vnode == NULL || !node->base_reference)
		return;
	if (node->published)
		return;
	vnode = node->vnode;
	node->published = false;
	node->base_reference = false;
	vrele(vnode);
}

bool
vmmfs_node_is_published(const struct vmmfs_node *node)
{

	return (node != NULL && node->published);
}

void
vmmfs_node_inactive(struct vmmfs_node *node, struct vnode *vnode)
{
	if (node != NULL && node->vnode == vnode && !node->published &&
	    !node->base_reference)
		(void)vrecycle(vnode);
}

bool
vmmfs_node_reclaim(struct vmmfs_node *node, struct vnode *vnode)
{
	if (node == NULL || node->vnode != vnode)
		return (false);
	node->vnode = NULL;
	vnode->v_data = NULL;
	node->published = false;
	node->base_reference = false;
	return (true);
}
