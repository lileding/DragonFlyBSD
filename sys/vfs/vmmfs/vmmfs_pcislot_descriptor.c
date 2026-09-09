/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot descriptor transaction node.
 */
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/stdarg.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/limits.h>

#include <sys/libkern.h>
#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_auth.h"
#include "vmmfs_pcislot_descriptor.h"

#define VMMFS_PCISLOT_DESCRIPTOR_MODE 0644

static int vmmfs_pcislot_descriptor_open(struct vop_open_args *);
static int vmmfs_pcislot_descriptor_load(struct vmmfs_node *, char *, size_t, size_t *);
static int vmmfs_pcislot_descriptor_store(struct vmmfs_node *, const char *, size_t);
static int vmmfs_pcislot_descriptor_parse(const char *, size_t, struct vmmfs_pcislot_descriptor_value *);
static int vmmfs_pcislot_descriptor_validate(const struct vmmfs_pcislot_descriptor_value *);
static int vmmfs_pcislot_descriptor_render(struct vmmfs_pcislot_descriptor_value *);
static bool vmmfs_pcislot_descriptor_ranges_overlap(uint64_t, uint64_t,
	uint64_t, uint64_t);
static void vmmfs_pcislot_descriptor_drop(struct vmmfs_node *);

static bool
vmmfs_pcislot_descriptor_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_descriptor *descriptor =
	    (struct vmmfs_pcislot_descriptor *)node;
	struct vmmfs_pcislot *slot = vmmfs_pcislot_descriptor_slot(descriptor);
	struct vmmfs_pcislot_auth *auth;
	int error;

	lwkt_gettoken(&slot->token);
	while (descriptor->updating) {
		error = tsleep(descriptor, 0, "vmmdescdrain", 0);
		if (error != 0)
			kprintf("vmmfs: descriptor close drain: %d\n", error);
	}
	auth = descriptor->auth;
	descriptor->auth = NULL;
	descriptor->committed = false;
	lwkt_reltoken(&slot->token);
	vmmfs_pcislot_auth_revoke(auth);
	return (true);
}

struct vop_ops vmmfs_pcislot_descriptor_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_pcislot_descriptor_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_node_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_setattr = vmmfs_node_setattr,
	.vop_write = vmmfs_node_write,
};

int
vmmfs_pcislot_descriptor_init(struct vmmfs_node *parent,
	struct vmmfs_pcislot_descriptor *descriptor)
{
	struct vmmfs_root *root;
	int error;

	if (parent == NULL || descriptor == NULL)
		return (EINVAL);
	root = (struct vmmfs_root *)parent->mount->root;
	bzero(descriptor, sizeof(*descriptor));
	descriptor->node.inode = vmmfs_root_allocate_inode(root);
	descriptor->node.parent = parent;
	descriptor->node.mount = parent->mount;
	descriptor->node.dead = false;
	descriptor->node.references = 1;
	lockinit(&descriptor->node.lock, "vmmfsnode", 0, 0);
	descriptor->node.drop = vmmfs_pcislot_descriptor_drop;
	vmmfs_node_hold(parent);
	descriptor->node.load_limit = VMMFS_PCISLOT_DESCRIPTOR_MAX;
	descriptor->node.store_limit = VMMFS_PCI_DESCRIPTOR_MAX;
	descriptor->node.load = vmmfs_pcislot_descriptor_load;
	descriptor->node.store = vmmfs_pcislot_descriptor_store;
	descriptor->node.mode = VMMFS_PCISLOT_DESCRIPTOR_MODE;
	descriptor->node.size = 0;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->pcislot_descriptor_vops, VREG, &descriptor->node);
	if (error != 0)
		vmmfs_node_put(&descriptor->node);
	else
		descriptor->node.deactivate = vmmfs_pcislot_descriptor_deactivate;
	return (error);
}

static void
vmmfs_pcislot_descriptor_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_descriptor *descriptor =
	    (struct vmmfs_pcislot_descriptor *)node;

	KKASSERT(!descriptor->updating);
	KKASSERT(descriptor->auth == NULL);
	bzero(descriptor, sizeof(*descriptor));
}
static int
vmmfs_pcislot_descriptor_writable(struct vmmfs_pcislot_descriptor *descriptor)
{
	struct vmmfs_machine *machine;

	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(
	    vmmfs_pcislot_descriptor_slot(descriptor)));
	lwkt_gettoken(&machine->token);
	if (machine->machine != NULL) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->token);
	return (0);
}

static int
vmmfs_pcislot_descriptor_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_descriptor *descriptor = ap->a_vp->v_data;
	int error = 0;

	if ((ap->a_mode & FWRITE) != 0)
		error = VMMFS_WORK(descriptor,
		    vmmfs_pcislot_descriptor_writable(descriptor));
	return (error == 0 ? vop_stdopen(ap) : error);
}

static int
vmmfs_pcislot_descriptor_load(struct vmmfs_node *node, char *buffer,
	size_t capacity, size_t *lengthp)
{
	struct vmmfs_pcislot_descriptor *descriptor;
	struct vmmfs_pcislot *slot;

	descriptor = (struct vmmfs_pcislot_descriptor *)node;
	if (descriptor == NULL)
		return (ENOENT);
	slot = vmmfs_pcislot_descriptor_slot(descriptor);
	if (capacity < VMMFS_PCISLOT_DESCRIPTOR_MAX)
		return (EOVERFLOW);
	lwkt_gettoken(&slot->token);
	*lengthp = descriptor->committed ? descriptor->value.length : 0;
	if (*lengthp != 0)
		bcopy(descriptor->value.text, buffer, *lengthp);
	lwkt_reltoken(&slot->token);
	return (0);
}

static int
vmmfs_pcislot_descriptor_store(struct vmmfs_node *node, const char *text,
	size_t length)
{
	struct vmmfs_pcislot_descriptor *descriptor;
	struct vmmfs_pcislot_descriptor_value *value;
	struct vmmfs_pcislot_auth *new_auth;
	struct vmmfs_pcislot_auth *old_auth;
	struct vmmfs_machine *machine;
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pcislot *slot;
	struct file *file;
	int fd;
	uint64_t generation;
	bool removing;
	bool updating;
	bool committed;
	int error;
	descriptor = (struct vmmfs_pcislot_descriptor *)node;
	if (descriptor == NULL)
		return (ENOENT);
	slot = vmmfs_pcislot_descriptor_slot(descriptor);
	pciroot = vmmfs_pcislot_pciroot(slot);
	machine = vmmfs_pciroot_machine(pciroot);
	file = NULL;
	fd = -1;
	value = NULL;
	new_auth = NULL;
	updating = false;
	removing = length == 0;
	if (!removing) {
		value = kmalloc(sizeof(*value), M_VMMFS, M_WAITOK | M_ZERO);
		error = vmmfs_pcislot_descriptor_parse(text, length, value);
		if (error != 0)
			goto failed;
	}
	lwkt_gettoken(&slot->token);
	lwkt_gettoken(&machine->token);
	if (machine->machine != NULL || descriptor->updating ||
	    descriptor->generation == UINT64_MAX) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&slot->token);
		error = EBUSY;
		goto failed;
	}
	descriptor->updating = true;
	updating = true;
	generation = descriptor->generation + 1;
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&slot->token);
	if (!removing) {
		error = vmmfs_pcislot_auth_create(slot, generation,
		    &new_auth, &file);
		if (error != 0)
			goto failed;
		error = fdalloc(curproc, 0, &fd);
		if (error != 0)
			goto failed;
	}

	/* Commit once; no fallible operation follows the ownership transfer. */
	lwkt_gettoken(&slot->token);
	lwkt_gettoken(&machine->token);
	if (machine->machine != NULL) {
		error = EBUSY;
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&slot->token);
		goto failed;
	}
	old_auth = descriptor->auth;
	descriptor->auth = new_auth;
	if (removing) {
		bzero(&descriptor->value, sizeof(descriptor->value));
		descriptor->committed = false;
		descriptor->node.size = 0;
	} else {
		descriptor->value = *value;
		descriptor->committed = true;
		descriptor->node.size = (off_t)descriptor->value.length;
	}
	descriptor->generation = generation;
	committed = !removing;
	/* Publication cannot fail; until now the reserved fd had no file. */
	if (file != NULL)
		fsetfd(curproc->p_fd, file, fd);
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&slot->token);
	vmmfs_pcislot_auth_revoke(old_auth);
	vmmfs_pcislot_config_descriptor_changed(&slot->config, generation,
	    committed);
	vmmfs_pciroot_invalidate_slot(pciroot, slot);

	error = 0;
	goto finished;

failed:
	if (fd >= 0)
		fsetfd(curproc->p_fd, NULL, fd);
	vmmfs_pcislot_auth_revoke(new_auth);
finished:
	if (updating) {
		lwkt_gettoken(&slot->token);
		descriptor->updating = false;
		lwkt_reltoken(&slot->token);
		wakeup(descriptor);
	}
	if (value != NULL)
		kfree(value, M_VMMFS);
	if (file != NULL)
		fdrop(file);
	return (error);
}

static int
vmmfs_pcislot_descriptor_parse(const char *text, size_t length, struct vmmfs_pcislot_descriptor_value *value)
{
	struct vmmfs_pci_descriptor header;
	struct vmmfs_pci_doorbell doorbell;
	struct vmmfs_pci_register reg;
	struct vmmfs_pci_capability cap, expected;
	struct vmmfs_pci_ext_capability ecap;
	const char *cursor;
	size_t size;
	uint32_t config_offset, cap_size;
	unsigned int index;
	int error;
	static const uint8_t zero[6];

	if (length < sizeof(header))
		return (EINVAL);
	bcopy(text, &header, sizeof(header));
	if (header.version != VMMFS_PCI_DESCRIPTOR_VERSION || header.reserved ||
	    header.class_code > 0xffffff || header.intx_pin > 4 ||
	    header.doorbell_count > VMMFS_PCI_MAX_DOORBELLS ||
	    header.config_count > VMMFS_PCI_MAX_CONFIGS ||
	    header.cap_count > VMMFS_PCI_MAX_CAPS ||
	    header.ecap_count > VMMFS_PCI_MAX_ECAPS ||
	    header.data_size > VMMFS_PCI_MAX_DATA)
		return (EINVAL);
	/* Bounded counts make this sum overflow-free on every host ABI. */
	size = sizeof(header) + header.doorbell_count * sizeof(doorbell) +
	    header.config_count * sizeof(reg) + header.cap_count * sizeof(cap) +
	    header.ecap_count * sizeof(ecap) + header.data_size;
	if (length != size)
		return (EINVAL);
	bzero(value, sizeof(*value));
	value->vendor_id = header.vendor_id;
	value->device_id = header.device_id;
	value->subsystem_vendor_id = header.subsystem_vendor_id;
	value->subsystem_device_id = header.subsystem_device_id;
	value->class = header.class_code;
	value->revision = header.revision;
	value->intx_pin = header.intx_pin;
	value->rom_size = header.rom_size;
	value->rom_present = header.rom_size != 0;
	if (header.rom_size != 0 && (header.rom_size < PAGE_SIZE ||
	    header.rom_size > (1ULL << 32) || !powerof2(header.rom_size)))
		return (EINVAL);
	for (index = 0; index < VMMFS_PCI_MAX_BARS; ++index) {
		const struct vmmfs_pci_bar *bar = &header.bars[index];
		if (bcmp(bar->reserved, zero, sizeof(bar->reserved)) != 0 ||
		    bar->prefetchable > 1)
			return (EINVAL);
		if (bar->size == 0) {
			if (bar->type != 0 || bar->prefetchable != 0)
				return (EINVAL);
			continue;
		}
		if (!powerof2(bar->size) || bar->type < VMMFS_PCI_BAR_IO ||
		    bar->type > VMMFS_PCI_BAR_MEM64 ||
		    (bar->type == VMMFS_PCI_BAR_IO && bar->size > 0x10000) ||
		    (bar->type == VMMFS_PCI_BAR_MEM32 && bar->size > (1ULL << 32)))
			return (EINVAL);
		value->bars[index].present = true;
		value->bars[index].type = bar->type;
		value->bars[index].size = bar->size;
		value->bars[index].prefetchable = bar->prefetchable;
	}
	cursor = text + sizeof(header);
	for (index = 0; index < header.doorbell_count; ++index) {
		bcopy(cursor, &doorbell, sizeof(doorbell));
		cursor += sizeof(doorbell);
		if (bcmp(doorbell.reserved, zero, sizeof(doorbell.reserved)) != 0 ||
		    doorbell.bar >= VMMFS_PCI_MAX_BARS || doorbell.size == 0 ||
		    (doorbell.width != 1 && doorbell.width != 2 &&
		    doorbell.width != 4 && doorbell.width != 8) ||
		    (doorbell.space != VMMFS_PCI_CONFIG_MMIO &&
		    doorbell.space != VMMFS_PCI_CONFIG_PIO))
			return (EINVAL);
		value->doorbells[index].present = true;
		value->doorbells[index].bar = doorbell.bar;
		value->doorbells[index].offset = doorbell.offset;
		value->doorbells[index].size = doorbell.size;
		value->doorbells[index].width = doorbell.width;
		value->doorbells[index].space = doorbell.space == VMMFS_PCI_CONFIG_MMIO ?
		    VMMFS_PCISLOT_DOORBELL_MMIO : VMMFS_PCISLOT_DOORBELL_PIO;
	}
	for (index = 0; index < header.config_count; ++index) {
		bcopy(cursor, &reg, sizeof(reg));
		cursor += sizeof(reg);
		if (bcmp(reg.reserved, zero, sizeof(reg.reserved)) != 0 ||
		    reg.bar >= VMMFS_PCI_MAX_BARS ||
		    (reg.width != 1 && reg.width != 2 && reg.width != 4 && reg.width != 8) ||
		    (reg.space != VMMFS_PCI_CONFIG_MMIO && reg.space != VMMFS_PCI_CONFIG_PIO))
			return (EINVAL);
		value->configs[index].present = true;
		value->configs[index].bar = reg.bar;
		value->configs[index].offset = reg.offset;
		value->configs[index].width = reg.width;
		value->configs[index].space = reg.space;
	}
	config_offset = 0x40;
	for (index = 0; index < header.cap_count; ++index) {
		bcopy(cursor, &cap, sizeof(cap));
		cursor += sizeof(cap);
		bzero(&expected, sizeof(expected));
		expected.kind = cap.kind;
		switch (cap.kind) {
		case VMMFS_PCI_CAP_PCIE:
			cap_size = 0x3c;
			break;
		case VMMFS_PCI_CAP_MSI:
			if (cap.vectors == 0 || cap.vectors > 32 || !powerof2(cap.vectors) ||
			    (cap.address_width != 32 && cap.address_width != 64) || cap.maskable > 1)
				return (EINVAL);
			expected.vectors = cap.vectors;
			expected.address_width = cap.address_width;
			expected.maskable = cap.maskable;
			cap_size = 10 + (cap.address_width == 64 ? 4 : 0) + (cap.maskable ? 8 : 0);
			break;
		case VMMFS_PCI_CAP_MSIX:
			if (cap.vectors == 0 || cap.vectors > VMMFS_PCISLOT_MAX_MSIX_VECTORS ||
			    cap.table_bar >= VMMFS_PCI_MAX_BARS || cap.pba_bar >= VMMFS_PCI_MAX_BARS ||
			    cap.table_offset > UINT32_MAX || cap.pba_offset > UINT32_MAX ||
			    (cap.table_offset & 7) != 0 || (cap.pba_offset & 7) != 0 ||
			    (cap.table_bar == cap.pba_bar &&
			    vmmfs_pcislot_descriptor_ranges_overlap(cap.table_offset,
			    (uint64_t)cap.vectors * 16, cap.pba_offset,
			    ((uint64_t)cap.vectors + 63) / 64 * 8)))
				return (EINVAL);
			expected.vectors = cap.vectors;
			expected.table_bar = cap.table_bar;
			expected.pba_bar = cap.pba_bar;
			expected.table_offset = cap.table_offset;
			expected.pba_offset = cap.pba_offset;
			cap_size = 12;
			break;
		case VMMFS_PCI_CAP_BLOB:
			if (cap.data_length == 0 || cap.data_offset > header.data_size ||
			    cap.data_length > header.data_size - cap.data_offset)
				return (EINVAL);
			expected.id = cap.id;
			expected.data_offset = cap.data_offset;
			expected.data_length = cap.data_length;
			cap_size = 3 + cap.data_length;
			break;
		default:
			return (EINVAL);
		}
		/* Reject every field not meaningful for this capability kind. */
		if (bcmp(&cap, &expected, sizeof(cap)) != 0)
			return (EINVAL);
		cap_size = (cap_size + 3) & ~3U;
		if (cap_size > 0x100 - config_offset)
			return (E2BIG);
		config_offset += cap_size;
		value->caps[index].present = true;
		value->caps[index].kind = cap.kind - 1;
		value->caps[index].vectors = cap.vectors;
		value->caps[index].address_width = cap.address_width;
		value->caps[index].maskable = cap.maskable;
		value->caps[index].table_bar = cap.table_bar;
		value->caps[index].pba_bar = cap.pba_bar;
		value->caps[index].table_offset = cap.table_offset;
		value->caps[index].pba_offset = cap.pba_offset;
		value->caps[index].id = cap.id;
		value->caps[index].data_offset = cap.data_offset;
		value->caps[index].data_length = cap.data_length;
	}
	config_offset = 0x100;
	for (index = 0; index < header.ecap_count; ++index) {
		bcopy(cursor, &ecap, sizeof(ecap));
		cursor += sizeof(ecap);
		if (bcmp(ecap.reserved, zero, sizeof(ecap.reserved)) != 0 ||
		    ecap.version == 0 || ecap.version > 15 || ecap.data_length == 0 ||
		    ecap.data_offset > header.data_size ||
		    ecap.data_length > header.data_size - ecap.data_offset)
			return (EINVAL);
		cap_size = (4 + ecap.data_length + 3) & ~3U;
		if (cap_size > 0x1000 - config_offset)
			return (E2BIG);
		config_offset += cap_size;
		value->ecaps[index].present = true;
		value->ecaps[index].id = ecap.id;
		value->ecaps[index].version = ecap.version;
		value->ecaps[index].data_offset = ecap.data_offset;
		value->ecaps[index].data_length = ecap.data_length;
	}
	bcopy(cursor, value->cap_data, header.data_size);
	value->cap_data_length = header.data_size;
	error = vmmfs_pcislot_descriptor_validate(value);
	if (error == 0)
		error = vmmfs_pcislot_descriptor_render(value);
	return (error);
}

static int
vmmfs_pcislot_descriptor_validate(const struct vmmfs_pcislot_descriptor_value *value)
{
	uint64_t range_size;
	unsigned int bar, index, other, cap, pcie_caps;
	uint8_t cap_kind;

	for (bar = 0; bar < VMMFS_PCISLOT_MAX_BARS; ++bar) {
		if (!value->bars[bar].present)
			continue;
		if ((value->bars[bar].type == VMMFS_PCISLOT_BAR_IO &&
		    value->bars[bar].size < 4) ||
		    (value->bars[bar].type != VMMFS_PCISLOT_BAR_IO &&
		    value->bars[bar].size < 16))
			return (EINVAL);
		if (value->bars[bar].type == VMMFS_PCISLOT_BAR_IO &&
		    value->bars[bar].prefetchable)
			return (EINVAL);
		if (value->bars[bar].type == VMMFS_PCISLOT_BAR_MEM64 &&
		    (bar == VMMFS_PCISLOT_MAX_BARS - 1 || value->bars[bar + 1].present))
			return (EINVAL);
	}
	for (index = 0; index < VMMFS_PCISLOT_MAX_DOORBELLS; ++index) {
		if (!value->doorbells[index].present)
			continue;
		if (!value->bars[value->doorbells[index].bar].present ||
		    value->doorbells[index].offset > value->bars[
		    value->doorbells[index].bar].size || value->doorbells[index].size >
		    value->bars[value->doorbells[index].bar].size -
		    value->doorbells[index].offset ||
		    (value->doorbells[index].offset % value->doorbells[index].width) != 0 ||
		    (value->doorbells[index].size % value->doorbells[index].width) != 0 ||
		    (value->bars[value->doorbells[index].bar].type ==
		    VMMFS_PCISLOT_BAR_IO) != (value->doorbells[index].space ==
		    VMMFS_PCISLOT_DOORBELL_PIO) ||
		    (value->doorbells[index].space == VMMFS_PCISLOT_DOORBELL_PIO &&
		    value->doorbells[index].width == 8))
			return (EINVAL);
		for (other = 0; other < index; ++other) {
			if (!value->doorbells[other].present ||
			    value->doorbells[other].bar != value->doorbells[index].bar)
				continue;
			if (vmmfs_pcislot_descriptor_ranges_overlap(
			    value->doorbells[other].offset, value->doorbells[other].size,
			    value->doorbells[index].offset, value->doorbells[index].size))
				return (EINVAL);
		}
	}
	for (index = 0; index < VMMFS_PCISLOT_MAX_CONFIGS; ++index) {
		if (!value->configs[index].present)
			continue;
		if (!value->bars[value->configs[index].bar].present ||
		    value->configs[index].offset > value->bars[
		    value->configs[index].bar].size || value->configs[index].width >
		    value->bars[value->configs[index].bar].size -
		    value->configs[index].offset ||
		    (value->configs[index].offset % value->configs[index].width) != 0 ||
		    (value->bars[value->configs[index].bar].type ==
		    VMMFS_PCISLOT_BAR_IO) != (value->configs[index].space ==
		    VMMFS_PCISLOT_CONFIG_PIO) ||
		    (value->configs[index].space == VMMFS_PCISLOT_CONFIG_PIO &&
		    value->configs[index].width == 8))
			return (EINVAL);
		for (other = 0; other < index; ++other) {
			if (value->configs[other].bar != value->configs[index].bar)
				continue;
			if (vmmfs_pcislot_descriptor_ranges_overlap(
			    value->configs[other].offset, value->configs[other].width,
			    value->configs[index].offset, value->configs[index].width))
				return (EINVAL);
		}
		for (other = 0; other < VMMFS_PCISLOT_MAX_DOORBELLS; ++other) {
			if (!value->doorbells[other].present ||
			    value->doorbells[other].bar != value->configs[index].bar)
				continue;
			if (vmmfs_pcislot_descriptor_ranges_overlap(
			    value->doorbells[other].offset, value->doorbells[other].size,
			    value->configs[index].offset, value->configs[index].width))
				return (EINVAL);
		}
	}
	pcie_caps = 0;
	for (cap = 0; cap < VMMFS_PCISLOT_MAX_CAPS; ++cap) {
		if (!value->caps[cap].present)
			continue;
		cap_kind = value->caps[cap].kind;
		if (cap_kind == VMMFS_PCISLOT_CAP_PCIE && ++pcie_caps != 1)
			return (EINVAL);
		if (cap_kind == VMMFS_PCISLOT_CAP_MSI &&
		    (value->caps[cap].vectors > 32 ||
		    (value->caps[cap].vectors & (value->caps[cap].vectors - 1)) != 0))
			return (EINVAL);
		if (cap_kind != VMMFS_PCISLOT_CAP_MSIX)
			continue;
		if (!value->bars[value->caps[cap].table_bar].present ||
		    !value->bars[value->caps[cap].pba_bar].present ||
		    value->bars[value->caps[cap].table_bar].type == VMMFS_PCISLOT_BAR_IO ||
		    value->bars[value->caps[cap].pba_bar].type == VMMFS_PCISLOT_BAR_IO)
			return (EINVAL);
		range_size = (uint64_t)value->caps[cap].vectors * 16;
		if (value->caps[cap].table_offset > value->bars[
		    value->caps[cap].table_bar].size || range_size > value->bars[
		    value->caps[cap].table_bar].size - value->caps[cap].table_offset)
			return (EINVAL);
		range_size = ((uint64_t)value->caps[cap].vectors + 63) / 64 * 8;
		if (value->caps[cap].pba_offset > value->bars[
		    value->caps[cap].pba_bar].size || range_size > value->bars[
		    value->caps[cap].pba_bar].size - value->caps[cap].pba_offset)
			return (EINVAL);
		for (index = 0; index < VMMFS_PCISLOT_MAX_DOORBELLS; ++index) {
			if (!value->doorbells[index].present)
				continue;
			if ((value->doorbells[index].bar == value->caps[cap].table_bar &&
			    vmmfs_pcislot_descriptor_ranges_overlap(value->doorbells[index].offset,
			    value->doorbells[index].size, value->caps[cap].table_offset,
			    (uint64_t)value->caps[cap].vectors * 16)) ||
			    (value->doorbells[index].bar == value->caps[cap].pba_bar &&
			    vmmfs_pcislot_descriptor_ranges_overlap(value->doorbells[index].offset,
			    value->doorbells[index].size, value->caps[cap].pba_offset,
			    ((uint64_t)value->caps[cap].vectors + 63) / 64 * 8)))
				return (EINVAL);
		}
		for (index = 0; index < VMMFS_PCISLOT_MAX_CONFIGS; ++index) {
			if (!value->configs[index].present)
				continue;
			if ((value->configs[index].bar == value->caps[cap].table_bar &&
			    vmmfs_pcislot_descriptor_ranges_overlap(value->configs[index].offset,
			    value->configs[index].width, value->caps[cap].table_offset,
			    (uint64_t)value->caps[cap].vectors * 16)) ||
			    (value->configs[index].bar == value->caps[cap].pba_bar &&
			    vmmfs_pcislot_descriptor_ranges_overlap(value->configs[index].offset,
			    value->configs[index].width, value->caps[cap].pba_offset,
			    ((uint64_t)value->caps[cap].vectors + 63) / 64 * 8)))
				return (EINVAL);
		}
	}
	return (0);
}

static int
vmmfs_pcislot_descriptor_print(struct vmmfs_pcislot_descriptor_value *value,
	const char *format, ...)
{
	va_list args;
	size_t available = sizeof(value->text) - value->length;
	int length;

	va_start(args, format);
	length = kvsnprintf(value->text + value->length, available, format, args);
	va_end(args);
	if (length < 0 || (size_t)length >= available)
		return (E2BIG);
	value->length += length;
	return (0);
}

static int
vmmfs_pcislot_descriptor_render(struct vmmfs_pcislot_descriptor_value *value)
{
	static const char * const types[] = { "", "io", "mem32", "mem64" };
	static const char * const pins[] = { "none", "a", "b", "c", "d" };
	static const char * const kinds[] = { "pcie", "msi", "msix", "blob" };
	unsigned int index, byte;
	int error;

#define EMIT(...) do { \
	error = vmmfs_pcislot_descriptor_print(value, __VA_ARGS__); \
	if (error != 0) return (error); \
} while (0)
	EMIT("version=1\nheader.type=endpoint\nvendor_id=0x%04x\n"
	    "device_id=0x%04x\nsubsystem_vendor_id=0x%04x\nsubsystem_device_id=0x%04x\n"
	    "class=0x%06x\nrevision=%u\nintx.pin=%s\n", value->vendor_id,
	    value->device_id, value->subsystem_vendor_id, value->subsystem_device_id,
	    value->class, value->revision, pins[value->intx_pin]);
	for (index = 0; index < VMMFS_PCISLOT_MAX_BARS; ++index) {
		const struct vmmfs_pcislot_bar *item = &value->bars[index];
		if (!item->present)
			continue;
		EMIT("bar%u.type=%s\n", index, types[item->type]);
		EMIT("bar%u.size=0x%jx\n", index, (uintmax_t)item->size);
		EMIT("bar%u.prefetchable=%u\n", index, item->prefetchable);
	}
	for (index = 0; index < VMMFS_PCISLOT_MAX_DOORBELLS; ++index) {
		const struct vmmfs_pcislot_doorbell *item = &value->doorbells[index];
		if (!item->present)
			continue;
		EMIT("doorbell%u.bar=%u\n", index, item->bar);
		EMIT("doorbell%u.offset=0x%jx\n", index, (uintmax_t)item->offset);
		EMIT("doorbell%u.size=0x%jx\n", index, (uintmax_t)item->size);
		EMIT("doorbell%u.width=%u\n", index, item->width);
		EMIT("doorbell%u.space=%s\n", index, item->space == VMMFS_PCISLOT_DOORBELL_MMIO ? "mmio" : "pio");
	}
	for (index = 0; index < VMMFS_PCISLOT_MAX_CONFIGS; ++index) {
		const struct vmmfs_pcislot_config_register *item = &value->configs[index];
		if (!item->present)
			continue;
		EMIT("config%u.bar=%u\n", index, item->bar);
		EMIT("config%u.offset=0x%jx\n", index, (uintmax_t)item->offset);
		EMIT("config%u.width=%u\n", index, item->width);
		EMIT("config%u.space=%s\n", index, item->space == VMMFS_PCISLOT_CONFIG_MMIO ? "mmio" : "pio");
	}
	if (value->rom_present)
		EMIT("rom.size=0x%jx\n", (uintmax_t)value->rom_size);
	for (index = 0; index < VMMFS_PCISLOT_MAX_CAPS; ++index) {
		const struct vmmfs_pcislot_cap *cap = &value->caps[index];
		if (!cap->present)
			break;
		EMIT("cap%u.kind=%s\n", index, kinds[cap->kind]);
		switch (cap->kind) {
		case VMMFS_PCISLOT_CAP_PCIE:
			break;
		case VMMFS_PCISLOT_CAP_MSI:
			EMIT("cap%u.vectors=%u\ncap%u.address_width=%u\ncap%u.maskable=%u\n",
			    index, cap->vectors, index, cap->address_width, index, cap->maskable);
			break;
		case VMMFS_PCISLOT_CAP_MSIX:
			EMIT("cap%u.vectors=%u\ncap%u.table.bar=%u\ncap%u.table.offset=0x%jx\n"
			    "cap%u.pba.bar=%u\ncap%u.pba.offset=0x%jx\n", index, cap->vectors,
			    index, cap->table_bar, index, (uintmax_t)cap->table_offset,
			    index, cap->pba_bar, index, (uintmax_t)cap->pba_offset);
			break;
		case VMMFS_PCISLOT_CAP_BLOB:
			EMIT("cap%u.id=0x%02x\ncap%u.access=static\ncap%u.data=", index, cap->id, index, index);
			for (byte = 0; byte < cap->data_length; ++byte)
				EMIT("%02x", value->cap_data[cap->data_offset + byte]);
			EMIT("\n");
			break;
		}
	}
	for (index = 0; index < VMMFS_PCISLOT_MAX_ECAPS; ++index) {
		const struct vmmfs_pcislot_ecap *cap = &value->ecaps[index];
		if (!cap->present)
			break;
		EMIT("ecap%u.id=0x%04x\necap%u.version=%u\necap%u.access=static\necap%u.data=",
		    index, cap->id, index, cap->version, index, index);
		for (byte = 0; byte < cap->data_length; ++byte)
			EMIT("%02x", value->cap_data[cap->data_offset + byte]);
		EMIT("\n");
	}
#undef EMIT
	return (0);
}

static bool
vmmfs_pcislot_descriptor_ranges_overlap(uint64_t left_offset,
	uint64_t left_size, uint64_t right_offset, uint64_t right_size)
{

	if (left_size == 0 || right_size == 0 ||
	    left_offset > UINT64_MAX - left_size ||
	    right_offset > UINT64_MAX - right_size)
		return (true);
	return (left_offset < right_offset + right_size &&
	    right_offset < left_offset + left_size);
}
