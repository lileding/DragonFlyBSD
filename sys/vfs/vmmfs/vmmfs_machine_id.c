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
	struct vnode *vnode;
	u_int value;
	int error;

	if (machine == NULL || identity == NULL)
		return (EINVAL);
	bzero(identity, sizeof(*identity));
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
	if (state->machine_id_vops == NULL)
		return (ENXIO);
	value = atomic_fetchadd_int(&vmmfs_machine_next_id, 1) + 1;
	if (value > VMMFS_MACHINE_ID_MAX)
		return (ENOSPC);
	identity->machine = machine;
	identity->inode = atomic_fetchadd_int(&state->next_inode, 1);
	machine->id = value;
	error = getnewvnode(VT_SYNTH, machine->root->mount, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = identity;
	vnode->v_ops = &state->machine_id_vops;
	vnode->v_type = VREG;
	identity->vnode = vnode;
	vmmfs_machine_hold(machine);
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_machine_id_fini(struct vmmfs_machine_id *identity)
{
	if (identity == NULL)
		return (EINVAL);
	if (identity->vnode != NULL)
		return (EBUSY);
	identity->machine = NULL;
	return (0);
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
	if (identity == NULL || vmmfs_machine_is_dead(identity->machine))
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
	if (identity == NULL || vmmfs_machine_is_dead(identity->machine))
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
	if (identity == NULL || vmmfs_machine_is_dead(identity->machine))
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
	if (identity == NULL || vmmfs_machine_is_dead(identity->machine))
		return (ENOENT);
	if (ap->a_uio->uio_offset < 0)
		return (EINVAL);
	offset = ap->a_uio->uio_offset;
	error = ksnprintf(text, sizeof(text), "%06u\n", identity->machine->id);
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
	machine = identity->machine;
	if (!vmmfs_machine_vnode_detach(machine, &identity->vnode, ap->a_vp))
		return (0);
	ap->a_vp->v_data = NULL;
	vmmfs_machine_put(machine);
	vrecycle(ap->a_vp);
	return (0);
}

static int
vmmfs_machine_id_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_machine_id *identity;
	struct vmmfs_machine *machine;

	identity = ap->a_vp->v_data;
	if (identity != NULL) {
		machine = identity->machine;
		if (identity->vnode == ap->a_vp)
			identity->vnode = NULL;
	} else {
		machine = NULL;
	}
	ap->a_vp->v_data = NULL;
	if (machine != NULL)
		vmmfs_machine_put(machine);
	return (0);
}
