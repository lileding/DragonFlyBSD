/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine event stream.
 */
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/event.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/stdarg.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/cpufunc.h>
#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_parent.h"
#include "vmmfs_machine.h"
#include "vmmfs_events.h"

#define VMMFS_EVENTS_MODE 0644
#define VMMFS_EVENTS_LINE_SIZE 512
#define VMMFS_EVENTS_READ_SIZE 256

static int vmmfs_events_access(struct vop_access_args *);
static int vmmfs_events_getattr(struct vop_getattr_args *);
static int vmmfs_events_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_events_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_events_read(struct vop_read_args *);
static int vmmfs_events_inactive(struct vop_inactive_args *);
static int vmmfs_events_setattr(struct vop_setattr_args *);
static int vmmfs_events_write(struct vop_write_args *);
static void vmmfs_events_filter_detach(struct knote *);
static int vmmfs_events_filter_read(struct knote *, long);
static const char *vmmfs_machine_event_name(enum vmmfs_machine_event);
static void vmmfs_events_drop(struct vmmfs_node *);

static struct filterops vmmfs_events_read_filterops = {
	FILTEROP_ISFD | FILTEROP_MPSAFE,
	NULL,
	vmmfs_events_filter_detach,
	vmmfs_events_filter_read,
};

struct vop_ops vmmfs_events_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_events_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_events_getattr,
	.vop_getattr_lite = vmmfs_events_getattr_lite,
	.vop_kqfilter = vmmfs_events_kqfilter,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_events_read,
	.vop_inactive = vmmfs_events_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_setattr = vmmfs_events_setattr,
	.vop_write = vmmfs_events_write,
};

int
vmmfs_events_init(struct vmmfs_machine *machine, struct vmmfs_events *events,
	struct vnode **vnodep)
{
	struct vmmfs_mount *state;
	int error;

	if (machine == NULL || events == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	bzero(events, sizeof(*events));
	lwkt_token_init(&events->token, "vmmfsevents");
	vmmfs_node_setup(&events->node, &machine->branch, vmmfs_events_drop);
	SLIST_INIT(&events->kq.ki_note);
	events->buffer = kmalloc(VMMFS_EVENTS_BUFFER_SIZE, M_VMMFS,
	    M_WAITOK | M_ZERO);
	state = vmmfs_root_state(vmmfs_machine_root(machine));
	events->inode = atomic_fetchadd_int(&state->next_inode, 1);
	if (state->events_vops == NULL) {
		error = ENXIO;
		goto fail;
	}
	error = vmmfs_vnode_create_regular(state->mount,
	    &state->events_vops, VREG, &events->node, vnodep);
	if (error == 0)
		return (0);

fail:
	vmmfs_node_drop(&events->node);
	return (error);
}

static void
vmmfs_events_drop(struct vmmfs_node *node)
{
	struct vmmfs_events *events;

	events = (struct vmmfs_events *)node;
	KKASSERT(events != NULL);
	lwkt_gettoken(&events->token);
	lwkt_reltoken(&events->token);
	vmmfs_events_revoke(events);
	kfree(events->buffer, M_VMMFS);
	events->buffer = NULL;
	events->inode = 0;
	lwkt_token_uninit(&events->token);
	vmmfs_node_parent_put(node);
}


void
vmmfs_events_revoke(struct vmmfs_events *events)
{
	if (events == NULL)
		return;
	lwkt_gettoken(&events->token);
	events->closed = true;
	lwkt_reltoken(&events->token);
	wakeup(events);
	KNOTE(&events->kq.ki_note, 0);
}

void
vmmfs_events_log(struct vmmfs_events *events, enum vmmfs_machine_event event,
	const char *args, ...)
{
	va_list ap;
	const char *name;
	char message[VMMFS_EVENTS_LINE_SIZE];
	char text[VMMFS_EVENTS_LINE_SIZE];
	char line[VMMFS_EVENTS_LINE_SIZE];
	uint64_t tsc;
	size_t index;
	size_t length;
	size_t i;
	int result;

	if (events == NULL)
		return;
	name = vmmfs_machine_event_name(event);
	if (name == NULL)
		return;
	if (args == NULL) {
		result = ksnprintf(message, sizeof(message), "%s", name);
	} else {
		va_start(ap, args);
		result = kvsnprintf(text, sizeof(text), args, ap);
		va_end(ap);
		if (result >= 0)
			result = ksnprintf(message, sizeof(message), "%s %s", name,
			    text);
	}
	if (result < 0)
		return;
	length = strnlen(message, sizeof(text) - 1);
	for (i = 0; i < length; ++i) {
		if (message[i] == '\n' || message[i] == '\r')
			text[i] = ' ';
		else
			text[i] = message[i];
	}
	text[length] = '\0';

	lwkt_gettoken(&events->token);
	if (events->closed || events->buffer == NULL) {
		lwkt_reltoken(&events->token);
		return;
	}
	tsc = rdtsc();
	if (tsc <= events->last_tsc)
		tsc = events->last_tsc + 1;
	events->last_tsc = tsc;
	++events->sequence;
	result = ksnprintf(line, sizeof(line), "%010ju %016jx %s\n",
	    (uintmax_t)events->sequence, (uintmax_t)tsc, text);
	if (result < 0) {
		lwkt_reltoken(&events->token);
		return;
	}
	length = (size_t)result;
	if (length >= sizeof(line))
		length = sizeof(line) - 1;
	for (i = 0; i < length; ++i) {
		if (events->length < VMMFS_EVENTS_BUFFER_SIZE) {
			index = (events->start + events->length) %
			    VMMFS_EVENTS_BUFFER_SIZE;
			++events->length;
		} else {
			index = events->start;
			events->start = (events->start + 1) %
			    VMMFS_EVENTS_BUFFER_SIZE;
		}
		events->buffer[index] = line[i];
	}
	lwkt_reltoken(&events->token);
	wakeup(events);
	KNOTE(&events->kq.ki_note, 0);
}

static const char *
vmmfs_machine_event_name(enum vmmfs_machine_event event)
{
	switch (event) {
	case VMMFS_MACHINE_EVENT_CREATED:
		return ("machine created");
	case VMMFS_MACHINE_EVENT_DESTROY_REFUSED:
		return ("machine destroy refused");
	case VMMFS_MACHINE_EVENT_STOP_REQUESTED:
		return ("machine stop requested");
	case VMMFS_MACHINE_EVENT_STOPPED:
		return ("machine stopped");
	case VMMFS_MACHINE_EVENT_RESET_REQUESTED:
		return ("machine reset requested");
	case VMMFS_MACHINE_EVENT_RESET_STARTED:
		return ("machine reset started");
	case VMMFS_MACHINE_EVENT_RESET_COMPLETED:
		return ("machine reset completed");
	case VMMFS_MACHINE_EVENT_RESET_FAILED:
		return ("machine reset failed");
	case VMMFS_MACHINE_EVENT_START_REQUESTED:
		return ("machine start requested");
	case VMMFS_MACHINE_EVENT_START_COMPLETED:
		return ("machine start completed");
	case VMMFS_MACHINE_EVENT_START_FAILED:
		return ("machine start failed");
	case VMMFS_MACHINE_EVENT_BOOT_REQUESTED:
		return ("machine boot requested");
	case VMMFS_MACHINE_EVENT_BOOT_READY:
		return ("machine boot ready");
	case VMMFS_MACHINE_EVENT_BOOT_COMPLETED:
		return ("machine boot completed");
	case VMMFS_MACHINE_EVENT_BOOT_FAILED:
		return ("machine boot failed");
	case VMMFS_MACHINE_EVENT_LOADER_SUBMITTED_CPUSTATE:
		return ("machine loader submitted cpustate");
	case VMMFS_MACHINE_EVENT_LOADER_FAILED:
		return ("machine loader failed");
	case VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED:
		return ("machine serial create failed");
	case VMMFS_MACHINE_EVENT_PCI_CREATE_FAILED:
		return ("machine pci create failed");
	case VMMFS_MACHINE_EVENT_GUEST_STOP_REQUEST_FAILED:
		return ("machine guest stop request failed");
	case VMMFS_MACHINE_EVENT_GUEST_RESET_REQUEST_FAILED:
		return ("machine guest reset request failed");
	case VMMFS_MACHINE_EVENT_VCPU_INJECT_GP_FAILED:
		return ("machine vcpu inject-gp failed");
	case VMMFS_MACHINE_EVENT_VCPU_HALTED:
		return ("machine vcpu halted");
	case VMMFS_MACHINE_EVENT_VCPU_SHUTDOWN:
		return ("machine vcpu shutdown");
	case VMMFS_MACHINE_EVENT_VCPU_UNSUPPORTED_EXIT:
		return ("machine vcpu unsupported exit");
	case VMMFS_MACHINE_EVENT_VCPU_FAILED:
		return ("machine vcpu failed");
	}
	return (NULL);
}

static int
vmmfs_events_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_EVENTS_MODE, 0));
}

static int
vmmfs_events_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_events *events;
	struct vattr *vattr;

	events = ap->a_vp->v_data;
	if (events == NULL)
		return (ENOENT);
	if (events->node.dead)
		return (ENXIO);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_EVENTS_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = events->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

static int
vmmfs_events_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_EVENTS_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_events_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_events *events;

	events = ap->a_vp->v_data;
	if (events == NULL)
		return (ENOENT);
	if (ap->a_kn->kn_filter != EVFILT_READ)
		return (EOPNOTSUPP);
	lwkt_gettoken(&events->token);
	ap->a_kn->kn_fop = &vmmfs_events_read_filterops;
	ap->a_kn->kn_hook = (caddr_t)events;
	knote_insert(&events->kq.ki_note, ap->a_kn);
	lwkt_reltoken(&events->token);
	return (0);
}

static int
vmmfs_events_read(struct vop_read_args *ap)
{
	struct vmmfs_events *events;
	struct uio *uio;
	char buffer[VMMFS_EVENTS_READ_SIZE];
	size_t index;
	size_t length;
	size_t i;
	int error;

	events = ap->a_vp->v_data;
	if (events == NULL)
		return (ENOENT);
	if (events->node.dead)
		return (ENXIO);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);

	for (;;) {
		lwkt_gettoken(&events->token);
		if (events->length != 0)
			break;
		if (events->closed) {
			lwkt_reltoken(&events->token);
			return (ENXIO);
		}
		if (ap->a_ioflag & IO_NDELAY) {
			lwkt_reltoken(&events->token);
			return (EAGAIN);
		}
		tsleep_interlock(events, PCATCH);
		lwkt_reltoken(&events->token);
		error = tsleep(events, PINTERLOCKED | PCATCH, "vmmevents", 0);
		if (error != 0)
			return (error);
	}
	length = events->length;
	if (length > sizeof(buffer))
		length = sizeof(buffer);
	if (length > (size_t)uio->uio_resid)
		length = (size_t)uio->uio_resid;
	for (i = 0; i < length; ++i) {
		index = (events->start + i) % VMMFS_EVENTS_BUFFER_SIZE;
		buffer[i] = events->buffer[index];
	}
	events->start = (events->start + length) % VMMFS_EVENTS_BUFFER_SIZE;
	events->length -= length;
	lwkt_reltoken(&events->token);
	return (uiomove(buffer, length, uio));
}

static int
vmmfs_events_filter_read(struct knote *knote, long hint)
{
	struct vmmfs_events *events;

	(void)hint;
	events = (struct vmmfs_events *)knote->kn_hook;
	if (events == NULL)
		return (0);
	lwkt_gettoken(&events->token);
	if (vmmfs_events_machine(events) == NULL || events->node.dead) {
		knote->kn_data = 0;
		knote->kn_flags |= EV_EOF;
	} else {
		knote->kn_data = events->length;
		if (events->closed)
			knote->kn_flags |= EV_EOF;
	}
	lwkt_reltoken(&events->token);
	return (knote->kn_data != 0 || (knote->kn_flags & EV_EOF) != 0);
}

static void
vmmfs_events_filter_detach(struct knote *knote)
{
	struct vmmfs_events *events;

	events = (struct vmmfs_events *)knote->kn_hook;
	if (events == NULL)
		return;
	lwkt_gettoken(&events->token);
	knote_remove(&events->kq.ki_note, knote);
	lwkt_reltoken(&events->token);
}

static int
vmmfs_events_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_events *events;
	struct vmmfs_machine *machine;

	events = ap->a_vp->v_data;
	if (events == NULL)
		return (0);
	machine = vmmfs_events_machine(events);
	if (!events->node.dead)
		return (0);
	vmmfs_node_inactive(&events->node, ap->a_vp);
	return (0);
}


static int
vmmfs_events_setattr(struct vop_setattr_args *ap)
{
	/* Accept the O_TRUNC size update performed before a control write. */
	(void)ap;
	return (0);
}

static int
vmmfs_events_write(struct vop_write_args *ap)
{
	struct vmmfs_events *events;
	struct uio *uio;
	char buffer[32];
	size_t length;
	int error;

	events = ap->a_vp->v_data;
	if (events == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset != 0 || uio->uio_resid == 0)
		return (EINVAL);
	length = (size_t)uio->uio_resid;
	if (length >= sizeof(buffer))
		return (E2BIG);
	error = uiomove(buffer, length, uio);
	if (error != 0)
		return (error);
	buffer[length] = '\0';
	if (strcmp(buffer, "reset") != 0 && strcmp(buffer, "reset\n") != 0)
		return (EINVAL);
	return (vmmfs_machine_reset(vmmfs_events_machine(events)));
}
