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

static int vmmfs_pcislot_events_access(struct vop_access_args *);
static int vmmfs_pcislot_events_getattr(struct vop_getattr_args *);
static int vmmfs_pcislot_events_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_pcislot_events_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_pcislot_events_open(struct vop_open_args *);
static int vmmfs_pcislot_events_read(struct vop_read_args *);
static int vmmfs_pcislot_events_inactive(struct vop_inactive_args *);
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

struct vop_ops vmmfs_pcislot_events_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_pcislot_events_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_pcislot_events_getattr,
	.vop_getattr_lite = vmmfs_pcislot_events_getattr_lite,
	.vop_kqfilter = vmmfs_pcislot_events_kqfilter,
	.vop_open = vmmfs_pcislot_events_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_events_read,
	.vop_inactive = vmmfs_pcislot_events_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

int
vmmfs_pcislot_events_init(struct vmmfs_pcislot *slot,
	struct vmmfs_pcislot_events *state_node, struct vnode **vnodep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_mount *mount;
	int error;

	if (slot == NULL || vmmfs_pcislot_pciroot(slot) == NULL ||
	    vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot)) == NULL ||
	    state_node == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	machine = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot));
	mount = vmmfs_root_state(vmmfs_machine_root(machine));
	if (mount->pcislot_events_vops == NULL)
		return (ENXIO);
	bzero(state_node, sizeof(*state_node));
	lwkt_token_init(&state_node->token, "vmmfspcievents");
	SLIST_INIT(&state_node->kq.ki_note);
	state_node->buffer = kmalloc(VMMFS_PCISLOT_EVENTS_BUFFER_SIZE, M_VMMFS,
	    M_WAITOK | M_ZERO);
	state_node->inode = atomic_fetchadd_int(&mount->next_inode, 1);
	vmmfs_node_setup(&state_node->node, &slot->branch,
	    vmmfs_pcislot_events_drop);
	error = vmmfs_vnode_create_regular(mount->mount,
	    &mount->pcislot_events_vops, VREG, &state_node->node, vnodep);
	if (error != 0)
		vmmfs_node_drop(&state_node->node);
	return (error);
}

static void
vmmfs_pcislot_events_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_events *state_node;

	state_node = (struct vmmfs_pcislot_events *)node;
	KKASSERT(state_node != NULL);
	vmmfs_pcislot_events_revoke(state_node);
	kfree(state_node->buffer, M_VMMFS);
	state_node->buffer = NULL;
	state_node->inode = 0;
	lwkt_token_uninit(&state_node->token);
	vmmfs_node_parent_put(node);
}

void
vmmfs_pcislot_events_revoke(struct vmmfs_pcislot_events *state_node)
{
	if (state_node == NULL)
		return;
	lwkt_gettoken(&state_node->token);
	state_node->closed = true;
	lwkt_reltoken(&state_node->token);
	wakeup(state_node);
	KNOTE(&state_node->kq.ki_note, 0);
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
vmmfs_pcislot_events_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_PCISLOT_EVENTS_MODE, 0));
}

static int
vmmfs_pcislot_events_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_pcislot_events *state_node;
	struct vattr *vattr;

	state_node = ap->a_vp->v_data;
	if (state_node == NULL)
		return (ENOENT);
	if (vmmfs_pcislot_events_slot(state_node) == NULL || vmmfs_pcislot_pciroot(vmmfs_pcislot_events_slot(state_node)) == NULL ||
	    (state_node)->node.dead)
		return (ENXIO);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_PCISLOT_EVENTS_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = state_node->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	return (0);
}

static int
vmmfs_pcislot_events_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_PCISLOT_EVENTS_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_pcislot_events_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_pcislot_events *events;

	events = ap->a_vp->v_data;
	if (events == NULL || vmmfs_pcislot_events_slot(events) == NULL ||
	    (events)->node.dead)
		return (ENOENT);
	if (ap->a_kn->kn_filter != EVFILT_READ)
		return (EOPNOTSUPP);
	lwkt_gettoken(&events->token);
	ap->a_kn->kn_fop = &vmmfs_pcislot_events_read_filterops;
	ap->a_kn->kn_hook = (caddr_t)events;
	knote_insert(&events->kq.ki_note, ap->a_kn);
	lwkt_reltoken(&events->token);
	return (0);
}

static int
vmmfs_pcislot_events_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_events *events;

	events = ap->a_vp->v_data;
	if (events == NULL || vmmfs_pcislot_events_slot(events) == NULL)
		return (ENOENT);
	if ((events)->node.dead)
		return (ENXIO);
	if (vmmfs_pcislot_auth_check(vmmfs_pcislot_events_slot(events)) != 0)
		return (EACCES);
	return (vop_stdopen(ap));
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
		lwkt_gettoken(&state_node->token);
		if (state_node->length != 0)
			break;
		if (state_node->closed) {
			lwkt_reltoken(&state_node->token);
			return (ENXIO);
		}
		if (ap->a_ioflag & IO_NDELAY) {
			lwkt_reltoken(&state_node->token);
			return (EAGAIN);
		}
		tsleep_interlock(state_node, PCATCH);
		lwkt_reltoken(&state_node->token);
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
	lwkt_gettoken(&events->token);
	if (vmmfs_pcislot_events_slot(events) == NULL || vmmfs_pcislot_pciroot(vmmfs_pcislot_events_slot(events)) == NULL ||
	    (events)->node.dead) {
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

static int
vmmfs_pcislot_events_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_pcislot_events *events;

	events = ap->a_vp->v_data;
	if (events == NULL || !events->node.dead)
		return (0);
	vmmfs_node_inactive(&events->node, ap->a_vp);
	return (0);
}
