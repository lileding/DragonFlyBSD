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
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_resource.h"
#include "vmmfs_vcpu.h"

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
static int vmmfs_pciroot_parse_bdf(const char *, size_t, uint16_t *);
static int vmmfs_pciroot_parse_hex(char, unsigned int *);
static void vmmfs_pciroot_format_bdf(uint16_t, char *, size_t);
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
static int vmmfs_pciroot_ecam_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_pciroot_ecam_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static bool vmmfs_pciroot_config_contains(uint16_t, uint64_t,
	enum vmm_io_width);
static bool vmmfs_pciroot_ecam_contains(uint64_t, enum vmm_io_width);
static struct vmmfs_pcislot *vmmfs_pciroot_find_locked(
	struct vmmfs_pciroot *, uint16_t);
static uint32_t vmmfs_pciroot_absent_value(enum vmm_io_width);
static int vmmfs_pciroot_config_read_locked(struct vmmfs_pciroot *,
	vmm_vcpu_t, uint16_t, uint16_t, enum vmm_io_width, uint32_t *);

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
	vmmfs_machine_hold(machine);
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_pciroot_destroy(struct vmmfs_pciroot *pciroot)
{
	struct vmmfs_pcislot *slot;
	int error;

	if (pciroot == NULL)
		return (EINVAL);
	if (pciroot->machine == NULL)
		return (0);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine != NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (EBUSY);
	}
	lwkt_reltoken(&pciroot->machine->token);
	if (pciroot->vnode != NULL)
		return (EBUSY);
	for (;;) {
		lwkt_gettoken(&pciroot->machine->token);
		slot = RB_ROOT(&pciroot->slots);
		if (slot != NULL)
			RB_REMOVE(vmmfs_pcislot_tree, &pciroot->slots, slot);
		lwkt_reltoken(&pciroot->machine->token);
		if (slot == NULL)
			break;
		error = vmmfs_pcislot_destroy(slot);
		if (error == 0)
			continue;
		lwkt_gettoken(&pciroot->machine->token);
		(void)RB_INSERT(vmmfs_pcislot_tree, &pciroot->slots, slot);
		lwkt_reltoken(&pciroot->machine->token);
		return (error);
	}
	pciroot->machine = NULL;
	return (0);
}

void
vmmfs_pciroot_release_vnodes(struct vmmfs_pciroot *pciroot)
{
	struct vmmfs_pcislot *slot;

	if (pciroot == NULL)
		return;
	RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots)
		vmmfs_pcislot_release_vnodes(slot);
	vmmfs_vnode_discard(pciroot->vnode);
}

int
vmmfs_pciroot_start(struct vmmfs_pciroot *pciroot, vmm_machine_t machine)
{
	struct vmmfs_pcislot *slot;
	int error;

	if (pciroot == NULL || pciroot->machine == NULL || machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine != NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (EBUSY);
	}
	RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots) {
		if (slot->descriptor.writer_buffer != NULL ||
		    slot->descriptor.committing) {
			lwkt_reltoken(&pciroot->machine->token);
			return (EBUSY);
		}
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
	lwkt_gettoken(&pciroot->machine->token);
	pciroot->runtime_machine = machine;
	pciroot->mmio_next = VMMFS_PCI_MMIO_GPA;
	pciroot->pio_next = VMMFS_PCI_PIO_GPA;
	pciroot->config_address = 0;
	lwkt_reltoken(&pciroot->machine->token);
	RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots) {
		if (!slot->descriptor.committed)
			continue;
		error = vmmfs_pcislot_power_on(slot, machine);
		if (error != 0)
			goto fail_slots;
	}
	return (0);

fail_slots:
	RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots)
		vmmfs_pcislot_power_off(slot);
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
vmmfs_pciroot_stop(struct vmmfs_pciroot *pciroot)
{
	vmm_machine_t machine;
	struct vmmfs_pcislot *slot;
	vmm_io_t config_address_read;
	vmm_io_t config_address_write;
	vmm_io_t config_data_read;
	vmm_io_t config_data_write;
	vmm_io_t ecam_read;
	vmm_io_t ecam_write;
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
	ecam_read = pciroot->ecam_read;
	ecam_write = pciroot->ecam_write;
	lwkt_reltoken(&pciroot->machine->token);
	RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots)
		vmmfs_pcislot_power_off(slot);
	lwkt_gettoken(&pciroot->machine->token);
	pciroot->runtime_machine = NULL;
	pciroot->config_address = 0;
	pciroot->config_address_read = NULL;
	pciroot->config_address_write = NULL;
	pciroot->config_data_read = NULL;
	pciroot->config_data_write = NULL;
	pciroot->ecam_read = NULL;
	pciroot->ecam_write = NULL;
	lwkt_reltoken(&pciroot->machine->token);
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
	struct vmmfs_pcislot *slot;
	uint64_t relative;
	uint16_t bdf;
	uint16_t offset;
	uint32_t value;
	bool write;
	vmm_vcpu_t vcpu;
	int error;

	if (pciroot == NULL || thread == NULL || thread->vcpu == NULL ||
	    exit == NULL ||
	    exit->reason != VMM_CPUEXIT_MEMORY)
		return (ENOENT);
	vcpu = thread->vcpu;
	if (!vmmfs_pciroot_ecam_contains(exit->u.mem.gpa, exit->u.mem.width)) {
		RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots) {
			error = vmmfs_pcislot_resources_memory(
			    slot->descriptor.resources, thread, exit);
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
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (ENOENT);
	}
	slot = vmmfs_pciroot_find_locked(pciroot, bdf);
	if (slot == NULL || !slot->type0.powered) {
		lwkt_reltoken(&pciroot->machine->token);
		return (ENOENT);
	}
	lwkt_reltoken(&pciroot->machine->token);
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
		RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots) {
			error = vmmfs_pcislot_resources_io(slot->descriptor.resources,
			    thread, state, exit);
			if (error != ENOENT)
				return (error);
		}
		return (ENOENT);
	}
	lwkt_gettoken(&pciroot->machine->token);
	address = pciroot->config_address;
	if (pciroot->runtime_machine == NULL ||
	    (address & 0x80000000U) == 0) {
		lwkt_reltoken(&pciroot->machine->token);
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
		lwkt_reltoken(&pciroot->machine->token);
		if (exit->u.io.in) {
			mask = (1ULL << (exit->u.io.operand_size * NBBY)) - 1;
			state->gprs[VMM_X64_GPR_RAX] =
			    (state->gprs[VMM_X64_GPR_RAX] & ~mask) | mask;
		}
		state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
		return (0);
	}
	lwkt_reltoken(&pciroot->machine->token);
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
	uint16_t bdf;
	int error;

	pciroot = ap->a_dvp->v_data;
	if (pciroot == NULL || pciroot->machine == NULL)
		return (ENOENT);
	if (ap->a_vap->va_type != VDIR)
		return (EINVAL);
	ncp = ap->a_nch->ncp;
	error = vmmfs_pciroot_parse_bdf(ncp->nc_name, ncp->nc_nlen, &bdf);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_create(pciroot, bdf, &slot);
	if (error != 0)
		return (error);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->machine->root == NULL || pciroot->machine->dead) {
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
	struct vmmfs_pcislot *slot;
	struct namecache *ncp;
	struct vnode *vnode;
	uint16_t bdf;
	int error;

	pciroot = ap->a_dvp->v_data;
	if (pciroot == NULL || pciroot->machine == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	if (vmmfs_pciroot_parse_bdf(ncp->nc_name, ncp->nc_nlen, &bdf) != 0) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	lwkt_gettoken(&pciroot->machine->token);
	slot = vmmfs_pciroot_find_locked(pciroot, bdf);
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
	struct vmmfs_machine *machine;

	pciroot = ap->a_vp->v_data;
	if (pciroot != NULL && pciroot->machine != NULL) {
		machine = pciroot->machine;
		lwkt_gettoken(&machine->token);
		if (pciroot->vnode == ap->a_vp)
			pciroot->vnode = NULL;
		lwkt_reltoken(&machine->token);
	} else {
		machine = NULL;
	}
	ap->a_vp->v_data = NULL;
	if (machine != NULL)
		vmmfs_machine_put(machine);
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
		vmmfs_pciroot_format_bdf(slot->bdf, item->name,
		    sizeof(item->name));
		lwkt_reltoken(&pciroot->machine->token);
		return (0);
	}
	lwkt_reltoken(&pciroot->machine->token);
	return (ENOENT);
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
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (ENOENT);
	}
	value = pciroot->config_address;
	lwkt_reltoken(&pciroot->machine->token);
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
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (ENOENT);
	}
	shift = (write->address - VMMFS_PCI_CONFIG_ADDRESS) * NBBY;
	mask = (UINT32_MAX >> ((sizeof(mask) - write->width) * NBBY)) << shift;
	pciroot->config_address = (pciroot->config_address & ~mask) |
	    (((uint32_t)write->value << shift) & mask);
	lwkt_reltoken(&pciroot->machine->token);
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
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL ||
	    (pciroot->config_address & 0x80000000U) == 0) {
		lwkt_reltoken(&pciroot->machine->token);
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
	lwkt_reltoken(&pciroot->machine->token);
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
	uint32_t address;
	uint16_t bdf;
	uint16_t offset;
	int error;

	pciroot = argument;
	(void)vcpu;
	if (pciroot == NULL || !vmmfs_pciroot_config_contains(
	    VMMFS_PCI_CONFIG_DATA, write->address, write->width))
		return (ENOENT);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL ||
	    (pciroot->config_address & 0x80000000U) == 0) {
		lwkt_reltoken(&pciroot->machine->token);
		return (0);
	}
	address = pciroot->config_address;
	bdf = ((address >> 16) & 0xff) << 8 |
	    ((address >> 11) & 0x1f) << 3 | ((address >> 8) & 0x7);
	offset = (address & 0xfc) +
	    (uint16_t)(write->address - VMMFS_PCI_CONFIG_DATA);
	if (vmmfs_pciroot_find_locked(pciroot, bdf) != NULL)
		error = ENOENT;
	else
		error = 0;
	lwkt_reltoken(&pciroot->machine->token);
	return (error == ENOENT ? ENOENT : 0);
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
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		read->value = vmmfs_pciroot_absent_value(read->width);
		return (0);
	}
	relative = read->address - VMMFS_PCI_ECAM_GPA;
	bdf = ((relative >> 20) & 0xff) << 8 |
	    ((relative >> 15) & 0x1f) << 3 | ((relative >> 12) & 0x7);
	offset = relative & 0xfff;
	error = vmmfs_pciroot_config_read_locked(pciroot, vcpu, bdf, offset,
	    read->width, &value);
	lwkt_reltoken(&pciroot->machine->token);
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
	uint64_t relative;
	uint16_t bdf;
	uint16_t offset;
	int error;

	pciroot = argument;
	(void)vcpu;
	if (pciroot == NULL || write == NULL ||
	    !vmmfs_pciroot_ecam_contains(write->address, write->width))
		return (ENOENT);
	lwkt_gettoken(&pciroot->machine->token);
	if (pciroot->runtime_machine == NULL) {
		lwkt_reltoken(&pciroot->machine->token);
		return (0);
	}
	relative = write->address - VMMFS_PCI_ECAM_GPA;
	bdf = ((relative >> 20) & 0xff) << 8 |
	    ((relative >> 15) & 0x1f) << 3 | ((relative >> 12) & 0x7);
	offset = relative & 0xfff;
	if (vmmfs_pciroot_find_locked(pciroot, bdf) != NULL)
		error = ENOENT;
	else
		error = 0;
	lwkt_reltoken(&pciroot->machine->token);
	return (error == ENOENT ? ENOENT : 0);
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
	struct vmmfs_pcislot *slot;

	RB_FOREACH(slot, vmmfs_pcislot_tree, &pciroot->slots) {
		if (slot->bdf == bdf)
			return (slot);
		if (slot->bdf > bdf)
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
