/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs stopped declaration node.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_stopped.h"
#include "vmmfs_root.h"
#include "vmmfs_parent.h"
#include "vmmfs_node.h"
#include "vmmfs_machine.h"

#define VMMFS_STOPPED_MODE 0644

static void vmmfs_stopped_drop(struct vmmfs_node *);
static int vmmfs_stopped_setattr(struct vop_setattr_args *);

static int
vmmfs_stopped_deactivate(struct vmmfs_node *node)
{
	(void)node;
	return (0);
}

struct vop_ops vmmfs_stopped_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_node_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_setattr = vmmfs_stopped_setattr,
	.vop_write = vmmfs_node_write,
};

static int
vmmfs_stopped_setattr(struct vop_setattr_args *ap)
{
	struct vmmfs_node *node = ap->a_vp->v_data;
	struct vmmfs_machine *machine = (struct vmmfs_machine *)node->parent;

	/* The request can consume this stopped vnode: do not lock the child. */
	return (VMMFS_WORK(machine,
	    vmmfs_machine_request_stop(machine, "external")));
}

int
vmmfs_stopped_create(struct vmmfs_node *parent,
	struct vnode **vnodep)
{
	struct vmmfs_root *root;
	struct vmmfs_stopped *stopped;
	int error;

	if (parent == NULL || vnodep == NULL)
		return (ENXIO);
	root = parent->mount->root_vnode->v_data;
	*vnodep = NULL;
	stopped = kmalloc(sizeof(*stopped), M_VMMFS, M_WAITOK | M_ZERO);
	stopped->node.parent = parent;
	stopped->node.mount = parent->mount;
	stopped->node.dead = false;
	stopped->node.references = 1;
	lwkt_token_init(&stopped->node.token, "vmmfsnode");
	lockinit(&stopped->node.lock, "vmmfsnode", 0, 0);
	stopped->node.deactivate = vmmfs_stopped_deactivate;
	stopped->node.drop = vmmfs_stopped_drop;
	vmmfs_node_hold(parent);
	stopped->node.load_limit = 0;
	stopped->node.store_limit = 0;
	stopped->node.load = NULL;
	stopped->node.store = NULL;
	stopped->node.inode = vmmfs_root_allocate_inode(root);
	stopped->node.mode = VMMFS_STOPPED_MODE;
	stopped->node.size = 0;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->stopped_vops, VREG, &stopped->node, vnodep);
	if (error != 0) {
		vmmfs_node_put(&stopped->node);
		return (error);
	}
	return (0);
}

static void
vmmfs_stopped_drop(struct vmmfs_node *node)
{
	struct vmmfs_stopped *stopped;

	stopped = (struct vmmfs_stopped *)node;
	KKASSERT(stopped != NULL);

	kfree(stopped, M_VMMFS);
}
