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
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_pciroot.h"
#include "vmmfs_parent.h"
#include "vmmfs_machine.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_resource.h"
#include "vmmfs_vcpu.h"

#define VMMFS_PCIROOT_MODE 0555
#define VMMFS_PCI_CONFIG_ADDRESS 0xcf8U
#define VMMFS_PCI_CONFIG_DATA 0xcfcU
#define VMMFS_PCI_HOSTBRIDGE_BDF 0U
#define VMMFS_PCI_HOSTBRIDGE_VENDOR_ID 0x8086U
#define VMMFS_PCI_HOSTBRIDGE_DEVICE_ID 0x0d57U
#define VMMFS_PCI_HOSTBRIDGE_CLASS 0x060000U
#define VMMFS_PCI_HOSTBRIDGE_CONFIG_SIZE 0x100U

struct vmmfs_pciroot_slot {
	RB_ENTRY(vmmfs_pciroot_slot) entry;
	struct vmmfs_pcislot *slot;
	struct vnode *vnode;
};

RB_HEAD(vmmfs_pcislot_tree, vmmfs_pciroot_slot);
RB_PROTOTYPE(vmmfs_pcislot_tree, vmmfs_pciroot_slot, entry,
	vmmfs_pciroot_slot_compare);

struct vmmfs_pciroot_registry {
	struct vmmfs_pcislot_tree slots;
};

static int vmmfs_pciroot_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_pciroot_parse_bdf(const char *, size_t, uint16_t *);
static int vmmfs_pciroot_parse_hex(char, unsigned int *);
static void vmmfs_pciroot_format_bdf(uint16_t, char *, size_t);
static int vmmfs_pciroot_get_item(struct vmmfs_node *, const char *, size_t,
	struct vnode **);
static int vmmfs_pciroot_read_item(struct vmmfs_node *, uint64_t,
	struct vmmfs_node_item *);
static int vmmfs_pciroot_create_item(struct vmmfs_node *, struct mount *,
	const char *, size_t, struct vnode **);
static void vmmfs_pciroot_remove_item(struct vmmfs_node *, const char *,
	size_t);
static int vmmfs_pciroot_config_address_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_pciroot_config_address_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static int vmmfs_pciroot_config_data_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_pciroot_config_data_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static int vmmfs_pciroot_ecam_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_pciroot_ecam_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static bool vmmfs_pciroot_config_contains(uint16_t, uint64_t,
	enum vmm_io_width);
static bool vmmfs_pciroot_ecam_contains(uint64_t, enum vmm_io_width);
static int vmmfs_pciroot_slot_compare(struct vmmfs_pciroot_slot *,
	struct vmmfs_pciroot_slot *);
static struct vmmfs_pcislot *vmmfs_pciroot_find_locked(
	struct vmmfs_pciroot *, uint16_t);
static struct vmmfs_pciroot_slot *vmmfs_pciroot_entry_find_locked(
	struct vmmfs_pciroot *, uint16_t);
static void vmmfs_pciroot_drop(struct vmmfs_node *);
static int vmmfs_pciroot_deactivate(struct vmmfs_node *);
static uint32_t vmmfs_pciroot_absent_value(enum vmm_io_width);
static int vmmfs_pciroot_hostbridge_read(uint16_t, enum vmm_io_width,
	uint32_t *);
static int vmmfs_pciroot_config_read_locked(struct vmmfs_pciroot *,
	vmm_vcpu_t, uint16_t, uint16_t, enum vmm_io_width, uint32_t *);


struct vop_ops vmmfs_pciroot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_nlookupdotdot = vmmfs_pciroot_nlookupdotdot,
	.vop_nmkdir = vmmfs_node_nmkdir,
	.vop_nresolve = vmmfs_node_nresolve,
	.vop_nrmdir = vmmfs_node_nrmdir,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_node_readdir,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

static int
vmmfs_pciroot_slot_compare(struct vmmfs_pciroot_slot *left,
	struct vmmfs_pciroot_slot *right)
{
	return ((int)left->slot->bdf - (int)right->slot->bdf);
}

RB_GENERATE(vmmfs_pcislot_tree, vmmfs_pciroot_slot, entry,
	vmmfs_pciroot_slot_compare);

int
vmmfs_pciroot_init(struct vmmfs_mount *mount, struct vmmfs_node *parent,
	struct vmmfs_pciroot *pciroot, struct vnode **vnodep)
{
	struct vmmfs_mount *state;
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	int error;

	if (mount == NULL || parent == NULL || pciroot == NULL || vnodep == NULL)
		return (EINVAL);
	root = mount->root_vnode == NULL ? NULL : mount->root_vnode->v_data;
	if (root == NULL)
		return (EINVAL);
	*vnodep = NULL;
	machine = (struct vmmfs_machine *)parent;
	state = mount;
	if (state->pciroot_vops == NULL)
		return (ENXIO);
	bzero(pciroot, sizeof(*pciroot));
	pciroot->registry = kmalloc(sizeof(*pciroot->registry), M_VMMFS,
	    M_WAITOK | M_ZERO);
	if (pciroot->registry == NULL)
		return (ENOMEM);
	pciroot->node.parent = parent;
	pciroot->node.references = 1;
	lwkt_token_init(&pciroot->node.token, "vmmfsnode");
	pciroot->node.drop = vmmfs_pciroot_drop;
	if (parent != NULL)
		vmmfs_node_hold(parent);
	pciroot->node.deactivate = vmmfs_pciroot_deactivate;
	pciroot->node.get_item = vmmfs_pciroot_get_item;
	pciroot->node.read_item = vmmfs_pciroot_read_item;
	pciroot->node.create_item = vmmfs_pciroot_create_item;
	pciroot->node.remove_item = vmmfs_pciroot_remove_item;
	pciroot->node.inode = vmmfs_root_allocate_inode(root);
	pciroot->node.mode = VMMFS_PCIROOT_MODE;
	pciroot->node.size = 0;
	RB_INIT(&pciroot->registry->slots);
	error = vmmfs_vnode_create_regular(state->mount,
	    &state->pciroot_vops, VDIR, &pciroot->node, vnodep);
	if (error != 0)
		vmmfs_node_put(&pciroot->node);
	return (error);
}

static void
vmmfs_pciroot_drop(struct vmmfs_node *node)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_machine *machine;

	pciroot = (struct vmmfs_pciroot *)node;
	KKASSERT(pciroot != NULL);
	machine = vmmfs_pciroot_machine(pciroot);
	KKASSERT(machine != NULL);
	KKASSERT(pciroot->node.references == 0);
	lwkt_gettoken(&machine->node.token);
	if (pciroot->runtime_machine != NULL) {
		lwkt_reltoken(&machine->node.token);
		panic("vmmfs_pciroot_drop: runtime PCI root is still active");
	}
	lwkt_reltoken(&machine->node.token);
	KKASSERT(pciroot->registry != NULL);
	KKASSERT(RB_EMPTY(&pciroot->registry->slots));
	kfree(pciroot->registry, M_VMMFS);
	pciroot->registry = NULL;
}

static void
vmmfs_pciroot_release_entry(struct vmmfs_pciroot *root,
	struct vmmfs_pciroot_slot *entry)
{
	struct vmmfs_machine *machine = vmmfs_pciroot_machine(root);
	struct vmmfs_pcislot *slot = entry->slot;

	lwkt_gettoken(&machine->node.token);
	slot->entry = NULL;
	if (slot->topology_reference) {
		KKASSERT(machine->runtime_references != 0);
		slot->topology_reference = false;
		--machine->runtime_references;
	}
	lwkt_reltoken(&machine->node.token);
}

static int
vmmfs_pciroot_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_pciroot *root = (struct vmmfs_pciroot *)node;
	struct vmmfs_pciroot_slot *entry;
	struct vnode *vnode;
	int error;

	for (;;) {
		lwkt_gettoken(&node->token);
		entry = RB_ROOT(&root->registry->slots);
		vnode = entry == NULL ? NULL : entry->vnode;
		if (vnode != NULL)
			vref(vnode);
		lwkt_reltoken(&node->token);
		if (vnode == NULL)
			return (0);
		error = vmmfs_vnode_deactivate(vnode);
		if (error != 0) {
			/* Another remover may already have released the entry. */
			vrele(vnode);
			return (error);
		}
		/* Only the successful deactivate caller may detach this entry. */
		lwkt_gettoken(&node->token);
		RB_REMOVE(vmmfs_pcislot_tree, &root->registry->slots, entry);
		vmmfs_pciroot_release_entry(root, entry);
		lwkt_reltoken(&node->token);
		vrele(vnode); /* Registry reference. */
		vrele(vnode); /* Lookup reference. */
		kfree(entry, M_VMMFS);
	}
}



void
vmmfs_pciroot_invalidate_slot(struct vmmfs_pciroot *pciroot,
	struct vmmfs_pcislot *slot)
{
	struct vmmfs_pciroot_slot *entry;
	struct vnode *vnode;

	if (pciroot == NULL || slot == NULL)
		return;
	lwkt_gettoken(&pciroot->node.token);
	entry = vmmfs_pciroot_entry_find_locked(pciroot, slot->bdf);
	vnode = entry == NULL || entry->slot != slot ? NULL : entry->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&pciroot->node.token);
	if (vnode == NULL)
		return;
	cache_inval_vp(vnode, CINV_CHILDREN);
	vdrop(vnode);
}

int
vmmfs_pciroot_start(struct vmmfs_pciroot *pciroot, vmm_machine_t machine)
{
	struct vmmfs_pciroot_slot *entry;
	int error;

	if (pciroot == NULL || vmmfs_pciroot_machine(pciroot) == NULL ||
	    machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&pciroot->node.token);
	if (pciroot->runtime_machine != NULL) {
		lwkt_reltoken(&pciroot->node.token);
		return (EBUSY);
	}
	RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots) {
		if (entry->slot->descriptor.updating) {
			lwkt_reltoken(&pciroot->node.token);
			return (EBUSY);
		}
	}
	lwkt_reltoken(&pciroot->node.token);
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
	error = vmm_machine_trap_mmio_read(machine, VMMFS_PCI_ECAM_GPA,
	    VMMFS_PCI_ECAM_SIZE, vmmfs_pciroot_ecam_read, pciroot,
	    &pciroot->ecam_read);
	if (error != 0)
		goto fail_data_write;
	error = vmm_machine_trap_mmio_write(machine, VMMFS_PCI_ECAM_GPA,
	    VMMFS_PCI_ECAM_SIZE, vmmfs_pciroot_ecam_write, pciroot,
	    &pciroot->ecam_write);
	if (error != 0)
		goto fail_ecam_read;
	lwkt_gettoken(&pciroot->node.token);
	pciroot->runtime_machine = machine;
	pciroot->mmio_next = VMMFS_PCI_MMIO_GPA;
	pciroot->pio_next = VMMFS_PCI_PIO_GPA;
	pciroot->config_address = 0;
	lwkt_reltoken(&pciroot->node.token);
	RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots) {
		if (!entry->slot->descriptor.committed)
			continue;
		if (entry->slot->resources != NULL)
			error = vmmfs_pcislot_resources_rebind(
			    entry->slot->resources, machine);
		else
			error = vmmfs_pcislot_power_on(entry->slot, machine);
		if (error != 0)
			goto fail_slots;
	}
	return (0);

fail_slots:
	RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots)
		vmmfs_pcislot_power_off(entry->slot);
	(void)vmmfs_pciroot_stop(pciroot);
	return (error);

fail_ecam_read:
	(void)vmm_machine_untrap(machine, pciroot->ecam_read);
	pciroot->ecam_read = NULL;
fail_data_write:
	(void)vmm_machine_untrap(machine, pciroot->config_data_write);
	pciroot->config_data_write = NULL;
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
vmmfs_pciroot_reset(struct vmmfs_pciroot *pciroot)
{
	vmm_machine_t machine;
	struct vmmfs_pciroot_slot *entry;
	vmm_io_t config_address_read;
	vmm_io_t config_address_write;
	vmm_io_t config_data_read;
	vmm_io_t config_data_write;
	vmm_io_t ecam_read;
	vmm_io_t ecam_write;
	int error;

	if (pciroot == NULL || vmmfs_pciroot_machine(pciroot) == NULL)
		return (EINVAL);
	lwkt_gettoken(&pciroot->node.token);
	machine = pciroot->runtime_machine;
	if (machine == NULL) {
		lwkt_reltoken(&pciroot->node.token);
		RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots)
			vmmfs_pcislot_power_off(entry->slot);
		return (0);
	}
	config_address_read = pciroot->config_address_read;
	config_address_write = pciroot->config_address_write;
	config_data_read = pciroot->config_data_read;
	config_data_write = pciroot->config_data_write;
	ecam_read = pciroot->ecam_read;
	ecam_write = pciroot->ecam_write;
	pciroot->mmio_next = VMMFS_PCI_MMIO_GPA;
	pciroot->pio_next = VMMFS_PCI_PIO_GPA;
	lwkt_reltoken(&pciroot->node.token);

	RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots) {
		if (entry->slot->resources != NULL) {
			error = vmmfs_pcislot_reset(entry->slot);
			if (error != 0)
				return (error);
			vmmfs_pcislot_resources_unbind(
			    entry->slot->resources);
		}
	}
	lwkt_gettoken(&pciroot->node.token);
	pciroot->runtime_machine = NULL;
	pciroot->config_address = 0;
	pciroot->config_address_read = NULL;
	pciroot->config_address_write = NULL;
	pciroot->config_data_read = NULL;
	pciroot->config_data_write = NULL;
	pciroot->ecam_read = NULL;
	pciroot->ecam_write = NULL;
	lwkt_reltoken(&pciroot->node.token);
	if (ecam_write != NULL)
		(void)vmm_machine_untrap(machine, ecam_write);
	if (ecam_read != NULL)
		(void)vmm_machine_untrap(machine, ecam_read);
	if (config_data_write != NULL)
		(void)vmm_machine_untrap(machine, config_data_write);
	if (config_data_read != NULL)
		(void)vmm_machine_untrap(machine, config_data_read);
	if (config_address_write != NULL)
		(void)vmm_machine_untrap(machine, config_address_write);
	if (config_address_read != NULL)
		(void)vmm_machine_untrap(machine, config_address_read);
	return (0);
}

int
vmmfs_pciroot_stop(struct vmmfs_pciroot *pciroot)
{
	vmm_machine_t machine;
	struct vmmfs_pciroot_slot *entry;
	vmm_io_t config_address_read;
	vmm_io_t config_address_write;
	vmm_io_t config_data_read;
	vmm_io_t config_data_write;
	vmm_io_t ecam_read;
	vmm_io_t ecam_write;
	int error;
	int result;

	if (pciroot == NULL || vmmfs_pciroot_machine(pciroot) == NULL)
		return (EINVAL);
	lwkt_gettoken(&pciroot->node.token);
	machine = pciroot->runtime_machine;
	if (machine == NULL) {
		lwkt_reltoken(&pciroot->node.token);
		return (0);
	}
	config_address_read = pciroot->config_address_read;
	config_address_write = pciroot->config_address_write;
	config_data_read = pciroot->config_data_read;
	config_data_write = pciroot->config_data_write;
	ecam_read = pciroot->ecam_read;
	ecam_write = pciroot->ecam_write;
	lwkt_reltoken(&pciroot->node.token);
	RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots)
		vmmfs_pcislot_power_off(entry->slot);
	lwkt_gettoken(&pciroot->node.token);
	pciroot->runtime_machine = NULL;
	pciroot->config_address = 0;
	pciroot->config_address_read = NULL;
	pciroot->config_address_write = NULL;
	pciroot->config_data_read = NULL;
	pciroot->config_data_write = NULL;
	pciroot->ecam_read = NULL;
	pciroot->ecam_write = NULL;
	lwkt_reltoken(&pciroot->node.token);
	result = 0;
	if (ecam_write != NULL) {
		error = vmm_machine_untrap(machine, ecam_write);
		if (result == 0)
			result = error;
	}
	if (ecam_read != NULL) {
		error = vmm_machine_untrap(machine, ecam_read);
		if (result == 0)
			result = error;
	}
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

int
vmmfs_pciroot_memory(struct vmmfs_pciroot *pciroot,
	struct vmmfs_vcpu_thread *thread,
	const struct vmm_cpuexit *exit)
{
	struct vmmfs_pciroot_slot *entry;
	struct vmmfs_pcislot *slot;
	uint64_t relative;
	uint16_t bdf;
	uint16_t offset;
	uint32_t value;
	bool write;
	vmm_vcpu_t vcpu;
	int error;

	if (pciroot == NULL || thread == NULL || thread->vcpu == NULL ||
	    exit == NULL || exit->reason != VMM_CPUEXIT_MEMORY)
		return (ENOENT);
	vcpu = thread->vcpu;
	if (!vmmfs_pciroot_ecam_contains(exit->u.mem.gpa, exit->u.mem.width)) {
		RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots) {
			slot = entry->slot;
			error = vmmfs_pcislot_resources_memory(
			    slot->resources, thread, exit);
			if (error != ENOENT)
				return (error);
		}
		return (ENOENT);
	}
	relative = exit->u.mem.gpa - VMMFS_PCI_ECAM_GPA;
	bdf = ((relative >> 20) & 0xff) << 8 |
	    ((relative >> 15) & 0x1f) << 3 | ((relative >> 12) & 0x7);
	offset = relative & 0xfff;
	write = (exit->u.mem.prot & VM_PROT_WRITE) != 0;
	lwkt_gettoken(&pciroot->node.token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->node.token);
		return (ENOENT);
	}
	slot = vmmfs_pciroot_find_locked(pciroot, bdf);
	if (slot == NULL || !slot->type0.powered) {
		lwkt_reltoken(&pciroot->node.token);
		return (ENOENT);
	}
	lwkt_reltoken(&pciroot->node.token);
	value = 0;
	if (write) {
		error = vmmfs_pcislot_type0_config_write(slot, vcpu, offset,
		    exit->u.mem.width, (uint32_t)exit->u.mem.value);
	} else {
		error = vmmfs_pcislot_type0_config_read(slot, vcpu, offset,
		    exit->u.mem.width, &value);
	}
	if (error == ENXIO) {
		if (!write)
			value = vmmfs_pciroot_absent_value(exit->u.mem.width);
		error = 0;
	}
	if (error != 0)
		return (error);
	if (write)
		return (vmm_vcpu_complete_mmio_write(vcpu));
	return (vmm_vcpu_complete_mmio_read(vcpu, &value, exit->u.mem.width));
}

int
vmmfs_pciroot_io(struct vmmfs_pciroot *pciroot,
	struct vmmfs_vcpu_thread *thread,
	struct vmm_cpustate *state, const struct vmm_cpuexit *exit)
{
	struct vmmfs_pciroot_slot *entry;
	struct vmmfs_pcislot *slot;
	uint32_t address;
	uint32_t value;
	uint16_t bdf;
	uint16_t offset;
	uint64_t mask;
	bool write;
	vmm_vcpu_t vcpu;
	int error;

	if (pciroot == NULL || thread == NULL || thread->vcpu == NULL ||
	    state == NULL || exit == NULL ||
	    exit->reason != VMM_CPUEXIT_IO || exit->u.io.str || exit->u.io.rep ||
	    (exit->u.io.operand_size != 1 && exit->u.io.operand_size != 2 &&
	    exit->u.io.operand_size != 4))
		return (ENOENT);
	vcpu = thread->vcpu;
	if (exit->u.io.port < VMMFS_PCI_CONFIG_DATA ||
	    exit->u.io.port + exit->u.io.operand_size >
	    VMMFS_PCI_CONFIG_DATA + sizeof(uint32_t)) {
		RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots) {
			slot = entry->slot;
			error = vmmfs_pcislot_resources_io(slot->resources,
			    thread, state, exit);
			if (error != ENOENT)
				return (error);
		}
		return (ENOENT);
	}
	lwkt_gettoken(&pciroot->node.token);
	address = pciroot->config_address;
	if (pciroot->runtime_machine == NULL ||
	    (address & 0x80000000U) == 0) {
		lwkt_reltoken(&pciroot->node.token);
		if (exit->u.io.in) {
			mask = (1ULL << (exit->u.io.operand_size * NBBY)) - 1;
			state->gprs[VMM_X64_GPR_RAX] =
			    (state->gprs[VMM_X64_GPR_RAX] & ~mask) | mask;
		}
		state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
		return (0);
	}
	bdf = ((address >> 16) & 0xff) << 8 |
	    ((address >> 11) & 0x1f) << 3 | ((address >> 8) & 0x7);
	offset = (address & 0xfc) +
	    (uint16_t)(exit->u.io.port - VMMFS_PCI_CONFIG_DATA);
	slot = vmmfs_pciroot_find_locked(pciroot, bdf);
	if (slot == NULL || !slot->type0.powered) {
		lwkt_reltoken(&pciroot->node.token);
		if (exit->u.io.in) {
			mask = (1ULL << (exit->u.io.operand_size * NBBY)) - 1;
			state->gprs[VMM_X64_GPR_RAX] =
			    (state->gprs[VMM_X64_GPR_RAX] & ~mask) | mask;
		}
		state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
		return (0);
	}
	lwkt_reltoken(&pciroot->node.token);
	write = !exit->u.io.in;
	value = (uint32_t)state->gprs[VMM_X64_GPR_RAX];
	if (write)
		error = vmmfs_pcislot_type0_config_write(slot, vcpu, offset,
		    (enum vmm_io_width)exit->u.io.operand_size, value);
	else
		error = vmmfs_pcislot_type0_config_read(slot, vcpu, offset,
		    (enum vmm_io_width)exit->u.io.operand_size, &value);
	if (error == ENXIO) {
		error = 0;
		value = vmmfs_pciroot_absent_value(
		    (enum vmm_io_width)exit->u.io.operand_size);
	}
	if (error != 0)
		return (error);
	if (!write) {
		mask = (1ULL << (exit->u.io.operand_size * NBBY)) - 1;
		state->gprs[VMM_X64_GPR_RAX] =
		    (state->gprs[VMM_X64_GPR_RAX] & ~mask) | (value & mask);
	}
	state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
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
	if (pciroot == NULL || pciroot->node.dead)
		return (ENOENT);
	machine = vmmfs_pciroot_machine(pciroot);
	if (machine == NULL)
		return (ENOENT);
	lwkt_gettoken(&machine->node.token);
	vnode = machine->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->node.token);
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
vmmfs_pciroot_get_item(struct vmmfs_node *node, const char *name,
	size_t namelen, struct vnode **vnodep)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pciroot_slot *entry;
	uint16_t bdf;

	if (node == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	pciroot = (struct vmmfs_pciroot *)node;
	if (pciroot->node.dead ||
	    vmmfs_pciroot_machine(pciroot) == NULL)
		return (ENOENT);
	if (vmmfs_pciroot_parse_bdf(name, namelen, &bdf) != 0)
		return (ENOENT);
	lwkt_gettoken(&pciroot->node.token);
	if (node->dead) {
		lwkt_reltoken(&pciroot->node.token);
		return (ENOENT);
	}
	entry = vmmfs_pciroot_entry_find_locked(pciroot, bdf);
	if (entry != NULL) {
		*vnodep = entry->vnode;
		vhold(*vnodep);
	}
	lwkt_reltoken(&pciroot->node.token);
	return (*vnodep == NULL ? ENOENT : 0);
}

static int
vmmfs_pciroot_read_item(struct vmmfs_node *node, uint64_t index,
	struct vmmfs_node_item *item)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pciroot_slot *entry;
	uint64_t current;

	if (node == NULL || item == NULL)
		return (EINVAL);
	pciroot = (struct vmmfs_pciroot *)node;
	if (pciroot->node.dead)
		return (ENOENT);
	bzero(item, sizeof(*item));
	lwkt_gettoken(&pciroot->node.token);
	if (node->dead) {
		lwkt_reltoken(&pciroot->node.token);
		return (ENOENT);
	}
	current = 0;
	RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots) {
		if (current++ != index)
			continue;
		item->vnode = entry->vnode;
		item->inode = entry->slot->node.inode;
		vmmfs_pciroot_format_bdf(entry->slot->bdf, item->name,
		    sizeof(item->name));
		vhold(item->vnode);
		lwkt_reltoken(&pciroot->node.token);
		return (0);
	}
	lwkt_reltoken(&pciroot->node.token);
	return (ENOENT);
}

static int
vmmfs_pciroot_create_item(struct vmmfs_node *node, struct mount *mount,
	const char *name, size_t namelen, struct vnode **vnodep)
{
	struct vmmfs_pciroot *root = (struct vmmfs_pciroot *)node;
	struct vmmfs_machine *machine = vmmfs_pciroot_machine(root);
	struct vmmfs_mount *state = (struct vmmfs_mount *)mount->mnt_data;
	struct vmmfs_pciroot_slot *entry;
	struct vnode *vnode;
	uint16_t bdf;
	int error, cleanup_error;

	*vnodep = NULL;
	error = vmmfs_pciroot_parse_bdf(name, namelen, &bdf);
	if (error != 0)
		return (error);
	entry = kmalloc(sizeof(*entry), M_VMMFS, M_WAITOK | M_ZERO);
	error = vmmfs_pcislot_create(state, node, bdf, &vnode);
	if (error != 0) {
		kfree(entry, M_VMMFS);
		return (error);
	}
	lwkt_gettoken(&node->token);
	lwkt_gettoken(&machine->node.token);
	if (node->dead || machine->node.dead)
		error = ENOENT;
	else if (machine->machine != NULL)
		error = EBUSY;
	else if (vmmfs_pciroot_entry_find_locked(root, bdf) != NULL)
		error = EEXIST;
	else {
		entry->slot = vnode->v_data;
		entry->vnode = vnode;
		RB_INSERT(vmmfs_pcislot_tree, &root->registry->slots, entry);
		entry->slot->entry = entry;
		vref(vnode); /* create_item caller, independent of registry. */
	}
	lwkt_reltoken(&machine->node.token);
	lwkt_reltoken(&node->token);
	if (error != 0) {
		cleanup_error = vmmfs_vnode_deactivate(vnode);
		if (cleanup_error != 0)
			kprintf("vmmfs: rejected PCI slot cleanup: %d\n", cleanup_error);
		vrele(vnode);
		kfree(entry, M_VMMFS);
		return (error);
	}
	*vnodep = vnode;
	return (0);
}

static void
vmmfs_pciroot_remove_item(struct vmmfs_node *node, const char *name,
	size_t namelen)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pciroot_slot *entry;
	struct vnode *vnode;
	uint16_t bdf;
	int error;

	KKASSERT(node != NULL);
	pciroot = (struct vmmfs_pciroot *)node;
	error = vmmfs_pciroot_parse_bdf(name, namelen, &bdf);
	KKASSERT(error == 0);
	lwkt_gettoken(&pciroot->node.token);
	entry = vmmfs_pciroot_entry_find_locked(pciroot, bdf);
	if (entry == NULL) {
		lwkt_reltoken(&pciroot->node.token);
		return;
	}
	RB_REMOVE(vmmfs_pcislot_tree, &pciroot->registry->slots, entry);
	vnode = entry->vnode;
	vmmfs_pciroot_release_entry(pciroot, entry);
	kfree(entry, M_VMMFS);
	lwkt_reltoken(&pciroot->node.token);
	vrele(vnode);
}

static int
vmmfs_pciroot_parse_bdf(const char *name, size_t namelen, uint16_t *bdfp)
{
	unsigned int bus;
	unsigned int device;
	unsigned int function;
	unsigned int value;
	int error;

	if (name == NULL || bdfp == NULL || namelen != 12 ||
	    bcmp(name, "0000:", 5) != 0 || name[7] != ':' ||
	    name[10] != '.')
		return (EINVAL);
	error = vmmfs_pciroot_parse_hex(name[5], &bus);
	if (error != 0)
		return (error);
	error = vmmfs_pciroot_parse_hex(name[6], &value);
	if (error != 0)
		return (error);
	bus = (bus << 4) | value;
	error = vmmfs_pciroot_parse_hex(name[8], &device);
	if (error != 0)
		return (error);
	error = vmmfs_pciroot_parse_hex(name[9], &value);
	if (error != 0)
		return (error);
	device = (device << 4) | value;
	error = vmmfs_pciroot_parse_hex(name[11], &function);
	if (error != 0 || device >= 32)
		return (EINVAL);
	*bdfp = (uint16_t)((bus << 8) | (device << 3) | function);
	if (*bdfp == 0)
		return (EINVAL);
	return (0);
}

static int
vmmfs_pciroot_parse_hex(char character, unsigned int *value)
{

	if (character >= '0' && character <= '9')
		*value = character - '0';
	else if (character >= 'a' && character <= 'f')
		*value = character - 'a' + 10U;
	else if (character >= 'A' && character <= 'F')
		*value = character - 'A' + 10U;
	else
		return (EINVAL);
	return (0);
}

static void
vmmfs_pciroot_format_bdf(uint16_t bdf, char *name, size_t namesize)
{

	ksnprintf(name, namesize, "0000:%02x:%02x.%x", bdf >> 8,
	    (bdf >> 3) & 0x1f, bdf & 0x7);
}

static int
vmmfs_pciroot_config_address_read(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	struct vmmfs_pciroot *pciroot;
	uint32_t value;
	uint64_t shift;

	(void)vcpu;
	pciroot = argument;
	if (pciroot == NULL || !vmmfs_pciroot_config_contains(
	    VMMFS_PCI_CONFIG_ADDRESS, read->address, read->width))
		return (ENOENT);
	lwkt_gettoken(&pciroot->node.token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->node.token);
		return (ENOENT);
	}
	value = pciroot->config_address;
	lwkt_reltoken(&pciroot->node.token);
	shift = (read->address - VMMFS_PCI_CONFIG_ADDRESS) * NBBY;
	read->value = (value >> shift) &
	    (UINT32_MAX >> ((sizeof(value) - read->width) * NBBY));
	return (0);
}

static int
vmmfs_pciroot_config_address_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	struct vmmfs_pciroot *pciroot;
	uint32_t mask;
	uint64_t shift;

	(void)vcpu;
	pciroot = argument;
	if (pciroot == NULL || !vmmfs_pciroot_config_contains(
	    VMMFS_PCI_CONFIG_ADDRESS, write->address, write->width))
		return (ENOENT);
	lwkt_gettoken(&pciroot->node.token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->node.token);
		return (ENOENT);
	}
	shift = (write->address - VMMFS_PCI_CONFIG_ADDRESS) * NBBY;
	mask = (UINT32_MAX >> ((sizeof(mask) - write->width) * NBBY)) << shift;
	pciroot->config_address = (pciroot->config_address & ~mask) |
	    (((uint32_t)write->value << shift) & mask);
	lwkt_reltoken(&pciroot->node.token);
	return (0);
}

static int
vmmfs_pciroot_config_data_read(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	struct vmmfs_pciroot *pciroot;
	uint32_t address;
	uint32_t value;
	uint16_t bdf;
	uint16_t offset;
	int error;

	pciroot = argument;
	if (pciroot == NULL || !vmmfs_pciroot_config_contains(
	    VMMFS_PCI_CONFIG_DATA, read->address, read->width))
		return (ENOENT);
	lwkt_gettoken(&pciroot->node.token);
	if (pciroot->runtime_machine == NULL ||
	    (pciroot->config_address & 0x80000000U) == 0) {
		lwkt_reltoken(&pciroot->node.token);
		read->value = vmmfs_pciroot_absent_value(read->width);
		return (0);
	}
	address = pciroot->config_address;
	bdf = ((address >> 16) & 0xff) << 8 |
	    ((address >> 11) & 0x1f) << 3 | ((address >> 8) & 0x7);
	offset = (address & 0xfc) +
	    (uint16_t)(read->address - VMMFS_PCI_CONFIG_DATA);
	error = vmmfs_pciroot_config_read_locked(pciroot, vcpu, bdf, offset,
	    read->width, &value);
	lwkt_reltoken(&pciroot->node.token);
	if (error == ENOENT)
		return (ENOENT);
	if (error != 0)
		value = vmmfs_pciroot_absent_value(read->width);
	read->value = value;
	return (0);
}

static int
vmmfs_pciroot_config_data_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pcislot *slot;
	uint32_t address;
	uint16_t bdf;
	uint16_t offset;

	pciroot = argument;
	if (pciroot == NULL || !vmmfs_pciroot_config_contains(
	    VMMFS_PCI_CONFIG_DATA, write->address, write->width))
		return (ENOENT);
	lwkt_gettoken(&pciroot->node.token);
	if (pciroot->runtime_machine == NULL ||
	    (pciroot->config_address & 0x80000000U) == 0) {
		lwkt_reltoken(&pciroot->node.token);
		return (0);
	}
	address = pciroot->config_address;
	bdf = ((address >> 16) & 0xff) << 8 |
	    ((address >> 11) & 0x1f) << 3 | ((address >> 8) & 0x7);
	offset = (address & 0xfc) +
	    (uint16_t)(write->address - VMMFS_PCI_CONFIG_DATA);
	slot = vmmfs_pciroot_find_locked(pciroot, bdf);
	if (slot == NULL || !slot->type0.powered) {
		lwkt_reltoken(&pciroot->node.token);
		return (0);
	}
	lwkt_reltoken(&pciroot->node.token);
	return (vmmfs_pcislot_type0_config_write(slot, vcpu, offset,
	    write->width, (uint32_t)write->value));
}

static int
vmmfs_pciroot_ecam_read(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	struct vmmfs_pciroot *pciroot;
	uint64_t relative;
	uint16_t bdf;
	uint16_t offset;
	uint32_t value;
	int error;

	pciroot = argument;
	if (pciroot == NULL || read == NULL ||
	    !vmmfs_pciroot_ecam_contains(read->address, read->width))
		return (ENOENT);
	lwkt_gettoken(&pciroot->node.token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->node.token);
		read->value = vmmfs_pciroot_absent_value(read->width);
		return (0);
	}
	relative = read->address - VMMFS_PCI_ECAM_GPA;
	bdf = ((relative >> 20) & 0xff) << 8 |
	    ((relative >> 15) & 0x1f) << 3 | ((relative >> 12) & 0x7);
	offset = relative & 0xfff;
	error = vmmfs_pciroot_config_read_locked(pciroot, vcpu, bdf, offset,
	    read->width, &value);
	lwkt_reltoken(&pciroot->node.token);
	if (error == ENOENT)
		return (ENOENT);
	if (error != 0)
		value = vmmfs_pciroot_absent_value(read->width);
	read->value = value;
	return (0);
}

static int
vmmfs_pciroot_ecam_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	struct vmmfs_pciroot *pciroot;
	struct vmmfs_pcislot *slot;
	uint64_t relative;
	uint16_t bdf;
	uint16_t offset;

	pciroot = argument;
	if (pciroot == NULL || write == NULL ||
	    !vmmfs_pciroot_ecam_contains(write->address, write->width))
		return (ENOENT);
	lwkt_gettoken(&pciroot->node.token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->node.token);
		return (0);
	}
	relative = write->address - VMMFS_PCI_ECAM_GPA;
	bdf = ((relative >> 20) & 0xff) << 8 |
	    ((relative >> 15) & 0x1f) << 3 | ((relative >> 12) & 0x7);
	offset = relative & 0xfff;
	slot = vmmfs_pciroot_find_locked(pciroot, bdf);
	if (slot == NULL || !slot->type0.powered) {
		lwkt_reltoken(&pciroot->node.token);
		return (0);
	}
	lwkt_reltoken(&pciroot->node.token);
	return (vmmfs_pcislot_type0_config_write(slot, vcpu, offset,
	    write->width, (uint32_t)write->value));
}

static bool
vmmfs_pciroot_config_contains(uint16_t base, uint64_t address,
	enum vmm_io_width width)
{

	return address >= base && width != 0 && width <= sizeof(uint32_t) &&
	    address - base <= sizeof(uint32_t) - width;
}

static bool
vmmfs_pciroot_ecam_contains(uint64_t address, enum vmm_io_width width)
{

	return address >= VMMFS_PCI_ECAM_GPA && width != 0 &&
	    width <= sizeof(uint32_t) && address - VMMFS_PCI_ECAM_GPA <=
	    VMMFS_PCI_ECAM_SIZE - width;
}

static struct vmmfs_pcislot *
vmmfs_pciroot_find_locked(struct vmmfs_pciroot *pciroot, uint16_t bdf)
{
	struct vmmfs_pciroot_slot *entry;

	entry = vmmfs_pciroot_entry_find_locked(pciroot, bdf);
	return (entry == NULL ? NULL : entry->slot);
}

static struct vmmfs_pciroot_slot *
vmmfs_pciroot_entry_find_locked(struct vmmfs_pciroot *pciroot, uint16_t bdf)
{
	struct vmmfs_pciroot_slot *entry;

	RB_FOREACH(entry, vmmfs_pcislot_tree, &pciroot->registry->slots) {
		if (entry->slot->bdf == bdf)
			return (entry);
		if (entry->slot->bdf > bdf)
			break;
	}
	return (NULL);
}

static uint32_t
vmmfs_pciroot_absent_value(enum vmm_io_width width)
{
	switch (width) {
	case VMM_IO_WIDTH_8:
		return (UINT8_MAX);
	case VMM_IO_WIDTH_16:
		return (UINT16_MAX);
	case VMM_IO_WIDTH_32:
		return (UINT32_MAX);
	default:
		return (UINT32_MAX);
	}
}

static int
vmmfs_pciroot_config_read_locked(struct vmmfs_pciroot *pciroot,
	vmm_vcpu_t vcpu, uint16_t bdf, uint16_t offset, enum vmm_io_width width,
	uint32_t *value)
{
	struct vmmfs_pcislot *slot;
	int error;

	if (offset > VMMFS_PCISLOT_CONFIG_SIZE - width) {
		*value = vmmfs_pciroot_absent_value(width);
		return (0);
	}
	if (bdf == VMMFS_PCI_HOSTBRIDGE_BDF)
		return (vmmfs_pciroot_hostbridge_read(offset, width, value));
	slot = vmmfs_pciroot_find_locked(pciroot, bdf);
	if (slot == NULL || !slot->type0.powered) {
		*value = vmmfs_pciroot_absent_value(width);
		return (0);
	}
	error = vmmfs_pcislot_type0_config_read(slot, vcpu, offset, width, value);
	if (error == ENOENT)
		return (ENOENT);
	if (error != 0) {
		*value = vmmfs_pciroot_absent_value(width);
		return (0);
	}
	return (0);
}

static int
vmmfs_pciroot_hostbridge_read(uint16_t offset, enum vmm_io_width width,
	uint32_t *value)
{
	uint8_t byte;

	if (value == NULL || width == 0 || width > VMM_IO_WIDTH_32 ||
	    offset > VMMFS_PCI_HOSTBRIDGE_CONFIG_SIZE - width)
		return (EINVAL);
	*value = 0;
	for (unsigned int index = 0; index < width; ++index) {
		switch (offset + index) {
		case 0x00:
			byte = VMMFS_PCI_HOSTBRIDGE_VENDOR_ID & UINT8_MAX;
			break;
		case 0x01:
			byte = VMMFS_PCI_HOSTBRIDGE_VENDOR_ID >> 8;
			break;
		case 0x02:
			byte = VMMFS_PCI_HOSTBRIDGE_DEVICE_ID & UINT8_MAX;
			break;
		case 0x03:
			byte = VMMFS_PCI_HOSTBRIDGE_DEVICE_ID >> 8;
			break;
		case 0x0b:
			byte = VMMFS_PCI_HOSTBRIDGE_CLASS >> 16;
			break;
		default:
			byte = 0;
			break;
		}
		*value |= (uint32_t)byte << (index * NBBY);
	}
	return (0);
}
