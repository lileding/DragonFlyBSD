/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot events stream.
 */
#include <sys/errno.h>
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
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_auth.h"
#include "vmmfs_pcislot_events.h"

#define VMMFS_PCISLOT_EVENTS_MODE 0444
#define VMMFS_PCISLOT_EVENTS_LINE_SIZE 512
#define VMMFS_PCISLOT_EVENTS_READ_SIZE 256

static int vmmfs_pcislot_events_access(struct vop_access_args *);
static int vmmfs_pcislot_events_getattr(struct vop_getattr_args *);
static int vmmfs_pcislot_events_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_pcislot_events_open(struct vop_open_args *);
static int vmmfs_pcislot_events_read(struct vop_read_args *);
static int vmmfs_pcislot_events_reclaim(struct vop_reclaim_args *);

struct vop_ops vmmfs_pcislot_events_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_pcislot_events_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_pcislot_events_getattr,
	.vop_getattr_lite = vmmfs_pcislot_events_getattr_lite,
	.vop_open = vmmfs_pcislot_events_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_pcislot_events_read,
	.vop_reclaim = vmmfs_pcislot_events_reclaim,
};

int
vmmfs_pcislot_events_create(struct vmmfs_pcislot *slot,
	struct vmmfs_pcislot_events *state_node)
{
	struct vmmfs_mount *mount;
	struct vnode *vnode;
	int error;

	if (slot == NULL || slot->pciroot == NULL ||
	    slot->pciroot->machine == NULL || state_node == NULL)
		return (EINVAL);
	mount = (struct vmmfs_mount *)slot->pciroot->machine->root->mount->mnt_data;
	if (mount->pcislot_events_vops == NULL)
		return (ENXIO);
	bzero(state_node, sizeof(*state_node));
	state_node->slot = slot;
	lwkt_token_init(&state_node->token, "vmmfspcievents");
	state_node->buffer = kmalloc(VMMFS_PCISLOT_EVENTS_BUFFER_SIZE, M_VMMFS,
	    M_WAITOK | M_ZERO);
	state_node->inode = atomic_fetchadd_int(&mount->next_inode, 1);
	error = getnewvnode(VT_SYNTH, slot->pciroot->machine->root->mount,
	    &vnode, 0, 0);
	if (error != 0)
		goto fail_buffer;
	vnode->v_data = state_node;
	vnode->v_ops = &mount->pcislot_events_vops;
	vnode->v_type = VREG;
	state_node->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);

fail_buffer:
	kfree(state_node->buffer, M_VMMFS);
	state_node->buffer = NULL;
	lwkt_token_uninit(&state_node->token);
	state_node->slot = NULL;
	return (error);
}

int
vmmfs_pcislot_events_destroy(struct vmmfs_pcislot_events *state_node)
{
	struct vnode *vnode;

	if (state_node == NULL)
		return (EINVAL);
	lwkt_gettoken(&state_node->token);
	state_node->closed = true;
	vnode = state_node->vnode;
	lwkt_reltoken(&state_node->token);
	wakeup(state_node);
	if (vnode != NULL) {
		vmmfs_vnode_revoke(vnode);
	}
	KKASSERT(state_node->vnode == NULL);
	kfree(state_node->buffer, M_VMMFS);
	state_node->buffer = NULL;
	state_node->slot = NULL;
	lwkt_token_uninit(&state_node->token);
	return (0);
}

void
vmmfs_pcislot_events_log(struct vmmfs_pcislot_events *state_node,
	const char *format, ...)
{
	va_list ap;
	char message[VMMFS_PCISLOT_EVENTS_LINE_SIZE];
	char text[VMMFS_PCISLOT_EVENTS_LINE_SIZE];
	char line[VMMFS_PCISLOT_EVENTS_LINE_SIZE];
	uint64_t tsc;
	size_t index;
	size_t length;
	size_t i;
	int result;

	if (state_node == NULL || format == NULL)
		return;
	va_start(ap, format);
	result = kvsnprintf(message, sizeof(message), format, ap);
	va_end(ap);
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
vmmfs_pcislot_events_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_events *events;

	events = ap->a_vp->v_data;
	if (events == NULL || events->slot == NULL)
		return (ENOENT);
	if (vmmfs_pcislot_auth_check(events->slot) != 0)
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
vmmfs_pcislot_events_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_pcislot_events *state_node;

	state_node = ap->a_vp->v_data;
	if (state_node != NULL && state_node->vnode == ap->a_vp)
		state_node->vnode = NULL;
	ap->a_vp->v_data = NULL;
	return (0);
}
