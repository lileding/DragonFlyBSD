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
#include <sys/thread2.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <vm/vm.h>

#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_pciroot.h"
#include "vmmfs_parent.h"
#include "vmmfs_machine.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_auth.h"
#include "vmmfs_pcislot_config.h"

struct vmmfs_pcislot_config_request {
	TAILQ_ENTRY(vmmfs_pcislot_config_request) entry;
	vmm_vcpu_t vcpu;
	struct vmmfs_pci_config_request request;
	uint64_t response_value;
	uint32_t response_status;
	bool delivered;
	bool completed;
};
#include "vmmfs_pcislot_resource.h"
#include "vmmfs_vcpu.h"

#define VMMFS_PCISLOT_CONFIG_MODE 0600

static int vmmfs_pcislot_config_close(struct vop_close_args *);
static int vmmfs_pcislot_config_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_pcislot_config_open(struct vop_open_args *);
static int vmmfs_pcislot_config_read(struct vop_read_args *);
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

static bool
vmmfs_pcislot_config_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_config *config = (struct vmmfs_pcislot_config *)node;

	lwkt_gettoken(&config->token);
	config->closed = true;
	config->powered = false;
	config->responder = NULL;
	config->opening = false;
	vmmfs_pcislot_config_cancel_locked(config, VMMFS_PCI_CONFIG_FAILURE);
	lwkt_reltoken(&config->token);
	wakeup(config);
	KNOTE(&config->kq.ki_note, 0);
	return (true);
}

struct vop_ops vmmfs_pcislot_config_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vmmfs_pcislot_config_close,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_kqfilter = vmmfs_pcislot_config_kqfilter,
	.vop_open = vmmfs_pcislot_config_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_config_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_write = vmmfs_pcislot_config_write,
};

int
vmmfs_pcislot_config_init(struct vmmfs_node *parent,
	struct vmmfs_pcislot_config *config)
{
	struct vmmfs_root *root;
	int error;

	if (parent == NULL || config == NULL)
		return (EINVAL);
	root = (struct vmmfs_root *)parent->mount->root;
	bzero(config, sizeof(*config));
	config->node.inode = vmmfs_root_allocate_inode(root);
	lwkt_token_init(&config->token, "vmmfspcicfg");
	TAILQ_INIT(&config->requests);
	SLIST_INIT(&config->kq.ki_note);
	config->node.parent = parent;
	config->node.mount = parent->mount;
	config->node.dead = false;
	config->node.references = 1;
	lockinit(&config->node.lock, "vmmfsnode", 0, 0);
	config->node.drop = vmmfs_pcislot_config_drop;
	vmmfs_node_hold(parent);
	config->node.mode = VMMFS_PCISLOT_CONFIG_MODE;
	config->node.size = 0;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->pcislot_config_vops, VREG, &config->node);
	if (error != 0)
		vmmfs_node_put(&config->node);
	else
		config->node.deactivate = vmmfs_pcislot_config_deactivate;
	return (error);
}

static void
vmmfs_pcislot_config_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_config *config;

	config = (struct vmmfs_pcislot_config *)node;
	KKASSERT(config != NULL);
	KKASSERT(TAILQ_EMPTY(&config->requests));
	config->node.inode = 0;
	lwkt_token_uninit(&config->token);

}

void
vmmfs_pcislot_config_descriptor_changed(struct vmmfs_pcislot_config *config,
	uint64_t generation)
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
	uint64_t gpa;
	bool write;
	int error;

	if (config == NULL || thread == NULL || thread->vcpu == NULL ||
	    resource == NULL || exit == NULL ||
	    exit->reason != VMM_CPUEXIT_MEMORY)
		return (ENOENT);
	write = (exit->u.mem.prot & VM_PROT_WRITE) != 0;
	error = vmmfs_pcislot_resource_gpa(resource, &gpa);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_config_match(config, resource,
	    VMMFS_PCI_CONFIG_MMIO, exit->u.mem.gpa - gpa,
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
	uint64_t gpa;
	bool write;
	int error;

	if (config == NULL || thread == NULL || thread->vcpu == NULL ||
	    resource == NULL || state == NULL || exit == NULL ||
	    exit->reason != VMM_CPUEXIT_IO || exit->u.io.str || exit->u.io.rep)
		return (ENOENT);
	if (exit->u.io.operand_size != 1 && exit->u.io.operand_size != 2 &&
	    exit->u.io.operand_size != 4)
		return (EOPNOTSUPP);
	error = vmmfs_pcislot_resource_gpa(resource, &gpa);
	if (error != 0)
		return (error);
	error = vmmfs_pcislot_config_match(config, resource,
	    VMMFS_PCI_CONFIG_PIO, exit->u.io.port - gpa,
	    (enum vmm_io_width)exit->u.io.operand_size, &offset);
	write = !exit->u.io.in;
	if (error == EINVAL) {
		if (!write) {
			mask = (1ULL << (exit->u.io.operand_size * NBBY)) - 1;
			value = vmmfs_pcislot_config_absent_value(
			    (enum vmm_io_width)exit->u.io.operand_size);
			/* An IN to EAX zero-extends; AL/AX preserve the upper bits. */
			state->gprs[VMM_X64_GPR_RAX] =
			    exit->u.io.operand_size == 4 ? (uint32_t)value :
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
		/* An IN to EAX zero-extends; AL/AX preserve the upper bits. */
		state->gprs[VMM_X64_GPR_RAX] =
		    exit->u.io.operand_size == 4 ? (uint32_t)value :
		    (state->gprs[VMM_X64_GPR_RAX] & ~mask) | (value & mask);
	}
	state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
	return (0);
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
vmmfs_pcislot_config_subscribe(struct vmmfs_pcislot_config *config,
	struct knote *knote)
{
	int error = 0;

	if (config == NULL)
		return (ENOENT);
	switch (knote->kn_filter) {
	case EVFILT_READ:
		knote->kn_fop = &vmmfs_pcislot_config_read_filterops;
		break;
	case EVFILT_WRITE:
		knote->kn_fop = &vmmfs_pcislot_config_write_filterops;
		break;
	default:
		return (EOPNOTSUPP);
	}
	lwkt_gettoken(&config->token);
	if (config->closed)
		error = ENOENT;
	else {
		knote->kn_hook = (caddr_t)config;
		knote_insert(&config->kq.ki_note, knote);
	}
	lwkt_reltoken(&config->token);
	return (error);
}

static int
vmmfs_pcislot_config_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_pcislot_config *config = ap->a_vp->v_data;

	return (VMMFS_WORK(config,
	    vmmfs_pcislot_config_subscribe(config, ap->a_kn)));
}

static int
vmmfs_pcislot_config_claim(struct vmmfs_pcislot_config *config)
{
	struct vmmfs_pcislot *slot;
	int error;
	slot = vmmfs_pcislot_config_slot(config);
	lwkt_gettoken(&slot->token);
	error = vmmfs_pcislot_auth_check(slot);
	lwkt_reltoken(&slot->token);
	lwkt_gettoken(&config->token);
	if (error != 0)
		error = EACCES;
	else if (config->closed)
		error = ENOENT;
	else if (config->opening || config->responder != NULL)
		error = EBUSY;
	else
		config->opening = true;
	lwkt_reltoken(&config->token);
	if (error != 0)
		return (error);

	return (0);
}

static int
vmmfs_pcislot_config_finish_open(struct vmmfs_pcislot_config *config,
	struct file *file, int error)
{
	lwkt_gettoken(&config->token);
	config->opening = false;
	if (error == 0) {
		if (config->closed)
			error = ENOENT;
		else if (file == NULL)
			error = EIO;
		else
			config->responder = file;
	}
	lwkt_reltoken(&config->token);
	return (error);
}

static int
vmmfs_pcislot_config_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_config *config = ap->a_vp->v_data;
	int error;

	if (config == NULL)
		return (ENOENT);
	if ((ap->a_mode & (FREAD | FWRITE)) != (FREAD | FWRITE))
		return (EINVAL);
	error = VMMFS_WORK(config, vmmfs_pcislot_config_claim(config));
	if (error != 0)
		return (error);
	error = vop_stdopen(ap);
	return (vmmfs_pcislot_config_finish_open(config,
	    ap->a_fpp == NULL ? NULL : *ap->a_fpp, error));
}

static int
vmmfs_pcislot_config_receive(struct vmmfs_pcislot_config *config,
	struct file *file, struct vmmfs_pci_config_request *record)
{
	struct vmmfs_pcislot_config_request *request;
	int error = 0;

	lwkt_gettoken(&config->token);
	if (config->closed)
		error = ENXIO;
	else if (config->responder != file)
		error = EBADF;
	else {
		request = TAILQ_FIRST(&config->requests);
		if (request != NULL && !request->delivered) {
			*record = request->request;
			request->delivered = true;
		} else {
			tsleep_interlock(config, PCATCH);
			error = EAGAIN;
		}
	}
	lwkt_reltoken(&config->token);
	if (error == 0)
		KNOTE(&config->kq.ki_note, 0);
	return (error);
}

/* Completion of an admitted copyout: do not reopen admission on failure. */
static void
vmmfs_pcislot_config_redeliver(struct vmmfs_pcislot_config *config,
	struct file *file, const struct vmmfs_pci_config_request *record)
{
	struct vmmfs_pcislot_config_request *request;
	bool retry;

	/* The worker may have removed this request while copyout slept. */
	lwkt_gettoken(&config->token);
	request = TAILQ_FIRST(&config->requests);
	retry = !config->closed &&
	    config->responder == file && request != NULL &&
	    !request->completed &&
	    request->request.generation == record->generation &&
	    request->request.sequence == record->sequence;
	if (retry)
		request->delivered = false;
	lwkt_reltoken(&config->token);
	if (retry)
		vmmfs_pcislot_config_wake_next(config);
}

static int
vmmfs_pcislot_config_read(struct vop_read_args *ap)
{
	struct vmmfs_pcislot_config *config = ap->a_vp->v_data;
	struct vmmfs_pci_config_request record;
	int error;

	if (ap->a_uio->uio_resid != sizeof(record))
		return (EINVAL);
	for (;;) {
		error = VMMFS_WORK(config,
		    vmmfs_pcislot_config_receive(config, ap->a_fp, &record));
		if (error != EAGAIN)
			break;
		if ((ap->a_ioflag & IO_NDELAY) != 0) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			return (EAGAIN);
		}
		error = tsleep(config, PINTERLOCKED | PCATCH, "vmmfspcicfg", 0);
		if (error != 0)
			return (error);
	}
	if (error != 0)
		return (error);
	error = uiomove((caddr_t)&record, sizeof(record), ap->a_uio);
	if (error != 0)
		vmmfs_pcislot_config_redeliver(config, ap->a_fp, &record);
	return (error);
}

static int
vmmfs_pcislot_config_respond(struct vmmfs_pcislot_config *config,
	struct file *file, const struct vmmfs_pci_config_response *response)
{
	struct vmmfs_pcislot_config_request *request;
	vmm_vcpu_t vcpu = NULL;
	int error = 0;

	lwkt_gettoken(&config->token);
	request = TAILQ_FIRST(&config->requests);
	if (config->closed)
		error = ENXIO;
	else if (config->responder != file)
		error = EBADF;
	else if (request == NULL || !request->delivered || request->completed ||
	    request->request.generation != response->generation ||
	    request->request.sequence != response->sequence)
		error = EINVAL;
	else {
		request->response_value = response->value;
		request->response_status = response->status;
		request->completed = true;
		vcpu = request->vcpu;
	}
	lwkt_reltoken(&config->token);
	if (error == 0) {
		KNOTE(&config->kq.ki_note, 0);
		/* The channel is only an address, not a borrowed vCPU object. */
		wakeup(vcpu);
	}
	return (error);
}

static int
vmmfs_pcislot_config_write(struct vop_write_args *ap)
{
	struct vmmfs_pcislot_config *config = ap->a_vp->v_data;
	struct vmmfs_pci_config_response response;
	int error;

	if (config == NULL)
		return (ENOENT);
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
	return (VMMFS_WORK(config,
	    vmmfs_pcislot_config_respond(config, ap->a_fp, &response)));
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

	TAILQ_FOREACH(request, &config->requests, entry) {
		request->response_status = status;
		request->response_value = 0;
		request->completed = true;
		wakeup(request->vcpu);
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
	uint16_t resource_index;
	int error;

	if (config == NULL || thread == NULL || thread->group == NULL ||
	    thread->vcpu == NULL || resource == NULL || response == NULL)
		return (EINVAL);
	error = vmmfs_pcislot_resource_index(resource, &resource_index);
	if (error != 0)
		return (error);
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
	request->vcpu = thread->vcpu;
	request->request.generation = config->generation;
	request->request.sequence = ++config->next_sequence;
	request->request.offset = offset;
	request->request.value = value;
	request->request.bar = resource_index;
	request->request.space = space;
	request->request.width = width;
	request->request.operation = operation;
	notify = TAILQ_EMPTY(&config->requests);
	TAILQ_INSERT_TAIL(&config->requests, request, entry);
	lwkt_reltoken(&config->token);
	if (notify)
		vmmfs_pcislot_config_wake_next(config);

	error = 0;
	for (;;) {
		lwkt_gettoken(&thread->group->token);
		lwkt_gettoken(&config->token);
		stop_requested = thread->group->stop_requested;
		reset_requested = thread->group->reset_requested;
		if (stop_requested || reset_requested || request->completed ||
		    error != 0)
			break;
		tsleep_interlock(thread->vcpu, 0);
		lwkt_reltoken(&config->token);
		lwkt_reltoken(&thread->group->token);
		error = tsleep(thread->vcpu, PINTERLOCKED, "vmmfspcicfg", 0);
	}

	/* This worker alone removes and frees its request. */
	response->generation = request->request.generation;
	response->sequence = request->request.sequence;
	response->value = request->response_value;
	response->status = request->completed ? request->response_status :
	    VMMFS_PCI_CONFIG_FAILURE;
	response->reserved = 0;
	TAILQ_REMOVE(&config->requests, request, entry);
	lwkt_reltoken(&config->token);
	lwkt_reltoken(&thread->group->token);
	kfree(request, M_VMMFS);
	vmmfs_pcislot_config_wake_next(config);
	/* Reset discards guest state; return the failure response to its barrier. */
	return (error != 0 ? error : (stop_requested ? EINTR : 0));
}

static int
vmmfs_pcislot_config_match(struct vmmfs_pcislot_config *config,
	struct vmmfs_pcislot_resource *resource, uint8_t space,
	uint64_t offset, enum vmm_io_width width, uint64_t *match_offset)
{
	const struct vmmfs_pcislot_config_register *register_value;
	const struct vmmfs_pcislot_descriptor_value *value;
	unsigned int index;
	uint16_t resource_index;
	int error;

	if (config == NULL || resource == NULL || match_offset == NULL ||
	    (width != VMM_IO_WIDTH_8 && width != VMM_IO_WIDTH_16 &&
	    width != VMM_IO_WIDTH_32 && width != VMM_IO_WIDTH_64))
		return (EINVAL);
	error = vmmfs_pcislot_resource_index(resource, &resource_index);
	if (error != 0)
		return (error);
	value = &vmmfs_pcislot_config_slot(config)->descriptor.value;
	for (index = 0; index < VMMFS_PCISLOT_MAX_CONFIGS; ++index) {
		register_value = &value->configs[index];
		if (!register_value->present || register_value->bar != resource_index ||
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
