/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot directory object.
 */
#include <sys/dirent.h>
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

#define VMMFS_PCISLOT_MODE 0555

static int vmmfs_pcislot_access(struct vop_access_args *);
static int vmmfs_pcislot_getattr(struct vop_getattr_args *);
static int vmmfs_pcislot_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_pcislot_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_pcislot_nresolve(struct vop_nresolve_args *);
static int vmmfs_pcislot_open(struct vop_open_args *);
static int vmmfs_pcislot_readdir(struct vop_readdir_args *);
static int vmmfs_pcislot_reclaim(struct vop_reclaim_args *);

struct vop_ops vmmfs_pcislot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_pcislot_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_pcislot_getattr,
	.vop_getattr_lite = vmmfs_pcislot_getattr_lite,
	.vop_nlookupdotdot = vmmfs_pcislot_nlookupdotdot,
	.vop_nresolve = vmmfs_pcislot_nresolve,
	.vop_open = vmmfs_pcislot_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_pcislot_readdir,
	.vop_reclaim = vmmfs_pcislot_reclaim,
};

int
vmmfs_pcislot_compare(struct vmmfs_pcislot *left,
	struct vmmfs_pcislot *right)
{
	return (strcmp(left->name, right->name));
}

RB_GENERATE(vmmfs_pcislot_tree, vmmfs_pcislot, entry,
	vmmfs_pcislot_compare);

int
vmmfs_pcislot_create(struct vmmfs_pciroot *pciroot, const char *name,
	size_t namelen, struct vmmfs_pcislot **slotp)
{
	struct vmmfs_mount *state;
	struct vmmfs_pcislot *slot;
	struct vnode *vnode;
	int error;

	if (pciroot == NULL || pciroot->machine == NULL || namelen == 0 ||
	    namelen > NAME_MAX)
		return (EINVAL);
	state = (struct vmmfs_mount *)pciroot->machine->root->mount->mnt_data;
	if (state->pcislot_vops == NULL)
		return (ENXIO);
	slot = kmalloc(sizeof(*slot), M_VMMFS, M_WAITOK | M_ZERO);
	slot->pciroot = pciroot;
	slot->inode = atomic_fetchadd_int(&state->next_inode, 1);
	bcopy(name, slot->name, namelen);
	slot->name[namelen] = '\0';
	error = getnewvnode(VT_SYNTH, pciroot->machine->root->mount, &vnode,
	    0, 0);
	if (error != 0) {
		slot->pciroot = NULL;
		kfree(slot, M_VMMFS);
		return (error);
	}
	vnode->v_data = slot;
	vnode->v_ops = &state->pcislot_vops;
	vnode->v_type = VDIR;
	slot->vnode = vnode;
	error = vmmfs_pcislot_bdf_create(slot, &slot->bdf_node);
	if (error != 0)
		goto fail_vnode;
	error = vmmfs_pcislot_state_create(slot, &slot->state);
	if (error != 0)
		goto fail_bdf;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	*slotp = slot;
	return (0);

fail_bdf:
	(void)vmmfs_pcislot_bdf_destroy(&slot->bdf_node);
fail_vnode:
	vx_get(vnode);
	vgone_vxlocked(vnode);
	vx_put(vnode);
	vrele(vnode);
	slot->pciroot = NULL;
	kfree(slot, M_VMMFS);
	return (error);
}

int
vmmfs_pcislot_destroy(struct vmmfs_pcislot *slot)
{
	struct vmmfs_pciroot *pciroot;
	struct vnode *vnode;
	int error;

	if (slot == NULL)
		return (EINVAL);
	error = vmmfs_pcislot_state_destroy(&slot->state);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_bdf_destroy(&slot->bdf_node);
	if (error != 0)
		return (error);
	vnode = slot->vnode;
	if (vnode != NULL) {
		(void)vrevoke(vnode, proc0.p_ucred);
		vx_get(vnode);
		vgone_vxlocked(vnode);
		vx_put(vnode);
		vrele(vnode);
	}
	KKASSERT(slot->vnode == NULL);
	pciroot = slot->pciroot;
	if (pciroot != NULL && pciroot->machine != NULL && slot->bdf != 0) {
		lwkt_gettoken(&pciroot->machine->token);
		pciroot->bdf_mask &= ~(1U << ((slot->bdf >> 3) & 0x1f));
		lwkt_reltoken(&pciroot->machine->token);
	}
	slot->bdf = 0;
	slot->pciroot = NULL;
	kfree(slot, M_VMMFS);
	return (0);
}

static int
vmmfs_pcislot_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_PCISLOT_MODE, 0));
}

static int
vmmfs_pcislot_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_pcislot *slot;
	struct vattr *vattr;

	slot = ap->a_vp->v_data;
	if (slot == NULL)
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_PCISLOT_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = slot->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

static int
vmmfs_pcislot_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VDIR;
	ap->a_lvap->va_mode = VMMFS_PCISLOT_MODE;
	ap->a_lvap->va_nlink = 2;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_pcislot_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_pcislot *slot;
	struct vmmfs_pciroot *pciroot;
	struct vnode *vnode;
	int error;

	slot = ap->a_dvp->v_data;
	if (slot == NULL)
		return (ENOENT);
	pciroot = slot->pciroot;
	if (pciroot == NULL || pciroot->machine == NULL)
		return (ENOENT);
	lwkt_gettoken(&pciroot->machine->token);
	vnode = pciroot->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&pciroot->machine->token);
	if (vnode == NULL)
		return (ENOENT);
	error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
	vdrop(vnode);
	if (error != 0)
		return (error);
	*ap->a_vpp = vnode;
	vn_unlock(vnode);
	return (0);
}

static int
vmmfs_pcislot_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_pcislot *slot;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	slot = ap->a_dvp->v_data;
	if (slot == NULL || slot->pciroot == NULL ||
	    slot->pciroot->machine == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	lwkt_gettoken(&slot->pciroot->machine->token);
	if (ncp->nc_nlen == sizeof("bdf") - 1 &&
	    bcmp(ncp->nc_name, "bdf", sizeof("bdf") - 1) == 0)
		vnode = slot->bdf_node.vnode;
	else if (ncp->nc_nlen == sizeof("state") - 1 &&
	    bcmp(ncp->nc_name, "state", sizeof("state") - 1) == 0)
		vnode = slot->state.vnode;
	else
		vnode = NULL;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&slot->pciroot->machine->token);
	if (vnode == NULL) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	error = vget(vnode, LK_EXCLUSIVE);
	vdrop(vnode);
	if (error != 0)
		return (error);
	vn_unlock(vnode);
	cache_setvp(ap->a_nch, vnode);
	vrele(vnode);
	return (0);
}

static int
vmmfs_pcislot_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_pcislot_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_pcislot *slot;
	struct uio *uio;
	off_t offset;
	int error;
	int stop;

	slot = ap->a_vp->v_data;
	if (slot == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	offset = uio->uio_offset;
	error = 0;
	stop = 0;
	if (offset == 0) {
		stop = vop_write_dirent(&error, uio, slot->inode, DT_DIR, 1, ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, slot->pciroot->inode, DT_DIR,
		    2, "..");
		if (!stop)
			offset = 2;
	}
	if (!stop && offset == 2) {
		stop = vop_write_dirent(&error, uio, slot->bdf_node.inode, DT_REG,
		    sizeof("bdf") - 1, "bdf");
		if (!stop)
			offset = 3;
	}
	if (!stop && offset == 3) {
		stop = vop_write_dirent(&error, uio, slot->state.inode, DT_REG,
		    sizeof("state") - 1, "state");
		if (!stop)
			offset = 4;
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop;
	return (error);
}

static int
vmmfs_pcislot_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_pcislot *slot;
	struct vmmfs_pciroot *pciroot;

	slot = ap->a_vp->v_data;
	if (slot != NULL) {
		pciroot = slot->pciroot;
		if (pciroot != NULL && pciroot->machine != NULL) {
			lwkt_gettoken(&pciroot->machine->token);
			if (slot->vnode == ap->a_vp)
				slot->vnode = NULL;
			lwkt_reltoken(&pciroot->machine->token);
		} else if (slot->vnode == ap->a_vp) {
			slot->vnode = NULL;
		}
	}
	ap->a_vp->v_data = NULL;
	return (0);
}
