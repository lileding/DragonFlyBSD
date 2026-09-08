/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot runtime resources.
 */
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/thread2.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_parent.h"
#include "vmmfs_machine.h"
#include "vmmfs_memory.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_auth.h"
#include "vmmfs_node.h"
#include "vmmfs_pcislot_resource.h"
#include "vmmfs_vcpu.h"

enum vmmfs_pcislot_resource_kind {
	VMMFS_PCISLOT_RESOURCE_BAR,
	VMMFS_PCISLOT_RESOURCE_PIO,
	VMMFS_PCISLOT_RESOURCE_ROM,
	VMMFS_PCISLOT_RESOURCE_DMA,
	VMMFS_PCISLOT_RESOURCE_KICK,
	VMMFS_PCISLOT_RESOURCE_INTX,
	VMMFS_PCISLOT_RESOURCE_MSI,
	VMMFS_PCISLOT_RESOURCE_MSIX,
};

struct vmmfs_pcislot_resource_trap {
	uint64_t base;
	uint64_t size;
	vmm_io_t read_io;
	vmm_io_t write_io;
};

struct vmmfs_pcislot_object {
	SLIST_ENTRY(vmmfs_pcislot_object) entry;
	struct vm_object *object;
};
SLIST_HEAD(vmmfs_pcislot_objects, vmmfs_pcislot_object);

struct vmmfs_pcislot_resource {
	struct vmmfs_node node;
	enum vmmfs_pcislot_resource_kind kind;
	uint16_t index;
	uint16_t capability;
	uint16_t vector;
	struct lwkt_token token;
	struct kqinfo read_kq;
	struct vm_object *backing_object;
	struct vm_object *pager_object;
	struct vmmfs_pcislot_objects objects;
	struct vmspace *vmspace;
	struct cdev *dev;
	struct vmmfs_pcislot_resource_trap *traps;
	struct vmmfs_pci_kick kick;
	uint64_t gpa;
	uint64_t size;
	uint64_t mapping_size;
	size_t trap_count;
	bool kick_pending;
	uint64_t sequence;
	bool mapped;
	bool revoked;
	bool bus_master_enabled;
	bool intx_asserted;
};

struct vmmfs_pcislot_resources {
	struct vmmfs_node node;
	struct lwkt_token token;
	vmm_machine_t machine;
	u_int interrupt_users;
	uint64_t descriptor_generation;
	bool powered;
	bool destroying;
	size_t count;
	size_t initialized_count;
	struct vmmfs_pcislot_resource items[];
};

#define VMMFS_PCISLOT_RESOURCE_MODE 0600

static uint32_t vmmfs_pcislot_resource_serial;
static int vmmfs_msix_trace;

SYSCTL_NODE(_debug, OID_AUTO, vmmfs, CTLFLAG_RW, 0,
	"VMMFS debug controls");
SYSCTL_INT(_debug_vmmfs, OID_AUTO, msix_trace, CTLFLAG_RW,
	&vmmfs_msix_trace, 0,
	"Log VMMFS MSI-X delivery decisions");

static int vmmfs_pcislot_resource_close(struct vop_close_args *);
static int vmmfs_pcislot_resource_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_pcislot_resource_open(struct vop_open_args *);
static int vmmfs_pcislot_resource_read(struct vop_read_args *);
static int vmmfs_pcislot_resource_write(struct vop_write_args *);
static int vmmfs_pcislot_resource_dev_open(struct dev_open_args *);
static int vmmfs_pcislot_resource_dev_close(struct dev_close_args *);
static int vmmfs_pcislot_resource_dev_mmap_single(
	struct dev_mmap_single_args *);
static int vmmfs_pcislot_resource_pager_ctor(void *, vm_ooffset_t,
	vm_prot_t, vm_ooffset_t, struct ucred *, u_short *);
static void vmmfs_pcislot_resource_pager_dtor(void *);
static int vmmfs_pcislot_resource_pager_fault(vm_object_t, vm_ooffset_t,
	int, vm_page_t *);
static void vmmfs_pcislot_resource_filter_detach(struct knote *);
static int vmmfs_pcislot_resource_filter_read(struct knote *, long);
static int vmmfs_pcislot_resource_mmio_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static int vmmfs_pcislot_resource_pio_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static int vmmfs_pcislot_resource_mmio_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_pcislot_resource_pio_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_pcislot_resource_config_mmio_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static int vmmfs_pcislot_resource_config_pio_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static void vmmfs_pcislot_resources_drop(struct vmmfs_node *);
static void vmmfs_pcislot_resource_drop(struct vmmfs_node *);
static bool vmmfs_pcislot_resource_mappable(
	const struct vmmfs_pcislot_resource *);
static bool vmmfs_pcislot_resource_enabled(
	const struct vmmfs_pcislot_resource *);
static int vmmfs_pcislot_resource_create_mapping(
	struct vmmfs_pcislot_resource *);
static int vmmfs_pcislot_resource_create_vnode(
	struct vmmfs_pcislot_resource *, struct vmmfs_mount *);
static void vmmfs_pcislot_resource_drain(struct vm_object *,
	struct vmmfs_pcislot_objects *);
static int vmmfs_pcislot_resource_track_page(struct vmmfs_pcislot_resource *,
	vm_page_t, struct vmspace *);
static void vmmfs_pcislot_resource_revoke(
	struct vmmfs_pcislot_resource *);
static int vmmfs_pcislot_resource_map(struct vmmfs_pcislot_resource *);
static void vmmfs_pcislot_resource_unmap(struct vmmfs_pcislot_resource *);
static int vmmfs_pcislot_resource_install_traps(
	struct vmmfs_pcislot_resource *, vmm_machine_t);
static void vmmfs_pcislot_resource_remove_traps(
	struct vmmfs_pcislot_resource *, vmm_machine_t);
static int vmmfs_pcislot_resource_queue_kick(
	struct vmmfs_pcislot_resource *, uint64_t, enum vmm_io_width,
	uint64_t);
static int vmmfs_pcislot_resource_doorbell(
	struct vmmfs_pcislot_resource *, uint64_t, enum vmm_io_width,
	uint64_t);
static struct vmmfs_pcislot_resource *vmmfs_pcislot_resource_bar(
	struct vmmfs_pcislot_resources *, unsigned int);
static uint16_t vmmfs_pcislot_resource_read16(const uint8_t *, uint16_t);
static uint32_t vmmfs_pcislot_resource_read32(const uint8_t *, uint16_t);
static int vmmfs_pcislot_resource_object_read(
	struct vmmfs_pcislot_resource *, uint64_t, void *, size_t);
static int vmmfs_pcislot_resource_object_write(
	struct vmmfs_pcislot_resource *, uint64_t, const void *, size_t);
static uint32_t vmmfs_pcislot_resource_object_read32(
	struct vmmfs_pcislot_resource *, uint64_t);
static int vmmfs_pcislot_resource_raise_msi(
	struct vmmfs_pcislot_resource *, vmm_machine_t);
static int vmmfs_pcislot_resource_raise_msix(
	struct vmmfs_pcislot_resource *, vmm_machine_t);
static void vmmfs_pcislot_resource_msix_trace(
	struct vmmfs_pcislot_resource *, uint16_t, uint32_t, uint64_t, uint32_t,
	uint64_t, const char *, int);
static int vmmfs_pcislot_resource_name(
	const struct vmmfs_pcislot_resource *, char *, size_t, size_t *);
static void vmmfs_pcislot_resources_trace_msix_control(
	struct vmmfs_pcislot_resources *, unsigned int);

static struct dev_ops vmmfs_pcislot_resource_dev_ops = {
	{ "vmmfs_pci", 0, D_MPSAFE },
	.d_open = vmmfs_pcislot_resource_dev_open,
	.d_close = vmmfs_pcislot_resource_dev_close,
	.d_mmap_single = vmmfs_pcislot_resource_dev_mmap_single,
};

static struct cdev_pager_ops vmmfs_pcislot_resource_pager_ops = {
	.cdev_pg_fault = vmmfs_pcislot_resource_pager_fault,
	.cdev_pg_ctor = vmmfs_pcislot_resource_pager_ctor,
	.cdev_pg_dtor = vmmfs_pcislot_resource_pager_dtor,
};

static struct filterops vmmfs_pcislot_resource_read_filterops = {
	FILTEROP_ISFD | FILTEROP_MPSAFE,
	NULL,
	vmmfs_pcislot_resource_filter_detach,
	vmmfs_pcislot_resource_filter_read,
};

static bool
vmmfs_pcislot_resource_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_resource *resource =
	    (struct vmmfs_pcislot_resource *)node;
	struct vm_object *pager;

	vmmfs_pcislot_resource_unmap(resource);
	vmmfs_pcislot_resource_revoke(resource);
	lwkt_gettoken(&resource->token);
	pager = resource->pager_object;
	resource->pager_object = NULL;
	lwkt_reltoken(&resource->token);
	if (pager != NULL)
		vm_object_deallocate(pager);
	return (true);
}

struct vop_ops vmmfs_pcislot_resource_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vmmfs_pcislot_resource_close,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_kqfilter = vmmfs_pcislot_resource_kqfilter,
	.vop_open = vmmfs_pcislot_resource_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_resource_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_write = vmmfs_pcislot_resource_write,
};

int
vmmfs_pcislot_resources_create(struct vmmfs_pcislot *slot,
	vmm_machine_t machine, const struct vmmfs_pcislot_descriptor_value *value,
	uint64_t generation, struct vmmfs_pcislot_resources **resourcesp)
{
	struct vmmfs_pcislot_resources *resources;
	struct vmmfs_pcislot_resource *resource;
	size_t count;
	size_t index;
	unsigned int bar;
	unsigned int cap;
	unsigned int doorbell;
	unsigned int vector;
	unsigned int msi_index;
	unsigned int msix_index;
	int error;

	if (slot == NULL || machine == NULL || value == NULL ||
	    resourcesp == NULL || generation == 0)
		return (EINVAL);
	*resourcesp = NULL;
	count = 1;
	for (bar = 0; bar < VMMFS_PCISLOT_MAX_BARS; ++bar) {
		if (value->bars[bar].present)
			++count;
	}
	if (value->rom_present)
		++count;
	for (doorbell = 0; doorbell < VMMFS_PCISLOT_MAX_DOORBELLS;
	    ++doorbell) {
		if (value->doorbells[doorbell].present)
			++count;
	}
	if (value->intx_pin != VMMFS_PCISLOT_INTX_NONE)
		++count;
	for (cap = 0; cap < VMMFS_PCISLOT_MAX_CAPS; ++cap) {
		if (value->caps[cap].kind == VMMFS_PCISLOT_CAP_MSI ||
		    value->caps[cap].kind == VMMFS_PCISLOT_CAP_MSIX)
			count += value->caps[cap].vectors;
	}
	if (count > (SIZE_MAX - sizeof(*resources)) / sizeof(resources->items[0]))
		return (EOVERFLOW);
	resources = kmalloc(sizeof(*resources) + count * sizeof(resources->items[0]),
	    M_VMMFS, M_WAITOK | M_ZERO);
	resources->node.parent = &slot->node;
	resources->node.mount = slot->node.mount;
	resources->node.references = 1;
	lwkt_token_init(&resources->token, "vmmfsnode");
	lockinit(&resources->node.lock, "vmmfsnode", 0, 0);
	resources->node.drop = vmmfs_pcislot_resources_drop;
	vmmfs_node_hold(&slot->node);
	resources->machine = machine;
	resources->descriptor_generation = generation;
	resources->powered = true;
	resources->count = count;
	index = 0;
	for (bar = 0; bar < VMMFS_PCISLOT_MAX_BARS; ++bar) {
		if (!value->bars[bar].present)
			continue;
		resource = &resources->items[index++];
		resource->index = bar;
		resource->kind = value->bars[bar].type == VMMFS_PCISLOT_BAR_IO ?
		    VMMFS_PCISLOT_RESOURCE_PIO : VMMFS_PCISLOT_RESOURCE_BAR;
		resource->gpa = slot->type0.bar_address[bar];
		resource->size = resource->kind == VMMFS_PCISLOT_RESOURCE_BAR ?
		    max((uint64_t)PAGE_SIZE, value->bars[bar].size) :
		    value->bars[bar].size;
	}
	if (value->rom_present) {
		resource = &resources->items[index++];
		resource->kind = VMMFS_PCISLOT_RESOURCE_ROM;
		resource->gpa = slot->type0.rom_address;
		resource->size = max((uint64_t)PAGE_SIZE, value->rom_size);
	}
	resource = &resources->items[index++];
	resource->kind = VMMFS_PCISLOT_RESOURCE_DMA;
	resource->size = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot))->memory.size;
	for (doorbell = 0; doorbell < VMMFS_PCISLOT_MAX_DOORBELLS;
	    ++doorbell) {
		if (!value->doorbells[doorbell].present)
			continue;
		resource = &resources->items[index++];
		resource->kind = VMMFS_PCISLOT_RESOURCE_KICK;
		resource->index = doorbell;
	}
	if (value->intx_pin != VMMFS_PCISLOT_INTX_NONE) {
		resource = &resources->items[index++];
		resource->kind = VMMFS_PCISLOT_RESOURCE_INTX;
	}
	msi_index = 0;
	msix_index = 0;
	for (cap = 0; cap < VMMFS_PCISLOT_MAX_CAPS; ++cap) {
		if (value->caps[cap].kind != VMMFS_PCISLOT_CAP_MSI &&
		    value->caps[cap].kind != VMMFS_PCISLOT_CAP_MSIX)
			continue;
		for (vector = 0; vector < value->caps[cap].vectors; ++vector) {
			resource = &resources->items[index++];
			resource->kind = value->caps[cap].kind ==
			    VMMFS_PCISLOT_CAP_MSI ? VMMFS_PCISLOT_RESOURCE_MSI :
			    VMMFS_PCISLOT_RESOURCE_MSIX;
			resource->index = resource->kind == VMMFS_PCISLOT_RESOURCE_MSI ?
			    msi_index++ : msix_index++;
			resource->capability = cap;
			resource->vector = vector;
		}
	}
	KKASSERT(index == count);
	for (index = 0; index < count; ++index) {
		resource = &resources->items[index];
		resource->node.inode = vmmfs_root_allocate_inode(
		    (struct vmmfs_root *)slot->node.mount->root);
		lwkt_token_init(&resource->token, "vmmfspcires");
		++resources->initialized_count;
		SLIST_INIT(&resource->read_kq.ki_note);
		SLIST_INIT(&resource->objects);
		resource->node.parent = &resources->node;
		resource->node.mount = resources->node.mount;
		resource->node.dead = false;
		resource->node.references = 1;
		lockinit(&resource->node.lock, "vmmfsnode", 0, 0);
		resource->node.drop = vmmfs_pcislot_resource_drop;
		vmmfs_node_hold(&resources->node);
		resource->node.mode = VMMFS_PCISLOT_RESOURCE_MODE;
		resource->node.size = vmmfs_pcislot_resource_mappable(resource) ?
		    (off_t)resource->size : 0;
		error = vmmfs_pcislot_resource_create_mapping(resource);
		if (error != 0)
			goto fail;
	}
	for (index = 0; index < count; ++index) {
		resource = &resources->items[index];
		error = vmmfs_pcislot_resource_create_vnode(resource, slot->node.mount);
		if (error != 0)
			goto fail;
		resource->node.deactivate = vmmfs_pcislot_resource_deactivate;
	}
	*resourcesp = resources;
	return (0);

fail:
	vmmfs_pcislot_resources_deactivate(resources);
	return (error);
}


void
vmmfs_pcislot_resources_deactivate(struct vmmfs_pcislot_resources *resources)
{
	struct vmmfs_pcislot_resource *resource;
	struct vnode *vnode;
	size_t index;

	if (resources == NULL)
		return;
	/* This private group has no vnode, but owns published resource nodes. */
	(void)lockmgr(&resources->node.lock, LK_EXCLUSIVE);
	lwkt_gettoken(&resources->token);
	if (resources->destroying) {
		lwkt_reltoken(&resources->token);
		(void)lockmgr(&resources->node.lock, LK_RELEASE);
		return;
	}
	resources->node.dead = true;
	resources->destroying = true;
	resources->powered = false;
	lwkt_reltoken(&resources->token);
	(void)lockmgr(&resources->node.lock, LK_RELEASE);
	vmmfs_pcislot_resources_unbind(resources);
	for (index = 0; index < resources->initialized_count; ++index) {
		resource = &resources->items[index];
		lwkt_gettoken(&resources->token);
		vnode = resource->node.vnode;
		lwkt_reltoken(&resources->token);
		if (vnode != NULL) {
			(void)vmmfs_node_deactivate(&resource->node);
		} else {
			bool closed;

			/*
			 * Construction failed before vnode ownership.  Release the
			 * pager's resource reference before dropping this private node.
			 */
			closed = vmmfs_pcislot_resource_deactivate(&resource->node);
			KKASSERT(closed);
			vmmfs_node_put(&resource->node);
		}
	}

	lwkt_gettoken(&resources->token);
	resources->machine = NULL;
	lwkt_reltoken(&resources->token);
	vmmfs_node_put(&resources->node);
}

int
vmmfs_pcislot_resources_rebind(struct vmmfs_pcislot_resources *resources,
	vmm_machine_t machine)
{
	struct vmmfs_pcislot_resource *resource;
	struct vmmfs_pcislot_objects objects;
	struct vm_object *pager;
	struct vmspace *new_vmspace;
	struct vmspace *old_vmspace;
	size_t index;
	int error;

	if (resources == NULL || machine == NULL || resources->destroying ||
	    !resources->powered)
		return (EINVAL);
	if (resources->machine != NULL)
		return (EBUSY);
	new_vmspace = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(vmmfs_pcislot_resources_slot(resources)))->memory.run_vmspace;
	if (new_vmspace == NULL)
		return (ENXIO);
	for (index = 0; index < resources->count; ++index) {
		resource = &resources->items[index];
		if (resource->kind != VMMFS_PCISLOT_RESOURCE_DMA)
			continue;
		lwkt_gettoken(&resource->token);
		vmspace_ref(new_vmspace);
		old_vmspace = resource->vmspace;
		resource->vmspace = new_vmspace;
		objects = resource->objects;
		SLIST_INIT(&resource->objects);
		pager = resource->pager_object;
		if (pager != NULL)
			vm_object_reference_quick(pager);
		lwkt_reltoken(&resource->token);
		/*
		 * New faults use the replacement without a NULL window.
		 * The detached set covers every page accepted for the old image,
		 * including faults that have not reached the outer pmap_enter.
		 */
		vmmfs_pcislot_resource_drain(pager, &objects);
		if (old_vmspace != NULL)
			vmspace_rel(old_vmspace);
	}
	for (index = 0; index < resources->count; ++index) {
		resource = &resources->items[index];
		if (!resource->mapped)
			continue;
		error = vmmfs_pcislot_resource_install_traps(resource, machine);
		if (error == 0)
			continue;
		while (index-- != 0)
			vmmfs_pcislot_resource_remove_traps(&resources->items[index],
			    machine);
		return (error);
	}
	lwkt_gettoken(&resources->token);
	resources->machine = machine;
	lwkt_reltoken(&resources->token);
	return (0);
}

void
vmmfs_pcislot_resources_unbind(struct vmmfs_pcislot_resources *resources)
{
	struct vmmfs_pcislot_resource *resource;
	vmm_machine_t machine;
	size_t index;

	if (resources == NULL)
		return;
	lwkt_gettoken(&resources->token);
	machine = resources->machine;
	resources->machine = NULL;
	/* Close interrupt admission before draining operations that may fault. */
	while (resources->interrupt_users != 0) {
		tsleep_interlock(resources, 0);
		lwkt_reltoken(&resources->token);
		(void)tsleep(resources, PINTERLOCKED, "vmmfspciirq", 0);
		lwkt_gettoken(&resources->token);
	}
	lwkt_reltoken(&resources->token);
	if (machine == NULL)
		return;
	for (index = 0; index < resources->count; ++index) {
		resource = &resources->items[index];
		if (resource->mapped)
			vmmfs_pcislot_resource_remove_traps(resource, machine);
	}
	/*
	 * A warm reset retains the provider descriptors and old DMA vmspace.
	 * rebind publishes a new interrupt destination only after its traps exist.
	 */
}

int
vmmfs_pcislot_resources_lookup(struct vmmfs_pcislot_resources *resources,
	const char *name, size_t length, struct vnode **vnodep)
{
	char candidate[32];
	size_t candidate_length;
	size_t index;
	int error;

	if (resources == NULL || name == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	lwkt_gettoken(&resources->token);
	if (resources->destroying) {
		error = ENOENT;
		goto done;
	}
	for (index = 0; index < resources->count; ++index) {
		if (vmmfs_pcislot_resource_name(&resources->items[index],
		    candidate, sizeof(candidate), &candidate_length) != 0)
			continue;
		if (candidate_length == length &&
		    bcmp(candidate, name, length) == 0) {
			*vnodep = resources->items[index].node.vnode;
			error = 0;
			vref(*vnodep);
			goto done;
		}
	}
	error = ENOENT;
done:
	lwkt_reltoken(&resources->token);
	return (error);
}

int
vmmfs_pcislot_resources_read_item(struct vmmfs_pcislot_resources *resources,
	uint64_t index, ino_t *inode, char *name, size_t capacity,
	size_t *name_length)
{
	struct vmmfs_pcislot_resource *resource;
	int error;

	if (resources == NULL || inode == NULL || name == NULL ||
	    name_length == NULL)
		return (EINVAL);
	lwkt_gettoken(&resources->token);
	if (resources->destroying || index >= resources->count) {
		lwkt_reltoken(&resources->token);
		return (ENOENT);
	}
	resource = &resources->items[index];
	*inode = resource->node.inode;
	error = vmmfs_pcislot_resource_name(resource, name, capacity,
	    name_length);
	lwkt_reltoken(&resources->token);
	return (error);
}

int
vmmfs_pcislot_resource_index(const struct vmmfs_pcislot_resource *resource,
	uint16_t *index)
{
	if (resource == NULL || index == NULL)
		return (EINVAL);
	*index = resource->index;
	return (0);
}

int
vmmfs_pcislot_resource_gpa(const struct vmmfs_pcislot_resource *resource,
	uint64_t *gpa)
{
	if (resource == NULL || gpa == NULL)
		return (EINVAL);
	*gpa = resource->gpa;
	return (0);
}

static int
vmmfs_pcislot_resource_name(const struct vmmfs_pcislot_resource *resource,
	char *buffer, size_t capacity, size_t *length)
{
	int result;

	if (resource == NULL || buffer == NULL || length == NULL)
		return (EINVAL);
	switch (resource->kind) {
	case VMMFS_PCISLOT_RESOURCE_BAR:
		result = ksnprintf(buffer, capacity, "bar%u", resource->index);
		break;
	case VMMFS_PCISLOT_RESOURCE_PIO:
		result = ksnprintf(buffer, capacity, "pio%u", resource->index);
		break;
	case VMMFS_PCISLOT_RESOURCE_ROM:
		result = ksnprintf(buffer, capacity, "rom");
		break;
	case VMMFS_PCISLOT_RESOURCE_DMA:
		result = ksnprintf(buffer, capacity, "dma");
		break;
	case VMMFS_PCISLOT_RESOURCE_KICK:
		result = ksnprintf(buffer, capacity, "kick%u", resource->index);
		break;
	case VMMFS_PCISLOT_RESOURCE_INTX:
		result = ksnprintf(buffer, capacity, "intx");
		break;
	case VMMFS_PCISLOT_RESOURCE_MSI:
		result = ksnprintf(buffer, capacity, "msi%u", resource->index);
		break;
	case VMMFS_PCISLOT_RESOURCE_MSIX:
		result = ksnprintf(buffer, capacity, "msix%u", resource->index);
		break;
	default:
		return (EINVAL);
	}
	if (result < 0 || (size_t)result >= capacity)
		return (EOVERFLOW);
	*length = (size_t)result;
	return (0);
}

int
vmmfs_pcislot_resources_set_decode(struct vmmfs_pcislot_resources *resources,
	bool memory_enabled, bool io_enabled, bool bus_master_enabled)
{
	struct vmmfs_pcislot_resource *resource;
	struct vmmfs_pcislot_objects objects;
	struct vm_object *pager;
	size_t index;
	int error;

	if (resources == NULL || resources->destroying || !resources->powered)
		return (ENXIO);
	for (index = 0; index < resources->count; ++index) {
		resource = &resources->items[index];
		if (resource->kind == VMMFS_PCISLOT_RESOURCE_DMA) {
			lwkt_gettoken(&resource->token);
			resource->bus_master_enabled = bus_master_enabled;
			if (bus_master_enabled) {
				lwkt_reltoken(&resource->token);
				continue;
			}
			objects = resource->objects;
			SLIST_INIT(&resource->objects);
			pager = resource->pager_object;
			if (pager != NULL)
				vm_object_reference_quick(pager);
			lwkt_reltoken(&resource->token);
			vmmfs_pcislot_resource_drain(pager, &objects);
			continue;
		}
		if (resource->kind == VMMFS_PCISLOT_RESOURCE_BAR ||
		    resource->kind == VMMFS_PCISLOT_RESOURCE_ROM) {
			if (memory_enabled &&
			    (resource->kind != VMMFS_PCISLOT_RESOURCE_ROM ||
			    (vmmfs_pcislot_resources_slot(resources)->type0.bytes[0x30] & 1) != 0))
				error = vmmfs_pcislot_resource_map(resource);
			else {
				vmmfs_pcislot_resource_unmap(resource);
				error = 0;
			}
			if (error != 0)
				return (error);
			continue;
		}
		if (resource->kind == VMMFS_PCISLOT_RESOURCE_PIO) {
			if (io_enabled)
				error = vmmfs_pcislot_resource_map(resource);
			else {
				vmmfs_pcislot_resource_unmap(resource);
				error = 0;
			}
			if (error != 0)
				return (error);
		}
	}
	return (0);
}

int
vmmfs_pcislot_resources_bar_relocate(struct vmmfs_pcislot_resources *resources,
	unsigned int bar, uint64_t address)
{
	struct vmmfs_pcislot_resource *resource;
	bool mapped;

	if (resources == NULL || bar >= VMMFS_PCISLOT_MAX_BARS)
		return (EINVAL);
	resource = vmmfs_pcislot_resource_bar(resources, bar);
	if (resource == NULL)
		return (ENOENT);
	mapped = resource->mapped;
	if (mapped)
		vmmfs_pcislot_resource_unmap(resource);
	resource->gpa = address;
	if (mapped)
		return (vmmfs_pcislot_resource_map(resource));
	return (0);
}

int
vmmfs_pcislot_resources_rom_enable(struct vmmfs_pcislot_resources *resources,
	bool enabled)
{
	struct vmmfs_pcislot_resource *resource;

	if (resources == NULL)
		return (EINVAL);
	for (resource = resources->items;
	    resource < resources->items + resources->count; ++resource) {
		if (resource->kind != VMMFS_PCISLOT_RESOURCE_ROM)
			continue;
		if (resource->mapped && !enabled)
			vmmfs_pcislot_resource_unmap(resource);
		if (!resource->mapped && enabled &&
		    (vmmfs_pcislot_resources_slot(resources)->type0.bytes[0x04] & 2) != 0)
			return (vmmfs_pcislot_resource_map(resource));
		return (0);
	}
	return (ENOENT);
}

int
vmmfs_pcislot_resources_rom_relocate(struct vmmfs_pcislot_resources *resources,
	uint64_t address)
{
	struct vmmfs_pcislot_resource *resource;
	bool mapped;

	if (resources == NULL)
		return (EINVAL);
	for (resource = resources->items;
	    resource < resources->items + resources->count; ++resource) {
		if (resource->kind != VMMFS_PCISLOT_RESOURCE_ROM)
			continue;
		mapped = resource->mapped;
		if (mapped)
			vmmfs_pcislot_resource_unmap(resource);
		resource->gpa = address;
		if (mapped)
			return (vmmfs_pcislot_resource_map(resource));
		return (0);
	}
	return (ENOENT);
}

int
vmmfs_pcislot_resources_msix_unmask(struct vmmfs_pcislot_resources *resources,
	unsigned int capability)
{
	struct vmmfs_pcislot_resource *resource;
	struct vmmfs_pcislot_resource *pba;
	const struct vmmfs_pcislot_cap *cap;
	uint64_t pending;
	uint64_t pba_offset;
	int error;

	if (resources == NULL || capability >= VMMFS_PCISLOT_MAX_CAPS)
		return (EINVAL);
	cap = &vmmfs_pcislot_resources_slot(resources)->descriptor.value.caps[capability];
	if (!cap->present || cap->kind != VMMFS_PCISLOT_CAP_MSIX)
		return (EINVAL);
	vmmfs_pcislot_resources_trace_msix_control(resources, capability);
	pba = vmmfs_pcislot_resource_bar(resources, cap->pba_bar);
	if (pba == NULL || pba->backing_object == NULL)
		return (ENXIO);
	for (resource = resources->items;
	    resource < resources->items + resources->count; ++resource) {
		if (resource->kind != VMMFS_PCISLOT_RESOURCE_MSIX ||
		    resource->capability != capability)
			continue;
		pba_offset = cap->pba_offset + (uint64_t)(resource->vector / 64) * 8;
		error = vmmfs_pcislot_resource_object_read(pba, pba_offset,
		    &pending, sizeof(pending));
		if (error != 0)
			return (error);
		if ((pending & (1ULL << (resource->vector % 64))) == 0)
			continue;
		pending &= ~(1ULL << (resource->vector % 64));
		error = vmmfs_pcislot_resource_object_write(pba, pba_offset,
		    &pending, sizeof(pending));
		if (error != 0)
			return (error);
		error = vmmfs_pcislot_resource_raise_msix(resource,
		    resources->machine);
		if (error != 0)
			return (error);
	}
	return (0);
}

static void
vmmfs_pcislot_resources_trace_msix_control(
	struct vmmfs_pcislot_resources *resources, unsigned int capability)
{
	const struct vmmfs_pcislot_cap *cap;
	uint16_t offset;
	uint16_t control;

	if (vmmfs_msix_trace == 0 || resources == NULL ||
	    capability >= VMMFS_PCISLOT_MAX_CAPS)
		return;
	cap = &vmmfs_pcislot_resources_slot(resources)->descriptor.value.caps[capability];
	if (!cap->present || cap->kind != VMMFS_PCISLOT_CAP_MSIX)
		return;
	offset = vmmfs_pcislot_resources_slot(resources)->type0.cap_offset[capability];
	control = vmmfs_pcislot_resource_read16(vmmfs_pcislot_resources_slot(resources)->type0.bytes,
	    offset + 2);
	kprintf("vmmfs: msix_config bdf=%04x cap=%u control=%04x\n",
	    vmmfs_pcislot_resources_slot(resources)->bdf, capability, control);
}

int
vmmfs_pcislot_resources_memory(struct vmmfs_pcislot_resources *resources,
	struct vmmfs_vcpu_thread *thread, const struct vmm_cpuexit *exit)
{
	struct vmmfs_pcislot_resource *resource;
	uint64_t value;
	bool write;
	vmm_vcpu_t vcpu;
	size_t index;
	int error;

	if (resources == NULL || thread == NULL || thread->vcpu == NULL ||
	    exit == NULL ||
	    exit->reason != VMM_CPUEXIT_MEMORY)
		return (ENOENT);
	vcpu = thread->vcpu;
	write = (exit->u.mem.prot & VM_PROT_WRITE) != 0;
	value = 0;
	for (index = 0; index < resources->count; ++index) {
		resource = &resources->items[index];
		if ((resource->kind != VMMFS_PCISLOT_RESOURCE_BAR &&
		    resource->kind != VMMFS_PCISLOT_RESOURCE_ROM) ||
		    !resource->mapped || exit->u.mem.gpa < resource->gpa ||
		    exit->u.mem.gpa - resource->gpa > resource->size ||
			(uint64_t)exit->u.mem.width > resource->size -
			(exit->u.mem.gpa - resource->gpa))
			continue;
		error = vmmfs_pcislot_config_memory(&vmmfs_pcislot_resources_slot(resources)->config,
		    thread, resource, exit);
		if (error != ENOENT)
			return (error);
		if (write) {
			error = vmmfs_pcislot_resource_doorbell(resource,
			    exit->u.mem.gpa, exit->u.mem.width, exit->u.mem.value);
			if (error == 0)
				return (vmm_vcpu_complete_mmio_write(vcpu));
			if (error != ENOENT)
				return (error);
			if (resource->kind == VMMFS_PCISLOT_RESOURCE_ROM)
				return (vmm_vcpu_complete_mmio_write(vcpu));
			error = vmmfs_pcislot_resource_object_write(resource,
			    exit->u.mem.gpa - resource->gpa, &exit->u.mem.value,
			    exit->u.mem.width);
			if (error != 0)
				return (error);
			return (vmm_vcpu_complete_mmio_write(vcpu));
		}
		error = vmmfs_pcislot_resource_object_read(resource,
		    exit->u.mem.gpa - resource->gpa, &value, exit->u.mem.width);
		if (error != 0)
			return (error);
		return (vmm_vcpu_complete_mmio_read(vcpu, &value,
		    exit->u.mem.width));
	}
	return (ENOENT);
}

int
vmmfs_pcislot_resources_io(struct vmmfs_pcislot_resources *resources,
	struct vmmfs_vcpu_thread *thread, struct vmm_cpustate *state,
	const struct vmm_cpuexit *exit)
{
	struct vmmfs_pcislot_resource *resource;
	size_t index;
	int error;

	if (resources == NULL || thread == NULL || thread->vcpu == NULL ||
	    state == NULL || exit == NULL ||
	    exit->reason != VMM_CPUEXIT_IO || exit->u.io.str || exit->u.io.rep)
		return (ENOENT);
	if (exit->u.io.operand_size != 1 && exit->u.io.operand_size != 2 &&
	    exit->u.io.operand_size != 4)
		return (EOPNOTSUPP);
	for (index = 0; index < resources->count; ++index) {
		resource = &resources->items[index];
		if (resource->kind != VMMFS_PCISLOT_RESOURCE_PIO ||
		    !resource->mapped || exit->u.io.port < resource->gpa ||
		    exit->u.io.port - resource->gpa > resource->size ||
		    (uint64_t)exit->u.io.operand_size > resource->size -
		    (exit->u.io.port - resource->gpa))
			continue;
		error = vmmfs_pcislot_config_io(&vmmfs_pcislot_resources_slot(resources)->config, thread,
		    resource, state, exit);
		if (error != ENOENT)
			return (error);
		if (exit->u.io.in) {
			uint64_t value;
			uint64_t mask;

			value = 0;
			error = vmmfs_pcislot_resource_object_read(resource,
			    exit->u.io.port - resource->gpa, &value,
			    exit->u.io.operand_size);
			if (error != 0)
				return (error);
			mask = (1ULL << (exit->u.io.operand_size * NBBY)) - 1;
			/* An IN to EAX zero-extends; AL/AX preserve the upper bits. */
			state->gprs[VMM_X64_GPR_RAX] =
			    exit->u.io.operand_size == 4 ? (uint32_t)value :
			    (state->gprs[VMM_X64_GPR_RAX] & ~mask) | (value & mask);
		} else {
			error = vmmfs_pcislot_resource_doorbell(resource,
			    exit->u.io.port, (enum vmm_io_width)exit->u.io.operand_size,
			    state->gprs[VMM_X64_GPR_RAX]);
			if (error == ENOENT)
				error = vmmfs_pcislot_resource_object_write(resource,
				    exit->u.io.port - resource->gpa,
				    &state->gprs[VMM_X64_GPR_RAX],
				    exit->u.io.operand_size);
			if (error != 0)
				return (error);
		}
		state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
		return (0);
	}
	return (ENOENT);
}





static void
vmmfs_pcislot_resources_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_resources *resources;
	size_t index;

	resources = (struct vmmfs_pcislot_resources *)node;
	KKASSERT(resources->node.references == 0);
	KKASSERT(resources->destroying);
	KKASSERT(resources->machine == NULL);
	for (index = 0; index < resources->initialized_count; ++index)
		KKASSERT(resources->items[index].node.drop == NULL);
	lwkt_token_uninit(&resources->token);
	kfree(resources, M_VMMFS);
}

static void
vmmfs_pcislot_resource_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_resource *resource;

	resource = (struct vmmfs_pcislot_resource *)node;
	KKASSERT(node->references == 0);
	KKASSERT(resource->pager_object == NULL);
	KKASSERT(SLIST_EMPTY(&resource->objects));
	KKASSERT(!resource->mapped);
	if (resource->backing_object != NULL)
		vm_object_deallocate(resource->backing_object);
	if (resource->vmspace != NULL)
		vmspace_rel(resource->vmspace);
	if (resource->dev != NULL) {
		resource->dev->si_drv1 = NULL;
		destroy_only_dev(resource->dev);
	}
	if (resource->traps != NULL)
		kfree(resource->traps, M_VMMFS);
	lwkt_token_uninit(&resource->token);
}

static bool
vmmfs_pcislot_resource_mappable(const struct vmmfs_pcislot_resource *resource)
{
	return resource->kind == VMMFS_PCISLOT_RESOURCE_BAR ||
	    resource->kind == VMMFS_PCISLOT_RESOURCE_PIO ||
	    resource->kind == VMMFS_PCISLOT_RESOURCE_ROM ||
	    resource->kind == VMMFS_PCISLOT_RESOURCE_DMA;
}

static bool
vmmfs_pcislot_resource_enabled(const struct vmmfs_pcislot_resource *resource)
{
	return resource != NULL && vmmfs_pcislot_resource_resources(resource) != NULL &&
	    vmmfs_pcislot_resource_resources(resource)->powered && !vmmfs_pcislot_resource_resources(resource)->destroying &&
	    !resource->revoked;
}

static int
vmmfs_pcislot_resource_create_mapping(struct vmmfs_pcislot_resource *resource)
{
	struct vmspace *vmspace;
	struct vmmfs_machine *machine;
	struct vmmfs_pcislot *slot;
	struct vmmfs_pcislot_resources *resources;
	uint32_t serial;

	if (!vmmfs_pcislot_resource_mappable(resource))
		return (0);
	if (resource->size == 0 || resource->size > UINT64_MAX - PAGE_MASK)
		return (EINVAL);
	resource->mapping_size = round_page(resource->size);
	if (resource->kind != VMMFS_PCISLOT_RESOURCE_DMA) {
		resource->backing_object = vm_object_allocate(OBJT_DEFAULT,
		    OFF_TO_IDX(resource->mapping_size));
		if (resource->backing_object == NULL)
			return (ENOMEM);
	} else {
		resources = vmmfs_pcislot_resource_resources(resource);
		slot = vmmfs_pcislot_resources_slot(resources);
		machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot));
		vmspace = machine->memory.run_vmspace;
		if (vmspace == NULL)
			return (ENXIO);
		vmspace_ref(vmspace);
		resource->vmspace = vmspace;
	}
	resource->pager_object = cdev_pager_allocate(resource, OBJT_MGTDEVICE,
	    &vmmfs_pcislot_resource_pager_ops, resource->mapping_size,
	    VM_PROT_READ | VM_PROT_WRITE, 0, proc0.p_ucred);
	if (resource->pager_object == NULL)
		return (ENOMEM);
	serial = atomic_fetchadd_int(&vmmfs_pcislot_resource_serial, 1);
	resource->dev = make_only_dev(&vmmfs_pcislot_resource_dev_ops, serial,
	    UID_ROOT, GID_WHEEL, VMMFS_PCISLOT_RESOURCE_MODE, "vmmfspci%d",
	    serial);
	if (resource->dev == NULL)
		return (ENXIO);
	resource->dev->si_drv1 = resource;
	return (0);
}

static int
vmmfs_pcislot_resource_create_vnode(struct vmmfs_pcislot_resource *resource,
	struct vmmfs_mount *mount)
{
	if (vmmfs_pcislot_resource_mappable(resource)) {
		return (vmmfs_vnode_create_cdev(mount->mount,
		    &mount->pcislot_resource_vops, resource->dev, &resource->node));
	}
	return (vmmfs_vnode_create_regular(mount->mount,
	    &mount->pcislot_resource_vops, VREG, &resource->node));
}

/*
 * Caller holds the resource token and page busy.  Keep each returned page's
 * object alive until the corresponding provider mappings have been drained.
 * Allocation can sleep, so repeat admission and identity checks afterwards.
 */
static int
vmmfs_pcislot_resource_track_page(struct vmmfs_pcislot_resource *resource,
	vm_page_t page, struct vmspace *vmspace)
{
	struct vmmfs_pcislot_object *record, *candidate = NULL;
	int error;

	for (;;) {
		error = 0;
		if (resource->revoked)
			error = EFAULT;
		else if (resource->kind == VMMFS_PCISLOT_RESOURCE_DMA) {
			if (!resource->bus_master_enabled)
				error = EFAULT;
			else if (resource->vmspace != vmspace)
				error = EAGAIN;
		}
		if (error != 0 || page->object == NULL) {
			if (candidate != NULL)
				kfree(candidate, M_VMMFS);
			return (error != 0 ? error : EFAULT);
		}
		SLIST_FOREACH(record, &resource->objects, entry) {
			if (record->object != page->object)
				continue;
			if (candidate != NULL)
				kfree(candidate, M_VMMFS);
			return (0);
		}
		if (candidate == NULL) {
			candidate = kmalloc(sizeof(*candidate), M_VMMFS, M_WAITOK);
			continue;
		}
		candidate->object = page->object;
		vm_object_reference_quick(candidate->object);
		SLIST_INSERT_HEAD(&resource->objects, candidate, entry);
		return (0);
	}
}

static int
vmmfs_pcislot_resource_drain_page(vm_page_t page, void *argument)
{
	bool *retry = argument;

	if (vm_page_busy_try(page, TRUE)) {
		vm_page_sleep_busy(page, TRUE, "vmmrpcipg");
		*retry = true;
	} else {
		vm_page_wakeup(page);
	}
	return (0);
}

/* Consume the detached object references and the captured pager reference. */
static void
vmmfs_pcislot_resource_drain(struct vm_object *pager,
	struct vmmfs_pcislot_objects *objects)
{
	struct vmmfs_pcislot_object *record;
	bool retry;

	SLIST_FOREACH(record, objects, entry) {
		VM_OBJECT_LOCK(record->object);
		do {
			retry = false;
			vm_page_rb_tree_RB_SCAN(&record->object->rb_memq, NULL,
			    vmmfs_pcislot_resource_drain_page, &retry);
		} while (retry);
		VM_OBJECT_UNLOCK(record->object);
	}
	if (pager != NULL)
		vm_object_page_remove(pager, 0, 0, FALSE);
	while ((record = SLIST_FIRST(objects)) != NULL) {
		SLIST_REMOVE_HEAD(objects, entry);
		vm_object_deallocate(record->object);
		kfree(record, M_VMMFS);
	}
	if (pager != NULL)
		vm_object_deallocate(pager);
}

static void
vmmfs_pcislot_resource_revoke(struct vmmfs_pcislot_resource *resource)
{
	struct vmmfs_pcislot_objects objects;
	struct vm_object *pager_object;
	struct vmspace *vmspace;

	lwkt_gettoken(&resource->token);
	if (resource->revoked) {
		lwkt_reltoken(&resource->token);
		return;
	}
	resource->revoked = true;
	pager_object = resource->pager_object;
	if (pager_object != NULL)
		vm_object_reference_quick(pager_object);
	objects = resource->objects;
	SLIST_INIT(&resource->objects);
	vmspace = resource->vmspace;
	resource->vmspace = NULL;
	lwkt_reltoken(&resource->token);
	vmmfs_pcislot_resource_drain(pager_object, &objects);
	if (vmspace != NULL)
		vmspace_rel(vmspace);
	wakeup(resource);
	KNOTE(&resource->read_kq.ki_note, 0);
}

static int
vmmfs_pcislot_resource_map(struct vmmfs_pcislot_resource *resource)
{
	int error;

	if (!vmmfs_pcislot_resource_enabled(resource))
		return (ENXIO);
	if (resource->mapped)
		return (0);
	resource->mapped = true;
	error = vmmfs_pcislot_resource_install_traps(resource,
	    vmmfs_pcislot_resource_resources(resource)->machine);
	if (error != 0)
		resource->mapped = false;
	return (error);
}

static void
vmmfs_pcislot_resource_unmap(struct vmmfs_pcislot_resource *resource)
{
	if (resource == NULL || !resource->mapped)
		return;
	vmmfs_pcislot_resource_remove_traps(resource,
	    vmmfs_pcislot_resource_resources(resource)->machine);
	resource->mapped = false;
}

static int
vmmfs_pcislot_resource_install_traps(struct vmmfs_pcislot_resource *resource,
	vmm_machine_t machine)
{
	const struct vmmfs_pcislot_descriptor_value *value;
	const struct vmmfs_pcislot_doorbell *doorbell;
	const struct vmmfs_pcislot_config_register *config;
	struct vmmfs_pcislot_resource_trap *trap;
	size_t count;
	size_t index;
	unsigned int doorbell_index;
	unsigned int config_index;
	int error;

	if (resource->kind != VMMFS_PCISLOT_RESOURCE_BAR &&
	    resource->kind != VMMFS_PCISLOT_RESOURCE_PIO)
		return (0);
	value = &vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->descriptor.value;
	count = 0;
	for (doorbell_index = 0; doorbell_index < VMMFS_PCISLOT_MAX_DOORBELLS;
	    ++doorbell_index) {
		doorbell = &value->doorbells[doorbell_index];
		if (doorbell->present && doorbell->bar == resource->index)
			++count;
	}
	for (config_index = 0; config_index < VMMFS_PCISLOT_MAX_CONFIGS;
	    ++config_index) {
		config = &value->configs[config_index];
		if (config->present && config->bar == resource->index)
			++count;
	}
	if (count == 0)
		return (0);
	resource->traps = kmalloc(count * sizeof(*resource->traps), M_VMMFS,
	    M_WAITOK | M_ZERO);
	resource->trap_count = count;
	index = 0;
	for (doorbell_index = 0; doorbell_index < VMMFS_PCISLOT_MAX_DOORBELLS;
	    ++doorbell_index) {
		doorbell = &value->doorbells[doorbell_index];
		if (!doorbell->present || doorbell->bar != resource->index)
			continue;
		trap = &resource->traps[index++];
		trap->base = resource->gpa + doorbell->offset;
		trap->size = doorbell->size;
		if (resource->kind == VMMFS_PCISLOT_RESOURCE_BAR)
			error = vmm_machine_trap_mmio_write(machine,
			    trap->base, trap->size, vmmfs_pcislot_resource_mmio_write,
			    resource, &trap->write_io);
		else
			error = vmm_machine_trap_pio_write(machine,
			    (uint16_t)trap->base, (uint32_t)trap->size,
			    vmmfs_pcislot_resource_pio_write, resource, &trap->write_io);
		if (error != 0) {
			vmmfs_pcislot_resource_remove_traps(resource, machine);
			return (error);
		}
	}
	for (config_index = 0; config_index < VMMFS_PCISLOT_MAX_CONFIGS;
	    ++config_index) {
		config = &value->configs[config_index];
		if (!config->present || config->bar != resource->index)
			continue;
		trap = &resource->traps[index++];
		trap->base = resource->gpa + config->offset;
		trap->size = config->width;
		if (resource->kind == VMMFS_PCISLOT_RESOURCE_BAR) {
			error = vmm_machine_trap_mmio_read(machine,
			    trap->base, trap->size, vmmfs_pcislot_resource_mmio_read,
			    resource, &trap->read_io);
			if (error == 0)
				error = vmm_machine_trap_mmio_write(
				    machine, trap->base, trap->size,
				    vmmfs_pcislot_resource_config_mmio_write, resource,
				    &trap->write_io);
		} else {
			error = vmm_machine_trap_pio_read(machine,
			    (uint16_t)trap->base, (uint32_t)trap->size,
			    vmmfs_pcislot_resource_pio_read, resource, &trap->read_io);
			if (error == 0)
				error = vmm_machine_trap_pio_write(
				    machine, (uint16_t)trap->base,
				    (uint32_t)trap->size,
				    vmmfs_pcislot_resource_config_pio_write, resource,
				    &trap->write_io);
		}
		if (error != 0) {
			vmmfs_pcislot_resource_remove_traps(resource, machine);
			return (error);
		}
	}
	return (0);
}

static void
vmmfs_pcislot_resource_remove_traps(struct vmmfs_pcislot_resource *resource,
	vmm_machine_t machine)
{
	size_t index;

	if (resource == NULL || resource->traps == NULL)
		return;
	for (index = 0; index < resource->trap_count; ++index) {
		if (resource->traps[index].read_io != NULL)
			(void)vmm_machine_untrap(machine,
			    resource->traps[index].read_io);
		if (resource->traps[index].write_io != NULL)
			(void)vmm_machine_untrap(machine,
			    resource->traps[index].write_io);
	}
	kfree(resource->traps, M_VMMFS);
	resource->traps = NULL;
	resource->trap_count = 0;
}

static int
vmmfs_pcislot_resource_queue_kick(struct vmmfs_pcislot_resource *resource,
	uint64_t offset, enum vmm_io_width width, uint64_t value)
{
	bool notify;

	lwkt_gettoken(&resource->token);
	if (!vmmfs_pcislot_resource_enabled(resource)) {
		lwkt_reltoken(&resource->token);
		return (ENXIO);
	}
	notify = !resource->kick_pending;
	resource->kick.offset = offset;
	resource->kick.value = value;
	resource->kick.width = width;
	bzero(resource->kick.reserved, sizeof(resource->kick.reserved));
	resource->kick_pending = true;
	++resource->sequence;
	lwkt_reltoken(&resource->token);
	if (notify)
		KNOTE(&resource->read_kq.ki_note, 0);
	wakeup(resource);
	return (0);
}

static int
vmmfs_pcislot_resource_doorbell(struct vmmfs_pcislot_resource *resource,
	uint64_t address, enum vmm_io_width width, uint64_t value)
{
	const struct vmmfs_pcislot_descriptor_value *descriptor;
	const struct vmmfs_pcislot_doorbell *doorbell;
	struct vmmfs_pcislot_resource *kick;
	uint64_t base;
	unsigned int index;

	descriptor = &vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->descriptor.value;
	for (index = 0; index < VMMFS_PCISLOT_MAX_DOORBELLS; ++index) {
		doorbell = &descriptor->doorbells[index];
		if (!doorbell->present || doorbell->bar != resource->index ||
		    doorbell->width != width)
			continue;
		base = resource->gpa + doorbell->offset;
		if (address < base || (uint64_t)width > doorbell->size ||
		    address - base > doorbell->size - (uint64_t)width)
			continue;
		for (kick = vmmfs_pcislot_resource_resources(resource)->items;
		    kick < vmmfs_pcislot_resource_resources(resource)->items + vmmfs_pcislot_resource_resources(resource)->count;
		    ++kick) {
			if (kick->kind == VMMFS_PCISLOT_RESOURCE_KICK &&
			    kick->index == index)
				return (vmmfs_pcislot_resource_queue_kick(kick,
				    address - base, width, value));
		}
		return (ENOENT);
	}
	return (ENOENT);
}

static struct vmmfs_pcislot_resource *
vmmfs_pcislot_resource_bar(struct vmmfs_pcislot_resources *resources,
	unsigned int bar)
{
	struct vmmfs_pcislot_resource *resource;

	for (resource = resources->items;
	    resource < resources->items + resources->count; ++resource) {
		if ((resource->kind == VMMFS_PCISLOT_RESOURCE_BAR ||
		    resource->kind == VMMFS_PCISLOT_RESOURCE_PIO) &&
		    resource->index == bar)
			return (resource);
	}
	return (NULL);
}

static uint16_t
vmmfs_pcislot_resource_read16(const uint8_t *bytes, uint16_t offset)
{
	return (bytes[offset] | (uint16_t)bytes[offset + 1] << 8);
}

static uint32_t
vmmfs_pcislot_resource_read32(const uint8_t *bytes, uint16_t offset)
{
	return (bytes[offset] | (uint32_t)bytes[offset + 1] << 8 |
	    (uint32_t)bytes[offset + 2] << 16 |
	    (uint32_t)bytes[offset + 3] << 24);
}

static int
vmmfs_pcislot_resource_object_read(struct vmmfs_pcislot_resource *resource,
	uint64_t offset, void *buffer, size_t length)
{
	vm_page_t page;
	char *cursor;
	size_t chunk;

	if (resource == NULL || resource->backing_object == NULL || buffer == NULL ||
	    offset > resource->size || length > resource->size - offset)
		return (EINVAL);
	cursor = buffer;
	while (length != 0) {
		chunk = min(length, (size_t)PAGE_SIZE - (offset & PAGE_MASK));
		page = vm_page_grab(resource->backing_object, OFF_TO_IDX(offset),
		    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
		if (page == NULL)
			return (ENOMEM);
		if (page->valid != VM_PAGE_BITS_ALL)
			vm_page_zero_invalid(page, TRUE);
		bcopy(PHYS_TO_DMAP(VM_PAGE_TO_PHYS(page)) + (offset & PAGE_MASK),
		    cursor, chunk);
		vm_page_wakeup(page);
		cursor += chunk;
		offset += chunk;
		length -= chunk;
	}
	return (0);
}

static int
vmmfs_pcislot_resource_object_write(struct vmmfs_pcislot_resource *resource,
	uint64_t offset, const void *buffer, size_t length)
{
	vm_page_t page;
	const char *cursor;
	size_t chunk;

	if (resource == NULL || resource->backing_object == NULL || buffer == NULL ||
	    offset > resource->size || length > resource->size - offset)
		return (EINVAL);
	cursor = buffer;
	while (length != 0) {
		chunk = min(length, (size_t)PAGE_SIZE - (offset & PAGE_MASK));
		page = vm_page_grab(resource->backing_object, OFF_TO_IDX(offset),
		    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
		if (page == NULL)
			return (ENOMEM);
		if (page->valid != VM_PAGE_BITS_ALL)
			vm_page_zero_invalid(page, TRUE);
		bcopy(cursor, PHYS_TO_DMAP(VM_PAGE_TO_PHYS(page)) +
		    (offset & PAGE_MASK), chunk);
		vm_page_wakeup(page);
		cursor += chunk;
		offset += chunk;
		length -= chunk;
	}
	return (0);
}

static uint32_t
vmmfs_pcislot_resource_object_read32(struct vmmfs_pcislot_resource *resource,
	uint64_t offset)
{
	uint32_t value;

	if (vmmfs_pcislot_resource_object_read(resource, offset, &value,
	    sizeof(value)) != 0)
		return (UINT32_MAX);
	return (value);
}

static int
vmmfs_pcislot_resource_raise_msi(struct vmmfs_pcislot_resource *resource,
	vmm_machine_t machine)
{
	const struct vmmfs_pcislot_cap *cap;
	const uint8_t *bytes;
	uint16_t offset;
	uint16_t control;
	uint64_t address;
	uint32_t data;
	uint32_t mask;
	uint32_t enabled;

	cap = &vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->descriptor.value.caps[resource->capability];
	bytes = vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->type0.bytes;
	offset = vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->type0.cap_offset[resource->capability];
	control = vmmfs_pcislot_resource_read16(bytes, offset + 2);
	if ((control & 1) == 0)
		return (0);
	enabled = 1U << ((control >> 4) & 7);
	if (resource->vector >= enabled)
		return (0);
	if (cap->maskable) {
		mask = vmmfs_pcislot_resource_read32(bytes,
		    offset + (cap->address_width == 64 ? 16 : 12));
		if ((mask & (1U << resource->vector)) != 0)
			return (0);
	}
	address = vmmfs_pcislot_resource_read32(bytes, offset + 4);
	if (cap->address_width == 64) {
		address |= (uint64_t)vmmfs_pcislot_resource_read32(bytes, offset + 8)
		    << 32;
		data = vmmfs_pcislot_resource_read16(bytes, offset + 12);
	} else {
		data = vmmfs_pcislot_resource_read16(bytes, offset + 8);
	}
	return (vmm_machine_raise_msi(machine, address,
	    data + resource->vector));
}

static int
vmmfs_pcislot_resource_raise_msix(struct vmmfs_pcislot_resource *resource,
	vmm_machine_t machine)
{
	const struct vmmfs_pcislot_cap *cap;
	const uint8_t *bytes;
	struct vmmfs_pcislot_resource *table;
	struct vmmfs_pcislot_resource *pba;
	uint16_t offset;
	uint16_t control;
	uint64_t entry;
	uint64_t address;
	uint64_t pending;
	uint64_t pba_offset;
	uint32_t data;
	uint32_t vector_control;
	int error;

	cap = &vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->descriptor.value.caps[resource->capability];
	bytes = vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->type0.bytes;
	offset = vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->type0.cap_offset[resource->capability];
	control = vmmfs_pcislot_resource_read16(bytes, offset + 2);
	table = vmmfs_pcislot_resource_bar(vmmfs_pcislot_resource_resources(resource), cap->table_bar);
	pba = vmmfs_pcislot_resource_bar(vmmfs_pcislot_resource_resources(resource), cap->pba_bar);
	if (table == NULL || pba == NULL || table->backing_object == NULL ||
	    pba->backing_object == NULL) {
		vmmfs_pcislot_resource_msix_trace(resource, control, 0, 0, 0, 0,
		    "no_backing", ENXIO);
		return (ENXIO);
	}
	entry = cap->table_offset + (uint64_t)resource->vector * 16;
	vector_control = vmmfs_pcislot_resource_object_read32(table, entry + 12);
	address = vmmfs_pcislot_resource_object_read32(table, entry);
	address |= (uint64_t)vmmfs_pcislot_resource_object_read32(table,
	    entry + 4) << 32;
	data = vmmfs_pcislot_resource_object_read32(table, entry + 8);
	pba_offset = cap->pba_offset +
	    (uint64_t)(resource->vector / 64) * sizeof(uint64_t);
	error = vmmfs_pcislot_resource_object_read(pba, pba_offset, &pending,
	    sizeof(pending));
	if (error != 0) {
		vmmfs_pcislot_resource_msix_trace(resource, control, vector_control,
		    address, data, 0, "pba_read", error);
		return (error);
	}
	if ((control & 0x8000) == 0) {
		vmmfs_pcislot_resource_msix_trace(resource, control, vector_control,
		    address, data, pending, "disabled", 0);
		return (0);
	}
	if ((control & 0x4000) != 0 || (vector_control & 1) != 0) {
		pending |= 1ULL << (resource->vector % 64);
		error = vmmfs_pcislot_resource_object_write(pba, pba_offset,
		    &pending, sizeof(pending));
		vmmfs_pcislot_resource_msix_trace(resource, control, vector_control,
		    address, data, pending,
		    (control & 0x4000) != 0 ? "function_mask" : "vector_mask",
		    error);
		if (error != 0)
			return (error);
		return (0);
	}
	error = vmm_machine_raise_msi(machine, address, data);
	vmmfs_pcislot_resource_msix_trace(resource, control, vector_control,
	    address, data, pending, "raise", error);
	return (error);
}

static void
vmmfs_pcislot_resource_msix_trace(
	struct vmmfs_pcislot_resource *resource, uint16_t control,
	uint32_t vector_control, uint64_t address, uint32_t data,
	uint64_t pending, const char *outcome, int error)
{

	if (vmmfs_msix_trace == 0)
		return;
	kprintf("vmmfs: msix bdf=%04x vector=%u control=%04x "
	    "entry_control=%08x address=%016jx data=%08x pba=%016jx "
	    "outcome=%s error=%d\n", vmmfs_pcislot_resources_slot(vmmfs_pcislot_resource_resources(resource))->bdf,
	    resource->vector, control, vector_control, (uintmax_t)address, data,
	    (uintmax_t)pending, outcome, error);
}

static int
vmmfs_pcislot_resource_close(struct vop_close_args *ap)
{
	struct vnode *vnode;
	cdev_t dev;
	int error;

	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	error = 0;
	if (dev != NULL && vnode->v_opencount <= 1) {
		vn_unlock(vnode);
		error = dev_dclose(dev, ap->a_fflag, S_IFCHR, ap->a_fp);
		vn_lock(vnode, LK_SHARED | LK_RETRY);
	}
	if (vnode->v_opencount > 0)
		vop_stdclose(ap);
	return (error);
}

static int
vmmfs_pcislot_resource_subscribe(struct vmmfs_pcislot_resource *resource,
	struct knote *knote)
{
	int error;

	lwkt_gettoken(&resource->token);
	if (!vmmfs_pcislot_resource_enabled(resource)) {
		error = ENXIO;
	} else if (resource->kind != VMMFS_PCISLOT_RESOURCE_KICK ||
	    knote->kn_filter != EVFILT_READ) {
		error = EOPNOTSUPP;
	} else {
		knote->kn_fop = &vmmfs_pcislot_resource_read_filterops;
		knote->kn_hook = (caddr_t)resource;
		knote_insert(&resource->read_kq.ki_note, knote);
		error = 0;
	}
	lwkt_reltoken(&resource->token);
	return (error);
}

static int
vmmfs_pcislot_resource_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_pcislot_resource *resource = ap->a_vp->v_data;

	return (VMMFS_WORK(resource,
	    vmmfs_pcislot_resource_subscribe(resource, ap->a_kn)));
}

static int
vmmfs_pcislot_resource_authorize(struct vmmfs_pcislot_resource *resource)
{
	if (vmmfs_pcislot_auth_check(vmmfs_pcislot_resources_slot(
	    vmmfs_pcislot_resource_resources(resource))) != 0)
		return (EACCES);
	return (vmmfs_pcislot_resource_enabled(resource) ? 0 : ENXIO);
}

static int
vmmfs_pcislot_resource_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_resource *resource;
	struct vnode *vnode;
	cdev_t dev;
	int error;

	resource = ap->a_vp->v_data;
	error = VMMFS_WORK(resource,
	    vmmfs_pcislot_resource_authorize(resource));
	if (error != 0)
		return (error);
	if (!vmmfs_pcislot_resource_mappable(resource))
		return (vop_stdopen(ap));
	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	if (dev == NULL)
		return (ENXIO);
	vn_unlock(vnode);
	error = dev_dopen(dev, ap->a_mode, S_IFCHR, ap->a_cred, ap->a_fpp,
	    vnode);
	vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0)
		return (error);
	error = VMMFS_WORK(resource,
	    vmmfs_pcislot_resource_authorize(resource));
	if (error != 0) {
		vn_unlock(vnode);
		(void)dev_dclose(dev, ap->a_mode, S_IFCHR, *ap->a_fpp);
		vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
		return (ENXIO);
	}
	return (vop_stdopen(ap));
}

static int
vmmfs_pcislot_resource_receive(struct vmmfs_pcislot_resource *resource,
	struct vmmfs_pci_kick *kick)
{
	int error = 0;

	lwkt_gettoken(&resource->token);
	if (!vmmfs_pcislot_resource_enabled(resource))
		error = ENXIO;
	else if (resource->kind != VMMFS_PCISLOT_RESOURCE_KICK)
		error = EOPNOTSUPP;
	else if (resource->kick_pending) {
		*kick = resource->kick;
		resource->kick_pending = false;
	} else {
		tsleep_interlock(resource, PCATCH);
		error = EAGAIN;
	}
	lwkt_reltoken(&resource->token);
	return (error);
}

static int
vmmfs_pcislot_resource_read(struct vop_read_args *ap)
{
	struct vmmfs_pcislot_resource *resource = ap->a_vp->v_data;
	struct vmmfs_pci_kick kick;
	int error;

	if (ap->a_uio->uio_resid != sizeof(kick))
		return (EINVAL);
	for (;;) {
		error = VMMFS_WORK(resource,
		    vmmfs_pcislot_resource_receive(resource, &kick));
		if (error != EAGAIN)
			break;
		if ((ap->a_ioflag & IO_NDELAY) != 0) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			return (EAGAIN);
		}
		error = tsleep(resource, PINTERLOCKED | PCATCH, "vmmfspcikick", 0);
		if (error != 0)
			return (error);
	}
	if (error == 0)
		error = uiomove((caddr_t)&kick, sizeof(kick), ap->a_uio);
	return (error);
}


static int
vmmfs_pcislot_resource_raise(struct vmmfs_pcislot_resource *resource,
	bool asserted)
{
	struct vmmfs_pcislot_resources *resources;
	struct vmmfs_vcpu *vcpu;
	vmm_machine_t machine;
	bool wake;
	int error;

	resources = vmmfs_pcislot_resource_resources(resource);
	vcpu = &vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(
	    vmmfs_pcislot_resources_slot(resources)))->vcpu;
	lwkt_gettoken(&resources->token);
	lwkt_gettoken(&vcpu->token);
	machine = NULL;
	if (!vmmfs_pcislot_resource_enabled(resource)) {
		error = ENXIO;
	} else {
		error = 0;
		if (!vcpu->stop_requested && !vcpu->reset_requested)
			machine = resources->machine;
		if (machine != NULL)
			++resources->interrupt_users;
	}
	lwkt_reltoken(&vcpu->token);
	lwkt_reltoken(&resources->token);
	/* Reset discards completions while the old destination is unbound. */
	if (error != 0 || machine == NULL)
		return (error);

	if (resource->kind == VMMFS_PCISLOT_RESOURCE_INTX) {
		lwkt_gettoken(&resource->token);
		resource->intx_asserted = asserted != 0;
		error = vmm_machine_set_irq(machine,
		    vmmfs_pcislot_resources_slot(resources)->type0.intx_gsi,
		    resource->intx_asserted);
		lwkt_reltoken(&resource->token);
	} else if (resource->kind == VMMFS_PCISLOT_RESOURCE_MSI) {
		error = vmmfs_pcislot_resource_raise_msi(resource, machine);
	} else {
		error = vmmfs_pcislot_resource_raise_msix(resource, machine);
	}
	/* AP teardown precedes PCI unbind at the reset/stop barrier. */
	lwkt_gettoken(&vcpu->token);
	if (error == ENOENT &&
	    (vcpu->stop_requested || vcpu->reset_requested))
		error = 0;
	lwkt_reltoken(&vcpu->token);
	lwkt_gettoken(&resources->token);
	if (error == ENOENT && resources->machine != machine)
		error = 0;
	KKASSERT(resources->interrupt_users != 0);
	wake = --resources->interrupt_users == 0;
	lwkt_reltoken(&resources->token);
	if (wake)
		wakeup(resources);
	return (error);
}

static int
vmmfs_pcislot_resource_write(struct vop_write_args *ap)
{
	struct vmmfs_pcislot_resource *resource;
	struct vmmfs_pci_intx intx;
	struct vmmfs_pci_interrupt interrupt;
	int error;

	resource = ap->a_vp->v_data;
	if (resource == NULL)
		return (ENXIO);
	/* User faults cannot pin the guest runtime or postpone power-off. */
	if (resource->kind == VMMFS_PCISLOT_RESOURCE_INTX) {
		if (ap->a_uio->uio_resid != sizeof(intx))
			return (EINVAL);
		error = uiomove((caddr_t)&intx, sizeof(intx), ap->a_uio);
		if (error != 0)
			return (error);
		if (intx.asserted > 1 || bcmp(intx.reserved,
		    "\0\0\0\0\0\0\0", sizeof(intx.reserved)) != 0)
			return (EINVAL);
	} else {
		if (resource->kind != VMMFS_PCISLOT_RESOURCE_MSI &&
		    resource->kind != VMMFS_PCISLOT_RESOURCE_MSIX)
			return (EOPNOTSUPP);
		if (ap->a_uio->uio_resid != sizeof(interrupt))
			return (EINVAL);
		error = uiomove((caddr_t)&interrupt, sizeof(interrupt), ap->a_uio);
		if (error != 0)
			return (error);
		if (interrupt.reserved != 0)
			return (EINVAL);
	}

	return (VMMFS_WORK(resource, vmmfs_pcislot_resource_raise(resource,
	    resource->kind == VMMFS_PCISLOT_RESOURCE_INTX && intx.asserted != 0)));
}

static int
vmmfs_pcislot_resource_dev_open(struct dev_open_args *ap)
{
	(void)ap;
	return (0);
}

static int
vmmfs_pcislot_resource_dev_close(struct dev_close_args *ap)
{
	(void)ap;
	return (0);
}

static int
vmmfs_pcislot_resource_get_mapping(struct vmmfs_pcislot_resource *resource,
	vm_ooffset_t offset, vm_size_t size, struct vm_object **objectp)
{
	struct vm_object *object;
	lwkt_gettoken(&resource->token);
	if (!vmmfs_pcislot_resource_enabled(resource) ||
	    resource->pager_object == NULL) {
		lwkt_reltoken(&resource->token);
		return (EINVAL);
	}
	if (offset < 0 || offset > resource->mapping_size ||
	    size > resource->mapping_size - offset) {
		lwkt_reltoken(&resource->token);
		return (EINVAL);
	}
	object = resource->pager_object;
	VM_OBJECT_LOCK(object);
	if (resource->revoked) {
		VM_OBJECT_UNLOCK(object);
		lwkt_reltoken(&resource->token);
		return (EINVAL);
	}
	vm_object_reference_locked(object);
	VM_OBJECT_UNLOCK(object);
	lwkt_reltoken(&resource->token);
	*objectp = object;
	return (0);
}

static int
vmmfs_pcislot_resource_dev_mmap_single(struct dev_mmap_single_args *ap)
{
	struct vmmfs_pcislot_resource *resource;

	if (ap == NULL || ap->a_head.a_dev == NULL)
		return (EINVAL);
	resource = ap->a_head.a_dev->si_drv1;
	if (resource == NULL || (ap->a_nprot & VM_PROT_EXECUTE) != 0)
		return (EINVAL);
	return (VMMFS_WORK(resource,
	    vmmfs_pcislot_resource_get_mapping(resource, *ap->a_offset,
	    ap->a_size, ap->a_object)));
}

static int
vmmfs_pcislot_resource_pager_ctor(void *handle, vm_ooffset_t size,
	vm_prot_t prot, vm_ooffset_t offset, struct ucred *cred, u_short *color)
{
	struct vmmfs_pcislot_resource *resource;

	(void)cred;
	resource = handle;
	if (resource == NULL || color == NULL || (prot & VM_PROT_EXECUTE) != 0 ||
	    offset < 0 || offset > resource->mapping_size ||
	    size > resource->mapping_size - offset)
		return (EINVAL);
	lwkt_gettoken(&resource->token);
	if (resource->revoked) {
		lwkt_reltoken(&resource->token);
		return (EINVAL);
	}
	vmmfs_node_hold(&resource->node);
	lwkt_reltoken(&resource->token);
	*color = 0;
	return (0);
}

static void
vmmfs_pcislot_resource_pager_dtor(void *handle)
{
	struct vmmfs_pcislot_resource *resource;

	resource = handle;
	if (resource != NULL)
		vmmfs_node_put(&resource->node);
}

static int
vmmfs_pcislot_resource_pager_fault(vm_object_t object, vm_ooffset_t offset,
	int prot, vm_page_t *page_result)
{
	struct vmmfs_pcislot_resource *resource;
	struct vm_object *backing;
	struct vmspace *vmspace;
	vm_page_t page;
	int busy;
	int error;

	resource = object->handle;
	if (resource == NULL || page_result == NULL || offset < 0 ||
	    offset >= resource->mapping_size || (prot & VM_PROT_EXECUTE) != 0)
		return (VM_PAGER_ERROR);
retry:
	lwkt_gettoken(&resource->token);
	if (resource->revoked) {
		lwkt_reltoken(&resource->token);
		return (VM_PAGER_ERROR);
	}
	if (resource->kind == VMMFS_PCISLOT_RESOURCE_DMA) {
		if (!resource->bus_master_enabled || resource->vmspace == NULL) {
			lwkt_reltoken(&resource->token);
			return (VM_PAGER_ERROR);
		}
		vmspace = resource->vmspace;
		vmspace_ref(vmspace);
		lwkt_reltoken(&resource->token);
		busy = 0;
		page = vm_fault_page(&vmspace->vm_map, offset,
		    VM_PROT_READ | VM_PROT_WRITE, VM_FAULT_DIRTY, &error, &busy);
		if (error != 0 || page == NULL || !busy) {
			if (page != NULL) {
				if (busy)
					vm_page_wakeup(page);
				else
					vm_page_unhold(page);
			}
			vmspace_rel(vmspace);
			return (VM_PAGER_ERROR);
		}
		/* A sleeping fault may outlive decode, reset or resource revoke. */
		lwkt_gettoken(&resource->token);
		if (resource->revoked || !resource->bus_master_enabled) {
			vm_page_wakeup(page);
			lwkt_reltoken(&resource->token);
			vmspace_rel(vmspace);
			return (VM_PAGER_ERROR);
		}
		if (resource->vmspace != vmspace) {
			/* Reset retains this fd; retry against its new memory. */
			vm_page_wakeup(page);
			lwkt_reltoken(&resource->token);
			vmspace_rel(vmspace);
			goto retry;
		}
		error = vmmfs_pcislot_resource_track_page(resource, page, vmspace);
		if (error != 0) {
			vm_page_wakeup(page);
			lwkt_reltoken(&resource->token);
			vmspace_rel(vmspace);
			if (error == EAGAIN)
				goto retry;
			return (VM_PAGER_ERROR);
		}
		*page_result = page;
		/* The tracked object reference also covers the outer pmap_enter. */
		vmspace_rel(vmspace);
		lwkt_reltoken(&resource->token);
		return (VM_PAGER_OK);
	}
	backing = resource->backing_object;
	if (backing != NULL)
		vm_object_reference_quick(backing);
	lwkt_reltoken(&resource->token);
	if (backing == NULL)
		return (VM_PAGER_ERROR);
	page = vm_page_grab(backing, OFF_TO_IDX(offset),
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	lwkt_gettoken(&resource->token);
	if (page == NULL || resource->revoked) {
		if (page != NULL)
			vm_page_wakeup(page);
		lwkt_reltoken(&resource->token);
		vm_object_deallocate(backing);
		return (VM_PAGER_ERROR);
	}
	if (page->valid != VM_PAGE_BITS_ALL)
		vm_page_zero_invalid(page, TRUE);
	error = vmmfs_pcislot_resource_track_page(resource, page, NULL);
	if (error != 0) {
		vm_page_wakeup(page);
		lwkt_reltoken(&resource->token);
		vm_object_deallocate(backing);
		return (VM_PAGER_ERROR);
	}
	*page_result = page;
	vm_object_deallocate(backing);
	lwkt_reltoken(&resource->token);
	return (VM_PAGER_OK);
}

static void
vmmfs_pcislot_resource_filter_detach(struct knote *knote)
{
	struct vmmfs_pcislot_resource *resource;

	resource = (struct vmmfs_pcislot_resource *)knote->kn_hook;
	if (resource == NULL)
		return;
	lwkt_gettoken(&resource->token);
	knote_remove(&resource->read_kq.ki_note, knote);
	lwkt_reltoken(&resource->token);
}

static int
vmmfs_pcislot_resource_filter_read(struct knote *knote, long hint)
{
	struct vmmfs_pcislot_resource *resource;

	(void)hint;
	resource = (struct vmmfs_pcislot_resource *)knote->kn_hook;
	if (resource == NULL)
		return (0);
	lwkt_gettoken(&resource->token);
	knote->kn_data = resource->kick_pending ?
	    sizeof(struct vmmfs_pci_kick) : 0;
	if (resource->revoked)
		knote->kn_flags |= EV_EOF;
	lwkt_reltoken(&resource->token);
	return (knote->kn_data != 0 || (knote->kn_flags & EV_EOF) != 0);
}

static int
vmmfs_pcislot_resource_mmio_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	(void)vcpu;
	return (vmmfs_pcislot_resource_doorbell(argument, write->address,
	    write->width, write->value));
}

static int
vmmfs_pcislot_resource_pio_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	(void)vcpu;
	return (vmmfs_pcislot_resource_doorbell(argument, write->address,
	    write->width, write->value));
}

static int
vmmfs_pcislot_resource_mmio_read(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	(void)vcpu;
	(void)argument;
	(void)read;
	return (ENOENT);
}

static int
vmmfs_pcislot_resource_pio_read(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	(void)vcpu;
	(void)argument;
	(void)read;
	return (ENOENT);
}

static int
vmmfs_pcislot_resource_config_mmio_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	(void)vcpu;
	(void)argument;
	(void)write;
	return (ENOENT);
}

static int
vmmfs_pcislot_resource_config_pio_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	(void)vcpu;
	(void)argument;
	(void)write;
	return (ENOENT);
}
