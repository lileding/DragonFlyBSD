/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine directory object.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <machine/limits.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_launch.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"
#include "vmmfs_platform_x64.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_serialport.h"
#include "vmmfs_stopped.h"

#define VMMFS_MACHINE_MODE 0555

static int vmmfs_machine_ncreate(struct vop_ncreate_args *);
static int vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_machine_nremove(struct vop_nremove_args *);
static int vmmfs_machine_nresolve(struct vop_nresolve_args *);
static int vmmfs_machine_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_machine_prepare_stopped(struct vmmfs_machine *);
static int vmmfs_machine_readdir(struct vop_readdir_args *);
static int vmmfs_machine_start(struct vmmfs_machine *, struct ucred *);
static int vmmfs_machine_release_runtime(struct vmmfs_machine *);
static int vmmfs_machine_release_to_stopped(struct vmmfs_machine *);
static int vmmfs_machine_create_stopped(struct vmmfs_machine *);
static void vmmfs_machine_runtime_put(struct vmmfs_machine *);
static void vmmfs_machine_runtime_wait(struct vmmfs_machine *);
static void vmmfs_machine_cleanup_stopped(struct vmmfs_machine *);
static void vmmfs_machine_drop(struct vmmfs_node *);
static void vmmfs_machine_cleanup_partial(struct vmmfs_machine *, struct vnode *);
static int vmmfs_machine_deactivate(struct vmmfs_node *);
static void vmmfs_machine_invalidate_children(struct vmmfs_machine *);

struct vop_ops vmmfs_machine_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_ncreate = vmmfs_machine_ncreate,
	.vop_nlookupdotdot = vmmfs_machine_nlookupdotdot,
	.vop_nremove = vmmfs_machine_nremove,
	.vop_nresolve = vmmfs_machine_nresolve,
	.vop_nrmdir = vmmfs_machine_nrmdir,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_machine_readdir,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

int
vmmfs_machine_create(struct vmmfs_mount *mount, struct vmmfs_node *parent,
	const char *name, size_t namelen, struct vnode **vnodep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_mount *state;
	struct vmmfs_root *root;
	struct vnode *vnode;
	int error;

	if (parent == NULL || mount == NULL || name == NULL || vnodep == NULL ||
	    namelen == 0 || namelen > NAME_MAX)
		return (EINVAL);
	root = mount->root_vnode == NULL ? NULL : mount->root_vnode->v_data;
	if (root == NULL)
		return (ENXIO);
	*vnodep = NULL;
	vnode = NULL;
	machine = kmalloc(sizeof(*machine), M_VMMFS, M_WAITOK | M_ZERO);
	machine->node.parent = parent;
	machine->node.references = 1;
	lwkt_token_init(&machine->node.token, "vmmfsnode");
	machine->node.drop = vmmfs_machine_drop;
	if (parent != NULL)
		vmmfs_node_hold(parent);
	machine->node.deactivate = vmmfs_machine_deactivate;
	machine->mount = mount;
	machine->node.inode = vmmfs_root_allocate_inode(root);
	machine->node.mode = VMMFS_MACHINE_MODE;
	machine->node.size = 0;
	bcopy(name, machine->name, namelen);
	machine->name[namelen] = 0;
	error = vmmfs_machine_id_init(mount, &machine->node, &machine->id_node,
	    &machine->id_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_vcpu_init(mount, &machine->node, &machine->vcpu,
	    &machine->vcpu_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_memory_init(mount, &machine->node, &machine->memory,
	    &machine->memory_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_loader_init(mount, &machine->node, &machine->loader,
	    &machine->loader_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_boot_init(mount, &machine->node, &machine->boot,
	    &machine->boot_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_machine_create_stopped(machine);
	if (error != 0)
		goto fail;
	error = vmmfs_pciroot_init(mount, &machine->node, &machine->pciroot,
	    &machine->pciroot_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_platform_x64_init(machine, &machine->platform);
	if (error != 0)
		goto fail;
	error = vmmfs_rtc_init(machine, &machine->rtc);
	if (error != 0)
		goto fail;
	error = vmmfs_serialroot_init(mount, &machine->node, &machine->serialroot,
	    &machine->serialroot_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_events_init(mount, &machine->node, &machine->events,
	    &machine->events_vnode);
	if (error != 0)
		goto fail;
	state = machine->mount;
	if (state == NULL || state->machine_vops == NULL) {
		error = ENXIO;
		goto fail;
	}
	error = vmmfs_vnode_create_regular(state->mount, &state->machine_vops,
	    VDIR, &machine->node, &vnode);
	if (error != 0)
		goto fail;
	machine->vnode = vnode;
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_CREATED, NULL);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOPPED,
	    "reason=create");
	*vnodep = vnode;
	return (0);

fail:
	vmmfs_machine_cleanup_partial(machine, vnode);
	return (error);
}

static void
vmmfs_machine_cleanup_partial(struct vmmfs_machine *machine, struct vnode *vnode)
{
	if (machine == NULL)
		return;
	lwkt_gettoken(&machine->node.token);
	machine->node.dead = true;
	lwkt_reltoken(&machine->node.token);
	if (machine->events.node.drop != NULL) {
		vmmfs_vnode_discard(machine->events_vnode);
		machine->events_vnode = NULL;
		vmmfs_node_put(&machine->events.node);
	}
	if (machine->serialroot.node.drop != NULL) {
		vmmfs_vnode_discard(machine->serialroot_vnode);
		machine->serialroot_vnode = NULL;
		vmmfs_node_put(&machine->serialroot.node);
	}
	if (machine->rtc.machine != NULL)
		vmmfs_rtc_fini(&machine->rtc);
	if (machine->platform.machine != NULL)
		vmmfs_platform_x64_fini(&machine->platform);
	if (machine->pciroot.node.drop != NULL) {
		vmmfs_vnode_discard(machine->pciroot_vnode);
		machine->pciroot_vnode = NULL;
		vmmfs_node_put(&machine->pciroot.node);
	}
	vmmfs_machine_cleanup_stopped(machine);
	if (machine->boot.node.drop != NULL) {
		vmmfs_vnode_discard(machine->boot_vnode);
		machine->boot_vnode = NULL;
		vmmfs_node_put(&machine->boot.node);
	}
	if (machine->loader.node.drop != NULL) {
		vmmfs_vnode_discard(machine->loader_vnode);
		machine->loader_vnode = NULL;
		vmmfs_node_put(&machine->loader.node);
	}
	if (machine->memory.node.drop != NULL) {
		vmmfs_vnode_discard(machine->memory_vnode);
		machine->memory_vnode = NULL;
		vmmfs_node_put(&machine->memory.node);
	}
	if (machine->vcpu.node.drop != NULL) {
		vmmfs_vnode_discard(machine->vcpu_vnode);
		machine->vcpu_vnode = NULL;
		vmmfs_node_put(&machine->vcpu.node);
	}
	if (machine->id_node.node.drop != NULL) {
		vmmfs_vnode_discard(machine->id_vnode);
		machine->id_vnode = NULL;
		vmmfs_node_put(&machine->id_node.node);
	}
	vmmfs_vnode_discard(vnode);
	vmmfs_node_put(&machine->node);
}

static int
vmmfs_machine_prepare_stopped(struct vmmfs_machine *machine)
{
	struct vmmfs_stopped *stopped;
	struct vnode *stopped_vnode;
	int error;

	error = vmmfs_stopped_create(machine->mount, &machine->node,
	    &stopped_vnode);
	if (error != 0)
		return (error);
	stopped = stopped_vnode->v_data;
	lwkt_gettoken(&machine->node.token);
	if (machine->node.dead || machine->runtime_releasing) {
		lwkt_reltoken(&machine->node.token);
		vmmfs_vnode_discard(stopped_vnode);
		vmmfs_node_put(&stopped->node);
		return (EBUSY);
	}
	if (machine->stopped_vnode != NULL) {
		lwkt_reltoken(&machine->node.token);
		vmmfs_vnode_discard(stopped_vnode);
		vmmfs_node_put(&stopped->node);
		return (0);
	}
	machine->stopped_vnode = stopped_vnode;
	lwkt_reltoken(&machine->node.token);
	return (0);
}

static int
vmmfs_machine_create_stopped(struct vmmfs_machine *machine)
{
	int error;

	error = vmmfs_machine_prepare_stopped(machine);
	if (error != 0)
		return (error);
	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->node.token);
	if (machine->node.dead || machine->runtime_releasing ||
	    (machine->machine != NULL && !machine->runtime_released)) {
		lwkt_reltoken(&machine->node.token);
		lwkt_reltoken(&machine->vcpu.token);
		return (EBUSY);
	}
	/* A stop before VCPU startup has no worker to consume requests. */
	if (machine->vcpu.threads == NULL) {
		machine->vcpu.stop_requested = false;
		machine->vcpu.reset_requested = false;
	}
	machine->machine = NULL;
	machine->runtime_releasing = false;
	machine->runtime_released = false;
	lwkt_reltoken(&machine->node.token);
	lwkt_reltoken(&machine->vcpu.token);
	vmmfs_machine_invalidate_children(machine);
	return (0);
}

static void
vmmfs_machine_cleanup_stopped(struct vmmfs_machine *machine)
{
	struct vnode *vnode;
	int error;

	lwkt_gettoken(&machine->node.token);
	vnode = machine->stopped_vnode;
	machine->stopped_vnode = NULL;
	lwkt_reltoken(&machine->node.token);
	if (vnode == NULL)
		return;
	error = vmmfs_vnode_deactivate(vnode);
	if (error != 0)
		kprintf("vmmfs: stopped deactivate: %d\n", error);
	vrele(vnode);
}

static int
vmmfs_machine_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)node;
	struct vnode **children[] = {
		&machine->id_vnode, &machine->vcpu_vnode,
		&machine->memory_vnode, &machine->loader_vnode,
		&machine->boot_vnode, &machine->stopped_vnode,
		&machine->pciroot_vnode, &machine->serialroot_vnode,
		&machine->events_vnode
	};
	struct vnode *vnode;
	size_t index;
	int error;

	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&node->token);
	error = machine->machine != NULL || machine->runtime_releasing ||
	    machine->runtime_released || machine->runtime_references != 0 ||
	    machine->vcpu.active_count != 0 ||
	    machine->vcpu.threads != NULL ? EBUSY : 0;
	lwkt_reltoken(&node->token);
	lwkt_reltoken(&machine->vcpu.token);
	if (error != 0)
		return (error);

	for (index = 0; index < NELEM(children); ++index) {
		lwkt_gettoken(&node->token);
		vnode = *children[index];
		lwkt_reltoken(&node->token);
		if (vnode == NULL)
			continue;
		error = vmmfs_vnode_deactivate(vnode);
		if (error != 0)
			return (error);
		lwkt_gettoken(&node->token);
		*children[index] = NULL;
		lwkt_reltoken(&node->token);
		vrele(vnode);
	}
	lwkt_gettoken(&node->token);
	machine->vnode = NULL;
	lwkt_reltoken(&node->token);
	return (0);
}

static void
vmmfs_machine_drop(struct vmmfs_node *node)
{
	struct vmmfs_machine *machine;

	machine = (struct vmmfs_machine *)node;
	KKASSERT(machine != NULL);
	KKASSERT(machine->node.dead);
	KKASSERT(machine->node.references == 0);
	KKASSERT(machine->id_node.node.drop == NULL);
	KKASSERT(machine->vcpu.node.drop == NULL);
	KKASSERT(machine->memory.node.drop == NULL);
	KKASSERT(machine->loader.node.drop == NULL);
	KKASSERT(machine->boot.node.drop == NULL);
	KKASSERT(machine->stopped_vnode == NULL);
	KKASSERT(machine->events.node.drop == NULL);
	KKASSERT(machine->pciroot.node.drop == NULL);
	KKASSERT(machine->serialroot.node.drop == NULL);
	KKASSERT(machine->pciroot.runtime_machine == NULL);
	KKASSERT(machine->vnode == NULL);
	if (machine->rtc.machine != NULL)
		vmmfs_rtc_fini(&machine->rtc);
	if (machine->platform.machine != NULL)
		vmmfs_platform_x64_fini(&machine->platform);
	kfree(machine, M_VMMFS);
}

static void
vmmfs_machine_invalidate_children(struct vmmfs_machine *machine)
{
	struct vnode *vnode;

	if (machine == NULL)
		return;
	lwkt_gettoken(&machine->node.token);
	vnode = machine->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->node.token);
	if (vnode == NULL)
		return;
	cache_inval_vp(vnode, CINV_CHILDREN);
	vdrop(vnode);
}




static void
vmmfs_machine_runtime_put(struct vmmfs_machine *machine)
{
	bool wake;

	KKASSERT(machine != NULL);
	lwkt_gettoken(&machine->node.token);
	KKASSERT(machine->runtime_references != 0);
	wake = --machine->runtime_references == 0;
	lwkt_reltoken(&machine->node.token);
	if (wake)
		wakeup(machine);
}

static void
vmmfs_machine_runtime_wait(struct vmmfs_machine *machine)
{
	KKASSERT(machine != NULL);
	for (;;) {
		tsleep_interlock(machine, 0);
		lwkt_gettoken(&machine->node.token);
		if (machine->runtime_references == 0) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			lwkt_reltoken(&machine->node.token);
			return;
		}
		lwkt_reltoken(&machine->node.token);
		(void)tsleep(machine, PINTERLOCKED, "vmmfsrt", 0);
	}
}

int
vmmfs_machine_request_stop(struct vmmfs_machine *machine, const char *reason)
{
	struct vnode *vnode;
	bool running, released;
	int error = 0;

	lwkt_gettoken(&machine->node.token);
	if (machine->node.dead) {
		lwkt_reltoken(&machine->node.token);
		return (ENOENT);
	}
	vnode = machine->launch_vnode;
	if (vnode != NULL)
		vref(vnode);
	running = vnode == NULL && machine->machine != NULL &&
	    !machine->runtime_releasing && !machine->runtime_released;
	released = machine->runtime_released;
	/* Keep this request attached to the runtime admitted above. */
	if (running)
		++machine->runtime_references;
	vmmfs_node_hold(&machine->vcpu.node);
	vmmfs_node_hold(&machine->events.node);
	lwkt_reltoken(&machine->node.token);
	if (vnode != NULL) {
		error = vmmfs_machine_abort(vnode->v_data);
		vrele(vnode);
	} else if (released) {
		error = vmmfs_machine_create_stopped(machine);
	} else if (running) {
		vmmfs_vcpu_request_stop(&machine->vcpu);
		vmmfs_machine_runtime_put(machine);
	}
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOP_REQUESTED,
	    "reason=%s", reason);
	vmmfs_node_put(&machine->vcpu.node);
	vmmfs_node_put(&machine->events.node);
	return (error);
}

int
vmmfs_machine_reset(struct vmmfs_machine *machine)
{
	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->node.token);
	if (machine->node.dead) {
		lwkt_reltoken(&machine->node.token);
		return (ENOENT);
	}
	if (machine->machine == NULL || machine->launch_vnode != NULL ||
	    machine->runtime_releasing || machine->runtime_released) {
		lwkt_reltoken(&machine->node.token);
		return (EBUSY);
	}
	++machine->runtime_references;
	vmmfs_node_hold(&machine->vcpu.node);
	vmmfs_node_hold(&machine->events.node);
	lwkt_reltoken(&machine->node.token);
	vmmfs_vcpu_request_reset(&machine->vcpu);
	vmmfs_machine_runtime_put(machine);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_RESET_REQUESTED,
	    "reason=external");
	vmmfs_node_put(&machine->vcpu.node);
	vmmfs_node_put(&machine->events.node);
	return (0);
}


static int
vmmfs_machine_ncreate(struct vop_ncreate_args *ap)
{
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	struct namecache *ncp;
	int error;

	machine = ap->a_dvp->v_data;
	if (machine == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	if (ncp->nc_nlen != sizeof("stopped") - 1 ||
	    bcmp(ncp->nc_name, "stopped", sizeof("stopped") - 1) != 0)
		return (EOPNOTSUPP);
	if (ap->a_vap->va_type != VREG)
		return (EINVAL);
	error = vmmfs_machine_prepare_stopped(machine);
	if (error != 0)
		return (error);
	error = vmmfs_machine_request_stop(machine, "external");
	if (error != 0)
		return (error);
	lwkt_gettoken(&machine->node.token);
	vnode = machine->stopped_vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->node.token);
	if (vnode == NULL)
		return (ENOENT);
	error = vget(vnode, LK_EXCLUSIVE);
	vdrop(vnode);
	if (error != 0)
		return (error);
	cache_setunresolved(ap->a_nch);
	*ap->a_vpp = vnode;
	return (0);
}

static int
vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_machine *machine;
	struct vmmfs_mount *mount;
	struct vnode *vnode;
	int error;

	machine = ap->a_dvp->v_data;
	mount = (struct vmmfs_mount *)ap->a_dvp->v_mount->mnt_data;
	if (machine == NULL || mount == NULL)
		return (ENOENT);
	lwkt_gettoken(&machine->node.token);
	if (machine->node.dead) {
		lwkt_reltoken(&machine->node.token);
		return (ENOENT);
	}
	vnode = mount->root_vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->node.token);
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
vmmfs_machine_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	struct namecache *ncp;
	int error;

	machine = ap->a_dvp->v_data;
	if (machine == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	lwkt_gettoken(&machine->node.token);
	if (machine->node.dead) {
		lwkt_reltoken(&machine->node.token);
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	if (ncp->nc_nlen == sizeof("id") - 1 &&
	    bcmp(ncp->nc_name, "id", sizeof("id") - 1) == 0)
		vnode = machine->id_vnode;
	else if (ncp->nc_nlen == sizeof("vcpu") - 1 &&
	    bcmp(ncp->nc_name, "vcpu", sizeof("vcpu") - 1) == 0)
		vnode = machine->vcpu_vnode;
	else if (ncp->nc_nlen == sizeof("mem") - 1 &&
	    bcmp(ncp->nc_name, "mem", sizeof("mem") - 1) == 0)
		vnode = machine->memory_vnode;
	else if (ncp->nc_nlen == sizeof("loader") - 1 &&
	    bcmp(ncp->nc_name, "loader", sizeof("loader") - 1) == 0)
		vnode = machine->loader_vnode;
	else if (ncp->nc_nlen == sizeof("boot") - 1 &&
	    bcmp(ncp->nc_name, "boot", sizeof("boot") - 1) == 0)
		vnode = machine->boot_vnode;
	else if (ncp->nc_nlen == sizeof("events") - 1 &&
	    bcmp(ncp->nc_name, "events", sizeof("events") - 1) == 0)
		vnode = machine->events_vnode;
	else if (ncp->nc_nlen == sizeof("pci") - 1 &&
	    bcmp(ncp->nc_name, "pci", sizeof("pci") - 1) == 0)
		vnode = machine->pciroot_vnode;
	else if (ncp->nc_nlen == sizeof("serial") - 1 &&
	    bcmp(ncp->nc_name, "serial", sizeof("serial") - 1) == 0)
		vnode = machine->serialroot_vnode;
	else if (ncp->nc_nlen == sizeof("stopped") - 1 &&
	    bcmp(ncp->nc_name, "stopped", sizeof("stopped") - 1) == 0)
		vnode = machine->machine == NULL || machine->launch_vnode != NULL ?
		    machine->stopped_vnode : NULL;
	else {
		lwkt_reltoken(&machine->node.token);
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->node.token);
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
vmmfs_machine_nremove(struct vop_nremove_args *ap)
{
	struct vmmfs_machine *machine = ap->a_dvp->v_data;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vnode *original;
	int error;

	if (ncp->nc_nlen != sizeof("stopped") - 1 ||
	    bcmp(ncp->nc_name, "stopped", sizeof("stopped") - 1) != 0)
		return (EOPNOTSUPP);
	/*
	 * A launch write runs on another thread and may invalidate stopped.
	 * Keep the old vnode alive, but never hold its namecache lock while
	 * waiting.  Do not unlink a new stopped node published by a fast stop.
	 */
	original = ncp->nc_vp;
	if (original != NULL)
		vref(original);
	cache_unlock(ap->a_nch);
	error = vmmfs_machine_start(machine, ap->a_cred);
	cache_lock(ap->a_nch);
	if (error == 0 && ncp->nc_vp == original)
		cache_unlink(ap->a_nch);
	if (original != NULL)
		vrele(original);
	return (error);
}

static int
vmmfs_machine_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vmmfs_machine *machine;

	machine = ap->a_dvp->v_data;
	if (machine == NULL)
		return (ENOENT);
	return (EOPNOTSUPP);
}

static int
vmmfs_machine_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_machine *machine;
	struct vmmfs_node *stopped;
	struct vmmfs_mount *mount;
	struct uio *uio;
	off_t offset;
	ino_t inode;
	int error;
	int present;
	int stop;

	machine = ap->a_vp->v_data;
	mount = (struct vmmfs_mount *)ap->a_vp->v_mount->mnt_data;
	if (machine == NULL || mount == NULL || mount->root_inode == 0 ||
	    machine->node.dead)
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
		stop = vop_write_dirent(&error, uio, machine->node.inode, DT_DIR, 1,
		    ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, mount->root_inode, DT_DIR, 2,
		    "..");
		if (!stop)
			offset = 2;
	}
	if (!stop && offset == 2) {
		stop = vop_write_dirent(&error, uio, machine->id_node.node.inode,
		    DT_REG, sizeof("id") - 1, "id");
		if (!stop)
			offset = 3;
	}
	if (!stop && offset == 3) {
		stop = vop_write_dirent(&error, uio, machine->vcpu.node.inode,
		    DT_REG, sizeof("vcpu") - 1, "vcpu");
		if (!stop)
			offset = 4;
	}
	if (!stop && offset == 4) {
		stop = vop_write_dirent(&error, uio, machine->memory.node.inode,
		    DT_REG, sizeof("mem") - 1, "mem");
		if (!stop)
			offset = 5;
	}
	if (!stop && offset == 5) {
		stop = vop_write_dirent(&error, uio, machine->loader.node.inode,
		    DT_REG, sizeof("loader") - 1, "loader");
		if (!stop)
			offset = 6;
	}
	if (!stop && offset == 6) {
		stop = vop_write_dirent(&error, uio, machine->boot.node.inode,
		    DT_CHR, sizeof("boot") - 1, "boot");
		if (!stop)
			offset = 7;
	}
	if (!stop && offset == 7) {
		stop = vop_write_dirent(&error, uio, machine->events.node.inode,
		    DT_REG, sizeof("events") - 1, "events");
		if (!stop)
			offset = 8;
	}
	if (!stop && offset == 8) {
		lwkt_gettoken(&machine->node.token);
		stopped = machine->stopped_vnode == NULL ? NULL :
		    machine->stopped_vnode->v_data;
		present = stopped != NULL && (machine->machine == NULL ||
		    machine->launch_vnode != NULL);
		inode = present ? stopped->inode : 0;
		lwkt_reltoken(&machine->node.token);
		if (present) {
			stop = vop_write_dirent(&error, uio, inode, DT_REG,
			    sizeof("stopped") - 1, "stopped");
		}
		if (!stop)
			offset = 9;
	}
	if (!stop && offset == 9) {
		lwkt_gettoken(&machine->node.token);
		inode = machine->pciroot.node.inode;
		lwkt_reltoken(&machine->node.token);
		stop = vop_write_dirent(&error, uio, inode, DT_DIR,
		    sizeof("pci") - 1, "pci");
		if (!stop)
			offset = 10;
	}
	if (!stop && offset == 10) {
		lwkt_gettoken(&machine->node.token);
		inode = machine->serialroot.node.inode;
		lwkt_reltoken(&machine->node.token);
		stop = vop_write_dirent(&error, uio, inode, DT_DIR,
		    sizeof("serial") - 1, "serial");
		if (!stop)
			offset = 11;
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop;
	return (error);
}


int
vmmfs_machine_boot(struct vmmfs_machine *machine, struct vnode **vnodep)
{
	struct vmmfs_memory memory;
	struct vmmfs_launch *launch;
	struct vnode *vnode, *vcpu_vnode;
	vmm_machine_t runtime;
	uint32_t count;
	int error, cleanup_error;
	bool current;

	*vnodep = NULL;
	bzero(&memory, sizeof(memory));
	memory.node.parent = &machine->node;
	lwkt_gettoken(&machine->node.token);
	if (machine->node.dead || machine->vcpu_vnode == NULL) {
		lwkt_reltoken(&machine->node.token);
		return (ENOENT);
	}
	/* Private PREPARE may outlive namespace deactivation. */
	vcpu_vnode = machine->vcpu_vnode;
	vref(vcpu_vnode);
	memory.size = machine->memory.size;
	lwkt_reltoken(&machine->node.token);
	runtime = NULL;
	vnode = NULL;

	/* Private candidates may sleep; no shared topology is changed yet. */
	error = vmmfs_launch_create(machine->mount, &machine->node,
	    memory.size, &vnode);
	if (error != 0)
		goto finished;
	launch = vnode->v_data;
	error = vmmfs_memory_prepare(&memory, memory.size);
	if (error != 0)
		goto rejected;
	error = vmm_machine_create(memory.run_vmspace, &runtime);
	if (error != 0)
		goto rejected;
	error = vmmfs_memory_map(&memory);
	if (error != 0)
		goto rejected;
	error = vmmfs_launch_map(launch, memory.object);
	if (error != 0)
		goto rejected;

	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->node.token);
	if (machine->node.dead || machine->vcpu_vnode != vcpu_vnode ||
	    machine->machine != NULL ||
	    machine->vcpu.threads != NULL || machine->vcpu.active_count != 0 ||
	    machine->launch_vnode != NULL || machine->runtime_releasing ||
	    machine->runtime_released || machine->runtime_references != 0 ||
	    machine->memory.size != memory.size || machine->vcpu.count == 0) {
		lwkt_reltoken(&machine->node.token);
		lwkt_reltoken(&machine->vcpu.token);
		error = EBUSY;
		goto rejected;
	}
	count = machine->vcpu.count;
	machine->memory.object = memory.object;
	machine->memory.run_vmspace = memory.run_vmspace;
	machine->memory.mapped = memory.mapped;
	memory.object = NULL;
	memory.run_vmspace = NULL;
	machine->machine = runtime;
	machine->launch_vnode = vnode;
	vref(vnode);
	/* Pin PREPARE across token-releasing platform allocations. */
	++machine->runtime_references;
	lwkt_reltoken(&machine->node.token);
	lwkt_reltoken(&machine->vcpu.token);

	error = vmm_machine_create_irqchip(runtime);
	if (error == 0)
		error = vmm_machine_create_pit(runtime);
	if (error == 0)
		error = vmmfs_platform_x64_prepare(&machine->platform,
		    &machine->memory, count, &machine->pciroot,
		    &machine->serialroot);
	if (error == 0)
		error = vmmfs_rtc_start(&machine->rtc, runtime);
	if (error == 0)
		error = vmmfs_pciroot_start(&machine->pciroot, runtime);
	if (error == 0)
		error = vmmfs_serialroot_start(&machine->serialroot, runtime);
	if (error == 0)
		error = vmmfs_platform_x64_start(&machine->platform, runtime);
	lwkt_gettoken(&machine->node.token);
	current = machine->launch_vnode == vnode;
	lwkt_reltoken(&machine->node.token);
	vmmfs_machine_runtime_put(machine);
	if (error == 0 && !current)
		error = ECANCELED;
	if (error != 0) {
		cleanup_error = vmmfs_machine_abort(launch);
		if (cleanup_error != 0)
			error = cleanup_error;
		vrele(vnode);
		goto finished;
	}
	*vnodep = vnode;
	goto finished;

rejected:
	/* No published runtime owns these candidates. */
	vmmfs_launch_revoke(launch);
	if (runtime != NULL) {
		cleanup_error = vmm_machine_destroy(runtime);
		if (cleanup_error != 0)
			panic("vmmfs: empty candidate destroy: %d", cleanup_error);
	}
	vmmfs_memory_release(&memory);
	cleanup_error = vmmfs_vnode_deactivate(vnode);
	if (cleanup_error != 0)
		kprintf("vmmfs: rejected launch deactivate: %d\n", cleanup_error);
	vrele(vnode);
finished:
	vrele(vcpu_vnode);
	return (error);
}

static int
vmmfs_machine_start(struct vmmfs_machine *machine, struct ucred *cred)
{
	struct vnode *vnode, *loader_vnode;
	struct vmmfs_launch *launch;
	int error, abort_error;

	/* Cancellation can retire the loader node before fd 3 is delivered. */
	lwkt_gettoken(&machine->node.token);
	loader_vnode = machine->loader_vnode;
	if (loader_vnode != NULL)
		vref(loader_vnode);
	lwkt_reltoken(&machine->node.token);
	if (loader_vnode == NULL)
		return (ENOENT);
	error = vmmfs_machine_boot(machine, &vnode);
	if (error != 0) {
		vrele(loader_vnode);
		return (error);
	}
	launch = vnode->v_data;
	error = vmmfs_loader_run(loader_vnode->v_data, vnode, cred);
	vrele(loader_vnode);
	if (error == 0)
		error = vmmfs_launch_wait(launch);
	else {
		abort_error = vmmfs_machine_abort(launch);
		if (abort_error != 0)
			error = abort_error;
	}
	vrele(vnode);
	return (error);
}



int
vmmfs_machine_abort(struct vmmfs_launch *launch)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)launch->node.parent;
	struct vnode *vnode;
	int error, deactivate_error;
	bool closing;

	lwkt_gettoken(&launch->node.token);
	lwkt_gettoken(&machine->node.token);
	vnode = machine->launch_vnode;
	if (vnode == NULL || vnode->v_data != launch) {
		lwkt_reltoken(&machine->node.token);
		lwkt_reltoken(&launch->node.token);
		return (0);
	}
	vmmfs_node_hold(&machine->events.node);
	machine->launch_vnode = NULL;
	lwkt_reltoken(&machine->node.token);
	lwkt_reltoken(&launch->node.token);
	/* PREPARE has no fd user yet, but may still be constructing devices. */
	vmmfs_machine_runtime_wait(machine);
	vmmfs_launch_revoke(launch);
	error = vmmfs_machine_release_to_stopped(machine);
	lwkt_gettoken(&launch->node.token);
	closing = launch->node.dead;
	lwkt_reltoken(&launch->node.token);
	deactivate_error = closing ? 0 : vmmfs_vnode_deactivate(vnode);
	if (deactivate_error == EBUSY)
		deactivate_error = 0;
	if (error == 0)
		error = deactivate_error;
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_FAILED,
	    "error=%d", error == 0 ? ECANCELED : error);
	vmmfs_launch_complete(launch, error == 0 ? ECANCELED : error);
	vmmfs_node_put(&machine->events.node);
	vrele(vnode);
	return (error);
}

int
vmmfs_machine_run(struct vmmfs_launch *launch)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)launch->node.parent;
	struct vmm_cpustate state;
	struct vnode *vnode;
	vmm_machine_t runtime;
	uint32_t count;
	int error, cleanup_error;
	bool closing;

	lwkt_gettoken(&launch->node.token);
	lwkt_gettoken(&machine->node.token);
	vnode = machine->launch_vnode;
	if (vnode == NULL || vnode->v_data != launch) {
		lwkt_reltoken(&machine->node.token);
		lwkt_reltoken(&launch->node.token);
		return (EPIPE);
	}
	machine->launch_vnode = NULL;
	runtime = machine->machine;
	count = machine->vcpu.count;
	state = launch->cpustate;
	++machine->runtime_references;
	vmmfs_node_hold(&machine->events.node);
	lwkt_reltoken(&machine->node.token);
	/* Consuming the launch identity is the run/abort linearization point. */
	vmmfs_launch_revoke(launch);
	lwkt_reltoken(&launch->node.token);
	error = vmmfs_memory_snapshot(&machine->memory);
	if (error == 0) {
		machine->boot_state = state;
		error = vmmfs_vcpu_start(&machine->vcpu, count, runtime, &state);
	}
	if (error == 0)
		vmmfs_machine_cleanup_stopped(machine);
	vmmfs_machine_runtime_put(machine);
	if (error != 0) {
		cleanup_error = vmmfs_machine_release_to_stopped(machine);
		if (cleanup_error != 0)
			error = cleanup_error;
	}
	lwkt_gettoken(&launch->node.token);
	closing = launch->node.dead;
	lwkt_reltoken(&launch->node.token);
	cleanup_error = closing ? 0 : vmmfs_vnode_deactivate(vnode);
	if (cleanup_error == EBUSY)
		cleanup_error = 0;
	if (error == 0)
		error = cleanup_error;
	vmmfs_events_log(&machine->events, error == 0 ?
	    VMMFS_MACHINE_EVENT_BOOT_COMPLETED : VMMFS_MACHINE_EVENT_BOOT_FAILED,
	    "error=%d", error);
	vmmfs_launch_complete(launch, error);
	vmmfs_node_put(&machine->events.node);
	vrele(vnode);
	return (error);
}

static int
vmmfs_machine_release_runtime(struct vmmfs_machine *machine)
{
	vmm_machine_t runtime_machine;
	int error;

	lwkt_gettoken(&machine->node.token);
	runtime_machine = machine->machine;
	if (runtime_machine == NULL) {
		lwkt_reltoken(&machine->node.token);
		return (0);
	}
	if (machine->runtime_releasing) {
		lwkt_reltoken(&machine->node.token);
		return (EBUSY);
	}
	if (machine->runtime_released) {
		lwkt_reltoken(&machine->node.token);
		return (0);
	}
	machine->runtime_releasing = true;
	lwkt_reltoken(&machine->node.token);
	/* New users are excluded before waiting for borrowed runtime handles. */
	vmmfs_machine_runtime_wait(machine);
	(void)vmmfs_platform_x64_stop(&machine->platform);
	(void)vmmfs_serialroot_stop(&machine->serialroot);
	(void)vmmfs_pciroot_stop(&machine->pciroot);
	(void)vmmfs_rtc_stop(&machine->rtc);
	error = vmm_machine_destroy(runtime_machine);
	if (error != 0) {
		lwkt_gettoken(&machine->node.token);
		machine->runtime_releasing = false;
		lwkt_reltoken(&machine->node.token);
		return (error);
	}
	vmmfs_memory_release(&machine->memory);
	lwkt_gettoken(&machine->node.token);
	KKASSERT(machine->machine == runtime_machine);
	machine->runtime_releasing = false;
	machine->runtime_released = true;
	lwkt_reltoken(&machine->node.token);
	return (0);
}

static int
vmmfs_machine_release_to_stopped(struct vmmfs_machine *machine)
{
	int error;

	error = vmmfs_machine_release_runtime(machine);
	if (error != 0)
		return (error);
	return (vmmfs_machine_create_stopped(machine));
}
void
vmmfs_machine_vcpu_stopped(struct vmmfs_machine *machine)
{
	int error;

	if (machine == NULL)
		return;
	error = vmmfs_machine_release_to_stopped(machine);
	if (error != 0) {
		vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_VCPU_FAILED,
		    "index=0 stopped-publish-error=%d", error);
		return;
	}
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOPPED,
	    "reason=vcpu");
}

int
vmmfs_machine_vcpu_reset(struct vmmfs_machine *machine)
{
	vmm_machine_t old_machine;
	vmm_machine_t runtime_machine;
	struct vmspace *old_vmspace;
	int error;

	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->node.token);
	old_machine = machine->machine;
	lwkt_reltoken(&machine->node.token);
	if (old_machine == NULL)
		return (EINVAL);

	runtime_machine = NULL;
	old_vmspace = NULL;
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_RESET_STARTED,
	    NULL);
	(void)vmmfs_platform_x64_stop(&machine->platform);
	(void)vmmfs_serialroot_stop(&machine->serialroot);
	error = vmmfs_pciroot_reset(&machine->pciroot);
	if (error != 0)
		goto failed;
	(void)vmmfs_rtc_stop(&machine->rtc);
	error = vmmfs_memory_reset_begin(&machine->memory, &old_vmspace);
	if (error != 0)
		goto failed;
	error = vmm_machine_create(machine->memory.run_vmspace, &runtime_machine);
	if (error != 0)
		goto failed;
	error = vmm_machine_create_irqchip(runtime_machine);
	if (error != 0)
		goto failed;
	error = vmm_machine_create_pit(runtime_machine);
	if (error != 0)
		goto failed;
	error = vmmfs_rtc_start(&machine->rtc, runtime_machine);
	if (error != 0)
		goto failed;
	error = vmmfs_pciroot_start(&machine->pciroot, runtime_machine);
	if (error != 0)
		goto failed;
	error = vmmfs_serialroot_start(&machine->serialroot, runtime_machine);
	if (error != 0)
		goto failed;
	error = vmmfs_platform_x64_start(&machine->platform, runtime_machine);
	if (error != 0)
		goto failed;

	/* The old instance remains the BSP finalizer's fallback until this point. */
	error = vmm_machine_destroy(old_machine);
	if (error != 0)
		goto failed;
	vmmfs_memory_reset_commit(&machine->memory, old_vmspace);
	lwkt_gettoken(&machine->vcpu.token);
	machine->vcpu.runtime_machine = runtime_machine;
	lwkt_reltoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->node.token);
	KKASSERT(machine->machine == old_machine);
	machine->machine = runtime_machine;
	lwkt_reltoken(&machine->node.token);
	error = vmmfs_vcpu_reset(&machine->vcpu, runtime_machine,
	    &machine->boot_state);
	if (error != 0)
		return (error);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_RESET_COMPLETED,
	    "phase=rebuild");
	return (0);

failed:
	(void)vmmfs_platform_x64_stop(&machine->platform);
	(void)vmmfs_serialroot_stop(&machine->serialroot);
	(void)vmmfs_pciroot_stop(&machine->pciroot);
	(void)vmmfs_rtc_stop(&machine->rtc);
	if (runtime_machine != NULL)
		(void)vmm_machine_destroy(runtime_machine);
	if (old_vmspace != NULL)
		vmmfs_memory_reset_abort(&machine->memory, old_vmspace);
	return (error);
}
