/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine identity node.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_machine_id.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"

#define VMMFS_MACHINE_ID_MODE 0444

static volatile u_int vmmfs_machine_next_id;

static int vmmfs_machine_id_load(struct vmmfs_node *, char *, size_t, size_t *);
static void vmmfs_machine_id_drop(struct vmmfs_node *);

static int
vmmfs_machine_id_deactivate(struct vmmfs_node *node)
{
	(void)node;
	return (0);
}

struct vop_ops vmmfs_machine_id_vops = {
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
	.vop_setattr = vmmfs_node_setattr,
	.vop_write = vmmfs_node_write,
};

int
vmmfs_machine_id_init(struct vmmfs_node *parent,
	struct vmmfs_machine_id *identity, struct vnode **vnodep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	u_int value;
	int error;

	if (parent == NULL || identity == NULL || vnodep == NULL)
		return (EINVAL);
	machine = (struct vmmfs_machine *)parent;
	root = parent->mount->root_vnode->v_data;
	*vnodep = NULL;
	bzero(identity, sizeof(*identity));
	value = atomic_fetchadd_int(&vmmfs_machine_next_id, 1) + 1;
	if (value > VMMFS_MACHINE_ID_MAX)
		return (ENOSPC);
	identity->node.parent = parent;
	identity->node.mount = parent->mount;
	identity->node.dead = false;
	identity->node.references = 1;
	lwkt_token_init(&identity->node.token, "vmmfsnode");
	identity->node.deactivate = vmmfs_machine_id_deactivate;
	identity->node.drop = vmmfs_machine_id_drop;
	vmmfs_node_hold(parent);
	identity->node.load_limit = sizeof("999999\n");
	identity->node.store_limit = 0;
	identity->node.load = vmmfs_machine_id_load;
	identity->node.store = NULL;
	identity->node.inode = vmmfs_root_allocate_inode(root);
	machine->id = value;
	identity->node.mode = VMMFS_MACHINE_ID_MODE;
	identity->node.size = vmmfs_node_decimal_size(value);
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->machine_id_vops, VREG, &identity->node, vnodep);
	if (error == 0)
		return (0);
	identity->node.inode = 0;
	machine->id = 0;
	vmmfs_node_put(&identity->node);
	return (error);
}

static void
vmmfs_machine_id_drop(struct vmmfs_node *node)
{
	struct vmmfs_machine_id *identity;

	identity = (struct vmmfs_machine_id *)node;
	KKASSERT(identity != NULL);
	identity->node.inode = 0;

}

static int
vmmfs_machine_id_load(struct vmmfs_node *node, char *buffer,
	size_t capacity, size_t *length)
{
	struct vmmfs_machine_id *identity;
	int error;

	identity = (struct vmmfs_machine_id *)node;
	if (identity == NULL || identity->node.dead)
		return (ENOENT);
	error = ksnprintf(buffer, capacity, "%06u\n",
	    vmmfs_machine_id_machine(identity)->id);
	if (error < 0 || (size_t)error >= capacity)
		return (EOVERFLOW);
	*length = (size_t)error;
	return (0);
}
