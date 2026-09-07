/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot directory object.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <machine/limits.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_pciroot.h"
#include "vmmfs_parent.h"
#include "vmmfs_machine.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_resource.h"

#define VMMFS_PCISLOT_MODE 0555

struct vmmfs_pcislot_item {
	ino_t inode;
	uint8_t type;
	char name[32];
};

static int vmmfs_pcislot_nremove(struct vop_nremove_args *);
static int vmmfs_pcislot_nresolve(struct vop_nresolve_args *);
static int vmmfs_pcislot_open(struct vop_open_args *);
static int vmmfs_pcislot_readdir(struct vop_readdir_args *);
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
static void vmmfs_pcislot_drop(struct vmmfs_node *);
static int vmmfs_pcislot_deactivate(struct vmmfs_node *);

struct vop_ops vmmfs_pcislot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_nlookupdotdot = vmmfs_node_nlookupdotdot,
	.vop_nremove = vmmfs_pcislot_nremove,
	.vop_nresolve = vmmfs_pcislot_nresolve,
	.vop_open = vmmfs_pcislot_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_pcislot_readdir,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

int
vmmfs_pcislot_create(struct vmmfs_node *parent,
	uint16_t bdf,
	struct vnode **vnodep)
{
	struct vmmfs_root *root;
	struct vmmfs_pcislot *slot;
	int error;

	if (parent == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	root = parent->mount->root_vnode->v_data;
	slot = kmalloc(sizeof(*slot), M_VMMFS, M_WAITOK | M_ZERO);
	slot->node.inode = vmmfs_root_allocate_inode(root);
	slot->bdf = bdf;
	slot->node.parent = parent;
	slot->node.mount = parent->mount;
	slot->node.references = 1;
	lwkt_token_init(&slot->node.token, "vmmfsnode");
	lockinit(&slot->node.lock, "vmmfsnode", 0, 0);
	slot->node.drop = vmmfs_pcislot_drop;
	vmmfs_node_hold(parent);
	slot->node.deactivate = vmmfs_pcislot_deactivate;
	slot->node.mode = VMMFS_PCISLOT_MODE;
	slot->node.size = 0;
	error = vmmfs_pcislot_events_init(&slot->node, &slot->events,
	    &slot->events_vnode);
	if (error != 0)
		goto fail_slot;
	error = vmmfs_pcislot_config_init(&slot->node, &slot->config,
	    &slot->config_vnode);
	if (error != 0)
		goto fail_events;
	error = vmmfs_pcislot_descriptor_init(&slot->node, &slot->descriptor,
	    &slot->descriptor_vnode);
	if (error != 0)
		goto fail_config;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->pcislot_vops, VDIR, &slot->node, vnodep);
	if (error != 0)
		goto fail_descriptor;
	vmmfs_pcislot_events_log(&slot->events, VMMFS_PCI_EVENT_SLOT_CREATED,
	    "bdf=0000:%02x:%02x.%x", bdf >> 8, (bdf >> 3) & 0x1f,
	    bdf & 0x7);
	return (0);

fail_descriptor:
	vmmfs_vnode_discard(slot->descriptor_vnode);
	slot->descriptor_vnode = NULL;
	vmmfs_node_put(&slot->descriptor.node);
fail_config:
	vmmfs_vnode_discard(slot->config_vnode);
	slot->config_vnode = NULL;
	vmmfs_node_put(&slot->config.node);
fail_events:
	vmmfs_vnode_discard(slot->events_vnode);
	slot->events_vnode = NULL;
	vmmfs_node_put(&slot->events.node);
fail_slot:
	slot->node.dead = true;
	vmmfs_node_put(&slot->node);
	return (error);
}

static void
vmmfs_pcislot_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot *slot;

	slot = (struct vmmfs_pcislot *)node;
	KKASSERT(slot != NULL);
	KKASSERT(slot->node.dead);
	KKASSERT(slot->node.references == 0);
	KKASSERT(slot->entry == NULL);
	KKASSERT(!slot->topology_reference);
	KKASSERT(slot->resources == NULL);
	KKASSERT(slot->descriptor.node.drop == NULL);
	KKASSERT(slot->config.node.drop == NULL);
	KKASSERT(slot->events.node.drop == NULL);
	slot->bdf = 0;
	kfree(slot, M_VMMFS);
}

static int
vmmfs_pcislot_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_pcislot *slot = (struct vmmfs_pcislot *)node;
	struct vmmfs_machine *machine;
	int error;
	struct vmmfs_node *parent = node->parent;

	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot));
	/* Veto must not sleep and expose a provisional dead gate. */
	if (!lwkt_trytoken(&parent->token))
		return (EBUSY);
	if (slot->entry != NULL && !parent->dead) {
		if (!lwkt_trytoken(&machine->node.token)) {
			lwkt_reltoken(&parent->token);
			return (EBUSY);
		}
		error = machine->node.dead || machine->machine != NULL ? EBUSY : 0;
		if (error == 0) {
			/* The parent releases this when it detaches the slot. */
			slot->topology_reference = true;
			++machine->runtime_references;
		}
		lwkt_reltoken(&machine->node.token);
		lwkt_reltoken(&parent->token);
		if (error != 0)
			return (error);
	} else {
		lwkt_reltoken(&parent->token);
	}
	/* Detached candidates and children of a closing root cannot veto. */
	vmmfs_pcislot_power_off(slot);
	(void)vmmfs_vnode_deactivate(slot->descriptor_vnode);
	vrele(slot->descriptor_vnode);
	(void)vmmfs_vnode_deactivate(slot->config_vnode);
	vrele(slot->config_vnode);
	(void)vmmfs_vnode_deactivate(slot->events_vnode);
	vrele(slot->events_vnode);
	return (0);
}


int
vmmfs_pcislot_power_on(struct vmmfs_pcislot *slot, vmm_machine_t machine)
{
	int error;

	if (slot == NULL || machine == NULL || slot->node.dead ||
	    !slot->descriptor.committed || slot->resources != NULL ||
	    slot->type0.powered)
		return (EINVAL);
	error = vmmfs_pcislot_type0_build(slot);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_resources_create(slot, machine,
	    &slot->descriptor.value, slot->descriptor.generation,
	    &slot->resources);
	if (error != 0) {
		bzero(&slot->type0, sizeof(slot->type0));
		return (error);
	}
	vmmfs_pcislot_config_power_on(&slot->config,
	    slot->descriptor.generation);
	slot->type0.powered = true;
	vmmfs_pciroot_invalidate_slot(vmmfs_pcislot_pciroot(slot), slot);
	return (0);
}

void
vmmfs_pcislot_power_off(struct vmmfs_pcislot *slot)
{
	struct vmmfs_pcislot_resources *resources;

	if (slot == NULL)
		return;
	vmmfs_pciroot_invalidate_slot(vmmfs_pcislot_pciroot(slot), slot);
	vmmfs_pcislot_config_power_off(&slot->config);

	/* The slot owns the active generation reference. */
	lwkt_gettoken(&slot->node.token);
	resources = slot->resources;
	slot->resources = NULL;
	bzero(&slot->type0, sizeof(slot->type0));
	lwkt_reltoken(&slot->node.token);

	vmmfs_pcislot_resources_deactivate(resources);
	vmmfs_pciroot_invalidate_slot(vmmfs_pcislot_pciroot(slot), slot);
}

int
vmmfs_pcislot_reset(struct vmmfs_pcislot *slot)
{
	struct vmmfs_pcislot_resources *resources;
	unsigned int bar;
	int error;

	/*
	 * Resources identify the persistent provider session.  Type-0 config
	 * space is reset state and may already have been cleared while tearing
	 * down the previous vmm_machine.  Rebuild it rather than treating that
	 * derived state as a failed reset precondition.
	 */
	if (slot == NULL || slot->node.dead || !slot->descriptor.committed)
		return (EINVAL);
	resources = slot->resources;
	if (resources == NULL)
		return (ENXIO);

	/*
	 * A guest reset resets the PCI function, not its host provider session.
	 * Remove all decode before recreating config space so rebind installs no
	 * stale BAR traps on the new vmm_machine.
	 */
	error = vmmfs_pcislot_resources_set_decode(resources, false, false,
	    false);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_type0_build(slot);
	if (error != 0)
		return (error);
	/* Decode is off; synchronize the retained resources before rebind. */
	for (bar = 0; bar < VMMFS_PCISLOT_MAX_BARS; ++bar) {
		if (!slot->descriptor.value.bars[bar].present)
			continue;
		error = vmmfs_pcislot_resources_bar_relocate(resources, bar,
		    slot->type0.bar_address[bar]);
		if (error != 0)
			return (error);
	}
	if (slot->descriptor.value.rom_present) {
		error = vmmfs_pcislot_resources_rom_relocate(resources,
		    slot->type0.rom_address);
		if (error != 0)
			return (error);
	}
	vmmfs_pcislot_config_power_on(&slot->config,
	    slot->descriptor.generation);
	slot->type0.powered = true;
	vmmfs_pcislot_events_log(&slot->events, VMMFS_PCI_EVENT_RESET,
	    "generation=%ju", (uintmax_t)slot->descriptor.generation);
	return (0);
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
		resources = slot->resources;
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
			resources = slot->resources;
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
			    (old & ~((uint32_t)slot->descriptor.value.rom_size - 1)) |
				    (old & 1U));
			slot->type0.rom_address = vmmfs_pcislot_type0_read32(
			    slot->type0.bytes, 0x30) & ~1ULL;
			resources = slot->resources;
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
vmmfs_pcislot_get_item(struct vmmfs_pcislot *slot,
	const char *name, size_t length, struct vnode **vnodep)
{
	struct vnode *vnode;
	struct vmmfs_pcislot_resources *resources;
	int error;

	lwkt_gettoken(&slot->node.token);
	if (slot->node.dead) {
		lwkt_reltoken(&slot->node.token);
		return (ENOENT);
	}
	if (slot->descriptor.updating) {
		lwkt_reltoken(&slot->node.token);
		return (EBUSY);
	}
	if (length == sizeof("events") - 1 &&
	    bcmp(name, "events", sizeof("events") - 1) == 0)
		vnode = slot->descriptor.committed ? slot->events_vnode : NULL;
	else if (length == sizeof("config") - 1 &&
	    bcmp(name, "config", sizeof("config") - 1) == 0)
		vnode = slot->descriptor.committed ? slot->config_vnode : NULL;
	else if (length == sizeof("descriptor") - 1 &&
	    bcmp(name, "descriptor", sizeof("descriptor") - 1) == 0)
		vnode = slot->descriptor_vnode;
	else {
		resources = slot->resources;
		if (resources != NULL)
			vmmfs_node_hold((struct vmmfs_node *)resources);
		lwkt_reltoken(&slot->node.token);
		vnode = NULL;
		if (resources != NULL) {
			error = VMMFS_WORK(resources, vmmfs_pcislot_resources_lookup(resources,
			    name, length, &vnode));
			vmmfs_node_put((struct vmmfs_node *)resources);
			if (error != 0 && error != ENOENT)
				return (error);
		}
		goto resolved;
	}
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&slot->node.token);
resolved:
	if (vnode == NULL) {
		return (ENOENT);
	}
	*vnodep = vnode;
	return (0);
}

static int
vmmfs_pcislot_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_pcislot *slot = ap->a_dvp->v_data;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vnode *vnode;
	int error;

	error = VMMFS_WORK(slot, vmmfs_pcislot_get_item(slot,
	    ncp->nc_name, ncp->nc_nlen, &vnode));
	if (error != 0) {
		cache_setvp(ap->a_nch, NULL);
		return (error);
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
		stop = vop_write_dirent(&error, uio, slot->node.inode, DT_DIR, 1, ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, vmmfs_pcislot_pciroot(slot)->node.inode, DT_DIR,
		    2, "..");
		if (!stop)
			offset = 2;
	}
	index = offset - 2;
	while (!stop) {
		error = VMMFS_WORK(slot, vmmfs_pcislot_read_item(slot, index, &item));
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
vmmfs_pcislot_read_item(struct vmmfs_pcislot *slot, uint64_t index,
	struct vmmfs_pcislot_item *item)
{
	struct vmmfs_pcislot_resources *resources;
	size_t name_length;
	int error;

	lwkt_gettoken(&slot->node.token);
	if (slot->node.dead) {
		lwkt_reltoken(&slot->node.token);
		return (ENOENT);
	}
	if (slot->descriptor.updating) {
		lwkt_reltoken(&slot->node.token);
		return (EBUSY);
	}
	if (slot->descriptor.committed) {
		if (index == 0) {
			item->inode = slot->events.node.inode;
			item->type = DT_REG;
			bcopy("events", item->name, sizeof("events"));
			lwkt_reltoken(&slot->node.token);
			return (0);
		}
		--index;
		if (index == 0) {
			item->inode = slot->config.node.inode;
			item->type = DT_REG;
			bcopy("config", item->name, sizeof("config"));
			lwkt_reltoken(&slot->node.token);
			return (0);
		}
		--index;
	}
	if (slot->descriptor_vnode != NULL) {
		if (index == 0) {
			item->inode = slot->descriptor.node.inode;
			item->type = DT_REG;
			bcopy("descriptor", item->name, sizeof("descriptor"));
			lwkt_reltoken(&slot->node.token);
			return (0);
		}
		--index;
	}
	resources = slot->resources;
	if (resources == NULL) {
		lwkt_reltoken(&slot->node.token);
		return (ENOENT);
	}
	vmmfs_node_hold((struct vmmfs_node *)resources);
	lwkt_reltoken(&slot->node.token);
	error = VMMFS_WORK(resources, vmmfs_pcislot_resources_read_item(resources, index,
	    &item->inode, item->name, sizeof(item->name), &name_length));
	vmmfs_node_put((struct vmmfs_node *)resources);
	if (error == 0)
		item->type = DT_REG;
	return (error);
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
	pciroot = vmmfs_pcislot_pciroot(slot);
	lwkt_gettoken(&pciroot->node.token);
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
	lwkt_reltoken(&pciroot->node.token);
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
	pciroot = vmmfs_pcislot_pciroot(slot);
	lwkt_gettoken(&pciroot->node.token);
	error = vmmfs_pcislot_type0_align(pciroot->mmio_next,
	    slot->descriptor.value.rom_size, &base);
	if (error == 0 && (base >= VMMFS_PCI_ECAM_GPA ||
	    slot->descriptor.value.rom_size > VMMFS_PCI_ECAM_GPA - base))
		error = ENOSPC;
	if (error == 0)
		pciroot->mmio_next = base + slot->descriptor.value.rom_size;
	lwkt_reltoken(&pciroot->node.token);
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
	uint16_t length;
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
		if (offset < base || offset - base >= length)
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
			resources = slot->resources;
			if (resources != NULL) {
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
