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

void
vmmfs_node_setup(struct vmmfs_node *node, struct vmmfs_node *parent,
	void (*drop)(struct vmmfs_node *),
	bool (*is_dead)(struct vmmfs_node *))
{
	KKASSERT(node != NULL);
	KKASSERT(drop != NULL);
	node->vnode = NULL;
	node->parent = parent;
	node->drop = drop;
	node->is_dead = is_dead;
}

void
vmmfs_node_drop(struct vmmfs_node *node)
{
	void (*drop)(struct vmmfs_node *);

	KKASSERT(node != NULL);
	drop = node->drop;
	KKASSERT(drop != NULL);
	node->drop = NULL;
	drop(node);
}

struct vmmfs_node *
vmmfs_node_detach_parent(struct vmmfs_node *node)
{
	struct vmmfs_node *parent;

	KKASSERT(node != NULL);
	parent = node->parent;
	KKASSERT(parent != NULL);
	node->parent = NULL;
	return (parent);
}

bool
vmmfs_node_is_dead(struct vmmfs_node *node)
{
	if (node == NULL)
		return (true);
	if (node->is_dead != NULL && node->is_dead(node))
		return (true);
	return (node->parent != NULL && vmmfs_node_is_dead(node->parent));
}

int
vmmfs_node_publish_regular(struct vmmfs_node *node, struct mount *mount,
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
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_node_publish_cdev(struct vmmfs_node *node, struct mount *mount,
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
	return (0);
}


void
vmmfs_node_abort(struct vmmfs_node *node)
{
	struct vnode *vnode;

	if (node == NULL || node->vnode == NULL)
		return;
	vnode = node->vnode;
	node->vnode = NULL;
	vx_get(vnode);
	vnode->v_data = NULL;
	vnode->v_type = VBAD;
	vx_put(vnode);
	/* Drop the base reference that publish retained. */
	vrele(vnode);
}

void
vmmfs_node_unpublish(struct vmmfs_node *node)
{
	struct vnode *vnode;

	KKASSERT(node != NULL);
	KKASSERT(node->vnode != NULL);
	vnode = node->vnode;
	node->vnode = NULL;
	cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	vfinalize(vnode);
	/* Drop the base reference that keeps a published object alive. */
	vrele(vnode);
}

void
vmmfs_node_release_transient(struct vmmfs_node *node)
{
	struct vnode *vnode;

	if (node == NULL || node->vnode == NULL)
		return;
	vnode = node->vnode;
	node->vnode = NULL;
	vrele(vnode);
}


void
vmmfs_node_inactive(struct vmmfs_node *node, struct vnode *vnode)
{
	if (node != NULL && vnode != NULL && vnode->v_data != NULL &&
	    node->drop != NULL)
		(void)vrecycle(vnode);
}

bool
vmmfs_node_reclaim(struct vmmfs_node *node, struct vnode *vnode)
{
	if (node == NULL || vnode == NULL || vnode->v_data == NULL ||
	    node->drop == NULL)
		return (false);
	vnode->v_data = NULL;
	return (true);
}
