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
#include "vmmfs_machine_id.h"

#define VMMFS_MACHINE_ID_MODE 0444

static volatile u_int vmmfs_machine_next_id;

static int vmmfs_machine_id_read(struct vop_read_args *);
static void vmmfs_machine_id_drop(struct vmmfs_node *);

struct vop_ops vmmfs_machine_id_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_machine_id_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

int
vmmfs_machine_id_init(struct vmmfs_mount *mount, struct vmmfs_branch *parent,
	struct vmmfs_machine_id *identity, struct vnode **vnodep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	u_int value;
	int error;

	if (mount == NULL || parent == NULL || identity == NULL || vnodep == NULL)
		return (EINVAL);
	machine = (struct vmmfs_machine *)parent;
	root = mount->root_vnode == NULL ? NULL : mount->root_vnode->v_data;
	if (root == NULL)
		return (ENXIO);
	*vnodep = NULL;
	bzero(identity, sizeof(*identity));
	if (mount->machine_id_vops == NULL)
		return (ENXIO);
	value = atomic_fetchadd_int(&vmmfs_machine_next_id, 1) + 1;
	if (value > VMMFS_MACHINE_ID_MAX)
		return (ENOSPC);
	vmmfs_node_setup(&identity->node, parent, vmmfs_machine_id_drop);
	identity->inode = vmmfs_root_allocate_inode(root);
	machine->id = value;
	vmmfs_node_set_metadata(&identity->node, identity->inode,
	    VMMFS_MACHINE_ID_MODE, vmmfs_node_decimal_size(value));
	error = vmmfs_vnode_create_regular(mount->mount,
	    &mount->machine_id_vops, VREG, &identity->node, vnodep);
	if (error == 0)
		return (0);
	identity->inode = 0;
	machine->id = 0;
	vmmfs_machine_id_drop(&identity->node);
	return (error);
}

static void
vmmfs_machine_id_drop(struct vmmfs_node *node)
{
	struct vmmfs_machine_id *identity;

	identity = (struct vmmfs_machine_id *)node;
	KKASSERT(identity != NULL);
	identity->inode = 0;
	vmmfs_node_parent_put(node);
}

static int
vmmfs_machine_id_read(struct vop_read_args *ap)
{
	struct vmmfs_machine_id *identity;
	char text[sizeof("999999\n")];
	off_t offset;
	int error;

	identity = ap->a_vp->v_data;
	if (identity == NULL || identity->node.dead)
		return (ENOENT);
	if (ap->a_uio->uio_offset < 0)
		return (EINVAL);
	offset = ap->a_uio->uio_offset;
	error = ksnprintf(text, sizeof(text), "%06u\n", vmmfs_machine_id_machine(identity)->id);
	if (error < 0 || (size_t)error >= sizeof(text))
		return (EOVERFLOW);
	if (offset >= error)
		return (0);
	return (uiomove(text + (size_t)offset,
	    (size_t)error - (size_t)offset, ap->a_uio));
}
