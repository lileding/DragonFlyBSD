/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine event stream.
 */
#include <sys/errno.h>
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

#define VMMFS_EVENTS_MODE 0644
#define VMMFS_EVENTS_LINE_SIZE 512
#define VMMFS_EVENTS_READ_SIZE 256

static int vmmfs_events_access(struct vop_access_args *);
static int vmmfs_events_getattr(struct vop_getattr_args *);
static int vmmfs_events_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_events_open(struct vop_open_args *);
static int vmmfs_events_read(struct vop_read_args *);
static int vmmfs_events_reclaim(struct vop_reclaim_args *);
static int vmmfs_events_setattr(struct vop_setattr_args *);
static int vmmfs_events_write(struct vop_write_args *);

struct vop_ops vmmfs_events_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_events_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_events_getattr,
	.vop_getattr_lite = vmmfs_events_getattr_lite,
	.vop_open = vmmfs_events_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_events_read,
	.vop_reclaim = vmmfs_events_reclaim,
	.vop_setattr = vmmfs_events_setattr,
	.vop_write = vmmfs_events_write,
};

int
vmmfs_events_create(struct vmmfs_machine *machine, struct vmmfs_events *events)
{
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	bzero(events, sizeof(*events));
	events->machine = machine;
	lwkt_token_init(&events->token, "vmmfsevents");
	events->buffer = kmalloc(VMMFS_EVENTS_BUFFER_SIZE, M_VMMFS,
	    M_WAITOK | M_ZERO);
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
	events->inode = atomic_fetchadd_int(&state->next_inode, 1);
	if (state->events_vops == NULL) {
		error = ENXIO;
		goto fail_buffer;
	}
	error = getnewvnode(VT_SYNTH, machine->root->mount, &vnode, 0, 0);
	if (error != 0)
		goto fail_buffer;
	vnode->v_data = events;
	vnode->v_ops = &state->events_vops;
	vnode->v_type = VREG;
	events->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);

fail_buffer:
	kfree(events->buffer, M_VMMFS);
	events->buffer = NULL;
	lwkt_token_uninit(&events->token);
	events->machine = NULL;
	return (error);
}

int
vmmfs_events_destroy(struct vmmfs_events *events)
{
	struct vnode *vnode;

	if (events == NULL)
		return (EINVAL);
	lwkt_gettoken(&events->token);
	events->closed = true;
	vnode = events->vnode;
	lwkt_reltoken(&events->token);
	wakeup(events);
	if (vnode != NULL) {
		vmmfs_vnode_revoke(vnode);
	}
	KKASSERT(events->vnode == NULL);
	kfree(events->buffer, M_VMMFS);
	events->buffer = NULL;
	events->machine = NULL;
	lwkt_token_uninit(&events->token);
	return (0);
}

void
vmmfs_events_log(struct vmmfs_events *events, const char *format, ...)
{
	va_list ap;
	char message[VMMFS_EVENTS_LINE_SIZE];
	char text[VMMFS_EVENTS_LINE_SIZE];
	char line[VMMFS_EVENTS_LINE_SIZE];
	uint64_t tsc;
	size_t index;
	size_t length;
	size_t i;
	int result;

	if (events == NULL || format == NULL)
		return;
	va_start(ap, format);
	result = kvsnprintf(message, sizeof(message), format, ap);
	va_end(ap);
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
vmmfs_events_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
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
vmmfs_events_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_events *events;

	events = ap->a_vp->v_data;
	if (events != NULL && events->vnode == ap->a_vp)
		events->vnode = NULL;
	ap->a_vp->v_data = NULL;
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
	return (vmmfs_machine_reset(events->machine, ap->a_cred));
}
