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
#include <sys/namecache.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_resource.h"

#define VMMFS_PCISLOT_MODE 0555

struct vmmfs_pcislot_item {
	ino_t inode;
	uint8_t type;
	char name[32];
};

static int vmmfs_pcislot_access(struct vop_access_args *);
static int vmmfs_pcislot_getattr(struct vop_getattr_args *);
static int vmmfs_pcislot_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_pcislot_ncreate(struct vop_ncreate_args *);
static int vmmfs_pcislot_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_pcislot_nremove(struct vop_nremove_args *);
static int vmmfs_pcislot_nresolve(struct vop_nresolve_args *);
static int vmmfs_pcislot_open(struct vop_open_args *);
static int vmmfs_pcislot_readdir(struct vop_readdir_args *);
static int vmmfs_pcislot_reclaim(struct vop_reclaim_args *);
static int vmmfs_pcislot_read_item(struct vmmfs_pcislot *, uint64_t,
	struct vmmfs_pcislot_item *);
static int vmmfs_pcislot_type0_build(struct vmmfs_pcislot *);
static int vmmfs_pcislot_type0_allocate_bar(struct vmmfs_pcislot *,
	unsigned int);
static int vmmfs_pcislot_type0_allocate_rom(struct vmmfs_pcislot *);
static int vmmfs_pcislot_type0_append_capabilities(struct vmmfs_pcislot *);
static int vmmfs_pcislot_type0_cap_write(struct vmmfs_pcislot *, uint16_t,
	enum vmm_io_width, uint32_t);
static void vmmfs_pcislot_type0_write16(uint8_t *, uint16_t, uint16_t);
static void vmmfs_pcislot_type0_write32(uint8_t *, uint16_t, uint32_t);
static uint32_t vmmfs_pcislot_type0_read32(const uint8_t *, uint16_t);
static uint32_t vmmfs_pcislot_type0_read(const uint8_t *, uint16_t,
	enum vmm_io_width);
static uint32_t vmmfs_pcislot_type0_width_mask(enum vmm_io_width);
static int vmmfs_pcislot_type0_bar_index(uint16_t, unsigned int *);
static int vmmfs_pcislot_type0_align(uint64_t, uint64_t, uint64_t *);
static void vmmfs_pcislot_type0_refresh_bar(struct vmmfs_pcislot *,
	unsigned int);

struct vop_ops vmmfs_pcislot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_pcislot_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_pcislot_getattr,
	.vop_getattr_lite = vmmfs_pcislot_getattr_lite,
	.vop_ncreate = vmmfs_pcislot_ncreate,
	.vop_nlookupdotdot = vmmfs_pcislot_nlookupdotdot,
	.vop_nremove = vmmfs_pcislot_nremove,
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
	return ((int)left->bdf - (int)right->bdf);
}

RB_GENERATE(vmmfs_pcislot_tree, vmmfs_pcislot, entry,
	vmmfs_pcislot_compare);

int
vmmfs_pcislot_create(struct vmmfs_pciroot *pciroot, uint16_t bdf,
	struct vmmfs_pcislot **slotp)
{
	struct vmmfs_mount *mount;
	struct vmmfs_pcislot *slot;
	struct vnode *vnode;
	int error;

	if (pciroot == NULL || pciroot->machine == NULL || slotp == NULL)
		return (EINVAL);
	mount = (struct vmmfs_mount *)pciroot->machine->root->mount->mnt_data;
	if (mount->pcislot_vops == NULL)
		return (ENXIO);
	slot = kmalloc(sizeof(*slot), M_VMMFS, M_WAITOK | M_ZERO);
	slot->pciroot = pciroot;
	slot->inode = atomic_fetchadd_int(&mount->next_inode, 1);
	slot->bdf = bdf;
	error = getnewvnode(VT_SYNTH, pciroot->machine->root->mount, &vnode,
	    0, 0);
	if (error != 0)
		goto fail_slot;
	vnode->v_data = slot;
	vnode->v_ops = &mount->pcislot_vops;
	vnode->v_type = VDIR;
	slot->vnode = vnode;
	vmmfs_machine_hold(pciroot->machine);
	error = vmmfs_pcislot_events_create(slot, &slot->events);
	if (error != 0)
		goto fail_vnode;
	error = vmmfs_pcislot_config_create(slot, &slot->config);
	if (error != 0)
		goto fail_events;
	vmmfs_pcislot_events_log(&slot->events, "slot created bdf=0000:%02x:%02x.%x",
	    bdf >> 8, (bdf >> 3) & 0x1f, bdf & 0x7);
	vx_downgrade(vnode);
	vn_unlock(vnode);
	*slotp = slot;
	return (0);

fail_events:
	vmmfs_vnode_discard(slot->events.vnode);
	(void)vmmfs_pcislot_events_destroy(&slot->events);
fail_vnode:
	vx_downgrade(vnode);
	vn_unlock(vnode);
	vmmfs_vnode_discard(vnode);
fail_slot:
	slot->pciroot = NULL;
	kfree(slot, M_VMMFS);
	return (error);
}

int
vmmfs_pcislot_destroy(struct vmmfs_pcislot *slot)
{
	int error;

	if (slot == NULL)
		return (EINVAL);
	if (slot->vnode != NULL)
		return (EBUSY);
	error = vmmfs_pcislot_config_destroy(&slot->config);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_descriptor_destroy(&slot->descriptor);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_events_destroy(&slot->events);
	if (error != 0)
		return (error);
	slot->bdf = 0;
	slot->pciroot = NULL;
	kfree(slot, M_VMMFS);
	return (0);
}

int
vmmfs_pcislot_power_on(struct vmmfs_pcislot *slot, vmm_machine_t machine)
{
	int error;

	if (slot == NULL || slot->pciroot == NULL || machine == NULL ||
	    !slot->descriptor.committed || slot->descriptor.resources != NULL ||
	    slot->type0.powered)
		return (EINVAL);
	error = vmmfs_pcislot_type0_build(slot);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_resources_create(slot, machine,
	    &slot->descriptor.value, slot->descriptor.generation,
	    &slot->descriptor.resources);
	if (error != 0) {
		bzero(&slot->type0, sizeof(slot->type0));
		return (error);
	}
	vmmfs_pcislot_config_power_on(&slot->config,
	    slot->descriptor.generation);
	slot->type0.powered = true;
	cache_inval_vp(slot->vnode, CINV_CHILDREN);
	return (0);
}

void
vmmfs_pcislot_power_off(struct vmmfs_pcislot *slot)
{
	struct vmmfs_pcislot_resources *resources;

	if (slot == NULL)
		return;
	cache_inval_vp(slot->vnode, CINV_CHILDREN);
	vmmfs_pcislot_config_power_off(&slot->config);
	resources = slot->descriptor.resources;
	slot->descriptor.resources = NULL;
	(void)vmmfs_pcislot_resources_destroy(resources);
	bzero(&slot->type0, sizeof(slot->type0));
	cache_inval_vp(slot->vnode, CINV_CHILDREN);
}

int
vmmfs_pcislot_type0_config_read(struct vmmfs_pcislot *slot, vmm_vcpu_t vcpu,
	uint16_t offset, enum vmm_io_width width, uint32_t *value)
{
	const struct vmmfs_pcislot_bar *bar;
	if (slot == NULL || value == NULL || !slot->type0.powered ||
	    (width != VMM_IO_WIDTH_8 && width != VMM_IO_WIDTH_16 &&
	    width != VMM_IO_WIDTH_32) ||
	    offset > VMMFS_PCISLOT_CONFIG_SIZE - width)
		return (EINVAL);
	if (offset >= 0x10 && offset < 0x28 &&
	    (slot->type0.bar_probe[(offset - 0x10) / 4] & 1) != 0) {
		unsigned int index;
		unsigned int owner;
		uint32_t probe;

		index = (offset - 0x10) / 4;
		owner = index;
		if (!slot->descriptor.value.bars[owner].present && owner > 0 &&
		    slot->descriptor.value.bars[owner - 1].present &&
		    slot->descriptor.value.bars[owner - 1].type ==
		    VMMFS_PCISLOT_BAR_MEM64) {
			owner--;
			probe = (uint32_t)(~(slot->descriptor.value.bars[owner].size - 1) >> 32);
		} else if (slot->descriptor.value.bars[owner].present) {
			probe = (uint32_t)(~(slot->descriptor.value.bars[owner].size - 1));
		} else {
			*value = vmmfs_pcislot_type0_read(slot->type0.bytes, offset,
			    width);
			return (0);
		}
		bar = &slot->descriptor.value.bars[owner];
		if (index == owner && bar->type == VMMFS_PCISLOT_BAR_IO)
			probe |= 1;
		else if (index == owner && bar->type ==
		    VMMFS_PCISLOT_BAR_MEM64)
			probe |= 4;
		if (index == owner && bar->prefetchable)
			probe |= 8;
		*value = (probe >> ((offset & 3) * 8)) &
		    vmmfs_pcislot_type0_width_mask(width);
		return (0);
	}
	if (offset >= 0x30 && offset < 0x34 && slot->type0.rom_probe) {
		uint32_t probe;

		probe = (uint32_t)(~(slot->descriptor.value.rom_size - 1));
		*value = (probe >> ((offset & 3) * 8)) &
		    vmmfs_pcislot_type0_width_mask(width);
		return (0);
	}
	*value = vmmfs_pcislot_type0_read(slot->type0.bytes, offset, width);
	(void)vcpu;
	return (0);
}

int
vmmfs_pcislot_type0_config_write(struct vmmfs_pcislot *slot, vmm_vcpu_t vcpu,
	uint16_t offset, enum vmm_io_width width, uint32_t value)
{
	const struct vmmfs_pcislot_bar *bar_value;
	struct vmmfs_pcislot_resources *resources;
	uint32_t old;
	uint32_t mask;
	unsigned int bar;
	unsigned int owner;
	uint16_t dword;

	if (slot == NULL || !slot->type0.powered ||
	    (width != VMM_IO_WIDTH_8 && width != VMM_IO_WIDTH_16 &&
	    width != VMM_IO_WIDTH_32) ||
	    offset > VMMFS_PCISLOT_CONFIG_SIZE - width)
		return (EINVAL);
	if (offset < 0x06 && offset + width > 0x04) {
		old = vmmfs_pcislot_type0_read(slot->type0.bytes, 0x04,
		    VMM_IO_WIDTH_16);
		mask = vmmfs_pcislot_type0_width_mask(width) <<
		    ((offset > 0x04 ? offset - 0x04 : 0) * 8);
		old = (old & ~mask) | ((value << ((offset > 0x04 ? offset - 0x04 : 0) * 8)) & mask);
		vmmfs_pcislot_type0_write16(slot->type0.bytes, 0x04, old);
		slot->type0.bus_master_enabled = (old & 0x0004) != 0;
		resources = slot->descriptor.resources;
		if (resources == NULL)
			return (ENXIO);
		return (vmmfs_pcislot_resources_set_decode(resources,
		    (old & 0x0002) != 0, (old & 0x0001) != 0,
		    (old & 0x0004) != 0));
	}
	if (vmmfs_pcislot_type0_bar_index(offset, &bar) == 0) {
		dword = offset & ~3U;
		owner = bar;
		if (!slot->descriptor.value.bars[owner].present && owner > 0 &&
		    slot->descriptor.value.bars[owner - 1].present &&
		    slot->descriptor.value.bars[owner - 1].type ==
		    VMMFS_PCISLOT_BAR_MEM64)
			owner--;
		if (!slot->descriptor.value.bars[owner].present)
			return (0);
		bar_value = &slot->descriptor.value.bars[owner];
		old = vmmfs_pcislot_type0_read32(slot->type0.bytes, dword);
		mask = vmmfs_pcislot_type0_width_mask(width) << ((offset - dword) * 8);
		old = (old & ~mask) | ((value << ((offset - dword) * 8)) & mask);
		if (dword == 0x10 + owner * 4 && old == UINT32_MAX) {
			slot->type0.bar_probe[owner] = 1;
			if (bar_value->type == VMMFS_PCISLOT_BAR_MEM64)
				slot->type0.bar_probe[owner + 1] = 1;
		} else {
			slot->type0.bar_probe[owner] = 0;
			if (bar_value->type == VMMFS_PCISLOT_BAR_MEM64)
				slot->type0.bar_probe[owner + 1] = 0;
			if (dword == 0x10 + owner * 4) {
				if (bar_value->type == VMMFS_PCISLOT_BAR_IO)
					old = (old & ~3U) | 1U;
				else {
					old &= ~0xfU;
					if (bar_value->type == VMMFS_PCISLOT_BAR_MEM64)
						old |= 4U;
					if (bar_value->prefetchable)
						old |= 8U;
				}
			}
			vmmfs_pcislot_type0_write32(slot->type0.bytes, dword, old);
			vmmfs_pcislot_type0_refresh_bar(slot, owner);
			resources = slot->descriptor.resources;
			if (resources != NULL) {
				int error;

				error = vmmfs_pcislot_resources_bar_relocate(resources,
				    owner, slot->type0.bar_address[owner]);
				if (error != 0)
					return (error);
			}
		}
		(void)vcpu;
		return (0);
	}
	if (slot->descriptor.value.rom_present && offset >= 0x30 && offset < 0x34) {
		old = vmmfs_pcislot_type0_read32(slot->type0.bytes, 0x30);
		mask = vmmfs_pcislot_type0_width_mask(width) << ((offset - 0x30) * 8);
		old = (old & ~mask) | ((value << ((offset - 0x30) * 8)) & mask);
		if (old == UINT32_MAX)
			slot->type0.rom_probe = true;
		else {
			slot->type0.rom_probe = false;
			vmmfs_pcislot_type0_write32(slot->type0.bytes, 0x30,
			    old & ~((uint32_t)slot->descriptor.value.rom_size - 1));
			slot->type0.rom_address = vmmfs_pcislot_type0_read32(
			    slot->type0.bytes, 0x30) & ~1ULL;
			resources = slot->descriptor.resources;
			if (resources != NULL) {
				int error;

				error = vmmfs_pcislot_resources_rom_relocate(resources,
				    slot->type0.rom_address);
				if (error != 0)
					return (error);
				error = vmmfs_pcislot_resources_rom_enable(resources,
				    (old & 1) != 0);
				if (error != 0)
					return (error);
			}
		}
		(void)vcpu;
		return (0);
	}
	if (vmmfs_pcislot_type0_cap_write(slot, offset, width, value) == 0)
		return (0);
	(void)vcpu;
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
vmmfs_pcislot_ncreate(struct vop_ncreate_args *ap)
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
	if (ncp->nc_nlen != sizeof("descriptor") - 1 ||
	    bcmp(ncp->nc_name, "descriptor", sizeof("descriptor") - 1) != 0)
		return (EOPNOTSUPP);
	if (ap->a_vap->va_type != VREG ||
	    (ap->a_vap->va_vaflags & VA_EXCLUSIVE) == 0)
		return (EINVAL);
	lwkt_gettoken(&slot->pciroot->machine->token);
	if (!slot->pciroot->machine->stopped.expect_stopped ||
	    slot->pciroot->machine->machine != NULL) {
		lwkt_reltoken(&slot->pciroot->machine->token);
		return (EBUSY);
	}
	if (slot->descriptor.vnode != NULL) {
		lwkt_reltoken(&slot->pciroot->machine->token);
		return (EEXIST);
	}
	lwkt_reltoken(&slot->pciroot->machine->token);
	error = vmmfs_pcislot_descriptor_create(slot, &slot->descriptor);
	if (error != 0)
		return (error);
	vnode = slot->descriptor.vnode;
	error = vget(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		(void)vmmfs_pcislot_descriptor_destroy(&slot->descriptor);
		return (error);
	}
	*ap->a_vpp = vnode;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
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
vmmfs_pcislot_nremove(struct vop_nremove_args *ap)
{
	struct namecache *ncp;

	ncp = ap->a_nch->ncp;
	if (ncp->nc_nlen == sizeof("descriptor") - 1 &&
	    bcmp(ncp->nc_name, "descriptor", sizeof("descriptor") - 1) == 0)
		return (EOPNOTSUPP);
	return (ENOENT);
}

static int
vmmfs_pcislot_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_pcislot *slot;
	struct vmmfs_pcislot_resource *resource;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	slot = ap->a_dvp->v_data;
	if (slot == NULL || slot->pciroot == NULL ||
	    slot->pciroot->machine == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	lwkt_gettoken(&slot->pciroot->machine->token);
	if (slot->descriptor.committing) {
		lwkt_reltoken(&slot->pciroot->machine->token);
		return (EBUSY);
	}
	if (ncp->nc_nlen == sizeof("events") - 1 &&
	    bcmp(ncp->nc_name, "events", sizeof("events") - 1) == 0)
		vnode = slot->descriptor.committed ? slot->events.vnode : NULL;
	else if (ncp->nc_nlen == sizeof("config") - 1 &&
	    bcmp(ncp->nc_name, "config", sizeof("config") - 1) == 0)
		vnode = slot->descriptor.committed ? slot->config.vnode : NULL;
	else if (ncp->nc_nlen == sizeof("descriptor") - 1 &&
	    bcmp(ncp->nc_name, "descriptor", sizeof("descriptor") - 1) == 0)
		vnode = slot->descriptor.vnode;
	else {
		resource = vmmfs_pcislot_resources_find(slot->descriptor.resources,
		    ncp->nc_name, ncp->nc_nlen);
		vnode = resource == NULL ? NULL : resource->vnode;
	}
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
	struct vmmfs_pcislot_item item;
	struct uio *uio;
	off_t offset;
	uint64_t index;
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
	index = offset - 2;
	while (!stop) {
		error = vmmfs_pcislot_read_item(slot, index, &item);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		stop = vop_write_dirent(&error, uio, item.inode, item.type,
		    (uint16_t)strlen(item.name), item.name);
		if (!stop) {
			++offset;
			++index;
		}
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop && error == 0;
	return (error);
}

static int
vmmfs_pcislot_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_pcislot *slot;
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_machine *machine;

	slot = ap->a_vp->v_data;
	if (slot != NULL) {
		pciroot = slot->pciroot;
		if (pciroot != NULL && pciroot->machine != NULL) {
			machine = pciroot->machine;
			lwkt_gettoken(&machine->token);
			if (slot->vnode == ap->a_vp)
				slot->vnode = NULL;
			lwkt_reltoken(&machine->token);
		} else if (slot->vnode == ap->a_vp) {
			slot->vnode = NULL;
			machine = NULL;
		} else {
			machine = NULL;
		}
	} else {
		machine = NULL;
	}
	ap->a_vp->v_data = NULL;
	if (machine != NULL)
		vmmfs_machine_put(machine);
	return (0);
}

static int
vmmfs_pcislot_read_item(struct vmmfs_pcislot *slot, uint64_t index,
	struct vmmfs_pcislot_item *item)
{
	struct vmmfs_pcislot_resource *resource;
	size_t name_length;

	lwkt_gettoken(&slot->pciroot->machine->token);
	if (slot->descriptor.committing) {
		lwkt_reltoken(&slot->pciroot->machine->token);
		return (EBUSY);
	}
	if (slot->descriptor.committed) {
		if (index == 0) {
		item->inode = slot->events.inode;
		item->type = DT_REG;
		bcopy("events", item->name, sizeof("events"));
			lwkt_reltoken(&slot->pciroot->machine->token);
			return (0);
		}
		--index;
		if (index == 0) {
			item->inode = slot->config.inode;
			item->type = DT_REG;
			bcopy("config", item->name, sizeof("config"));
			lwkt_reltoken(&slot->pciroot->machine->token);
			return (0);
		}
		--index;
	}
	if (slot->descriptor.vnode != NULL) {
		if (index == 0) {
		item->inode = slot->descriptor.inode;
		item->type = DT_REG;
		bcopy("descriptor", item->name, sizeof("descriptor"));
			lwkt_reltoken(&slot->pciroot->machine->token);
			return (0);
		}
		--index;
	}
	if (slot->descriptor.resources == NULL ||
	    index >= slot->descriptor.resources->count) {
		lwkt_reltoken(&slot->pciroot->machine->token);
		return (ENOENT);
	}
	resource = &slot->descriptor.resources->items[index];
	item->inode = resource->inode;
	item->type = DT_REG;
	if (vmmfs_pcislot_resource_name(resource, item->name,
	    sizeof(item->name), &name_length) != 0) {
		lwkt_reltoken(&slot->pciroot->machine->token);
		return (EIO);
	}
	lwkt_reltoken(&slot->pciroot->machine->token);
	return (0);
}

static int
vmmfs_pcislot_type0_build(struct vmmfs_pcislot *slot)
{
	unsigned int bar;
	int error;

	if (slot == NULL || !slot->descriptor.committed)
		return (EINVAL);
	bzero(&slot->type0, sizeof(slot->type0));
	vmmfs_pcislot_type0_write16(slot->type0.bytes, 0x00,
	    slot->descriptor.value.vendor_id);
	vmmfs_pcislot_type0_write16(slot->type0.bytes, 0x02,
	    slot->descriptor.value.device_id);
	slot->type0.bytes[0x08] = slot->descriptor.value.revision;
	slot->type0.bytes[0x09] = slot->descriptor.value.class & 0xff;
	slot->type0.bytes[0x0a] = (slot->descriptor.value.class >> 8) & 0xff;
	slot->type0.bytes[0x0b] = (slot->descriptor.value.class >> 16) & 0xff;
	slot->type0.bytes[0x0e] = 0;
	vmmfs_pcislot_type0_write16(slot->type0.bytes, 0x2c,
	    slot->descriptor.value.subsystem_vendor_id);
	vmmfs_pcislot_type0_write16(slot->type0.bytes, 0x2e,
	    slot->descriptor.value.subsystem_device_id);
	if (slot->descriptor.value.intx_pin == VMMFS_PCISLOT_INTX_NONE) {
		slot->type0.bytes[0x3c] = 0xff;
	} else {
		slot->type0.intx_gsi = 16 +
		    (((slot->bdf >> 3) + slot->descriptor.value.intx_pin - 1) & 3);
		slot->type0.bytes[0x3c] = slot->type0.intx_gsi;
	}
	slot->type0.bytes[0x3d] = slot->descriptor.value.intx_pin;
	for (bar = 0; bar < VMMFS_PCISLOT_MAX_BARS; ++bar) {
		if (!slot->descriptor.value.bars[bar].present)
			continue;
		error = vmmfs_pcislot_type0_allocate_bar(slot, bar);
		if (error != 0)
			return (error);
	}
	error = vmmfs_pcislot_type0_allocate_rom(slot);
	if (error != 0)
		return (error);
	return (vmmfs_pcislot_type0_append_capabilities(slot));
}

static int
vmmfs_pcislot_type0_allocate_bar(struct vmmfs_pcislot *slot,
	unsigned int index)
{
	const struct vmmfs_pcislot_bar *bar;
	struct vmmfs_pciroot *pciroot;
	uint64_t base;
	uint64_t limit;
	uint32_t value;
	int error;

	if (slot == NULL || index >= VMMFS_PCISLOT_MAX_BARS)
		return (EINVAL);
	bar = &slot->descriptor.value.bars[index];
	if (!bar->present)
		return (0);
	pciroot = slot->pciroot;
	if (pciroot == NULL || pciroot->machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&pciroot->machine->token);
	if (bar->type == VMMFS_PCISLOT_BAR_IO) {
		limit = VMMFS_PCI_PIO_END;
		error = vmmfs_pcislot_type0_align(pciroot->pio_next, bar->size,
		    &base);
		if (error == 0 && (base >= limit || bar->size > limit - base))
			error = ENOSPC;
		if (error == 0)
			pciroot->pio_next = (uint32_t)(base + bar->size);
	} else {
		limit = VMMFS_PCI_ECAM_GPA;
		error = vmmfs_pcislot_type0_align(pciroot->mmio_next, bar->size,
		    &base);
		if (error == 0 && (base >= limit || bar->size > limit - base))
			error = ENOSPC;
		if (error == 0)
			pciroot->mmio_next = base + bar->size;
	}
	lwkt_reltoken(&pciroot->machine->token);
	if (error != 0)
		return (error);
	slot->type0.bar_address[index] = base;
	if (bar->type == VMMFS_PCISLOT_BAR_IO) {
		value = (uint32_t)base | 1U;
	} else {
		value = (uint32_t)base;
		if (bar->type == VMMFS_PCISLOT_BAR_MEM64)
			value |= 4U;
		if (bar->prefetchable)
			value |= 8U;
	}
	vmmfs_pcislot_type0_write32(slot->type0.bytes, 0x10 + index * 4,
	    value);
	if (bar->type == VMMFS_PCISLOT_BAR_MEM64)
		vmmfs_pcislot_type0_write32(slot->type0.bytes, 0x14 + index * 4,
		    (uint32_t)(base >> 32));
	return (0);
}

static int
vmmfs_pcislot_type0_allocate_rom(struct vmmfs_pcislot *slot)
{
	struct vmmfs_pciroot *pciroot;
	uint64_t base;
	int error;

	if (slot == NULL || !slot->descriptor.value.rom_present)
		return (0);
	pciroot = slot->pciroot;
	if (pciroot == NULL || pciroot->machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&pciroot->machine->token);
	error = vmmfs_pcislot_type0_align(pciroot->mmio_next,
	    slot->descriptor.value.rom_size, &base);
	if (error == 0 && (base >= VMMFS_PCI_ECAM_GPA ||
	    slot->descriptor.value.rom_size > VMMFS_PCI_ECAM_GPA - base))
		error = ENOSPC;
	if (error == 0)
		pciroot->mmio_next = base + slot->descriptor.value.rom_size;
	lwkt_reltoken(&pciroot->machine->token);
	if (error != 0)
		return (error);
	slot->type0.rom_address = base;
	vmmfs_pcislot_type0_write32(slot->type0.bytes, 0x30, (uint32_t)base);
	return (0);
}

static int
vmmfs_pcislot_type0_append_capabilities(struct vmmfs_pcislot *slot)
{
	const struct vmmfs_pcislot_descriptor_value *value;
	struct vmmfs_pcislot_type0 *type0;
	const struct vmmfs_pcislot_cap *cap;
	const struct vmmfs_pcislot_ecap *ecap;
	uint16_t offset;
	uint16_t length;
	uint16_t next;
	unsigned int index;
	uint32_t header;

	value = &slot->descriptor.value;
	type0 = &slot->type0;
	offset = 0x40;
	for (index = 0; index < VMMFS_PCISLOT_MAX_CAPS; ++index) {
		cap = &value->caps[index];
		if (!cap->present)
			break;
		switch (cap->kind) {
		case VMMFS_PCISLOT_CAP_PCIE:
			length = 0x3c;
			break;
		case VMMFS_PCISLOT_CAP_MSI:
			length = 10 + (cap->address_width == 64 ? 4 : 0) +
			    (cap->maskable ? 8 : 0);
			break;
		case VMMFS_PCISLOT_CAP_MSIX:
			length = 12;
			break;
		case VMMFS_PCISLOT_CAP_BLOB:
			length = 3 + cap->data_length;
			break;
		default:
			return (EINVAL);
		}
		length = (length + 3) & ~3U;
		if (offset > 0xfc || length > 0x100 - offset)
			return (E2BIG);
		type0->cap_offset[index] = offset;
		offset += length;
	}
	if (index != 0) {
		type0->bytes[0x34] = 0x40;
		vmmfs_pcislot_type0_write16(type0->bytes, 0x06, 0x0010);
	}
	for (index = 0; index < VMMFS_PCISLOT_MAX_CAPS &&
	    value->caps[index].present; ++index) {
		cap = &value->caps[index];
		offset = type0->cap_offset[index];
		next = index + 1 < VMMFS_PCISLOT_MAX_CAPS &&
		    value->caps[index + 1].present ? type0->cap_offset[index + 1] : 0;
		switch (cap->kind) {
		case VMMFS_PCISLOT_CAP_PCIE:
			type0->bytes[offset] = 0x10;
			type0->bytes[offset + 1] = next;
			vmmfs_pcislot_type0_write16(type0->bytes, offset + 2, 0x0002);
			break;
		case VMMFS_PCISLOT_CAP_MSI:
			type0->bytes[offset] = 0x05;
			type0->bytes[offset + 1] = next;
			vmmfs_pcislot_type0_write16(type0->bytes, offset + 2,
			    ((uint16_t)__builtin_ctz(cap->vectors) << 1) |
			    (cap->address_width == 64 ? 0x0080 : 0) |
			    (cap->maskable ? 0x0100 : 0));
			break;
		case VMMFS_PCISLOT_CAP_MSIX:
			type0->bytes[offset] = 0x11;
			type0->bytes[offset + 1] = next;
			vmmfs_pcislot_type0_write16(type0->bytes, offset + 2,
			    cap->vectors - 1);
			vmmfs_pcislot_type0_write32(type0->bytes, offset + 4,
			    (uint32_t)cap->table_offset | cap->table_bar);
			vmmfs_pcislot_type0_write32(type0->bytes, offset + 8,
			    (uint32_t)cap->pba_offset | cap->pba_bar);
			break;
		case VMMFS_PCISLOT_CAP_BLOB:
			type0->bytes[offset] = cap->id;
			type0->bytes[offset + 1] = next;
			type0->bytes[offset + 2] = 3 + cap->data_length;
			bcopy(&value->cap_data[cap->data_offset],
			    &type0->bytes[offset + 3], cap->data_length);
			break;
		default:
			return (EINVAL);
		}
	}
	offset = 0x100;
	for (index = 0; index < VMMFS_PCISLOT_MAX_ECAPS; ++index) {
		ecap = &value->ecaps[index];
		if (!ecap->present)
			break;
		length = (uint16_t)((4 + ecap->data_length + 3) & ~3U);
		if (offset > VMMFS_PCISLOT_CONFIG_SIZE - length)
			return (E2BIG);
		type0->ecap_offset[index] = offset;
		offset += length;
	}
	for (index = 0; index < VMMFS_PCISLOT_MAX_ECAPS &&
	    value->ecaps[index].present; ++index) {
		ecap = &value->ecaps[index];
		offset = type0->ecap_offset[index];
		next = index + 1 < VMMFS_PCISLOT_MAX_ECAPS &&
		    value->ecaps[index + 1].present ? type0->ecap_offset[index + 1] : 0;
		header = ecap->id | ((uint32_t)ecap->version << 16) |
		    ((uint32_t)next << 20);
		vmmfs_pcislot_type0_write32(type0->bytes, offset, header);
		bcopy(&value->cap_data[ecap->data_offset],
		    &type0->bytes[offset + 4], ecap->data_length);
	}
	return (0);
}

static void
vmmfs_pcislot_type0_write16(uint8_t *bytes, uint16_t offset, uint16_t value)
{
	bytes[offset] = value;
	bytes[offset + 1] = value >> 8;
}

static void
vmmfs_pcislot_type0_write32(uint8_t *bytes, uint16_t offset, uint32_t value)
{
	bytes[offset] = value;
	bytes[offset + 1] = value >> 8;
	bytes[offset + 2] = value >> 16;
	bytes[offset + 3] = value >> 24;
}

static uint32_t
vmmfs_pcislot_type0_read32(const uint8_t *bytes, uint16_t offset)
{
	return (bytes[offset] | ((uint32_t)bytes[offset + 1] << 8) |
	    ((uint32_t)bytes[offset + 2] << 16) |
	    ((uint32_t)bytes[offset + 3] << 24));
}

static uint32_t
vmmfs_pcislot_type0_read(const uint8_t *bytes, uint16_t offset,
	enum vmm_io_width width)
{
	uint32_t value;

	value = 0;
	if (width >= VMM_IO_WIDTH_8)
		value |= bytes[offset];
	if (width >= VMM_IO_WIDTH_16)
		value |= (uint32_t)bytes[offset + 1] << 8;
	if (width >= VMM_IO_WIDTH_32)
		value |= (uint32_t)bytes[offset + 2] << 16 |
		    (uint32_t)bytes[offset + 3] << 24;
	return (value);
}

static uint32_t
vmmfs_pcislot_type0_width_mask(enum vmm_io_width width)
{
	switch (width) {
	case VMM_IO_WIDTH_8:
		return (UINT8_MAX);
	case VMM_IO_WIDTH_16:
		return (UINT16_MAX);
	case VMM_IO_WIDTH_32:
		return (UINT32_MAX);
	default:
		return (0);
	}
}

static int
vmmfs_pcislot_type0_cap_write(struct vmmfs_pcislot *slot, uint16_t offset,
	enum vmm_io_width width, uint32_t value)
{
	const struct vmmfs_pcislot_cap *cap;
	struct vmmfs_pcislot_resources *resources;
	uint16_t base;
	uint16_t address_end;
	uint16_t data_offset;
	uint16_t mask_offset;
	uint8_t byte;
	uint8_t writable;
	unsigned int index;
	unsigned int byte_index;
	unsigned int maximum;
	bool msix;

	for (index = 0; index < VMMFS_PCISLOT_MAX_CAPS; ++index) {
		cap = &slot->descriptor.value.caps[index];
		if (!cap->present)
			break;
		base = slot->type0.cap_offset[index];
		if (offset < base || offset - base >= 0x40)
			continue;
		msix = cap->kind == VMMFS_PCISLOT_CAP_MSIX;
		for (byte_index = 0; byte_index < width; ++byte_index) {
			if (offset + byte_index >= VMMFS_PCISLOT_CONFIG_SIZE)
				return (EINVAL);
			byte = value >> (byte_index * NBBY);
			writable = 0;
			if (cap->kind == VMMFS_PCISLOT_CAP_MSI) {
				address_end = base + 8 +
				    (cap->address_width == 64 ? 4 : 0);
				data_offset = address_end;
				mask_offset = data_offset + 2;
				if (offset + byte_index == base + 2)
					writable = 0x71;
				else if (offset + byte_index >= base + 4 &&
				    offset + byte_index < address_end + 2)
					writable = UINT8_MAX;
				else if (offset + byte_index >= data_offset &&
				    offset + byte_index < data_offset + 2)
					writable = UINT8_MAX;
				else if (cap->maskable &&
				    offset + byte_index >= mask_offset &&
				    offset + byte_index < mask_offset + 4)
					writable = UINT8_MAX;
			} else if (msix && offset + byte_index == base + 3) {
				writable = 0xc0;
			}
			if (writable != 0) {
				slot->type0.bytes[offset + byte_index] =
				    (slot->type0.bytes[offset + byte_index] & ~writable) |
				    (byte & writable);
			}
		}
		if (cap->kind == VMMFS_PCISLOT_CAP_MSI) {
			maximum = __builtin_ctz(cap->vectors);
			if (((slot->type0.bytes[base + 2] >> 4) & 7) > maximum) {
				slot->type0.bytes[base + 2] &= ~0x70;
				slot->type0.bytes[base + 2] |= maximum << 4;
			}
		} else if (msix) {
			resources = slot->descriptor.resources;
			if (resources != NULL) {
				vmmfs_pcislot_resources_trace_msix_control(resources,
				    index);
				return (vmmfs_pcislot_resources_msix_unmask(resources,
				    index));
			}
		}
		return (0);
	}
	return (ENOENT);
}

static int
vmmfs_pcislot_type0_bar_index(uint16_t offset, unsigned int *index)
{
	unsigned int current;

	if (index == NULL || offset < 0x10 || offset >= 0x28)
		return (ENOENT);
	current = (offset - 0x10) / 4;
	*index = current;
	return (0);
}

static int
vmmfs_pcislot_type0_align(uint64_t value, uint64_t align, uint64_t *result)
{
	if (align == 0 || (align & (align - 1)) != 0 || result == NULL ||
	    value > UINT64_MAX - (align - 1))
		return (EOVERFLOW);
	*result = (value + align - 1) & ~(align - 1);
	return (0);
}

static void
vmmfs_pcislot_type0_refresh_bar(struct vmmfs_pcislot *slot,
	unsigned int index)
{
	const struct vmmfs_pcislot_bar *bar;
	uint64_t address;

	bar = &slot->descriptor.value.bars[index];
	address = vmmfs_pcislot_type0_read32(slot->type0.bytes, 0x10 + index * 4);
	if (bar->type == VMMFS_PCISLOT_BAR_IO) {
		address &= ~3ULL;
	} else {
		address &= ~0xfULL;
		if (bar->type == VMMFS_PCISLOT_BAR_MEM64)
			address |= (uint64_t)vmmfs_pcislot_type0_read32(slot->type0.bytes,
			    0x14 + index * 4) << 32;
	}
	slot->type0.bar_address[index] = address;
}
