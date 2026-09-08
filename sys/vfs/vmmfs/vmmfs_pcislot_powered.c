/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot power state.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/thread2.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_parent.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_auth.h"
#include "vmmfs_pcislot_powered.h"

#define VMMFS_PCISLOT_POWERED_MODE 0444

static int vmmfs_pcislot_powered_load(struct vmmfs_node *, char *, size_t, size_t *);
static int vmmfs_pcislot_powered_open(struct vop_open_args *);
static int vmmfs_pcislot_powered_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_pcislot_powered_filter(struct knote *, long);
static void vmmfs_pcislot_powered_filter_detach(struct knote *);
static bool vmmfs_pcislot_powered_deactivate(struct vmmfs_node *);
static void vmmfs_pcislot_powered_drop(struct vmmfs_node *);

static struct filterops vmmfs_pcislot_powered_filterops = {
	FILTEROP_ISFD | FILTEROP_MPSAFE,
	NULL,
	vmmfs_pcislot_powered_filter_detach,
	vmmfs_pcislot_powered_filter,
};

struct vop_ops vmmfs_pcislot_powered_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_kqfilter = vmmfs_pcislot_powered_kqfilter,
	.vop_open = vmmfs_pcislot_powered_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_node_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

int
vmmfs_pcislot_powered_init(struct vmmfs_node *parent,
	struct vmmfs_pcislot_powered *state_node)
{
	struct vmmfs_root *root;
	int error;

	if (parent == NULL || state_node == NULL)
		return (EINVAL);
	root = (struct vmmfs_root *)parent->mount->root;
	bzero(state_node, sizeof(*state_node));
	lwkt_token_init(&state_node->token, "vmmfspcipower");
	SLIST_INIT(&state_node->kq.ki_note);
	state_node->node.inode = vmmfs_root_allocate_inode(root);
	state_node->node.parent = parent;
	state_node->node.mount = parent->mount;
	state_node->node.dead = false;
	state_node->node.references = 1;
	lockinit(&state_node->node.lock, "vmmfsnode", 0, 0);
	state_node->node.drop = vmmfs_pcislot_powered_drop;
	vmmfs_node_hold(parent);
	state_node->node.mode = VMMFS_PCISLOT_POWERED_MODE;
	state_node->node.size = 2;
	state_node->node.load_limit = 2;
	state_node->node.load = vmmfs_pcislot_powered_load;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->pcislot_powered_vops, VREG, &state_node->node);
	if (error != 0)
		vmmfs_node_put(&state_node->node);
	else
		state_node->node.deactivate = vmmfs_pcislot_powered_deactivate;
	return (error);
}

void
vmmfs_pcislot_powered_set(struct vmmfs_pcislot_powered *state_node, bool value)
{
	lwkt_gettoken(&state_node->token);
	if (state_node->value != value) {
		state_node->value = value;
		KNOTE(&state_node->kq.ki_note, NOTE_WRITE);
	}
	lwkt_reltoken(&state_node->token);
}

static bool
vmmfs_pcislot_powered_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_powered *state_node = (void *)node;

	lwkt_gettoken(&state_node->token);
	state_node->closed = true;
	state_node->value = false;
	KNOTE(&state_node->kq.ki_note, NOTE_REVOKE);
	lwkt_reltoken(&state_node->token);
	return (true);
}

static void
vmmfs_pcislot_powered_drop(struct vmmfs_node *node)
{
	struct vmmfs_pcislot_powered *state_node = (void *)node;

	lwkt_token_uninit(&state_node->token);
}

static int
vmmfs_pcislot_powered_load(struct vmmfs_node *node, char *buffer,
    size_t capacity, size_t *length)
{
	struct vmmfs_pcislot_powered *state_node = (void *)node;

	if (capacity < 2)
		return (EOVERFLOW);
	lwkt_gettoken(&state_node->token);
	buffer[0] = state_node->value ? '1' : '0';
	lwkt_reltoken(&state_node->token);
	buffer[1] = '\n';
	*length = 2;
	return (0);
}

static int
vmmfs_pcislot_powered_authorize(struct vmmfs_pcislot_powered *powered)
{
	struct vmmfs_pcislot *slot;
	int error;
	slot = vmmfs_pcislot_powered_slot(powered);
	lwkt_gettoken(&slot->token);
	error = vmmfs_pcislot_auth_check(slot);
	lwkt_reltoken(&slot->token);
	return (error == 0 ? 0 : EACCES);
}

static int
vmmfs_pcislot_powered_open(struct vop_open_args *ap)
{
	struct vmmfs_pcislot_powered *powered = ap->a_vp->v_data;
	int error;

	if (ap->a_mode & FWRITE)
		return (EACCES);
	error = VMMFS_WORK(powered, vmmfs_pcislot_powered_authorize(powered));
	return (error == 0 ? vop_stdopen(ap) : error);
}

static int
vmmfs_pcislot_powered_subscribe(struct vmmfs_pcislot_powered *powered,
    struct knote *knote)
{
	int error;

	if (knote->kn_filter != EVFILT_VNODE)
		return (EOPNOTSUPP);
	error = vmmfs_pcislot_powered_authorize(powered);
	if (error != 0)
		return (error);
	lwkt_gettoken(&powered->token);
	if (powered->closed) {
		error = ENOENT;
	} else {
		knote->kn_fop = &vmmfs_pcislot_powered_filterops;
		knote->kn_hook = (caddr_t)powered;
		knote_insert(&powered->kq.ki_note, knote);
	}
	lwkt_reltoken(&powered->token);
	return (error);
}

static int
vmmfs_pcislot_powered_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vmmfs_pcislot_powered *powered = ap->a_vp->v_data;

	return (VMMFS_WORK(powered, vmmfs_pcislot_powered_subscribe(powered, ap->a_kn)));
}

static int
vmmfs_pcislot_powered_filter(struct knote *knote, long hint)
{
	struct vmmfs_pcislot_powered *powered = (void *)knote->kn_hook;

	lwkt_gettoken(&powered->token);
	knote->kn_fflags |= knote->kn_sfflags & hint;
	if (powered->closed)
		knote->kn_flags |= EV_EOF | EV_NODATA;
	knote->kn_data = 0;
	lwkt_reltoken(&powered->token);
	return (knote->kn_fflags != 0 || (knote->kn_flags & EV_EOF) != 0);
}

static void
vmmfs_pcislot_powered_filter_detach(struct knote *knote)
{
	struct vmmfs_pcislot_powered *powered;

	powered = (struct vmmfs_pcislot_powered *)knote->kn_hook;
	if (powered == NULL)
		return;
	lwkt_gettoken(&powered->token);
	knote_remove(&powered->kq.ki_note, knote);
	lwkt_reltoken(&powered->token);
}
