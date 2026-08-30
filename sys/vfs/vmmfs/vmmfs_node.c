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
#include <sys/namecache.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include "vmmfs_node.h"
#include "vmmfs_branch.h"

void
vmmfs_node_setup(struct vmmfs_node *node, struct vmmfs_branch *parent,
	void (*drop)(struct vmmfs_node *))
{
	KKASSERT(node != NULL);
	KKASSERT(drop != NULL);
	node->parent = parent;
	node->dead = false;
	node->deactivate = vmmfs_node_default_deactivate;
	node->drop = drop;
	if (parent != NULL)
		vmmfs_branch_hold(parent);
}

void
vmmfs_node_parent_put(struct vmmfs_node *node)
{
	struct vmmfs_branch *parent;

	KKASSERT(node != NULL);
	parent = node->parent;
	node->parent = NULL;
	if (parent != NULL)
		vmmfs_branch_put(parent);
}

void
vmmfs_node_default_deactivate(struct vmmfs_node *node)
{
	KKASSERT(node != NULL);
	node->dead = true;
}

int
vmmfs_node_open(struct vop_open_args *ap)
{
	struct vmmfs_node *node;

	if (ap == NULL || ap->a_vp == NULL)
		return (EINVAL);
	node = ap->a_vp->v_data;
	if (node == NULL || node->dead)
		return (ENOENT);
	return (vop_stdopen(ap));
}

void
vmmfs_node_deactivate(struct vmmfs_node *node)
{
	if (node == NULL || node->deactivate == NULL)
		return;
	node->deactivate(node);
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
	vx_downgrade(vnode);
	vn_unlock(vnode);
	*vnodep = vnode;
	return (0);
}

void
vmmfs_vnode_discard(struct vnode *vnode)
{
	if (vnode == NULL)
		return;
	vx_get(vnode);
	vnode->v_data = NULL;
	vnode->v_type = VBAD;
	vx_put(vnode);
	vrele(vnode);
}

void
vmmfs_vnode_deactivate(struct vnode *vnode)
{
	if (vnode == NULL)
		return;
	(void)fdrevoke(vnode, DTYPE_VNODE, proc0.p_ucred);
	cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	vfinalize(vnode);
	vrele(vnode);
}



void
vmmfs_node_inactive(struct vmmfs_node *node, struct vnode *vnode)
{
	if (node != NULL && vnode != NULL && vnode->v_data != NULL &&
	    node->drop != NULL)
		(void)vrecycle(vnode);
}

int
vmmfs_node_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vnode;
	struct vmmfs_node *node;

	vnode = ap->a_vp;
	node = vnode->v_data;
	vnode->v_data = NULL;
	if (node != NULL)
		vmmfs_node_drop(node);
	return (0);
}
