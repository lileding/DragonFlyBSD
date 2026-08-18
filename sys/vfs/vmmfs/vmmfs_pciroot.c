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
#define VMMFS_PCI_CONFIG_ADDRESS 0xcf8U
#define VMMFS_PCI_CONFIG_DATA 0xcfcU

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
static uint16_t vmmfs_pciroot_bdf_alloc_locked(struct vmmfs_pciroot *);
static int vmmfs_pciroot_read_item(struct vmmfs_pciroot *, uint64_t,
	struct vmmfs_pciroot_item *);
static int vmmfs_pciroot_config_address_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_pciroot_config_address_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static int vmmfs_pciroot_config_data_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_pciroot_config_data_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);

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
	busy = pciroot->runtime_machine != NULL || !RB_EMPTY(&pciroot->slots);
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

int
vmmfs_pciroot_start(struct vmmfs_pciroot *pciroot, vmm_machine_t machine)
{
	int error;

	if (pciroot == NULL || pciroot->machine == NULL || machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine != NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (EBUSY);
	}
	lwkt_reltoken(&pciroot->machine->token);
	error = vmm_machine_trap_pio_read(machine, VMMFS_PCI_CONFIG_ADDRESS,
	    sizeof(uint32_t), vmmfs_pciroot_config_address_read, pciroot,
	    &pciroot->config_address_read);
	if (error != 0)
		return (error);
	error = vmm_machine_trap_pio_write(machine, VMMFS_PCI_CONFIG_ADDRESS,
	    sizeof(uint32_t), vmmfs_pciroot_config_address_write, pciroot,
	    &pciroot->config_address_write);
	if (error != 0)
		goto fail_address_read;
	error = vmm_machine_trap_pio_read(machine, VMMFS_PCI_CONFIG_DATA,
	    sizeof(uint32_t), vmmfs_pciroot_config_data_read, pciroot,
	    &pciroot->config_data_read);
	if (error != 0)
		goto fail_address_write;
	error = vmm_machine_trap_pio_write(machine, VMMFS_PCI_CONFIG_DATA,
	    sizeof(uint32_t), vmmfs_pciroot_config_data_write, pciroot,
	    &pciroot->config_data_write);
	if (error != 0)
		goto fail_data_read;
	lwkt_gettoken(&pciroot->machine->token);
	pciroot->runtime_machine = machine;
	pciroot->config_address = 0;
	lwkt_reltoken(&pciroot->machine->token);
	return (0);

fail_data_read:
	(void)vmm_machine_untrap(machine, pciroot->config_data_read);
	pciroot->config_data_read = NULL;
fail_address_write:
	(void)vmm_machine_untrap(machine, pciroot->config_address_write);
	pciroot->config_address_write = NULL;
fail_address_read:
	(void)vmm_machine_untrap(machine, pciroot->config_address_read);
	pciroot->config_address_read = NULL;
	return (error);
}

int
vmmfs_pciroot_stop(struct vmmfs_pciroot *pciroot)
{
	vmm_machine_t machine;
	vmm_io_t config_address_read;
	vmm_io_t config_address_write;
	vmm_io_t config_data_read;
	vmm_io_t config_data_write;
	int error;
	int result;

	if (pciroot == NULL || pciroot->machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&pciroot->machine->token);
	machine = pciroot->runtime_machine;
	if (machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (0);
	}
	config_address_read = pciroot->config_address_read;
	config_address_write = pciroot->config_address_write;
	config_data_read = pciroot->config_data_read;
	config_data_write = pciroot->config_data_write;
	pciroot->runtime_machine = NULL;
	pciroot->config_address = 0;
	pciroot->config_address_read = NULL;
	pciroot->config_address_write = NULL;
	pciroot->config_data_read = NULL;
	pciroot->config_data_write = NULL;
	lwkt_reltoken(&pciroot->machine->token);
	result = 0;
	if (config_data_write != NULL) {
		error = vmm_machine_untrap(machine, config_data_write);
		if (result == 0)
			result = error;
	}
	if (config_data_read != NULL) {
		error = vmm_machine_untrap(machine, config_data_read);
		if (result == 0)
			result = error;
	}
	if (config_address_write != NULL) {
		error = vmm_machine_untrap(machine, config_address_write);
		if (result == 0)
			result = error;
	}
	if (config_address_read != NULL) {
		error = vmm_machine_untrap(machine, config_address_read);
		if (result == 0)
			result = error;
	}
	return (result);
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
	if (!pciroot->machine->stopped.expect_stopped ||
	    pciroot->machine->machine != NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		(void)vmmfs_pcislot_destroy(slot);
		return (EBUSY);
	}
	slot->bdf = vmmfs_pciroot_bdf_alloc_locked(pciroot);
	if (slot->bdf == 0) {
		lwkt_reltoken(&pciroot->machine->token);
		(void)vmmfs_pcislot_destroy(slot);
		return (ENOSPC);
	}
	if (RB_INSERT(vmmfs_pcislot_tree, &pciroot->slots, slot) != NULL) {
		pciroot->bdf_mask &= ~(1U << ((slot->bdf >> 3) & 0x1f));
		slot->bdf = 0;
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
	if (!pciroot->machine->stopped.expect_stopped ||
	    pciroot->machine->machine != NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		vrele(vnode);
		return (EBUSY);
	}
	RB_REMOVE(vmmfs_pcislot_tree, &pciroot->slots, slot);
	lwkt_reltoken(&pciroot->machine->token);
	error = vmmfs_pcislot_destroy(slot);
	if (error == 0)
		cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	else {
		lwkt_gettoken(&pciroot->machine->token);
		(void)RB_INSERT(vmmfs_pcislot_tree, &pciroot->slots, slot);
		lwkt_reltoken(&pciroot->machine->token);
	}
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

static uint16_t
vmmfs_pciroot_bdf_alloc_locked(struct vmmfs_pciroot *pciroot)
{
	unsigned int device;

	for (device = 1; device < 32; ++device) {
		uint32_t bit;

		bit = 1U << device;
		if ((pciroot->bdf_mask & bit) != 0)
			continue;
		pciroot->bdf_mask |= bit;
		return ((uint16_t)(device << 3));
	}
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

static int
vmmfs_pciroot_config_address_read(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	struct vmmfs_pciroot *pciroot;

	(void)vcpu;
	pciroot = argument;
	if (pciroot == NULL || read->address != VMMFS_PCI_CONFIG_ADDRESS ||
	    read->width != VMM_IO_WIDTH_32)
		return (ENOENT);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (ENOENT);
	}
	read->value = pciroot->config_address;
	lwkt_reltoken(&pciroot->machine->token);
	return (0);
}

static int
vmmfs_pciroot_config_address_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	struct vmmfs_pciroot *pciroot;

	(void)vcpu;
	pciroot = argument;
	if (pciroot == NULL || write->address != VMMFS_PCI_CONFIG_ADDRESS ||
	    write->width != VMM_IO_WIDTH_32)
		return (ENOENT);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (ENOENT);
	}
	pciroot->config_address = (uint32_t)write->value;
	lwkt_reltoken(&pciroot->machine->token);
	return (0);
}

static int
vmmfs_pciroot_config_data_read(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	struct vmmfs_pciroot *pciroot;

	(void)vcpu;
	pciroot = argument;
	if (pciroot == NULL || read->address < VMMFS_PCI_CONFIG_DATA ||
	    read->address >= VMMFS_PCI_CONFIG_DATA + sizeof(uint32_t))
		return (ENOENT);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (ENOENT);
	}
	lwkt_reltoken(&pciroot->machine->token);
	switch (read->width) {
	case VMM_IO_WIDTH_8:
		read->value = UINT8_MAX;
		break;
	case VMM_IO_WIDTH_16:
		read->value = UINT16_MAX;
		break;
	case VMM_IO_WIDTH_32:
		read->value = UINT32_MAX;
		break;
	default:
		return (ENOENT);
	}
	return (0);
}

static int
vmmfs_pciroot_config_data_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	struct vmmfs_pciroot *pciroot;

	(void)vcpu;
	pciroot = argument;
	if (pciroot == NULL || write->address < VMMFS_PCI_CONFIG_DATA ||
	    write->address >= VMMFS_PCI_CONFIG_DATA + sizeof(uint32_t) ||
	    (write->width != VMM_IO_WIDTH_8 &&
	    write->width != VMM_IO_WIDTH_16 &&
	    write->width != VMM_IO_WIDTH_32))
		return (ENOENT);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (ENOENT);
	}
	lwkt_reltoken(&pciroot->machine->token);
	return (0);
}
