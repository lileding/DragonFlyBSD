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
#include "vmmfs_machine_id.h"

#define VMMFS_MACHINE_ID_MODE 0444

static volatile u_int vmmfs_machine_next_id;

static int vmmfs_machine_id_access(struct vop_access_args *);
static int vmmfs_machine_id_getattr(struct vop_getattr_args *);
static int vmmfs_machine_id_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_machine_id_open(struct vop_open_args *);
static int vmmfs_machine_id_read(struct vop_read_args *);
static int vmmfs_machine_id_inactive(struct vop_inactive_args *);
static int vmmfs_machine_id_reclaim(struct vop_reclaim_args *);
static void vmmfs_machine_id_drop(struct vmmfs_node *);

struct vop_ops vmmfs_machine_id_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_machine_id_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_machine_id_getattr,
	.vop_getattr_lite = vmmfs_machine_id_getattr_lite,
	.vop_open = vmmfs_machine_id_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_machine_id_read,
	.vop_inactive = vmmfs_machine_id_inactive,
	.vop_reclaim = vmmfs_machine_id_reclaim,
};

int
vmmfs_machine_id_init(struct vmmfs_machine *machine,
	struct vmmfs_machine_id *identity)
{
	struct vmmfs_mount *state;
	u_int value;
	int error;

	if (machine == NULL || identity == NULL)
		return (EINVAL);
	bzero(identity, sizeof(*identity));
	state = (struct vmmfs_mount *)vmmfs_machine_root(machine)->mount->mnt_data;
	if (state->machine_id_vops == NULL) {
		error = ENXIO;
		goto fail;
	}
	value = atomic_fetchadd_int(&vmmfs_machine_next_id, 1) + 1;
	if (value > VMMFS_MACHINE_ID_MAX) {
		error = ENOSPC;
		goto fail;
	}
	vmmfs_node_setup(&identity->node, &machine->branch.node, vmmfs_machine_id_drop, NULL);
	identity->inode = atomic_fetchadd_int(&state->next_inode, 1);
	machine->id = value;
	return (0);

fail:
	identity->inode = 0;
	machine->id = 0;
	return (error);
}

static void
vmmfs_machine_id_drop(struct vmmfs_node *node)
{
	struct vmmfs_machine_id *identity;

	identity = (struct vmmfs_machine_id *)node;
	KKASSERT(identity != NULL);
	if (identity->node.vnode != NULL)
		panic("vmmfs_machine_id_drop: vnode is still published");
	identity->inode = 0;
}

int
vmmfs_machine_id_publish(struct vmmfs_machine_id *identity)
{
	struct vmmfs_mount *state;

	if (identity == NULL || vmmfs_machine_id_machine(identity) == NULL)
		return (EINVAL);
	state = (struct vmmfs_mount *)vmmfs_machine_root(vmmfs_machine_id_machine(identity))->mount->mnt_data;
	if (state == NULL || state->machine_id_vops == NULL)
		return (ENXIO);
	return (vmmfs_node_publish_regular(&identity->node,
	    vmmfs_machine_root(vmmfs_machine_id_machine(identity))->mount, &state->machine_id_vops, VREG,
	    identity));
}


static int
vmmfs_machine_id_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_MACHINE_ID_MODE, 0));
}

static int
vmmfs_machine_id_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_machine_id *identity;
	struct vattr *vattr;

	identity = ap->a_vp->v_data;
	if (identity == NULL || vmmfs_machine_is_dead(vmmfs_machine_id_machine(identity)))
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_MACHINE_ID_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = identity->inode;
	vattr->va_size = sizeof("999\n") - 1;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = sizeof("999\n") - 1;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

static int
vmmfs_machine_id_getattr_lite(struct vop_getattr_lite_args *ap)
{
	struct vmmfs_machine_id *identity;
	struct vattr_lite *vattr;

	identity = ap->a_vp->v_data;
	if (identity == NULL || vmmfs_machine_is_dead(vmmfs_machine_id_machine(identity)))
		return (ENOENT);
	vattr = ap->a_lvap;
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_MACHINE_ID_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_size = sizeof("999\n") - 1;
	vattr->va_flags = 0;
	return (0);
}

static int
vmmfs_machine_id_open(struct vop_open_args *ap)
{
	struct vmmfs_machine_id *identity;

	identity = ap->a_vp->v_data;
	if (identity == NULL || vmmfs_machine_is_dead(vmmfs_machine_id_machine(identity)))
		return (ENOENT);
	return (vop_stdopen(ap));
}

static int
vmmfs_machine_id_read(struct vop_read_args *ap)
{
	struct vmmfs_machine_id *identity;
	char text[sizeof("999999\n")];
	off_t offset;
	int error;

	identity = ap->a_vp->v_data;
	if (identity == NULL || vmmfs_machine_is_dead(vmmfs_machine_id_machine(identity)))
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

static int
vmmfs_machine_id_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_machine_id *identity;
	struct vmmfs_machine *machine;

	identity = ap->a_vp->v_data;
	if (identity == NULL)
		return (0);
	machine = vmmfs_machine_id_machine(identity);
	if (!vmmfs_machine_is_dead(machine))
		return (0);
	vmmfs_node_inactive(&identity->node, ap->a_vp);
	return (0);
}

static int
vmmfs_machine_id_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_machine_id *identity;
	struct vmmfs_machine *machine;
	bool reclaim;

	identity = ap->a_vp->v_data;
	if (identity == NULL || vmmfs_machine_id_machine(identity) == NULL)
		return (0);
	machine = vmmfs_machine_id_machine(identity);
	lwkt_gettoken(&machine->token);
	reclaim = vmmfs_node_reclaim(&identity->node, ap->a_vp);
	lwkt_reltoken(&machine->token);
	if (reclaim)
		vmmfs_node_drop(&identity->node);
	return (0);
}
