/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot descriptor transaction node.
 */
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
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
#include "vmmfs_pcislot_resource.h"
#include "vmmfs_pcislot_events.h"

#define VMMFS_PCISLOT_DESCRIPTOR_MODE 0644

#define VMMFS_DESCRIPTOR_VERSION 0x0001
#define VMMFS_DESCRIPTOR_VENDOR 0x0002
#define VMMFS_DESCRIPTOR_DEVICE 0x0004
#define VMMFS_DESCRIPTOR_SUBSYSTEM_VENDOR 0x0008
#define VMMFS_DESCRIPTOR_SUBSYSTEM_DEVICE 0x0010
#define VMMFS_DESCRIPTOR_CLASS 0x0020
#define VMMFS_DESCRIPTOR_REVISION 0x0040
#define VMMFS_DESCRIPTOR_HEADER_TYPE 0x0080
#define VMMFS_DESCRIPTOR_INTX_PIN 0x0100

#define VMMFS_DESCRIPTOR_BAR_TYPE 0x01
#define VMMFS_DESCRIPTOR_BAR_SIZE 0x02
#define VMMFS_DESCRIPTOR_BAR_PREFETCHABLE 0x04

#define VMMFS_DESCRIPTOR_DOORBELL_BAR 0x01
#define VMMFS_DESCRIPTOR_DOORBELL_OFFSET 0x02
#define VMMFS_DESCRIPTOR_DOORBELL_WIDTH 0x04
#define VMMFS_DESCRIPTOR_DOORBELL_SIZE 0x08
#define VMMFS_DESCRIPTOR_DOORBELL_SPACE 0x10

#define VMMFS_DESCRIPTOR_CONFIG_BAR 0x01
#define VMMFS_DESCRIPTOR_CONFIG_OFFSET 0x02
#define VMMFS_DESCRIPTOR_CONFIG_WIDTH 0x04
#define VMMFS_DESCRIPTOR_CONFIG_SPACE 0x08

#define VMMFS_DESCRIPTOR_CAP_KIND 0x0001
#define VMMFS_DESCRIPTOR_CAP_VECTORS 0x0002
#define VMMFS_DESCRIPTOR_CAP_ADDRESS_WIDTH 0x0004
#define VMMFS_DESCRIPTOR_CAP_MASKABLE 0x0008
#define VMMFS_DESCRIPTOR_CAP_TABLE_BAR 0x0010
#define VMMFS_DESCRIPTOR_CAP_TABLE_OFFSET 0x0020
#define VMMFS_DESCRIPTOR_CAP_PBA_BAR 0x0040
#define VMMFS_DESCRIPTOR_CAP_PBA_OFFSET 0x0080
#define VMMFS_DESCRIPTOR_CAP_ID 0x0100
#define VMMFS_DESCRIPTOR_CAP_ACCESS 0x0200
#define VMMFS_DESCRIPTOR_CAP_DATA 0x0400

#define VMMFS_DESCRIPTOR_ECAP_ID 0x01
#define VMMFS_DESCRIPTOR_ECAP_VERSION 0x02
#define VMMFS_DESCRIPTOR_ECAP_ACCESS 0x04
#define VMMFS_DESCRIPTOR_ECAP_DATA 0x08

static int vmmfs_pcislot_descriptor_open(struct vop_open_args *);
static int vmmfs_pcislot_descriptor_read(struct vop_read_args *);
static int vmmfs_pcislot_descriptor_setattr(struct vop_setattr_args *);
static int vmmfs_pcislot_descriptor_write(struct vop_write_args *);
static int vmmfs_pcislot_descriptor_parse(struct vmmfs_pcislot *,
	const char *, size_t, struct vmmfs_pcislot_descriptor_value *);
static int vmmfs_pcislot_descriptor_parse_number(const char *, size_t,
	uint64_t *);
static int vmmfs_pcislot_descriptor_parse_boolean(const char *, size_t,
	bool *);
static int vmmfs_pcislot_descriptor_parse_bytes(const char *, size_t,
	struct vmmfs_pcislot_descriptor_value *, uint16_t *, uint16_t *);
static int vmmfs_pcislot_descriptor_index_key(const char *, size_t,
	const char *, const char *, unsigned int, unsigned int *);
static bool vmmfs_pcislot_descriptor_key(const char *, size_t, const char *);
static bool vmmfs_pcislot_descriptor_ranges_overlap(uint64_t, uint64_t,
	uint64_t, uint64_t);
static void vmmfs_pcislot_descriptor_drop(struct vmmfs_node *);

struct vop_ops vmmfs_pcislot_descriptor_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_pcislot_descriptor_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_descriptor_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_setattr = vmmfs_pcislot_descriptor_setattr,
	.vop_write = vmmfs_pcislot_descriptor_write,
};

int
vmmfs_pcislot_descriptor_init(struct vmmfs_mount *mount, struct vmmfs_branch *parent,
	struct vmmfs_pcislot_descriptor *descriptor, struct vnode **vnodep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	struct vmmfs_pcislot *slot;
	int error;

	if (mount == NULL || parent == NULL || descriptor == NULL || vnodep == NULL)
		return (EINVAL);
	slot = (struct vmmfs_pcislot *)parent;
	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot));
	root = mount->root_vnode == NULL ? NULL : mount->root_vnode->v_data;
	if (machine == NULL || root == NULL)
		return (ENXIO);
	*vnodep = NULL;
		return (ENXIO);
	bzero(descriptor, sizeof(*descriptor));
	lwkt_gettoken(&machine->branch.token);
	if (machine->machine != NULL) {
		lwkt_reltoken(&machine->branch.token);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->branch.token);
	descriptor->inode = vmmfs_root_allocate_inode(root);
	vmmfs_node_setup(&descriptor->node, parent,
	    vmmfs_pcislot_descriptor_drop);
	vmmfs_node_set_metadata(&descriptor->node, descriptor->inode,
	    VMMFS_PCISLOT_DESCRIPTOR_MODE, 0);
	error = vmmfs_vnode_create_regular(mount->mount,
	    &mount->pcislot_descriptor_vops, VREG, &descriptor->node, vnodep);
	if (error != 0)
		vmmfs_node_drop(&descriptor->node);
	return (error);
}

static void
vmmfs_pcislot_descriptor_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_descriptor *descriptor;
	struct vmmfs_pcislot_resources *resources;
	struct vmmfs_pcislot_auth *auth;
	struct vmmfs_machine *machine;
	struct vmmfs_pcislot *slot;

	descriptor = (struct vmmfs_pcislot_descriptor *)node;
	KKASSERT(descriptor != NULL);
	slot = vmmfs_pcislot_descriptor_slot(descriptor);
	KKASSERT(slot != NULL);
	if (vmmfs_pcislot_pciroot(slot) == NULL)
		panic("vmmfs_pcislot_descriptor_drop: slot lost its PCI root");
	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot));
	if (machine == NULL)
		panic("vmmfs_pcislot_descriptor_drop: PCI root lost its machine");
	lwkt_gettoken(&slot->branch.token);
	if (descriptor->updating) {
		lwkt_reltoken(&slot->branch.token);
		panic("vmmfs_pcislot_descriptor_drop: update is still active");
	}
	resources = descriptor->resources;
	auth = descriptor->auth;
	descriptor->resources = NULL;
	descriptor->auth = NULL;
	descriptor->committed = false;
	lwkt_reltoken(&slot->branch.token);
	vmmfs_pcislot_resources_deactivate(resources);
	vmmfs_pcislot_auth_revoke(auth);
	vmmfs_node_parent_put(node);
	bzero(descriptor, sizeof(*descriptor));
}
static int
vmmfs_pcislot_descriptor_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_descriptor *descriptor;
	struct vmmfs_machine *machine;

	descriptor = ap->a_vp->v_data;
	if (descriptor == NULL || vmmfs_pcislot_descriptor_slot(descriptor) == NULL ||
	    vmmfs_pcislot_pciroot(vmmfs_pcislot_descriptor_slot(descriptor)) == NULL ||
	    vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(vmmfs_pcislot_descriptor_slot(descriptor))) == NULL)
		return (ENOENT);
	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(vmmfs_pcislot_descriptor_slot(descriptor)));
	if (descriptor->node.dead)
		return (ENOENT);
	if ((ap->a_mode & FWRITE) == 0)
		return (vop_stdopen(ap));
	lwkt_gettoken(&machine->branch.token);
	if (machine->machine != NULL) {
		lwkt_reltoken(&machine->branch.token);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->branch.token);
	return (vop_stdopen(ap));
}

static int
vmmfs_pcislot_descriptor_read(struct vop_read_args *ap)
{
	struct vmmfs_pcislot_descriptor *descriptor;
	struct vmmfs_pcislot *slot;
	char *buffer;
	size_t length;
	off_t offset;
	int error;

	descriptor = ap->a_vp->v_data;
	if (descriptor == NULL || vmmfs_pcislot_descriptor_slot(descriptor) == NULL ||
	    vmmfs_pcislot_pciroot(vmmfs_pcislot_descriptor_slot(descriptor)) == NULL ||
	    vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(vmmfs_pcislot_descriptor_slot(descriptor))) == NULL)
		return (ENOENT);
	if (ap->a_uio->uio_offset < 0)
		return (EINVAL);
	slot = vmmfs_pcislot_descriptor_slot(descriptor);
	if (descriptor->node.dead)
		return (ENOENT);
	buffer = kmalloc(VMMFS_PCISLOT_DESCRIPTOR_MAX, M_VMMFS, M_WAITOK);
	lwkt_gettoken(&slot->branch.token);
	length = descriptor->committed ? descriptor->value.length : 0;
	if (length != 0)
		bcopy(descriptor->value.text, buffer, length);
	lwkt_reltoken(&slot->branch.token);
	offset = ap->a_uio->uio_offset;
	if ((size_t)offset >= length) {
		kfree(buffer, M_VMMFS);
		return (0);
	}
	error = uiomove(buffer + offset, length - (size_t)offset, ap->a_uio);
	kfree(buffer, M_VMMFS);
	return (error);
}


static int
vmmfs_pcislot_descriptor_setattr(struct vop_setattr_args *ap)
{
	/* Accept the O_TRUNC size update before a descriptor write transaction. */
	(void)ap;
	return (0);
}

static int
vmmfs_pcislot_descriptor_write(struct vop_write_args *ap)
{
	struct vmmfs_pcislot_descriptor *descriptor;
	struct vmmfs_pcislot_descriptor_value *value;
	struct vmmfs_pcislot_resources *old_resources;
	struct vmmfs_pcislot_auth *new_auth;
	struct vmmfs_pcislot_auth *old_auth;
	struct vmmfs_machine *machine;
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pcislot *slot;
	char *buffer;
	size_t length;
	uint64_t generation;
	bool removing;
	bool updating;
	bool committed;
	int error;

	descriptor = ap->a_vp->v_data;
	if (descriptor == NULL ||
	    (slot = vmmfs_pcislot_descriptor_slot(descriptor)) == NULL ||
	    (pciroot = vmmfs_pcislot_pciroot(slot)) == NULL ||
	    (machine = vmmfs_pciroot_machine(pciroot)) == NULL)
		return (ENOENT);
	if (descriptor->node.dead)
		return (ENOENT);
	if (ap->a_uio->uio_offset != 0 ||
	    ap->a_uio->uio_resid >= VMMFS_PCISLOT_DESCRIPTOR_MAX)
		return (EINVAL);
	length = (size_t)ap->a_uio->uio_resid;
	buffer = NULL;
	value = NULL;
	new_auth = NULL;
	updating = false;
	removing = length == 0;
	if (!removing) {
		buffer = kmalloc(length, M_VMMFS, M_WAITOK);
		value = kmalloc(sizeof(*value), M_VMMFS, M_WAITOK | M_ZERO);
		error = uiomove(buffer, length, ap->a_uio);
		if (error != 0)
			goto failed;
		error = vmmfs_pcislot_descriptor_parse(vmmfs_pcislot_descriptor_slot(descriptor), buffer,
		    length, value);
		if (error != 0)
			goto failed;
	}
	lwkt_gettoken(&machine->branch.token);
	lwkt_gettoken(&slot->branch.token);
	if (machine->branch.node.dead || slot->branch.node.dead ||
	    descriptor->node.dead) {
		lwkt_reltoken(&slot->branch.token);
		lwkt_reltoken(&machine->branch.token);
		error = ENOENT;
		goto failed;
	}
	if (machine->machine != NULL || descriptor->updating ||
	    descriptor->generation == UINT64_MAX) {
		lwkt_reltoken(&slot->branch.token);
		lwkt_reltoken(&machine->branch.token);
		error = EBUSY;
		goto failed;
	}
	descriptor->updating = true;
	updating = true;
	generation = descriptor->generation + 1;
	lwkt_reltoken(&slot->branch.token);
	lwkt_reltoken(&machine->branch.token);
	if (!removing) {
		error = vmmfs_pcislot_auth_create(slot, generation,
		    &new_auth);
		if (error != 0)
			goto failed;
	}

	/* Block capability lookup while the previous generation is revoked. */
	lwkt_gettoken(&machine->branch.token);
	lwkt_gettoken(&slot->branch.token);
	if (machine->branch.node.dead || slot->branch.node.dead ||
	    descriptor->node.dead || machine->machine != NULL) {
		lwkt_reltoken(&slot->branch.token);
		lwkt_reltoken(&machine->branch.token);
		error = (machine->branch.node.dead || slot->branch.node.dead ||
		    descriptor->node.dead) ? ENOENT : EBUSY;
		goto failed;
	}
	old_resources = descriptor->resources;
	old_auth = descriptor->auth;
	descriptor->resources = NULL;
	descriptor->auth = NULL;
	lwkt_reltoken(&slot->branch.token);
	lwkt_reltoken(&machine->branch.token);
	vmmfs_pcislot_resources_deactivate(old_resources);
	vmmfs_pcislot_auth_revoke(old_auth);
	lwkt_gettoken(&machine->branch.token);
	lwkt_gettoken(&slot->branch.token);
	if (machine->branch.node.dead || slot->branch.node.dead ||
	    descriptor->node.dead || machine->machine != NULL) {
		descriptor->updating = false;
		updating = false;
		lwkt_reltoken(&slot->branch.token);
		lwkt_reltoken(&machine->branch.token);
		error = (machine->branch.node.dead || slot->branch.node.dead ||
		    descriptor->node.dead) ? ENOENT : EBUSY;
		goto failed;
	}
	if (removing) {
		bzero(&descriptor->value, sizeof(descriptor->value));
		descriptor->committed = false;
		descriptor->node.size = 0;
	} else {
		descriptor->value = *value;
		descriptor->committed = true;
		descriptor->node.size = (off_t)descriptor->value.length;
		descriptor->auth = new_auth;
	}
	descriptor->generation = generation;
	descriptor->updating = false;
	updating = false;
	committed = !removing;
	lwkt_reltoken(&slot->branch.token);
	lwkt_reltoken(&machine->branch.token);
	vmmfs_pcislot_events_reset(&slot->events);
	vmmfs_pcislot_config_descriptor_changed(&slot->config, generation,
	    committed);
	vmmfs_pciroot_invalidate_slot(pciroot, slot);
	if (removing) {
		vmmfs_pcislot_events_log(&slot->events,
		    VMMFS_PCI_EVENT_DESCRIPTOR_REMOVED, "generation=%ju",
		    (uintmax_t)generation);
	} else {
		vmmfs_pcislot_events_log(&slot->events,
		    VMMFS_PCI_EVENT_DESCRIPTOR_COMMITTED, "generation=%ju",
		    (uintmax_t)generation);
	}
	if (value != NULL)
		kfree(value, M_VMMFS);
	if (buffer != NULL)
		kfree(buffer, M_VMMFS);
	return (0);

failed:
	vmmfs_pcislot_auth_revoke(new_auth);
	if (updating) {
		lwkt_gettoken(&slot->branch.token);
		descriptor->updating = false;
		lwkt_reltoken(&slot->branch.token);
	}
	if (value != NULL)
		kfree(value, M_VMMFS);
	if (buffer != NULL)
		kfree(buffer, M_VMMFS);
	return (error);
}

static int
vmmfs_pcislot_descriptor_parse(struct vmmfs_pcislot *slot,
	const char *text, size_t length, struct vmmfs_pcislot_descriptor_value *value)
{
	uint32_t fields;
	uint8_t bars[VMMFS_PCISLOT_MAX_BARS];
	uint8_t doorbells[VMMFS_PCISLOT_MAX_DOORBELLS];
	uint8_t configs[VMMFS_PCISLOT_MAX_CONFIGS];
	uint16_t caps[VMMFS_PCISLOT_MAX_CAPS];
	uint8_t ecaps[VMMFS_PCISLOT_MAX_ECAPS];
	const char *line;
	const char *equals;
	const char *cursor;
	const char *end;
	size_t line_length;
	size_t key_length;
	size_t value_length;
	uint64_t number;
	uint64_t range_size;
	size_t line_number;
	unsigned int index;
	unsigned int bar;
	unsigned int other;
	unsigned int cap;
	unsigned int ecap;
	unsigned int pcie_caps;
	bool boolean;
	bool gap;
	uint8_t cap_kind;
	int error;

	if (slot == NULL || text == NULL || value == NULL || length == 0 ||
	    length >= VMMFS_PCISLOT_DESCRIPTOR_MAX || text[length - 1] != '\n')
		return (EINVAL);
	bzero(value, sizeof(*value));
	bzero(bars, sizeof(bars));
	bzero(doorbells, sizeof(doorbells));
	bzero(configs, sizeof(configs));
	bzero(caps, sizeof(caps));
	bzero(ecaps, sizeof(ecaps));
	fields = 0;
	line_number = 0;
	line = text;
	end = text + length;
	while (line < end) {
		equals = NULL;
		for (cursor = line; cursor < end && *cursor != '\n'; ++cursor) {
			if (*cursor == '=') {
				if (equals != NULL)
					return (EINVAL);
				equals = cursor;
			}
		}
		if (cursor == end)
			return (EINVAL);
		line_length = (size_t)(cursor - line);
		if (line_length == 0 || equals == line || equals >= line + line_length)
			return (EINVAL);
		key_length = (size_t)(equals - line);
		value_length = line_length - key_length - 1;
		if (value_length == 0)
			return (EINVAL);
		if (vmmfs_pcislot_descriptor_key(line, key_length, "version")) {
			if (fields & VMMFS_DESCRIPTOR_VERSION)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number != 1)
				return (EINVAL);
			fields |= VMMFS_DESCRIPTOR_VERSION;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length,
		    "vendor_id")) {
			if (fields & VMMFS_DESCRIPTOR_VENDOR)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number > UINT16_MAX)
				return (EINVAL);
			value->vendor_id = number;
			fields |= VMMFS_DESCRIPTOR_VENDOR;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length,
		    "device_id")) {
			if (fields & VMMFS_DESCRIPTOR_DEVICE)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number > UINT16_MAX)
				return (EINVAL);
			value->device_id = number;
			fields |= VMMFS_DESCRIPTOR_DEVICE;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length,
		    "subsystem_vendor_id")) {
			if (fields & VMMFS_DESCRIPTOR_SUBSYSTEM_VENDOR)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number > UINT16_MAX)
				return (EINVAL);
			value->subsystem_vendor_id = number;
			fields |= VMMFS_DESCRIPTOR_SUBSYSTEM_VENDOR;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length,
		    "subsystem_device_id")) {
			if (fields & VMMFS_DESCRIPTOR_SUBSYSTEM_DEVICE)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number > UINT16_MAX)
				return (EINVAL);
			value->subsystem_device_id = number;
			fields |= VMMFS_DESCRIPTOR_SUBSYSTEM_DEVICE;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length, "class")) {
			if (fields & VMMFS_DESCRIPTOR_CLASS)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number > 0x00ffffffU)
				return (EINVAL);
			value->class = number;
			fields |= VMMFS_DESCRIPTOR_CLASS;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length,
		    "revision")) {
			if (fields & VMMFS_DESCRIPTOR_REVISION)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number > UINT8_MAX)
				return (EINVAL);
			value->revision = number;
			fields |= VMMFS_DESCRIPTOR_REVISION;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length,
		    "header.type")) {
			if (fields & VMMFS_DESCRIPTOR_HEADER_TYPE || value_length != 8 ||
			    bcmp(equals + 1, "endpoint", 8) != 0)
				return (EINVAL);
			fields |= VMMFS_DESCRIPTOR_HEADER_TYPE;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length,
		    "intx.pin")) {
			if (fields & VMMFS_DESCRIPTOR_INTX_PIN)
				return (EINVAL);
			if (value_length == 4 && bcmp(equals + 1, "none", 4) == 0)
				value->intx_pin = VMMFS_PCISLOT_INTX_NONE;
			else if (value_length == 1 && equals[1] >= 'a' && equals[1] <= 'd')
				value->intx_pin = VMMFS_PCISLOT_INTX_A + equals[1] - 'a';
			else
				return (EINVAL);
			fields |= VMMFS_DESCRIPTOR_INTX_PIN;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "bar", "type", VMMFS_PCISLOT_MAX_BARS, &index)) !=
		    ENOENT) {
			if (error != 0 || bars[index] & VMMFS_DESCRIPTOR_BAR_TYPE)
				return (EINVAL);
			if (value_length == 2 && bcmp(equals + 1, "io", 2) == 0)
				value->bars[index].type = VMMFS_PCISLOT_BAR_IO;
			else if (value_length == 5 && bcmp(equals + 1, "mem32", 5) == 0)
				value->bars[index].type = VMMFS_PCISLOT_BAR_MEM32;
			else if (value_length == 5 && bcmp(equals + 1, "mem64", 5) == 0)
				value->bars[index].type = VMMFS_PCISLOT_BAR_MEM64;
			else
				return (EINVAL);
			value->bars[index].present = true;
			bars[index] |= VMMFS_DESCRIPTOR_BAR_TYPE;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "bar", "size", VMMFS_PCISLOT_MAX_BARS, &index)) !=
		    ENOENT) {
			if (error != 0 || bars[index] & VMMFS_DESCRIPTOR_BAR_SIZE)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number == 0 ||
			    (number & (number - 1)) != 0)
				return (EINVAL);
			value->bars[index].size = number;
			bars[index] |= VMMFS_DESCRIPTOR_BAR_SIZE;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "bar", "prefetchable", VMMFS_PCISLOT_MAX_BARS,
		    &index)) != ENOENT) {
			if (error != 0 || bars[index] & VMMFS_DESCRIPTOR_BAR_PREFETCHABLE)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_boolean(equals + 1,
			    value_length, &boolean);
			if (error != 0)
				return (error);
			value->bars[index].prefetchable = boolean;
			bars[index] |= VMMFS_DESCRIPTOR_BAR_PREFETCHABLE;
		} else if (vmmfs_pcislot_descriptor_key(line, key_length,
		    "rom.size")) {
			if (value->rom_present)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number < PAGE_SIZE ||
			    (number & (number - 1)) != 0)
				return (EINVAL);
			value->rom_present = true;
			value->rom_size = number;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "doorbell", "bar", VMMFS_PCISLOT_MAX_DOORBELLS,
		    &index)) != ENOENT) {
			if (error != 0 || doorbells[index] & VMMFS_DESCRIPTOR_DOORBELL_BAR)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number >= VMMFS_PCISLOT_MAX_BARS)
				return (EINVAL);
			value->doorbells[index].present = true;
			value->doorbells[index].bar = number;
			doorbells[index] |= VMMFS_DESCRIPTOR_DOORBELL_BAR;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "doorbell", "offset", VMMFS_PCISLOT_MAX_DOORBELLS,
		    &index)) != ENOENT) {
			if (error != 0 || doorbells[index] & VMMFS_DESCRIPTOR_DOORBELL_OFFSET)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0)
				return (error);
			value->doorbells[index].offset = number;
			doorbells[index] |= VMMFS_DESCRIPTOR_DOORBELL_OFFSET;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "doorbell", "width", VMMFS_PCISLOT_MAX_DOORBELLS,
		    &index)) != ENOENT) {
			if (error != 0 || doorbells[index] & VMMFS_DESCRIPTOR_DOORBELL_WIDTH)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || (number != 1 && number != 2 && number != 4 &&
			    number != 8))
				return (EINVAL);
			value->doorbells[index].width = number;
			doorbells[index] |= VMMFS_DESCRIPTOR_DOORBELL_WIDTH;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "doorbell", "size", VMMFS_PCISLOT_MAX_DOORBELLS,
		    &index)) != ENOENT) {
			if (error != 0 || doorbells[index] & VMMFS_DESCRIPTOR_DOORBELL_SIZE)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number == 0)
				return (EINVAL);
			value->doorbells[index].size = number;
			doorbells[index] |= VMMFS_DESCRIPTOR_DOORBELL_SIZE;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "doorbell", "space", VMMFS_PCISLOT_MAX_DOORBELLS,
		    &index)) != ENOENT) {
			if (error != 0 || doorbells[index] & VMMFS_DESCRIPTOR_DOORBELL_SPACE)
				return (EINVAL);
			if (value_length == 4 && bcmp(equals + 1, "mmio", 4) == 0)
				value->doorbells[index].space = VMMFS_PCISLOT_DOORBELL_MMIO;
			else if (value_length == 3 && bcmp(equals + 1, "pio", 3) == 0)
				value->doorbells[index].space = VMMFS_PCISLOT_DOORBELL_PIO;
			else
				return (EINVAL);
			doorbells[index] |= VMMFS_DESCRIPTOR_DOORBELL_SPACE;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "config", "bar", VMMFS_PCISLOT_MAX_CONFIGS,
		    &index)) != ENOENT) {
			if (error != 0 || configs[index] & VMMFS_DESCRIPTOR_CONFIG_BAR)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number >= VMMFS_PCISLOT_MAX_BARS)
				return (EINVAL);
			value->configs[index].present = true;
			value->configs[index].bar = number;
			configs[index] |= VMMFS_DESCRIPTOR_CONFIG_BAR;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "config", "offset", VMMFS_PCISLOT_MAX_CONFIGS,
		    &index)) != ENOENT) {
			if (error != 0 || configs[index] & VMMFS_DESCRIPTOR_CONFIG_OFFSET)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0)
				return (error);
			value->configs[index].offset = number;
			configs[index] |= VMMFS_DESCRIPTOR_CONFIG_OFFSET;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "config", "width", VMMFS_PCISLOT_MAX_CONFIGS,
		    &index)) != ENOENT) {
			if (error != 0 || configs[index] & VMMFS_DESCRIPTOR_CONFIG_WIDTH)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || (number != 1 && number != 2 && number != 4 &&
			    number != 8))
				return (EINVAL);
			value->configs[index].width = number;
			configs[index] |= VMMFS_DESCRIPTOR_CONFIG_WIDTH;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "config", "space", VMMFS_PCISLOT_MAX_CONFIGS,
		    &index)) != ENOENT) {
			if (error != 0 || configs[index] & VMMFS_DESCRIPTOR_CONFIG_SPACE)
				return (EINVAL);
			if (value_length == 4 && bcmp(equals + 1, "mmio", 4) == 0)
				value->configs[index].space = VMMFS_PCISLOT_CONFIG_MMIO;
			else if (value_length == 3 && bcmp(equals + 1, "pio", 3) == 0)
				value->configs[index].space = VMMFS_PCISLOT_CONFIG_PIO;
			else
				return (EINVAL);
			configs[index] |= VMMFS_DESCRIPTOR_CONFIG_SPACE;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "kind", VMMFS_PCISLOT_MAX_CAPS, &index)) !=
		    ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_KIND)
				return (EINVAL);
			if (value_length == 4 && bcmp(equals + 1, "pcie", 4) == 0)
				value->caps[index].kind = VMMFS_PCISLOT_CAP_PCIE;
			else if (value_length == 3 && bcmp(equals + 1, "msi", 3) == 0)
				value->caps[index].kind = VMMFS_PCISLOT_CAP_MSI;
			else if (value_length == 4 && bcmp(equals + 1, "msix", 4) == 0)
				value->caps[index].kind = VMMFS_PCISLOT_CAP_MSIX;
			else if (value_length == 4 && bcmp(equals + 1, "blob", 4) == 0)
				value->caps[index].kind = VMMFS_PCISLOT_CAP_BLOB;
			else
				return (EINVAL);
			value->caps[index].present = true;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_KIND;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "vectors", VMMFS_PCISLOT_MAX_CAPS, &index)) !=
		    ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_VECTORS)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number == 0 || number > VMMFS_PCISLOT_MAX_MSIX_VECTORS)
				return (EINVAL);
			value->caps[index].vectors = number;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_VECTORS;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "address_width", VMMFS_PCISLOT_MAX_CAPS,
		    &index)) != ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_ADDRESS_WIDTH)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || (number != 32 && number != 64))
				return (EINVAL);
			value->caps[index].address_width = number;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_ADDRESS_WIDTH;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "maskable", VMMFS_PCISLOT_MAX_CAPS,
		    &index)) != ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_MASKABLE)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_boolean(equals + 1,
			    value_length, &boolean);
			if (error != 0)
				return (error);
			value->caps[index].maskable = boolean;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_MASKABLE;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "table.bar", VMMFS_PCISLOT_MAX_CAPS,
		    &index)) != ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_TABLE_BAR)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number >= VMMFS_PCISLOT_MAX_BARS)
				return (EINVAL);
			value->caps[index].table_bar = number;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_TABLE_BAR;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "table.offset", VMMFS_PCISLOT_MAX_CAPS,
		    &index)) != ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_TABLE_OFFSET)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0)
				return (error);
			value->caps[index].table_offset = number;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_TABLE_OFFSET;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "pba.bar", VMMFS_PCISLOT_MAX_CAPS,
		    &index)) != ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_PBA_BAR)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number >= VMMFS_PCISLOT_MAX_BARS)
				return (EINVAL);
			value->caps[index].pba_bar = number;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_PBA_BAR;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "pba.offset", VMMFS_PCISLOT_MAX_CAPS,
		    &index)) != ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_PBA_OFFSET)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0)
				return (error);
			value->caps[index].pba_offset = number;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_PBA_OFFSET;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "id", VMMFS_PCISLOT_MAX_CAPS, &index)) !=
		    ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_ID)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number > UINT8_MAX)
				return (EINVAL);
			value->caps[index].id = number;
			caps[index] |= VMMFS_DESCRIPTOR_CAP_ID;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "access", VMMFS_PCISLOT_MAX_CAPS, &index)) !=
		    ENOENT) {
				if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_ACCESS) {
					return (EINVAL);
				}
				if (value_length != 6 ||
				    bcmp(equals + 1, "static", 6) != 0) {
					return (EINVAL);
				}
				value->caps[index].access = VMMFS_PCISLOT_CAP_STATIC;
				caps[index] |= VMMFS_DESCRIPTOR_CAP_ACCESS;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "cap", "data", VMMFS_PCISLOT_MAX_CAPS, &index)) !=
		    ENOENT) {
			if (error != 0 || caps[index] & VMMFS_DESCRIPTOR_CAP_DATA)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_bytes(equals + 1,
			    value_length, value, &value->caps[index].data_offset,
			    &value->caps[index].data_length);
			if (error != 0)
				return (error);
			caps[index] |= VMMFS_DESCRIPTOR_CAP_DATA;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "ecap", "id", VMMFS_PCISLOT_MAX_ECAPS, &index)) !=
		    ENOENT) {
			if (error != 0 || ecaps[index] & VMMFS_DESCRIPTOR_ECAP_ID)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number > UINT16_MAX)
				return (EINVAL);
			value->ecaps[index].present = true;
			value->ecaps[index].id = number;
			ecaps[index] |= VMMFS_DESCRIPTOR_ECAP_ID;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "ecap", "version", VMMFS_PCISLOT_MAX_ECAPS,
		    &index)) != ENOENT) {
			if (error != 0 || ecaps[index] & VMMFS_DESCRIPTOR_ECAP_VERSION)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_number(equals + 1,
			    value_length, &number);
			if (error != 0 || number == 0 || number > 15)
				return (EINVAL);
			value->ecaps[index].version = number;
			ecaps[index] |= VMMFS_DESCRIPTOR_ECAP_VERSION;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "ecap", "access", VMMFS_PCISLOT_MAX_ECAPS,
		    &index)) != ENOENT) {
				if (error != 0 || ecaps[index] & VMMFS_DESCRIPTOR_ECAP_ACCESS) {
					return (EINVAL);
				}
				if (value_length != 6 ||
				    bcmp(equals + 1, "static", 6) != 0) {
					return (EINVAL);
				}
				value->ecaps[index].access = VMMFS_PCISLOT_CAP_STATIC;
				ecaps[index] |= VMMFS_DESCRIPTOR_ECAP_ACCESS;
		} else if ((error = vmmfs_pcislot_descriptor_index_key(line,
		    key_length, "ecap", "data", VMMFS_PCISLOT_MAX_ECAPS, &index)) !=
		    ENOENT) {
			if (error != 0 || ecaps[index] & VMMFS_DESCRIPTOR_ECAP_DATA)
				return (EINVAL);
			error = vmmfs_pcislot_descriptor_parse_bytes(equals + 1,
			    value_length, value, &value->ecaps[index].data_offset,
			    &value->ecaps[index].data_length);
			if (error != 0)
				return (error);
			ecaps[index] |= VMMFS_DESCRIPTOR_ECAP_DATA;
		} else {
			return (EINVAL);
		}
		line += line_length + 1;
		++line_number;
	}
	if (fields != (VMMFS_DESCRIPTOR_VERSION | VMMFS_DESCRIPTOR_VENDOR |
	    VMMFS_DESCRIPTOR_DEVICE | VMMFS_DESCRIPTOR_SUBSYSTEM_VENDOR |
	    VMMFS_DESCRIPTOR_SUBSYSTEM_DEVICE | VMMFS_DESCRIPTOR_CLASS |
	    VMMFS_DESCRIPTOR_REVISION | VMMFS_DESCRIPTOR_HEADER_TYPE |
	    VMMFS_DESCRIPTOR_INTX_PIN))
		return (EINVAL);
	for (bar = 0; bar < VMMFS_PCISLOT_MAX_BARS; ++bar) {
		if (bars[bar] != 0 && bars[bar] !=
		    (VMMFS_DESCRIPTOR_BAR_TYPE | VMMFS_DESCRIPTOR_BAR_SIZE |
		    VMMFS_DESCRIPTOR_BAR_PREFETCHABLE))
			return (EINVAL);
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
		    (bar == VMMFS_PCISLOT_MAX_BARS - 1 || bars[bar + 1] != 0))
			return (EINVAL);
	}
	for (index = 0; index < VMMFS_PCISLOT_MAX_DOORBELLS; ++index) {
		if (doorbells[index] == 0)
			continue;
		if (doorbells[index] != (VMMFS_DESCRIPTOR_DOORBELL_BAR |
		    VMMFS_DESCRIPTOR_DOORBELL_OFFSET | VMMFS_DESCRIPTOR_DOORBELL_WIDTH |
		    VMMFS_DESCRIPTOR_DOORBELL_SIZE | VMMFS_DESCRIPTOR_DOORBELL_SPACE) ||
		    !value->bars[value->doorbells[index].bar].present ||
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
	gap = false;
	for (index = 0; index < VMMFS_PCISLOT_MAX_CONFIGS; ++index) {
		if (configs[index] == 0) {
			gap = true;
			continue;
		}
		if (gap || configs[index] != (VMMFS_DESCRIPTOR_CONFIG_BAR |
		    VMMFS_DESCRIPTOR_CONFIG_OFFSET | VMMFS_DESCRIPTOR_CONFIG_WIDTH |
		    VMMFS_DESCRIPTOR_CONFIG_SPACE) ||
		    !value->bars[value->configs[index].bar].present ||
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
	gap = false;
	pcie_caps = 0;
	for (cap = 0; cap < VMMFS_PCISLOT_MAX_CAPS; ++cap) {
		if (caps[cap] == 0) {
			gap = true;
			continue;
		}
		if (gap)
			return (EINVAL);
		cap_kind = value->caps[cap].kind;
		if ((cap_kind == VMMFS_PCISLOT_CAP_PCIE &&
		    caps[cap] != VMMFS_DESCRIPTOR_CAP_KIND) ||
		    (cap_kind == VMMFS_PCISLOT_CAP_MSI && caps[cap] !=
		    (VMMFS_DESCRIPTOR_CAP_KIND | VMMFS_DESCRIPTOR_CAP_VECTORS |
		    VMMFS_DESCRIPTOR_CAP_ADDRESS_WIDTH | VMMFS_DESCRIPTOR_CAP_MASKABLE)) ||
		    (cap_kind == VMMFS_PCISLOT_CAP_MSIX && caps[cap] !=
		    (VMMFS_DESCRIPTOR_CAP_KIND | VMMFS_DESCRIPTOR_CAP_VECTORS |
		    VMMFS_DESCRIPTOR_CAP_TABLE_BAR | VMMFS_DESCRIPTOR_CAP_TABLE_OFFSET |
		    VMMFS_DESCRIPTOR_CAP_PBA_BAR | VMMFS_DESCRIPTOR_CAP_PBA_OFFSET)) ||
		    (cap_kind == VMMFS_PCISLOT_CAP_BLOB && caps[cap] !=
		    (VMMFS_DESCRIPTOR_CAP_KIND | VMMFS_DESCRIPTOR_CAP_ID |
		    VMMFS_DESCRIPTOR_CAP_ACCESS | VMMFS_DESCRIPTOR_CAP_DATA)))
			return (EINVAL);
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
		    value->bars[value->caps[cap].pba_bar].type == VMMFS_PCISLOT_BAR_IO ||
		    value->caps[cap].vectors > UINT64_MAX / 16)
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
	gap = false;
	for (ecap = 0; ecap < VMMFS_PCISLOT_MAX_ECAPS; ++ecap) {
		if (ecaps[ecap] == 0) {
			gap = true;
			continue;
		}
		if (gap || ecaps[ecap] != (VMMFS_DESCRIPTOR_ECAP_ID |
		    VMMFS_DESCRIPTOR_ECAP_VERSION | VMMFS_DESCRIPTOR_ECAP_ACCESS |
		    VMMFS_DESCRIPTOR_ECAP_DATA))
			return (EINVAL);
	}
	bcopy(text, value->text, length);
	value->text[length] = '\0';
	value->length = length;
	return (0);
}

static int
vmmfs_pcislot_descriptor_parse_number(const char *text, size_t length,
	uint64_t *number)
{
	uint64_t value;
	unsigned int base;
	unsigned int digit;
	size_t index;

	if (text == NULL || number == NULL || length == 0)
		return (EINVAL);
	base = 10;
	index = 0;
	if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		base = 16;
		index = 2;
		if (index == length)
			return (EINVAL);
	}
	value = 0;
	for (; index < length; ++index) {
		if (text[index] >= '0' && text[index] <= '9')
			digit = text[index] - '0';
		else if (text[index] >= 'a' && text[index] <= 'f')
			digit = text[index] - 'a' + 10;
		else if (text[index] >= 'A' && text[index] <= 'F')
			digit = text[index] - 'A' + 10;
		else
			return (EINVAL);
		if (digit >= base || value > (UINT64_MAX - digit) / base)
			return (ERANGE);
		value = value * base + digit;
	}
	*number = value;
	return (0);
}

static int
vmmfs_pcislot_descriptor_parse_boolean(const char *text, size_t length,
	bool *boolean)
{

	if (text == NULL || boolean == NULL || length != 1 ||
	    (text[0] != '0' && text[0] != '1'))
		return (EINVAL);
	*boolean = text[0] == '1';
	return (0);
}

static int
vmmfs_pcislot_descriptor_parse_bytes(const char *text, size_t length,
	struct vmmfs_pcislot_descriptor_value *value, uint16_t *offset,
	uint16_t *data_length)
{
	uint16_t start;
	uint16_t count;
	unsigned int high;
	unsigned int low;
	size_t index;

	if (text == NULL || value == NULL || offset == NULL || data_length == NULL ||
	    length == 0 || (length & 1) != 0 ||
	    length / 2 > VMMFS_PCISLOT_CAP_DATA_MAX - value->cap_data_length)
		return (EINVAL);
	start = value->cap_data_length;
	count = (uint16_t)(length / 2);
	for (index = 0; index < length; index += 2) {
		if (text[index] < '0' || text[index] > 'f' ||
		    text[index + 1] < '0' || text[index + 1] > 'f')
			return (EINVAL);
		if (text[index] >= 'a' && text[index] <= 'f')
			high = text[index] - 'a' + 10;
		else if (text[index] >= '0' && text[index] <= '9')
			high = text[index] - '0';
		else
			return (EINVAL);
		if (text[index + 1] >= 'a' && text[index + 1] <= 'f')
			low = text[index + 1] - 'a' + 10;
		else if (text[index + 1] >= '0' && text[index + 1] <= '9')
			low = text[index + 1] - '0';
		else
			return (EINVAL);
		value->cap_data[start + index / 2] = (high << 4) | low;
	}
	*offset = start;
	*data_length = count;
	value->cap_data_length += count;
	return (0);
}

static int
vmmfs_pcislot_descriptor_index_key(const char *key, size_t length,
	const char *prefix, const char *suffix, unsigned int maximum,
	unsigned int *indexp)
{
	size_t prefix_length;
	size_t suffix_length;
	size_t index_length;
	unsigned int index;
	size_t position;

	prefix_length = strlen(prefix);
	suffix_length = strlen(suffix);
	if (length < prefix_length || bcmp(key, prefix, prefix_length) != 0)
		return (ENOENT);
	if (length <= prefix_length + suffix_length + 1 ||
	    key[length - suffix_length - 1] != '.' ||
	    bcmp(key + length - suffix_length, suffix, suffix_length) != 0)
		return (ENOENT);
	index_length = length - prefix_length - suffix_length - 1;
	if (index_length == 0 || (index_length > 1 && key[prefix_length] == '0'))
		return (EINVAL);
	index = 0;
	for (position = 0; position < index_length; ++position) {
		if (key[prefix_length + position] < '0' ||
		    key[prefix_length + position] > '9' ||
		    index > (UINT_MAX - (key[prefix_length + position] - '0')) / 10)
			return (EINVAL);
		index = index * 10 + key[prefix_length + position] - '0';
	}
	if (index >= maximum)
		return (EINVAL);
	*indexp = index;
	return (0);
}

static bool
vmmfs_pcislot_descriptor_key(const char *key, size_t length,
	const char *expected)
{
	return (length == strlen(expected) && bcmp(key, expected, length) == 0);
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
