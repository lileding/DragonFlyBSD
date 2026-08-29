/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI synchronous device-register responder.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <vm/vm.h>

#include "vmmfs.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_auth.h"
#include "vmmfs_pcislot_config.h"
#include "vmmfs_pcislot_resource.h"
#include "vmmfs_vcpu.h"

#define VMMFS_PCISLOT_CONFIG_MODE 0600

static int vmmfs_pcislot_config_access(struct vop_access_args *);
static int vmmfs_pcislot_config_close(struct vop_close_args *);
static int vmmfs_pcislot_config_getattr(struct vop_getattr_args *);
static int vmmfs_pcislot_config_getattr_lite(
	struct vop_getattr_lite_args *);
static int vmmfs_pcislot_config_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_pcislot_config_open(struct vop_open_args *);
static int vmmfs_pcislot_config_read(struct vop_read_args *);
static int vmmfs_pcislot_config_inactive(struct vop_inactive_args *);
static int vmmfs_pcislot_config_reclaim(struct vop_reclaim_args *);
static int vmmfs_pcislot_config_write(struct vop_write_args *);
static void vmmfs_pcislot_config_filter_detach(struct knote *);
static int vmmfs_pcislot_config_filter_read(struct knote *, long);
static int vmmfs_pcislot_config_filter_write(struct knote *, long);
static void vmmfs_pcislot_config_cancel_locked(
	struct vmmfs_pcislot_config *, uint32_t);
static void vmmfs_pcislot_config_wake_next(struct vmmfs_pcislot_config *);
static int vmmfs_pcislot_config_submit(struct vmmfs_pcislot_config *,
	struct vmmfs_vcpu_thread *, struct vmmfs_pcislot_resource *, uint8_t,
	uint64_t, enum vmm_io_width, uint8_t, uint64_t,
	struct vmmfs_pci_config_response *);
static int vmmfs_pcislot_config_match(struct vmmfs_pcislot_config *,
	struct vmmfs_pcislot_resource *, uint8_t, uint64_t,
	enum vmm_io_width, uint64_t *);
static bool vmmfs_pcislot_config_overlap(uint64_t, uint64_t, uint64_t,
	uint64_t);
static uint64_t vmmfs_pcislot_config_absent_value(enum vmm_io_width);
static void vmmfs_pcislot_config_drop(struct vmmfs_node *);

static struct filterops vmmfs_pcislot_config_read_filterops = {
	FILTEROP_ISFD | FILTEROP_MPSAFE,
	NULL,
	vmmfs_pcislot_config_filter_detach,
	vmmfs_pcislot_config_filter_read,
};

static struct filterops vmmfs_pcislot_config_write_filterops = {
	FILTEROP_ISFD | FILTEROP_MPSAFE,
	NULL,
	vmmfs_pcislot_config_filter_detach,
	vmmfs_pcislot_config_filter_write,
};

struct vop_ops vmmfs_pcislot_config_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_pcislot_config_access,
	.vop_close = vmmfs_pcislot_config_close,
	.vop_getattr = vmmfs_pcislot_config_getattr,
	.vop_getattr_lite = vmmfs_pcislot_config_getattr_lite,
	.vop_kqfilter = vmmfs_pcislot_config_kqfilter,
	.vop_open = vmmfs_pcislot_config_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_config_read,
	.vop_inactive = vmmfs_pcislot_config_inactive,
	.vop_reclaim = vmmfs_pcislot_config_reclaim,
	.vop_write = vmmfs_pcislot_config_write,
};

int
vmmfs_pcislot_config_init(struct vmmfs_pcislot *slot,
	struct vmmfs_pcislot_config *config)
{
	struct vmmfs_machine *machine;
	struct vmmfs_mount *mount;

	if (slot == NULL || vmmfs_pcislot_pciroot(slot) == NULL ||
	    vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot)) == NULL || config == NULL)
		return (EINVAL);
	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot));
	mount = (struct vmmfs_mount *)vmmfs_machine_root(machine)->mount->mnt_data;
	if (mount->pcislot_config_vops == NULL)
		return (ENXIO);
	bzero(config, sizeof(*config));
	config->inode = atomic_fetchadd_int(&mount->next_inode, 1);
	lwkt_token_init(&config->token, "vmmfspcicfg");
	TAILQ_INIT(&config->requests);
	SLIST_INIT(&config->kq.ki_note);
	vmmfs_node_setup(&config->node, &slot->branch.node,
	    vmmfs_pcislot_config_drop, NULL);
	return (0);
}

int
vmmfs_pcislot_config_publish(struct vmmfs_pcislot_config *config)
{
	struct vmmfs_machine *machine;
	struct vmmfs_mount *mount;
	struct vmmfs_pcislot *slot;

	if (config == NULL || vmmfs_pcislot_config_slot(config) == NULL)
		return (EINVAL);
	slot = vmmfs_pcislot_config_slot(config);
	if (vmmfs_pcislot_pciroot(slot) == NULL || vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot)) == NULL)
		return (ENXIO);
	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot));
	mount = (struct vmmfs_mount *)vmmfs_machine_root(machine)->mount->mnt_data;
	if (mount == NULL || mount->pcislot_config_vops == NULL)
		return (ENXIO);
	return (vmmfs_node_publish_regular(&config->node,
	    vmmfs_machine_root(machine)->mount, &mount->pcislot_config_vops, VREG, config));
}

static void
vmmfs_pcislot_config_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_config *config;

	config = (struct vmmfs_pcislot_config *)node;
	KKASSERT(config != NULL);
	lwkt_gettoken(&config->token);
	if (config->node.vnode != NULL) {
		lwkt_reltoken(&config->token);
		panic("vmmfs_pcislot_config_drop: vnode is still live");
	}
	lwkt_reltoken(&config->token);
	vmmfs_pcislot_config_revoke(config);
	config->inode = 0;
	lwkt_token_uninit(&config->token);
}

void
vmmfs_pcislot_config_revoke(struct vmmfs_pcislot_config *config)
{
	if (config == NULL)
		return;
	lwkt_gettoken(&config->token);
	config->closed = true;
	config->powered = false;
	config->responder = NULL;
	config->opening = false;
	vmmfs_pcislot_config_cancel_locked(config, VMMFS_PCI_CONFIG_FAILURE);
	lwkt_reltoken(&config->token);
	wakeup(config);
	KNOTE(&config->kq.ki_note, 0);
}

void
vmmfs_pcislot_config_descriptor_changed(struct vmmfs_pcislot_config *config,
	uint64_t generation, bool present)
{
	if (config == NULL)
		return;
	lwkt_gettoken(&config->token);
	config->generation = generation;
	config->next_sequence = 0;
	config->powered = false;
	config->responder = NULL;
	vmmfs_pcislot_config_cancel_locked(config, VMMFS_PCI_CONFIG_FAILURE);
	lwkt_reltoken(&config->token);
	wakeup(config);
	KNOTE(&config->kq.ki_note, 0);
	(void)present;
}

void
vmmfs_pcislot_config_power_on(struct vmmfs_pcislot_config *config,
	uint64_t generation)
{
	if (config == NULL)
		return;
	lwkt_gettoken(&config->token);
	if (!config->closed && config->generation == generation)
		config->powered = true;
	lwkt_reltoken(&config->token);
}

void
vmmfs_pcislot_config_power_off(struct vmmfs_pcislot_config *config)
{
	if (config == NULL)
		return;
	lwkt_gettoken(&config->token);
	config->powered = false;
	config->responder = NULL;
	vmmfs_pcislot_config_cancel_locked(config, VMMFS_PCI_CONFIG_FAILURE);
	lwkt_reltoken(&config->token);
	wakeup(config);
	KNOTE(&config->kq.ki_note, 0);
}

int
vmmfs_pcislot_config_memory(struct vmmfs_pcislot_config *config,
	struct vmmfs_vcpu_thread *thread,
	struct vmmfs_pcislot_resource *resource,
	const struct vmm_cpuexit *exit)
{
	struct vmmfs_pci_config_response response;
	uint64_t value;
	uint64_t offset;
	bool write;
	int error;

	if (config == NULL || thread == NULL || thread->vcpu == NULL ||
	    resource == NULL || exit == NULL ||
	    exit->reason != VMM_CPUEXIT_MEMORY)
		return (ENOENT);
	write = (exit->u.mem.prot & VM_PROT_WRITE) != 0;
	error = vmmfs_pcislot_config_match(config, resource,
	    VMMFS_PCI_CONFIG_MMIO, exit->u.mem.gpa - resource->gpa,
	    exit->u.mem.width, &offset);
	if (error == EINVAL) {
		value = vmmfs_pcislot_config_absent_value(exit->u.mem.width);
		if (write)
			return (vmm_vcpu_complete_mmio_write(thread->vcpu));
		return (vmm_vcpu_complete_mmio_read(thread->vcpu, &value,
		    exit->u.mem.width));
	}
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_config_submit(config, thread, resource,
	    VMMFS_PCI_CONFIG_MMIO, offset, exit->u.mem.width,
	    write ? VMMFS_PCI_CONFIG_WRITE : VMMFS_PCI_CONFIG_READ,
	    exit->u.mem.value, &response);
	if (error != 0)
		return (error);
	if (write)
		return (vmm_vcpu_complete_mmio_write(thread->vcpu));
	value = response.status == VMMFS_PCI_CONFIG_SUCCESS ? response.value :
	    vmmfs_pcislot_config_absent_value(exit->u.mem.width);
	return (vmm_vcpu_complete_mmio_read(thread->vcpu, &value,
	    exit->u.mem.width));
}

int
vmmfs_pcislot_config_io(struct vmmfs_pcislot_config *config,
	struct vmmfs_vcpu_thread *thread,
	struct vmmfs_pcislot_resource *resource,
	struct vmm_cpustate *state, const struct vmm_cpuexit *exit)
{
	struct vmmfs_pci_config_response response;
	uint64_t mask;
	uint64_t value;
	uint64_t offset;
	bool write;
	int error;

	if (config == NULL || thread == NULL || thread->vcpu == NULL ||
	    resource == NULL || state == NULL || exit == NULL ||
	    exit->reason != VMM_CPUEXIT_IO || exit->u.io.str || exit->u.io.rep)
		return (ENOENT);
	error = vmmfs_pcislot_config_match(config, resource,
	    VMMFS_PCI_CONFIG_PIO, exit->u.io.port - resource->gpa,
	    (enum vmm_io_width)exit->u.io.operand_size, &offset);
	write = !exit->u.io.in;
	if (error == EINVAL) {
		if (!write) {
			mask = (1ULL << (exit->u.io.operand_size * NBBY)) - 1;
			value = vmmfs_pcislot_config_absent_value(
			    (enum vmm_io_width)exit->u.io.operand_size);
			state->gprs[VMM_X64_GPR_RAX] =
			    (state->gprs[VMM_X64_GPR_RAX] & ~mask) | (value & mask);
		}
		state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
		return (0);
	}
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_config_submit(config, thread, resource,
	    VMMFS_PCI_CONFIG_PIO, offset,
	    (enum vmm_io_width)exit->u.io.operand_size,
	    write ? VMMFS_PCI_CONFIG_WRITE : VMMFS_PCI_CONFIG_READ,
	    state->gprs[VMM_X64_GPR_RAX], &response);
	if (error != 0)
		return (error);
	if (!write) {
		mask = (1ULL << (exit->u.io.operand_size * NBBY)) - 1;
		value = response.status == VMMFS_PCI_CONFIG_SUCCESS ? response.value :
		    vmmfs_pcislot_config_absent_value(
		    (enum vmm_io_width)exit->u.io.operand_size);
		state->gprs[VMM_X64_GPR_RAX] =
		    (state->gprs[VMM_X64_GPR_RAX] & ~mask) | (value & mask);
	}
	state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
	return (0);
}

static int
vmmfs_pcislot_config_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_PCISLOT_CONFIG_MODE, 0));
}

static int
vmmfs_pcislot_config_close(struct vop_close_args *ap)
{
	struct vmmfs_pcislot_config *config;

	config = ap->a_vp->v_data;
	if (config != NULL) {
		lwkt_gettoken(&config->token);
		if (config->responder == ap->a_fp) {
			config->responder = NULL;
			vmmfs_pcislot_config_cancel_locked(config,
			    VMMFS_PCI_CONFIG_FAILURE);
		}
		lwkt_reltoken(&config->token);
		wakeup(config);
		KNOTE(&config->kq.ki_note, 0);
	}
	return (vop_stdclose(ap));
}

static int
vmmfs_pcislot_config_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_pcislot_config *config;
	struct vattr *vattr;

	config = ap->a_vp->v_data;
	if (config == NULL || vmmfs_pcislot_config_slot(config) == NULL ||
	    vmmfs_pcislot_is_dead(vmmfs_pcislot_config_slot(config)))
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_PCISLOT_CONFIG_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = config->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	return (0);
}

static int
vmmfs_pcislot_config_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_PCISLOT_CONFIG_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_pcislot_config_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_pcislot_config *config;

	config = ap->a_vp->v_data;
	if (config == NULL || vmmfs_pcislot_config_slot(config) == NULL ||
	    vmmfs_pcislot_is_dead(vmmfs_pcislot_config_slot(config)))
		return (ENOENT);
	switch (ap->a_kn->kn_filter) {
	case EVFILT_READ:
		ap->a_kn->kn_fop = &vmmfs_pcislot_config_read_filterops;
		break;
	case EVFILT_WRITE:
		ap->a_kn->kn_fop = &vmmfs_pcislot_config_write_filterops;
		break;
	default:
		return (EOPNOTSUPP);
	}
	lwkt_gettoken(&config->token);
	ap->a_kn->kn_hook = (caddr_t)config;
	knote_insert(&config->kq.ki_note, ap->a_kn);
	lwkt_reltoken(&config->token);
	return (0);
}

static int
vmmfs_pcislot_config_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_config *config;
	int error;

	config = ap->a_vp->v_data;
	if (config == NULL || vmmfs_pcislot_config_slot(config) == NULL)
		return (ENOENT);
	if (vmmfs_pcislot_pciroot(vmmfs_pcislot_config_slot(config)) == NULL ||
	    vmmfs_pcislot_is_dead(vmmfs_pcislot_config_slot(config)))
		return (ENOENT);
	if ((ap->a_mode & (FREAD | FWRITE)) != (FREAD | FWRITE))
		return (EINVAL);
	if (vmmfs_pcislot_auth_check(vmmfs_pcislot_config_slot(config)) != 0)
		return (EACCES);
	lwkt_gettoken(&config->token);
	if (config->closed || config->opening || config->responder != NULL) {
		lwkt_reltoken(&config->token);
		return (EBUSY);
	}
	config->opening = true;
	lwkt_reltoken(&config->token);
	error = vop_stdopen(ap);
	lwkt_gettoken(&config->token);
	config->opening = false;
	if (error == 0 && ap->a_fpp != NULL && *ap->a_fpp != NULL) {
		config->responder = *ap->a_fpp;
		lwkt_reltoken(&config->token);
		return (0);
	}
	lwkt_reltoken(&config->token);
	return (error == 0 ? EIO : error);
}

static int
vmmfs_pcislot_config_read(struct vop_read_args *ap)
{
	struct vmmfs_pcislot_config *config;
	struct vmmfs_pcislot_config_request *request;
	struct vmmfs_pci_config_request record;
	int error;

	config = ap->a_vp->v_data;
	if (config == NULL)
		return (ENOENT);
	if (vmmfs_pcislot_config_slot(config) == NULL || vmmfs_pcislot_pciroot(vmmfs_pcislot_config_slot(config)) == NULL ||
	    vmmfs_pcislot_is_dead(vmmfs_pcislot_config_slot(config)))
		return (ENXIO);
	if (ap->a_uio->uio_resid != sizeof(record))
		return (EINVAL);
	for (;;) {
		lwkt_gettoken(&config->token);
		if (config->responder != ap->a_fp) {
			lwkt_reltoken(&config->token);
			return (EBADF);
		}
		request = TAILQ_FIRST(&config->requests);
		if (request != NULL && !request->delivered) {
			record = request->request;
			request->delivered = true;
			lwkt_reltoken(&config->token);
			KNOTE(&config->kq.ki_note, 0);
			return (uiomove((caddr_t)&record, sizeof(record), ap->a_uio));
		}
		if (config->closed || config->responder == NULL) {
			lwkt_reltoken(&config->token);
			return (ENXIO);
		}
		if ((ap->a_ioflag & IO_NDELAY) != 0) {
			lwkt_reltoken(&config->token);
			return (EAGAIN);
		}
		tsleep_interlock(config, 0);
		lwkt_reltoken(&config->token);
		error = tsleep(config, PINTERLOCKED, "vmmfspcicfg", 0);
		if (error != 0)
			return (error);
	}
}

static int
vmmfs_pcislot_config_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_pcislot_config *config;
	struct vmmfs_machine *machine;

	config = ap->a_vp->v_data;
	if (config == NULL || vmmfs_pcislot_config_slot(config) == NULL ||
	    vmmfs_pcislot_pciroot(vmmfs_pcislot_config_slot(config)) == NULL)
		return (0);
	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(vmmfs_pcislot_config_slot(config)));
	if (machine == NULL || !vmmfs_pcislot_is_dead(vmmfs_pcislot_config_slot(config)))
		return (0);
	vmmfs_node_inactive(&config->node, ap->a_vp);
	return (0);
}

static int
vmmfs_pcislot_config_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_pcislot_config *config;
	struct vmmfs_machine *machine;
	bool reclaim;

	config = ap->a_vp->v_data;
	if (config == NULL || vmmfs_pcislot_config_slot(config) == NULL ||
	    vmmfs_pcislot_pciroot(vmmfs_pcislot_config_slot(config)) == NULL)
		return (0);
	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(vmmfs_pcislot_config_slot(config)));
	if (machine == NULL)
		return (0);
	lwkt_gettoken(&machine->token);
	reclaim = vmmfs_node_reclaim(&config->node, ap->a_vp);
	lwkt_reltoken(&machine->token);
	if (reclaim)
		vmmfs_node_drop(&config->node);
	return (0);
}

static int
vmmfs_pcislot_config_write(struct vop_write_args *ap)
{
	struct vmmfs_pcislot_config *config;
	struct vmmfs_pcislot_config_request *request;
	struct vmmfs_pci_config_response response;
	vmm_vcpu_t vcpu;
	int error;

	config = ap->a_vp->v_data;
	if (config == NULL)
		return (ENOENT);
	if (vmmfs_pcislot_config_slot(config) == NULL || vmmfs_pcislot_pciroot(vmmfs_pcislot_config_slot(config)) == NULL ||
	    vmmfs_pcislot_is_dead(vmmfs_pcislot_config_slot(config)))
		return (ENXIO);
	if (ap->a_uio->uio_resid != sizeof(response))
		return (EINVAL);
	error = uiomove((caddr_t)&response, sizeof(response), ap->a_uio);
	if (error != 0)
		return (error);
	if (response.reserved != 0 || (response.status !=
	    VMMFS_PCI_CONFIG_SUCCESS && response.status !=
	    VMMFS_PCI_CONFIG_UNSUPPORTED && response.status !=
	    VMMFS_PCI_CONFIG_FAILURE))
		return (EINVAL);
	vcpu = NULL;
	lwkt_gettoken(&config->token);
	if (config->responder != ap->a_fp) {
		lwkt_reltoken(&config->token);
		return (EBADF);
	}
	request = TAILQ_FIRST(&config->requests);
	if (request == NULL || !request->delivered || request->completed ||
	    request->request.generation != response.generation ||
	    request->request.sequence != response.sequence) {
		lwkt_reltoken(&config->token);
		return (EINVAL);
	}
	lwkt_gettoken(&request->thread->group->token);
	request->response_value = response.value;
	request->response_status = response.status;
	request->completed = true;
	if (request->thread->config_request == request)
		request->thread->config_done = true;
	vcpu = request->vcpu;
	lwkt_reltoken(&request->thread->group->token);
	lwkt_reltoken(&config->token);
	KNOTE(&config->kq.ki_note, 0);
	(void)vmm_vcpu_kick(vcpu);
	return (0);
}

static void
vmmfs_pcislot_config_filter_detach(struct knote *knote)
{
	struct vmmfs_pcislot_config *config;

	config = (struct vmmfs_pcislot_config *)knote->kn_hook;
	if (config == NULL)
		return;
	lwkt_gettoken(&config->token);
	knote_remove(&config->kq.ki_note, knote);
	lwkt_reltoken(&config->token);
}

static int
vmmfs_pcislot_config_filter_read(struct knote *knote, long hint)
{
	struct vmmfs_pcislot_config *config;
	struct vmmfs_pcislot_config_request *request;

	(void)hint;
	config = (struct vmmfs_pcislot_config *)knote->kn_hook;
	if (config == NULL)
		return (0);
	lwkt_gettoken(&config->token);
	request = TAILQ_FIRST(&config->requests);
	knote->kn_data = request != NULL && !request->delivered ?
	    sizeof(request->request) : 0;
	if (config->closed || config->responder == NULL)
		knote->kn_flags |= EV_EOF;
	lwkt_reltoken(&config->token);
	return (knote->kn_data != 0 || (knote->kn_flags & EV_EOF) != 0);
}

static int
vmmfs_pcislot_config_filter_write(struct knote *knote, long hint)
{
	struct vmmfs_pcislot_config *config;
	struct vmmfs_pcislot_config_request *request;

	(void)hint;
	config = (struct vmmfs_pcislot_config *)knote->kn_hook;
	if (config == NULL)
		return (0);
	lwkt_gettoken(&config->token);
	request = TAILQ_FIRST(&config->requests);
	knote->kn_data = request != NULL && request->delivered &&
	    !request->completed ? sizeof(struct vmmfs_pci_config_response) : 0;
	if (config->closed || config->responder == NULL)
		knote->kn_flags |= EV_EOF;
	lwkt_reltoken(&config->token);
	return (knote->kn_data != 0 || (knote->kn_flags & EV_EOF) != 0);
}

static void
vmmfs_pcislot_config_cancel_locked(struct vmmfs_pcislot_config *config,
	uint32_t status)
{
	struct vmmfs_pcislot_config_request *request;

	KKASSERT(config != NULL);
	TAILQ_FOREACH(request, &config->requests, entry) {
		lwkt_gettoken(&request->thread->group->token);
		request->response_status = status;
		request->response_value = 0;
		request->completed = true;
		if (request->thread->config_request == request)
			request->thread->config_done = true;
		lwkt_reltoken(&request->thread->group->token);
		(void)vmm_vcpu_kick(request->vcpu);
	}
}

static void
vmmfs_pcislot_config_wake_next(struct vmmfs_pcislot_config *config)
{
	wakeup(config);
	KNOTE(&config->kq.ki_note, 0);
}

static int
vmmfs_pcislot_config_submit(struct vmmfs_pcislot_config *config,
	struct vmmfs_vcpu_thread *thread,
	struct vmmfs_pcislot_resource *resource, uint8_t space,
	uint64_t offset, enum vmm_io_width width, uint8_t operation,
	uint64_t value, struct vmmfs_pci_config_response *response)
{
	struct vmmfs_pcislot_config_request *request;
	bool notify;
	bool reset_requested;
	bool stop_requested;
	int error;

	if (config == NULL || thread == NULL || thread->group == NULL ||
	    thread->vcpu == NULL || resource == NULL || response == NULL)
		return (EINVAL);
	bzero(response, sizeof(*response));
	request = kmalloc(sizeof(*request), M_VMMFS, M_WAITOK | M_ZERO);
	lwkt_gettoken(&config->token);
	if (config->closed || !config->powered || config->responder == NULL) {
		lwkt_reltoken(&config->token);
		response->status = VMMFS_PCI_CONFIG_UNSUPPORTED;
		response->value = 0;
		kfree(request, M_VMMFS);
		return (0);
	}
	if (config->next_sequence == UINT64_MAX) {
		lwkt_reltoken(&config->token);
		response->status = VMMFS_PCI_CONFIG_FAILURE;
		response->value = 0;
		kfree(request, M_VMMFS);
		return (0);
	}
	request->thread = thread;
	request->vcpu = thread->vcpu;
	request->request.generation = config->generation;
	request->request.sequence = ++config->next_sequence;
	request->request.offset = offset;
	request->request.value = value;
	request->request.bar = resource->index;
	request->request.space = space;
	request->request.width = width;
	request->request.operation = operation;
	notify = TAILQ_EMPTY(&config->requests);
	TAILQ_INSERT_TAIL(&config->requests, request, entry);
	lwkt_gettoken(&thread->group->token);
	thread->config_request = request;
	thread->config_done = false;
	lwkt_reltoken(&thread->group->token);
	lwkt_reltoken(&config->token);
	if (notify)
		vmmfs_pcislot_config_wake_next(config);

	for (;;) {
		lwkt_gettoken(&thread->group->token);
		stop_requested = thread->group->stop_requested;
		reset_requested = thread->group->reset_requested;
		if (stop_requested || reset_requested || thread->config_done) {
			lwkt_reltoken(&thread->group->token);
			break;
		}
		tsleep_interlock(thread->vcpu, 0);
		lwkt_reltoken(&thread->group->token);
		error = tsleep(thread->vcpu, PINTERLOCKED, "vmmfspcicfg", 0);
		if (error != 0)
			return (error);
	}

	lwkt_gettoken(&config->token);
	lwkt_gettoken(&thread->group->token);
	stop_requested = thread->group->stop_requested;
	if (thread->config_request == request) {
		thread->config_request = NULL;
		thread->config_done = false;
	}
	response->generation = request->request.generation;
	response->sequence = request->request.sequence;
	response->value = request->response_value;
	response->status = request->completed ? request->response_status :
	    VMMFS_PCI_CONFIG_FAILURE;
	response->reserved = 0;
	lwkt_reltoken(&thread->group->token);
	TAILQ_REMOVE(&config->requests, request, entry);
	lwkt_reltoken(&config->token);
	kfree(request, M_VMMFS);
	vmmfs_pcislot_config_wake_next(config);
	/*
	 * Reset discards the current guest state.  Complete this access with the
	 * synthesized failure response so the vCPU reaches its reset barrier.
	 */
	return (stop_requested ? EINTR : 0);
}

static int
vmmfs_pcislot_config_match(struct vmmfs_pcislot_config *config,
	struct vmmfs_pcislot_resource *resource, uint8_t space,
	uint64_t offset, enum vmm_io_width width, uint64_t *match_offset)
{
	const struct vmmfs_pcislot_config_register *register_value;
	const struct vmmfs_pcislot_descriptor_value *value;
	unsigned int index;

	if (config == NULL || resource == NULL || match_offset == NULL ||
	    (width != VMM_IO_WIDTH_8 && width != VMM_IO_WIDTH_16 &&
	    width != VMM_IO_WIDTH_32 && width != VMM_IO_WIDTH_64))
		return (EINVAL);
	value = &vmmfs_pcislot_config_slot(config)->descriptor.value;
	for (index = 0; index < VMMFS_PCISLOT_MAX_CONFIGS; ++index) {
		register_value = &value->configs[index];
		if (!register_value->present || register_value->bar != resource->index ||
		    register_value->space != space)
			continue;
		if (!vmmfs_pcislot_config_overlap(offset, width,
		    register_value->offset, register_value->width))
			continue;
		if (offset != register_value->offset || width != register_value->width)
			return (EINVAL);
		*match_offset = offset;
		return (0);
	}
	return (ENOENT);
}

static bool
vmmfs_pcislot_config_overlap(uint64_t left_base, uint64_t left_size,
	uint64_t right_base, uint64_t right_size)
{
	return left_base < right_base + right_size &&
	    right_base < left_base + left_size;
}

static uint64_t
vmmfs_pcislot_config_absent_value(enum vmm_io_width width)
{
	switch (width) {
	case VMM_IO_WIDTH_8:
		return (UINT8_MAX);
	case VMM_IO_WIDTH_16:
		return (UINT16_MAX);
	case VMM_IO_WIDTH_32:
		return (UINT32_MAX);
	case VMM_IO_WIDTH_64:
		return (UINT64_MAX);
	default:
		return (UINT64_MAX);
	}
}
