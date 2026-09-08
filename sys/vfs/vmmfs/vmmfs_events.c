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
#include <sys/thread2.h>
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

static int vmmfs_events_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_events_read(struct vop_read_args *);
static int vmmfs_events_store(struct vmmfs_node *, const char *, size_t);
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

static bool
vmmfs_events_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_events *events = (struct vmmfs_events *)node;

	lwkt_gettoken(&events->token);
	events->closed = true;
	lwkt_reltoken(&events->token);
	wakeup(events);
	KNOTE(&events->kq.ki_note, 0);
	return (true);
}

struct vop_ops vmmfs_events_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_kqfilter = vmmfs_events_kqfilter,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_events_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_setattr = vmmfs_node_setattr,
	.vop_write = vmmfs_node_write,
};

int
vmmfs_events_init(struct vmmfs_node *parent,
	struct vmmfs_events *events)
{
	struct vmmfs_root *root;
	int error;

	if (parent == NULL || events == NULL)
		return (EINVAL);
	root = (struct vmmfs_root *)parent->mount->root;
	bzero(events, sizeof(*events));
	lwkt_token_init(&events->token, "vmmfsevents");
	events->node.parent = parent;
	events->node.mount = parent->mount;
	events->node.dead = false;
	events->node.references = 1;
	lockinit(&events->node.lock, "vmmfsnode", 0, 0);
	events->node.drop = vmmfs_events_drop;
	vmmfs_node_hold(parent);
	events->node.load_limit = 0;
	events->node.store_limit = sizeof("reset\n") - 1;
	events->node.load = NULL;
	events->node.store = vmmfs_events_store;
	SLIST_INIT(&events->kq.ki_note);
	events->buffer = kmalloc(VMMFS_EVENTS_BUFFER_SIZE, M_VMMFS,
	    M_WAITOK | M_ZERO);
	events->node.inode = vmmfs_root_allocate_inode(root);
	events->node.mode = VMMFS_EVENTS_MODE;
	events->node.size = 0;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->events_vops, VREG, &events->node);
	if (error == 0) {
		events->node.deactivate = vmmfs_events_deactivate;
		return (0);
	}

	vmmfs_node_put(&events->node);
	return (error);
}

static void
vmmfs_events_drop(struct vmmfs_node *node)
{
	struct vmmfs_events *events = (struct vmmfs_events *)node;

	kfree(events->buffer, M_VMMFS);
	events->buffer = NULL;
	events->node.inode = 0;
	lwkt_token_uninit(&events->token);
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
	if (events->closed) {
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
vmmfs_events_subscribe(struct vmmfs_events *events, struct knote *knote)
{

	if (knote->kn_filter != EVFILT_READ)
		return (EOPNOTSUPP);
	lwkt_gettoken(&events->token);
	if (events->closed) {
		lwkt_reltoken(&events->token);
		return (ENXIO);
	}
	knote->kn_fop = &vmmfs_events_read_filterops;
	knote->kn_hook = (caddr_t)events;
	knote_insert(&events->kq.ki_note, knote);
	lwkt_reltoken(&events->token);
	return (0);
}

static int
vmmfs_events_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_events *events = ap->a_vp->v_data;

	return (VMMFS_WORK(events, vmmfs_events_subscribe(events, ap->a_kn)));
}

static int
vmmfs_events_read_data(struct vmmfs_events *events, char *buffer,
	size_t capacity, size_t *lengthp)
{
	size_t length, index, i;

	lwkt_gettoken(&events->token);
	if (events->closed) {
		lwkt_reltoken(&events->token);
		return (ENXIO);
	}
	length = MIN(events->length, capacity);
	if (length == 0) {
		tsleep_interlock(events, PCATCH);
		lwkt_reltoken(&events->token);
		return (EAGAIN);
	}
	for (i = 0; i < length; ++i) {
		index = (events->start + i) % VMMFS_EVENTS_BUFFER_SIZE;
		buffer[i] = events->buffer[index];
	}
	events->start = (events->start + length) % VMMFS_EVENTS_BUFFER_SIZE;
	events->length -= length;
	lwkt_reltoken(&events->token);
	*lengthp = length;
	return (0);
}

static int
vmmfs_events_read(struct vop_read_args *ap)
{
	struct vmmfs_events *events = ap->a_vp->v_data;
	struct uio *uio = ap->a_uio;
	char buffer[VMMFS_EVENTS_READ_SIZE];
	size_t length;
	int error;

	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);
	for (;;) {
		error = VMMFS_WORK(events, vmmfs_events_read_data(events,
		    buffer, MIN(sizeof(buffer), (size_t)uio->uio_resid), &length));
		if (error != EAGAIN)
			break;
		if (ap->a_ioflag & IO_NDELAY) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			return (EAGAIN);
		}
		/* The work step armed the sleep before releasing its data token. */
		error = tsleep(events, PINTERLOCKED | PCATCH, "vmmevents", 0);
		if (error != 0)
			return (error);
	}
	return (error != 0 ? error : uiomove(buffer, length, uio));
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
	knote->kn_data = events->length;
	if (events->closed)
		knote->kn_flags |= EV_EOF;
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
vmmfs_events_store(struct vmmfs_node *node, const char *buffer,
	size_t length)
{
	struct vmmfs_events *events;

	events = (struct vmmfs_events *)node;
	if (events == NULL)
		return (ENOENT);
	if (!((length == sizeof("reset") - 1 &&
	    bcmp(buffer, "reset", length) == 0) ||
	    (length == sizeof("reset\n") - 1 &&
	    bcmp(buffer, "reset\n", length) == 0)))
		return (EINVAL);
	return (VMMFS_WORK(vmmfs_events_machine(events),
	    ({
		vmmfs_events_log(events, VMMFS_MACHINE_EVENT_RESET_REQUESTED,
		    "reason=external");
		vmmfs_vcpu_request_reset(&vmmfs_events_machine(events)->vcpu);
		0;
	    })));
}
