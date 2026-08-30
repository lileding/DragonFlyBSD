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

static int vmmfs_stopped_access(struct vop_access_args *);
static int vmmfs_stopped_getattr(struct vop_getattr_args *);
static int vmmfs_stopped_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_stopped_read(struct vop_read_args *);
static int vmmfs_stopped_inactive(struct vop_inactive_args *);
static int vmmfs_stopped_setattr(struct vop_setattr_args *);
static int vmmfs_stopped_write(struct vop_write_args *);
static void vmmfs_stopped_drop(struct vmmfs_node *);

struct vop_ops vmmfs_stopped_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_stopped_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_stopped_getattr,
	.vop_getattr_lite = vmmfs_stopped_getattr_lite,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_stopped_read,
	.vop_inactive = vmmfs_stopped_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_setattr = vmmfs_stopped_setattr,
	.vop_write = vmmfs_stopped_write,
};

int
vmmfs_stopped_create(struct vmmfs_machine *machine,
	struct vmmfs_stopped **stoppedp, struct vnode **vnodep)
{
	struct vmmfs_mount *state;
	struct vmmfs_stopped *stopped;
	int error;

	if (machine == NULL || stoppedp == NULL || vnodep == NULL)
		return (EINVAL);
	*stoppedp = NULL;
	*vnodep = NULL;
	state = vmmfs_root_state(vmmfs_machine_root(machine));
	if (state->stopped_vops == NULL)
		return (ENXIO);
	stopped = kmalloc(sizeof(*stopped), M_VMMFS, M_WAITOK | M_ZERO);
	vmmfs_node_setup(&stopped->node, &machine->branch, vmmfs_stopped_drop);
	stopped->inode = atomic_fetchadd_int(&state->next_inode, 1);
	error = vmmfs_vnode_create_regular(state->mount,
	    &state->stopped_vops, VREG, &stopped->node, vnodep);
	if (error != 0) {
	vmmfs_node_drop(&stopped->node);
		return (error);
	}
	*stoppedp = stopped;
	return (0);
}

static void
vmmfs_stopped_drop(struct vmmfs_node *node)
{
	struct vmmfs_stopped *stopped;

	stopped = (struct vmmfs_stopped *)node;
	KKASSERT(stopped != NULL);
	vmmfs_node_parent_put(node);
	kfree(stopped, M_VMMFS);
}

static int
vmmfs_stopped_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_STOPPED_MODE, 0));
}

static int
vmmfs_stopped_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_stopped *stopped;
	struct vattr *vattr;

	stopped = ap->a_vp->v_data;
	if (stopped == NULL)
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_STOPPED_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = stopped->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

static int
vmmfs_stopped_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_STOPPED_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_stopped_read(struct vop_read_args *ap)
{
	if (ap->a_vp->v_data == NULL)
		return (ENOENT);
	if (ap->a_uio->uio_offset < 0)
		return (EINVAL);
	return (0);
}

static int
vmmfs_stopped_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_stopped *stopped;
	struct vmmfs_machine *machine;
	bool recycle;

	stopped = ap->a_vp->v_data;
	if (stopped == NULL)
		return (0);
	machine = vmmfs_stopped_machine(stopped);
	if (machine == NULL)
		return (0);
	lwkt_gettoken(&machine->branch.token);
	recycle = machine->branch.node.dead || stopped->node.dead;
	lwkt_reltoken(&machine->branch.token);
	if (!recycle)
		return (0);
	vmmfs_node_inactive(&stopped->node, ap->a_vp);
	return (0);
}


static int
vmmfs_stopped_setattr(struct vop_setattr_args *ap)
{
	(void)ap;
	return (0);
}

static int
vmmfs_stopped_write(struct vop_write_args *ap)
{
	(void)ap;
	return (EOPNOTSUPP);
}
