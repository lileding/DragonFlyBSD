/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI root directory object.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_pcislot.h"

#define VMMFS_PCIROOT_MODE 0555

struct vmmfs_pciroot_item {
	ino_t inode;
	char name[NAME_MAX + 1];
};

static int vmmfs_pciroot_access(struct vop_access_args *);
static int vmmfs_pciroot_getattr(struct vop_getattr_args *);
static int vmmfs_pciroot_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_pciroot_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_pciroot_nmkdir(struct vop_nmkdir_args *);
static int vmmfs_pciroot_nresolve(struct vop_nresolve_args *);
static int vmmfs_pciroot_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_pciroot_open(struct vop_open_args *);
static int vmmfs_pciroot_readdir(struct vop_readdir_args *);
static int vmmfs_pciroot_reclaim(struct vop_reclaim_args *);
static int vmmfs_pciroot_read_item(struct vmmfs_pciroot *, uint64_t,
	struct vmmfs_pciroot_item *);

struct vop_ops vmmfs_pciroot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_pciroot_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_pciroot_getattr,
	.vop_getattr_lite = vmmfs_pciroot_getattr_lite,
	.vop_nlookupdotdot = vmmfs_pciroot_nlookupdotdot,
	.vop_nmkdir = vmmfs_pciroot_nmkdir,
	.vop_nresolve = vmmfs_pciroot_nresolve,
	.vop_nrmdir = vmmfs_pciroot_nrmdir,
	.vop_open = vmmfs_pciroot_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_pciroot_readdir,
	.vop_reclaim = vmmfs_pciroot_reclaim,
};

int
vmmfs_pciroot_create(struct vmmfs_machine *machine,
	struct vmmfs_pciroot *pciroot)
{
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	if (machine == NULL || pciroot == NULL)
		return (EINVAL);
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
	if (state->pciroot_vops == NULL)
		return (ENXIO);
	bzero(pciroot, sizeof(*pciroot));
	pciroot->machine = machine;
	pciroot->inode = atomic_fetchadd_int(&state->next_inode, 1);
	RB_INIT(&pciroot->slots);
	error = getnewvnode(VT_SYNTH, machine->root->mount, &vnode, 0, 0);
	if (error != 0) {
		pciroot->machine = NULL;
		return (error);
	}
	vnode->v_data = pciroot;
	vnode->v_ops = &state->pciroot_vops;
	vnode->v_type = VDIR;
	pciroot->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_pciroot_destroy(struct vmmfs_pciroot *pciroot)
{
	struct vnode *vnode;
	int busy;

	if (pciroot == NULL)
		return (EINVAL);
	if (pciroot->machine == NULL)
		return (0);
	lwkt_gettoken(&pciroot->machine->token);
	busy = !RB_EMPTY(&pciroot->slots);
	lwkt_reltoken(&pciroot->machine->token);
	if (busy)
		return (EBUSY);
	vnode = pciroot->vnode;
	if (vnode != NULL) {
		vx_get(vnode);
		vgone_vxlocked(vnode);
		vx_put(vnode);
		vrele(vnode);
	}
	KKASSERT(pciroot->vnode == NULL);
	pciroot->machine = NULL;
	return (0);
}

static int
vmmfs_pciroot_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_PCIROOT_MODE, 0));
}

static int
vmmfs_pciroot_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_pciroot *pciroot;
	struct vattr *vattr;

	pciroot = ap->a_vp->v_data;
	if (pciroot == NULL)
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_PCIROOT_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = pciroot->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

static int
vmmfs_pciroot_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VDIR;
	ap->a_lvap->va_mode = VMMFS_PCIROOT_MODE;
	ap->a_lvap->va_nlink = 2;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_pciroot_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	int error;

	pciroot = ap->a_dvp->v_data;
	if (pciroot == NULL)
		return (ENOENT);
	machine = pciroot->machine;
	if (machine == NULL)
		return (ENOENT);
	lwkt_gettoken(&machine->token);
	vnode = machine->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->token);
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
vmmfs_pciroot_nmkdir(struct vop_nmkdir_args *ap)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pcislot *slot;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	pciroot = ap->a_dvp->v_data;
	if (pciroot == NULL || pciroot->machine == NULL)
		return (ENOENT);
	if (ap->a_vap->va_type != VDIR)
		return (EINVAL);
	ncp = ap->a_nch->ncp;
	error = vmmfs_pcislot_create(pciroot, ncp->nc_name, ncp->nc_nlen,
	    &slot);
	if (error != 0)
		return (error);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->machine->root == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		(void)vmmfs_pcislot_destroy(slot);
		return (ENOENT);
	}
	if (RB_INSERT(vmmfs_pcislot_tree, &pciroot->slots, slot) != NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		(void)vmmfs_pcislot_destroy(slot);
		return (EEXIST);
	}
	lwkt_reltoken(&pciroot->machine->token);
	vnode = slot->vnode;
	error = vget(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		lwkt_gettoken(&pciroot->machine->token);
		RB_REMOVE(vmmfs_pcislot_tree, &pciroot->slots, slot);
		lwkt_reltoken(&pciroot->machine->token);
		(void)vmmfs_pcislot_destroy(slot);
		return (error);
	}
	*ap->a_vpp = vnode;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
	return (0);
}

static int
vmmfs_pciroot_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pcislot key;
	struct vmmfs_pcislot *slot;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	pciroot = ap->a_dvp->v_data;
	if (pciroot == NULL || pciroot->machine == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen > NAME_MAX) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	bzero(&key, sizeof(key));
	bcopy(ncp->nc_name, key.name, ncp->nc_nlen);
	key.name[ncp->nc_nlen] = '\0';
	lwkt_gettoken(&pciroot->machine->token);
	slot = RB_FIND(vmmfs_pcislot_tree, &pciroot->slots, &key);
	vnode = slot == NULL ? NULL : slot->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&pciroot->machine->token);
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
vmmfs_pciroot_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pcislot *slot;
	struct vnode *vnode;
	int error;

	pciroot = ap->a_dvp->v_data;
	if (pciroot == NULL || pciroot->machine == NULL)
		return (ENOENT);
	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vnode);
	if (error != 0)
		return (error);
	vn_unlock(vnode);
	if (vnode->v_type != VDIR) {
		vrele(vnode);
		return (ENOTDIR);
	}
	slot = vnode->v_data;
	if (slot == NULL) {
		vrele(vnode);
		return (ENOENT);
	}
	lwkt_gettoken(&pciroot->machine->token);
	if (slot->pciroot != pciroot) {
		lwkt_reltoken(&pciroot->machine->token);
		vrele(vnode);
		return (ENOENT);
	}
	RB_REMOVE(vmmfs_pcislot_tree, &pciroot->slots, slot);
	lwkt_reltoken(&pciroot->machine->token);
	error = vmmfs_pcislot_destroy(slot);
	if (error == 0)
		cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	vrele(vnode);
	return (error);
}

static int
vmmfs_pciroot_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_pciroot_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pciroot_item item;
	struct uio *uio;
	off_t offset;
	uint64_t index;
	int error;
	int stop;

	pciroot = ap->a_vp->v_data;
	if (pciroot == NULL)
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
		stop = vop_write_dirent(&error, uio, pciroot->inode, DT_DIR, 1,
		    ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, pciroot->machine->inode,
		    DT_DIR, 2, "..");
		if (!stop)
			offset = 2;
	}
	index = offset - 2;
	while (!stop) {
		error = vmmfs_pciroot_read_item(pciroot, index, &item);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		stop = vop_write_dirent(&error, uio, item.inode, DT_DIR,
		    (uint16_t)strlen(item.name), item.name);
		if (!stop) {
			offset++;
			index++;
		}
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop && error == 0;
	return (error);
}

static int
vmmfs_pciroot_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_pciroot *pciroot;

	pciroot = ap->a_vp->v_data;
	if (pciroot != NULL && pciroot->machine != NULL) {
		lwkt_gettoken(&pciroot->machine->token);
		if (pciroot->vnode == ap->a_vp)
			pciroot->vnode = NULL;
		lwkt_reltoken(&pciroot->machine->token);
	}
	ap->a_vp->v_data = NULL;
	return (0);
}

static int
vmmfs_pciroot_read_item(struct vmmfs_pciroot *pciroot, uint64_t index,
	struct vmmfs_pciroot_item *item)
{
	struct vmmfs_pcislot *slot;
	uint64_t current;

	lwkt_gettoken(&pciroot->machine->token);
	current = 0;
	RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots) {
		if (current++ != index)
			continue;
		item->inode = slot->inode;
		bcopy(slot->name, item->name, sizeof(item->name));
		lwkt_reltoken(&pciroot->machine->token);
		return (0);
	}
	lwkt_reltoken(&pciroot->machine->token);
	return (ENOENT);
}
