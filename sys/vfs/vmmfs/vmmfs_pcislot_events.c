/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot events stream.
 */
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/event.h>
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

#include <machine/cpufunc.h>

#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_pciroot.h"
#include "vmmfs_parent.h"
#include "vmmfs_machine.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_auth.h"
#include "vmmfs_pcislot_events.h"

#define VMMFS_PCISLOT_EVENTS_MODE 0444
#define VMMFS_PCISLOT_EVENTS_LINE_SIZE 512
#define VMMFS_PCISLOT_EVENTS_READ_SIZE 256

static int vmmfs_pcislot_events_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_pcislot_events_open(struct vop_open_args *);
static int vmmfs_pcislot_events_read(struct vop_read_args *);
static void vmmfs_pcislot_events_filter_detach(struct knote *);
static int vmmfs_pcislot_events_filter_read(struct knote *, long);
static const char *vmmfs_pci_event_name(enum vmmfs_pci_event);
static void vmmfs_pcislot_events_drop(struct vmmfs_node *);

static struct filterops vmmfs_pcislot_events_read_filterops = {
	FILTEROP_ISFD | FILTEROP_MPSAFE,
	NULL,
	vmmfs_pcislot_events_filter_detach,
	vmmfs_pcislot_events_filter_read,
};

static int
vmmfs_pcislot_events_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_events *state_node = (struct vmmfs_pcislot_events *)node;

	lwkt_gettoken(&state_node->token);
	state_node->closed = true;
	lwkt_reltoken(&state_node->token);
	wakeup(state_node);
	KNOTE(&state_node->kq.ki_note, 0);
	return (0);
}

struct vop_ops vmmfs_pcislot_events_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_kqfilter = vmmfs_pcislot_events_kqfilter,
	.vop_open = vmmfs_pcislot_events_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_events_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

int
vmmfs_pcislot_events_init(struct vmmfs_node *parent,
	struct vmmfs_pcislot_events *state_node, struct vnode **vnodep)
{
	struct vmmfs_root *root;
	int error;

	if (parent == NULL || state_node == NULL || vnodep == NULL)
		return (EINVAL);
	root = parent->mount->root_vnode->v_data;
	*vnodep = NULL;
	bzero(state_node, sizeof(*state_node));
	lwkt_token_init(&state_node->token, "vmmfspcievents");
	SLIST_INIT(&state_node->kq.ki_note);
	state_node->buffer = kmalloc(VMMFS_PCISLOT_EVENTS_BUFFER_SIZE, M_VMMFS,
	    M_WAITOK | M_ZERO);
	state_node->node.inode = vmmfs_root_allocate_inode(root);
	state_node->node.parent = parent;
	state_node->node.mount = parent->mount;
	state_node->node.dead = false;
	state_node->node.references = 1;
	lwkt_token_init(&state_node->node.token, "vmmfsnode");
	state_node->node.deactivate = vmmfs_pcislot_events_deactivate;
	state_node->node.drop = vmmfs_pcislot_events_drop;
	vmmfs_node_hold(parent);
	state_node->node.mode = VMMFS_PCISLOT_EVENTS_MODE;
	state_node->node.size = 0;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->pcislot_events_vops, VREG, &state_node->node,
	    vnodep);
	if (error != 0)
		vmmfs_node_put(&state_node->node);
	return (error);
}

static void
vmmfs_pcislot_events_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_events *state_node;

	state_node = (struct vmmfs_pcislot_events *)node;
	KKASSERT(state_node != NULL);
	kfree(state_node->buffer, M_VMMFS);
	state_node->buffer = NULL;
	state_node->node.inode = 0;
	lwkt_token_uninit(&state_node->token);

}



void
vmmfs_pcislot_events_reset(struct vmmfs_pcislot_events *state_node)
{
	if (state_node == NULL)
		return;
	lwkt_gettoken(&state_node->token);
	if (!state_node->closed && state_node->buffer != NULL) {
		state_node->start = 0;
		state_node->length = 0;
	}
	lwkt_reltoken(&state_node->token);
}

void
vmmfs_pcislot_events_log(struct vmmfs_pcislot_events *state_node,
	enum vmmfs_pci_event event, const char *args, ...)
{
	va_list ap;
	const char *name;
	char message[VMMFS_PCISLOT_EVENTS_LINE_SIZE];
	char text[VMMFS_PCISLOT_EVENTS_LINE_SIZE];
	char line[VMMFS_PCISLOT_EVENTS_LINE_SIZE];
	uint64_t tsc;
	size_t index;
	size_t length;
	size_t i;
	int result;

	if (state_node == NULL)
		return;
	name = vmmfs_pci_event_name(event);
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
		text[i] = (message[i] == '\n' || message[i] == '\r') ? ' ' :
		    message[i];
	}
	text[length] = '\0';
	lwkt_gettoken(&state_node->token);
	if (state_node->closed || state_node->buffer == NULL) {
		lwkt_reltoken(&state_node->token);
		return;
	}
	tsc = rdtsc();
	if (tsc <= state_node->last_tsc)
		tsc = state_node->last_tsc + 1;
	state_node->last_tsc = tsc;
	++state_node->sequence;
	result = ksnprintf(line, sizeof(line), "%010ju %016jx %s\n",
	    (uintmax_t)state_node->sequence, (uintmax_t)tsc, text);
	if (result < 0) {
		lwkt_reltoken(&state_node->token);
		return;
	}
	length = (size_t)result;
	if (length >= sizeof(line))
		length = sizeof(line) - 1;
	for (i = 0; i < length; ++i) {
		if (state_node->length < VMMFS_PCISLOT_EVENTS_BUFFER_SIZE) {
			index = (state_node->start + state_node->length) %
			    VMMFS_PCISLOT_EVENTS_BUFFER_SIZE;
			++state_node->length;
		} else {
			index = state_node->start;
			state_node->start = (state_node->start + 1) %
			    VMMFS_PCISLOT_EVENTS_BUFFER_SIZE;
		}
		state_node->buffer[index] = line[i];
	}
	lwkt_reltoken(&state_node->token);
	wakeup(state_node);
	KNOTE(&state_node->kq.ki_note, 0);
}

static const char *
vmmfs_pci_event_name(enum vmmfs_pci_event event)
{
	switch (event) {
	case VMMFS_PCI_EVENT_SLOT_CREATED:
		return ("pci slot created");
	case VMMFS_PCI_EVENT_DESCRIPTOR_COMMITTED:
		return ("pci descriptor committed");
	case VMMFS_PCI_EVENT_DESCRIPTOR_REMOVED:
		return ("pci descriptor removed");
	case VMMFS_PCI_EVENT_POWER_ON:
		return ("pci power on");
	case VMMFS_PCI_EVENT_POWER_OFF:
		return ("pci power off");
	case VMMFS_PCI_EVENT_RESET:
		return ("pci reset");
	}
	return (NULL);
}

static int
vmmfs_pcislot_events_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_pcislot_events *events = ap->a_vp->v_data;
	int error = 0;

	if (events == NULL)
		return (ENOENT);
	if (ap->a_kn->kn_filter != EVFILT_READ)
		return (EOPNOTSUPP);
	lwkt_gettoken(&events->node.token);
	lwkt_gettoken(&events->token);
	if (events->node.dead || events->closed)
		error = ENOENT;
	else {
		ap->a_kn->kn_fop = &vmmfs_pcislot_events_read_filterops;
		ap->a_kn->kn_hook = (caddr_t)events;
		knote_insert(&events->kq.ki_note, ap->a_kn);
	}
	lwkt_reltoken(&events->token);
	lwkt_reltoken(&events->node.token);
	return (error);
}

static int
vmmfs_pcislot_events_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_events *events = ap->a_vp->v_data;
	struct vmmfs_pcislot *slot;
	int error;

	if (events == NULL)
		return (ENOENT);
	lwkt_gettoken(&events->node.token);
	slot = vmmfs_pcislot_events_slot(events);
	lwkt_gettoken(&slot->node.token);
	error = vmmfs_pcislot_auth_check(slot);
	lwkt_reltoken(&slot->node.token);
	if (error != 0)
		error = EACCES;
	else if (events->node.dead)
		error = ENXIO;
	else
		error = vop_stdopen(ap);
	lwkt_reltoken(&events->node.token);
	return (error);
}

static int
vmmfs_pcislot_events_read(struct vop_read_args *ap)
{
	struct vmmfs_pcislot_events *state_node;
	struct uio *uio;
	char buffer[VMMFS_PCISLOT_EVENTS_READ_SIZE];
	size_t index;
	size_t length;
	size_t i;
	int error;

	state_node = ap->a_vp->v_data;
	if (state_node == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);
	for (;;) {
		lwkt_gettoken(&state_node->node.token);
		lwkt_gettoken(&state_node->token);
		if (state_node->node.dead) {
			lwkt_reltoken(&state_node->token);
			lwkt_reltoken(&state_node->node.token);
			return (ENXIO);
		}
		if (state_node->length != 0)
			break;
		if (state_node->closed) {
			lwkt_reltoken(&state_node->token);
			lwkt_reltoken(&state_node->node.token);
			return (ENXIO);
		}
		if (ap->a_ioflag & IO_NDELAY) {
			lwkt_reltoken(&state_node->token);
			lwkt_reltoken(&state_node->node.token);
			return (EAGAIN);
		}
		tsleep_interlock(state_node, PCATCH);
		lwkt_reltoken(&state_node->token);
		lwkt_reltoken(&state_node->node.token);
		error = tsleep(state_node, PINTERLOCKED | PCATCH, "vmmpcievents", 0);
		if (error != 0)
			return (error);
	}
	length = state_node->length;
	if (length > sizeof(buffer))
		length = sizeof(buffer);
	if (length > (size_t)uio->uio_resid)
		length = (size_t)uio->uio_resid;
	for (i = 0; i < length; ++i) {
		index = (state_node->start + i) % VMMFS_PCISLOT_EVENTS_BUFFER_SIZE;
		buffer[i] = state_node->buffer[index];
	}
	state_node->start = (state_node->start + length) %
	    VMMFS_PCISLOT_EVENTS_BUFFER_SIZE;
	state_node->length -= length;
	lwkt_reltoken(&state_node->token);
	lwkt_reltoken(&state_node->node.token);
	return (uiomove(buffer, length, uio));
}

static int
vmmfs_pcislot_events_filter_read(struct knote *knote, long hint)
{
	struct vmmfs_pcislot_events *events;

	(void)hint;
	events = (struct vmmfs_pcislot_events *)knote->kn_hook;
	if (events == NULL)
		return (0);
	/* Existing knotes follow the stream's data-plane close, not admission. */
	lwkt_gettoken(&events->token);
	knote->kn_data = events->length;
	if (events->closed)
		knote->kn_flags |= EV_EOF;
	lwkt_reltoken(&events->token);
	return (knote->kn_data != 0 || (knote->kn_flags & EV_EOF) != 0);
}

static void
vmmfs_pcislot_events_filter_detach(struct knote *knote)
{
	struct vmmfs_pcislot_events *events;

	events = (struct vmmfs_pcislot_events *)knote->kn_hook;
	if (events == NULL)
		return;
	lwkt_gettoken(&events->token);
	knote_remove(&events->kq.ki_note, knote);
	lwkt_reltoken(&events->token);
}
