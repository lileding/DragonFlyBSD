/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot state information node.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_state.h"

#define VMMFS_PCISLOT_STATE_MODE 0444

static int vmmfs_pcislot_state_access(struct vop_access_args *);
static int vmmfs_pcislot_state_getattr(struct vop_getattr_args *);
static int vmmfs_pcislot_state_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_pcislot_state_open(struct vop_open_args *);
static int vmmfs_pcislot_state_read(struct vop_read_args *);
static int vmmfs_pcislot_state_reclaim(struct vop_reclaim_args *);
static int vmmfs_pcislot_state_load(struct vmmfs_pcislot_state *, char *,
	size_t, size_t *);

struct vop_ops vmmfs_pcislot_state_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_pcislot_state_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_pcislot_state_getattr,
	.vop_getattr_lite = vmmfs_pcislot_state_getattr_lite,
	.vop_open = vmmfs_pcislot_state_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_state_read,
	.vop_reclaim = vmmfs_pcislot_state_reclaim,
};

int
vmmfs_pcislot_state_create(struct vmmfs_pcislot *slot,
	struct vmmfs_pcislot_state *state_node)
{
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	if (slot == NULL || slot->pciroot == NULL ||
	    slot->pciroot->machine == NULL || state_node == NULL)
		return (EINVAL);
	state = (struct vmmfs_mount *)slot->pciroot->machine->root->mount->mnt_data;
	if (state->pcislot_state_vops == NULL)
		return (ENXIO);
	bzero(state_node, sizeof(*state_node));
	state_node->slot = slot;
	state_node->inode = atomic_fetchadd_int(&state->next_inode, 1);
	error = getnewvnode(VT_SYNTH, slot->pciroot->machine->root->mount,
	    &vnode, 0, 0);
	if (error != 0) {
		state_node->slot = NULL;
		return (error);
	}
	vnode->v_data = state_node;
	vnode->v_ops = &state->pcislot_state_vops;
	vnode->v_type = VREG;
	state_node->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_pcislot_state_destroy(struct vmmfs_pcislot_state *state_node)
{
	struct vnode *vnode;

	if (state_node == NULL)
		return (EINVAL);
	vnode = state_node->vnode;
	if (vnode != NULL) {
		(void)vrevoke(vnode, proc0.p_ucred);
		vx_get(vnode);
		vgone_vxlocked(vnode);
		vx_put(vnode);
		vrele(vnode);
	}
	KKASSERT(state_node->vnode == NULL);
	state_node->slot = NULL;
	return (0);
}

static int
vmmfs_pcislot_state_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_PCISLOT_STATE_MODE, 0));
}

static int
vmmfs_pcislot_state_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_pcislot_state *state_node;
	struct vattr *vattr;
	char buffer[64];
	size_t length;
	int error;

	state_node = ap->a_vp->v_data;
	if (state_node == NULL)
		return (ENOENT);
	error = vmmfs_pcislot_state_load(state_node, buffer, sizeof(buffer),
	    &length);
	if (error != 0)
		return (error);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_PCISLOT_STATE_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = state_node->inode;
	vattr->va_size = length;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = length;
	return (0);
}

static int
vmmfs_pcislot_state_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_PCISLOT_STATE_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_pcislot_state_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_pcislot_state_read(struct vop_read_args *ap)
{
	struct vmmfs_pcislot_state *state_node;
	char buffer[64];
	size_t length;
	off_t offset;
	int error;

	state_node = ap->a_vp->v_data;
	if (state_node == NULL)
		return (ENOENT);
	if (ap->a_uio->uio_offset < 0)
		return (EINVAL);
	error = vmmfs_pcislot_state_load(state_node, buffer, sizeof(buffer),
	    &length);
	if (error != 0)
		return (error);
	offset = ap->a_uio->uio_offset;
	if ((size_t)offset >= length)
		return (0);
	return (uiomove(buffer + offset, length - (size_t)offset, ap->a_uio));
}

static int
vmmfs_pcislot_state_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_pcislot_state *state_node;

	state_node = ap->a_vp->v_data;
	if (state_node != NULL && state_node->vnode == ap->a_vp)
		state_node->vnode = NULL;
	ap->a_vp->v_data = NULL;
	return (0);
}

static int
vmmfs_pcislot_state_load(struct vmmfs_pcislot_state *state_node,
	char *buffer, size_t capacity, size_t *length)
{
	int result;

	if (state_node->slot == NULL || state_node->slot->pciroot == NULL ||
	    state_node->slot->pciroot->machine == NULL)
		return (ENOENT);
	result = ksnprintf(buffer, capacity,
	    "provider=detached\nconsumer=root\n");
	if (result < 0 || (size_t)result >= capacity)
		return (EOVERFLOW);
	*length = (size_t)result;
	return (0);
}
