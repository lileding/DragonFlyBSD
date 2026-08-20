/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot BDF information node.
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
#include "vmmfs_pcislot_bdf.h"

#define VMMFS_PCISLOT_BDF_MODE 0444

static int vmmfs_pcislot_bdf_access(struct vop_access_args *);
static int vmmfs_pcislot_bdf_getattr(struct vop_getattr_args *);
static int vmmfs_pcislot_bdf_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_pcislot_bdf_open(struct vop_open_args *);
static int vmmfs_pcislot_bdf_read(struct vop_read_args *);
static int vmmfs_pcislot_bdf_reclaim(struct vop_reclaim_args *);

struct vop_ops vmmfs_pcislot_bdf_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_pcislot_bdf_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_pcislot_bdf_getattr,
	.vop_getattr_lite = vmmfs_pcislot_bdf_getattr_lite,
	.vop_open = vmmfs_pcislot_bdf_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_bdf_read,
	.vop_reclaim = vmmfs_pcislot_bdf_reclaim,
};

int
vmmfs_pcislot_bdf_create(struct vmmfs_pcislot *slot,
	struct vmmfs_pcislot_bdf *bdf)
{
	struct vmmfs_mount *mount;
	struct vnode *vnode;
	int error;

	if (slot == NULL || slot->pciroot == NULL ||
	    slot->pciroot->machine == NULL || bdf == NULL)
		return (EINVAL);
	mount = (struct vmmfs_mount *)slot->pciroot->machine->root->mount->mnt_data;
	if (mount->pcislot_bdf_vops == NULL)
		return (ENXIO);
	bzero(bdf, sizeof(*bdf));
	bdf->slot = slot;
	bdf->inode = atomic_fetchadd_int(&mount->next_inode, 1);
	error = getnewvnode(VT_SYNTH, slot->pciroot->machine->root->mount,
	    &vnode, 0, 0);
	if (error != 0) {
		bdf->slot = NULL;
		return (error);
	}
	vnode->v_data = bdf;
	vnode->v_ops = &mount->pcislot_bdf_vops;
	vnode->v_type = VREG;
	bdf->vnode = vnode;
	vmmfs_machine_hold(slot->pciroot->machine);
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_pcislot_bdf_destroy(struct vmmfs_pcislot_bdf *bdf)
{
	struct vnode *vnode;

	if (bdf == NULL)
		return (EINVAL);
	vnode = bdf->vnode;
	if (vnode != NULL) {
		vmmfs_vnode_revoke(vnode);
	}
	KKASSERT(bdf->vnode == NULL);
	bdf->slot = NULL;
	return (0);
}

static int
vmmfs_pcislot_bdf_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_PCISLOT_BDF_MODE, 0));
}

static int
vmmfs_pcislot_bdf_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_pcislot_bdf *bdf;
	struct vattr *vattr;

	bdf = ap->a_vp->v_data;
	if (bdf == NULL || bdf->slot == NULL)
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_PCISLOT_BDF_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = bdf->inode;
	vattr->va_size = sizeof("0000:00:00.0\n") - 1;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = vattr->va_size;
	return (0);
}

static int
vmmfs_pcislot_bdf_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_PCISLOT_BDF_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = sizeof("0000:00:00.0\n") - 1;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_pcislot_bdf_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_pcislot_bdf_read(struct vop_read_args *ap)
{
	struct vmmfs_pcislot_bdf *bdf;
	char text[sizeof("0000:00:00.0\n")];
	uint16_t value;
	size_t length;
	int result;

	bdf = ap->a_vp->v_data;
	if (bdf == NULL || bdf->slot == NULL || bdf->slot->pciroot == NULL ||
	    bdf->slot->pciroot->machine == NULL)
		return (ENOENT);
	if (ap->a_uio->uio_offset < 0)
		return (EINVAL);
	lwkt_gettoken(&bdf->slot->pciroot->machine->token);
	value = bdf->slot->bdf;
	lwkt_reltoken(&bdf->slot->pciroot->machine->token);
	result = ksnprintf(text, sizeof(text), "0000:%02x:%02x.%x\n",
	    value >> 8, (value >> 3) & 0x1f, value & 0x7);
	if (result < 0 || (size_t)result >= sizeof(text))
		return (EOVERFLOW);
	length = (size_t)result;
	if ((size_t)ap->a_uio->uio_offset >= length)
		return (0);
	return (uiomove(text + ap->a_uio->uio_offset,
	    length - (size_t)ap->a_uio->uio_offset, ap->a_uio));
}

static int
vmmfs_pcislot_bdf_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_pcislot_bdf *bdf;
	struct vmmfs_machine *machine;

	bdf = ap->a_vp->v_data;
	if (bdf != NULL && bdf->slot != NULL && bdf->slot->pciroot != NULL) {
		machine = bdf->slot->pciroot->machine;
		if (bdf->vnode == ap->a_vp)
			bdf->vnode = NULL;
	} else {
		machine = NULL;
	}
	ap->a_vp->v_data = NULL;
	if (machine != NULL)
		vmmfs_machine_put(machine);
	return (0);
}
