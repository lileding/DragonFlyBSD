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
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"
#include "vmmfs_platform_x64.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_serialport.h"

#define VMMFS_MACHINE_MODE 0555

static int vmmfs_machine_ncreate(struct vop_ncreate_args *);
static int vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_machine_nremove(struct vop_nremove_args *);
static int vmmfs_machine_nresolve(struct vop_nresolve_args *);
static int vmmfs_machine_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_machine_readdir(struct vop_readdir_args *);
static int vmmfs_machine_start(struct vmmfs_machine *, struct ucred *);
static int vmmfs_machine_prepare_start(struct vmmfs_machine *, char *,
	uint32_t *);
static int vmmfs_machine_release_runtime(struct vmmfs_machine *);
static int vmmfs_machine_create_stopped(struct vmmfs_machine *);
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
vmmfs_machine_create(struct vmmfs_mount *mount, struct vmmfs_branch *parent,
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
	vmmfs_branch_init(&machine->branch, parent, vmmfs_machine_drop);
	machine->branch.node.deactivate = vmmfs_machine_deactivate;
	machine->mount = mount;
	machine->branch.node.inode = vmmfs_root_allocate_inode(root);
	machine->branch.node.mode = VMMFS_MACHINE_MODE;
	machine->branch.node.size = 0;
	bcopy(name, machine->name, namelen);
	machine->name[namelen] = 0;
	error = vmmfs_machine_id_init(mount, &machine->branch, &machine->id_node,
	    &machine->id_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_vcpu_init(mount, &machine->branch, &machine->vcpu,
	    &machine->vcpu_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_memory_init(mount, &machine->branch, &machine->memory,
	    &machine->memory_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_loader_init(mount, &machine->branch, &machine->loader,
	    &machine->loader_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_boot_init(mount, &machine->branch, &machine->boot,
	    &machine->boot_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_machine_create_stopped(machine);
	if (error != 0)
		goto fail;
	error = vmmfs_pciroot_init(mount, &machine->branch, &machine->pciroot,
	    &machine->pciroot_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_platform_x64_init(machine, &machine->platform);
	if (error != 0)
		goto fail;
	error = vmmfs_rtc_init(machine, &machine->rtc);
	if (error != 0)
		goto fail;
	error = vmmfs_serialroot_init(mount, &machine->branch, &machine->serialroot,
	    &machine->serialroot_vnode);
	if (error != 0)
		goto fail;
	error = vmmfs_events_init(mount, &machine->branch, &machine->events,
	    &machine->events_vnode);
	if (error != 0)
		goto fail;
	state = machine->mount;
	if (state == NULL || state->machine_vops == NULL) {
		error = ENXIO;
		goto fail;
	}
	error = vmmfs_vnode_create_regular(state->mount, &state->machine_vops,
	    VDIR, &machine->branch.node, &vnode);
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
	lwkt_gettoken(&machine->branch.token);
	machine->branch.node.dead = true;
	lwkt_reltoken(&machine->branch.token);
	if (machine->events.node.drop != NULL) {
		vmmfs_vnode_discard(machine->events_vnode);
		machine->events_vnode = NULL;
		vmmfs_node_drop(&machine->events.node);
	}
	if (machine->serialroot.branch.node.drop != NULL) {
		vmmfs_vnode_discard(machine->serialroot_vnode);
		machine->serialroot_vnode = NULL;
		vmmfs_node_drop(&machine->serialroot.branch.node);
	}
	if (machine->rtc.machine != NULL)
		vmmfs_rtc_fini(&machine->rtc);
	if (machine->platform.machine != NULL)
		vmmfs_platform_x64_fini(&machine->platform);
	if (machine->pciroot.branch.node.drop != NULL) {
		vmmfs_vnode_discard(machine->pciroot_vnode);
		machine->pciroot_vnode = NULL;
		vmmfs_node_drop(&machine->pciroot.branch.node);
	}
	vmmfs_machine_cleanup_stopped(machine);
	if (machine->boot.node.drop != NULL) {
		vmmfs_vnode_discard(machine->boot_vnode);
		machine->boot_vnode = NULL;
		vmmfs_node_drop(&machine->boot.node);
	}
	if (machine->loader.node.drop != NULL) {
		vmmfs_vnode_discard(machine->loader_vnode);
		machine->loader_vnode = NULL;
		vmmfs_node_drop(&machine->loader.node);
	}
	if (machine->memory.node.drop != NULL) {
		vmmfs_vnode_discard(machine->memory_vnode);
		machine->memory_vnode = NULL;
		vmmfs_node_drop(&machine->memory.node);
	}
	if (machine->vcpu.node.drop != NULL) {
		vmmfs_vnode_discard(machine->vcpu_vnode);
		machine->vcpu_vnode = NULL;
		vmmfs_node_drop(&machine->vcpu.node);
	}
	if (machine->id_node.node.drop != NULL) {
		vmmfs_vnode_discard(machine->id_vnode);
		machine->id_vnode = NULL;
		vmmfs_node_drop(&machine->id_node.node);
	}
	vmmfs_vnode_discard(vnode);
	vmmfs_node_drop(&machine->branch.node);
}

static int
vmmfs_machine_create_stopped(struct vmmfs_machine *machine)
{
	struct vmmfs_stopped *stopped;
	struct vnode *stopped_vnode;
	int error;

	error = vmmfs_stopped_create(machine->mount, &machine->branch, &stopped_vnode);
	if (error != 0)
		return (error);
	stopped = stopped_vnode->v_data;
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead || machine->machine != NULL) {
		lwkt_reltoken(&machine->branch.token);
		vmmfs_vnode_discard(stopped_vnode);
		vmmfs_node_drop(&stopped->node);
		return (EBUSY);
	}
	if (machine->stopped != NULL) {
		lwkt_reltoken(&machine->branch.token);
		vmmfs_vnode_discard(stopped_vnode);
		vmmfs_node_drop(&stopped->node);
		return (0);
	}
	machine->stopped = stopped;
	machine->stopped_vnode = stopped_vnode;
	lwkt_reltoken(&machine->branch.token);
	vmmfs_machine_invalidate_children(machine);
	return (0);
}

static void
vmmfs_machine_cleanup_stopped(struct vmmfs_machine *machine)
{
	struct vmmfs_stopped *stopped;
	struct vnode *vnode;

	if (machine == NULL)
		return;
	lwkt_gettoken(&machine->branch.token);
	stopped = machine->stopped;
	vnode = machine->stopped_vnode;
	machine->stopped = NULL;
	machine->stopped_vnode = NULL;
	lwkt_reltoken(&machine->branch.token);
	if (stopped == NULL)
		return;
	vmmfs_vnode_discard(vnode);
	vmmfs_node_drop(&stopped->node);
}

static int
vmmfs_machine_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_machine *machine;
	struct vmmfs_stopped *stopped;
	struct vnode *events_vnode;
	struct vnode *stopped_vnode;
	struct vnode *boot_vnode;
	struct vnode *loader_vnode;
	struct vnode *memory_vnode;
	struct vnode *vcpu_vnode;
	struct vnode *id_vnode;
	struct vnode *serialroot_vnode;
	struct vnode *pciroot_vnode;

	machine = (struct vmmfs_machine *)node;
	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead) {
		lwkt_reltoken(&machine->branch.token);
		return (0);
	}
	if (machine->machine != NULL) {
		lwkt_reltoken(&machine->branch.token);
		return (EBUSY);
	}
	(void)vmmfs_node_default_deactivate(&machine->branch.node);
	(void)vmmfs_node_default_deactivate(&machine->id_node.node);
	(void)vmmfs_node_default_deactivate(&machine->vcpu.node);
	(void)vmmfs_node_default_deactivate(&machine->memory.node);
	(void)vmmfs_node_default_deactivate(&machine->loader.node);
	(void)vmmfs_node_default_deactivate(&machine->boot.node);
	(void)vmmfs_node_default_deactivate(&machine->events.node);
	KKASSERT(machine->pciroot.branch.node.deactivate(
	    &machine->pciroot.branch.node) == 0);
	KKASSERT(machine->serialroot.branch.node.deactivate(
	    &machine->serialroot.branch.node) == 0);
	if (machine->stopped != NULL)
		(void)vmmfs_node_default_deactivate(&machine->stopped->node);
	lwkt_reltoken(&machine->branch.token);
	vmmfs_machine_invalidate_children(machine);
	vmmfs_boot_revoke(&machine->boot);
	vmmfs_events_revoke(&machine->events);

	/* Detach every parent-owned vnode Arc before any VFS operation. */
	lwkt_gettoken(&machine->branch.token);
	events_vnode = machine->events_vnode;
	machine->events_vnode = NULL;
	stopped = machine->stopped;
	stopped_vnode = machine->stopped_vnode;
	machine->stopped = NULL;
	machine->stopped_vnode = NULL;
	boot_vnode = machine->boot_vnode;
	machine->boot_vnode = NULL;
	loader_vnode = machine->loader_vnode;
	machine->loader_vnode = NULL;
	memory_vnode = machine->memory_vnode;
	machine->memory_vnode = NULL;
	vcpu_vnode = machine->vcpu_vnode;
	machine->vcpu_vnode = NULL;
	id_vnode = machine->id_vnode;
	machine->id_vnode = NULL;
	serialroot_vnode = machine->serialroot_vnode;
	machine->serialroot_vnode = NULL;
	pciroot_vnode = machine->pciroot_vnode;
	machine->pciroot_vnode = NULL;
	machine->vnode = NULL;
	lwkt_reltoken(&machine->branch.token);

	vmmfs_pciroot_deactivate_slots(&machine->pciroot);
	vmmfs_serialroot_deactivate_ports(&machine->serialroot);
	KKASSERT(vmmfs_vnode_deactivate(events_vnode) == 0);
	vrele(events_vnode);
	if (stopped != NULL) {
		KKASSERT(vmmfs_vnode_deactivate(stopped_vnode) == 0);
		vrele(stopped_vnode);
	}
	KKASSERT(vmmfs_vnode_deactivate(boot_vnode) == 0);
	vrele(boot_vnode);
	KKASSERT(vmmfs_vnode_deactivate(loader_vnode) == 0);
	vrele(loader_vnode);
	KKASSERT(vmmfs_vnode_deactivate(memory_vnode) == 0);
	vrele(memory_vnode);
	KKASSERT(vmmfs_vnode_deactivate(vcpu_vnode) == 0);
	vrele(vcpu_vnode);
	KKASSERT(vmmfs_vnode_deactivate(id_vnode) == 0);
	vrele(id_vnode);
	KKASSERT(vmmfs_vnode_deactivate(serialroot_vnode) == 0);
	vrele(serialroot_vnode);
	KKASSERT(vmmfs_vnode_deactivate(pciroot_vnode) == 0);
	vrele(pciroot_vnode);
	return (0);
}

static void
vmmfs_machine_drop(struct vmmfs_node *node)
{
	struct vmmfs_machine *machine;

	machine = (struct vmmfs_machine *)node;
	KKASSERT(machine != NULL);
	KKASSERT(machine->branch.node.dead);
	KKASSERT(machine->branch.references == 0);
	KKASSERT(machine->id_node.node.drop == NULL);
	KKASSERT(machine->vcpu.node.drop == NULL);
	KKASSERT(machine->memory.node.drop == NULL);
	KKASSERT(machine->loader.node.drop == NULL);
	KKASSERT(machine->boot.node.drop == NULL);
	KKASSERT(machine->stopped == NULL);
	KKASSERT(machine->events.node.drop == NULL);
	KKASSERT(machine->pciroot.branch.node.drop == NULL);
	KKASSERT(machine->serialroot.branch.node.drop == NULL);
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
	lwkt_gettoken(&machine->branch.token);
	vnode = machine->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->branch.token);
	if (vnode == NULL)
		return;
	cache_inval_vp(vnode, CINV_CHILDREN);
	vdrop(vnode);
}


int
vmmfs_machine_request_stop(struct vmmfs_machine *machine, const char *reason)
{
	bool running;
	bool boot_pending;

	if (machine == NULL || reason == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead) {
		lwkt_reltoken(&machine->branch.token);
		return (ENOENT);
	}
	running = machine->machine != NULL;
	lwkt_reltoken(&machine->branch.token);
	if (!running)
		return (0);
	boot_pending = vmmfs_boot_is_active(&machine->boot);
	if (boot_pending) {
		vmmfs_events_log(&machine->events,
		    VMMFS_MACHINE_EVENT_STOP_REQUESTED, "reason=%s", reason);
		return (vmmfs_machine_release_runtime(machine));
	}
	vmmfs_vcpu_request_stop(&machine->vcpu);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOP_REQUESTED,
	    "reason=%s", reason);
	return (0);
}

int
vmmfs_machine_reset(struct vmmfs_machine *machine)
{
	bool running;

	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead) {
		lwkt_reltoken(&machine->branch.token);
		return (ENOENT);
	}
	running = machine->machine != NULL;
	lwkt_reltoken(&machine->branch.token);
	if (!running)
		return (EBUSY);
	vmmfs_vcpu_request_reset(&machine->vcpu);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_RESET_REQUESTED,
	    "reason=external");
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
	error = vmmfs_machine_request_stop(machine, "external");
	if (error != 0)
		return (error);
	error = vmmfs_machine_create_stopped(machine);
	if (error != 0)
		return (error);
	lwkt_gettoken(&machine->branch.token);
	vnode = machine->stopped_vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->branch.token);
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
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead) {
		lwkt_reltoken(&machine->branch.token);
		return (ENOENT);
	}
	vnode = mount->root_vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->branch.token);
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
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead) {
		lwkt_reltoken(&machine->branch.token);
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
		vnode = machine->machine == NULL ? machine->stopped_vnode : NULL;
	else {
		lwkt_reltoken(&machine->branch.token);
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->branch.token);
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
	struct vmmfs_machine *machine;
	struct namecache *ncp;
	int error;

	machine = ap->a_dvp->v_data;
	if (machine == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	if (ncp->nc_nlen != sizeof("stopped") - 1 ||
	    bcmp(ncp->nc_name, "stopped", sizeof("stopped") - 1) != 0)
		return (EOPNOTSUPP);
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead || machine->machine != NULL || machine->stopped == NULL) {
		lwkt_reltoken(&machine->branch.token);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->branch.token);
	error = vmmfs_machine_start(machine, ap->a_cred);
	if (error != 0)
		return (error);
	cache_unlink(ap->a_nch);
	return (0);
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
	struct vmmfs_stopped *stopped;
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
	    machine->branch.node.dead)
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
		stop = vop_write_dirent(&error, uio, machine->branch.node.inode, DT_DIR, 1,
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
		lwkt_gettoken(&machine->branch.token);
		stopped = machine->stopped;
		present = stopped != NULL;
		inode = present ? stopped->node.inode : 0;
		lwkt_reltoken(&machine->branch.token);
		if (present) {
			stop = vop_write_dirent(&error, uio, inode, DT_REG,
			    sizeof("stopped") - 1, "stopped");
		}
		if (!stop)
			offset = 9;
	}
	if (!stop && offset == 9) {
		lwkt_gettoken(&machine->branch.token);
		inode = machine->pciroot.branch.node.inode;
		lwkt_reltoken(&machine->branch.token);
		stop = vop_write_dirent(&error, uio, inode, DT_DIR,
		    sizeof("pci") - 1, "pci");
		if (!stop)
			offset = 10;
	}
	if (!stop && offset == 10) {
		lwkt_gettoken(&machine->branch.token);
		inode = machine->serialroot.branch.node.inode;
		lwkt_reltoken(&machine->branch.token);
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


static int
vmmfs_machine_prepare_start(struct vmmfs_machine *machine,
	char *loader_script, uint32_t *vcpu_countp)
{
	vmm_machine_t runtime_machine;
	struct vmmfs_stopped *stopped;
	struct vnode *stopped_vnode;
	uint64_t memory_size;
	uint32_t vcpu_count;
	bool stop_requested;
	int error;
	int release_error;

	if (machine == NULL)
		return (EINVAL);
	runtime_machine = NULL;
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead || machine->machine != NULL || machine->stopped == NULL) {
		lwkt_reltoken(&machine->branch.token);
		return (EBUSY);
	}
	memory_size = machine->memory.size;
	vcpu_count = machine->vcpu.count;
	if (loader_script != NULL)
		bcopy(machine->loader.script, loader_script,
		    sizeof(machine->loader.script));
	if (memory_size == 0 || vcpu_count == 0 ||
	    (loader_script != NULL && loader_script[0] == '\0')) {
		lwkt_reltoken(&machine->branch.token);
		return (EINVAL);
	}

	/*
	 * The running instance pointer is the sole topology write gate.  Keep the
	 * machine token from the stopped topology snapshot through its publication,
	 * so no VOP can change the topology between these two operations.
	 */
	error = vmmfs_memory_prepare(&machine->memory, memory_size);
	if (error != 0)
		goto failed_locked;
	error = vmm_machine_create(machine->memory.run_vmspace, &runtime_machine);
	if (error != 0)
		goto failed_locked;
	machine->machine = runtime_machine;
	stopped = machine->stopped;
	stopped_vnode = machine->stopped_vnode;
	machine->stopped = NULL;
	machine->stopped_vnode = NULL;
	lwkt_reltoken(&machine->branch.token);
	KKASSERT(vmmfs_vnode_deactivate(stopped_vnode) == 0);
	vrele(stopped_vnode);
	vmmfs_machine_invalidate_children(machine);

	error = vmmfs_memory_map(&machine->memory);
	if (error != 0)
		goto failed;
	error = vmm_machine_create_irqchip(runtime_machine);
	if (error != 0)
		goto failed;
	error = vmm_machine_create_pit(runtime_machine);
	if (error != 0)
		goto failed;
	error = vmmfs_platform_x64_prepare(&machine->platform,
	    &machine->memory, vcpu_count, &machine->pciroot,
	    &machine->serialroot);
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
	/*
	 * Both named boot and loader fd3 use this one session.  Do not expose it
	 * until platform construction has completed.
	 */
	lwkt_gettoken(&machine->branch.token);
	lwkt_gettoken(&machine->vcpu.token);
	stop_requested = machine->vcpu.stop_requested;
	lwkt_reltoken(&machine->vcpu.token);
	if (!stop_requested) {
		error = vmmfs_boot_arm_locked(&machine->boot,
		    machine->memory.object, memory_size);
	}
	lwkt_reltoken(&machine->branch.token);
	if (stop_requested) {
		error = EINTR;
		goto failed;
	}
	if (error != 0)
		goto failed;
	if (vcpu_countp != NULL)
		*vcpu_countp = vcpu_count;
	return (0);

failed_locked:
	lwkt_reltoken(&machine->branch.token);
failed:
	release_error = vmmfs_machine_release_runtime(machine);
	if (release_error != 0)
		error = release_error;
	return (error);
}

static int
vmmfs_machine_start(struct vmmfs_machine *machine, struct ucred *cred)
{
	char *loader_script;
	int error;
	int release_error;

	if (machine == NULL || cred == NULL)
		return (EINVAL);
	loader_script = kmalloc(sizeof(machine->loader.script), M_VMMFS,
	    M_WAITOK);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_START_REQUESTED,
	    NULL);
	error = vmmfs_machine_prepare_start(machine, loader_script, NULL);
	if (error != 0)
		goto failed;
	error = vmmfs_loader_run(&machine->loader, loader_script,
	    &machine->boot, cred);
	if (error != 0)
		goto failed_runtime;
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_START_COMPLETED,
	    NULL);
	kfree(loader_script, M_VMMFS);
	return (0);

failed_runtime:
	release_error = vmmfs_machine_release_runtime(machine);
	if (release_error != 0)
		error = release_error;
failed:
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_START_FAILED,
	    "error=%d", error);
	kfree(loader_script, M_VMMFS);
	return (error);
}

int
vmmfs_machine_boot_start(struct vmmfs_machine *machine)
{
	int error;

	if (machine == NULL)
		return (EINVAL);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_REQUESTED,
	    NULL);
	error = vmmfs_machine_prepare_start(machine, NULL, NULL);
	if (error != 0) {
		vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_FAILED,
		    "error=%d", error);
		return (error);
	}
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_READY, NULL);
	return (0);
}

int
vmmfs_machine_boot_abort(struct vmmfs_machine *machine)
{
	bool running;
	int error;

	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead) {
		lwkt_reltoken(&machine->branch.token);
		return (ENOENT);
	}
	running = machine->machine != NULL;
	lwkt_reltoken(&machine->branch.token);
	if (!running)
		return (0);
	error = vmmfs_machine_release_runtime(machine);
	if (error != 0)
		return (error);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_FAILED,
	    "error=%d", EPIPE);
	return (0);
}

int
vmmfs_machine_boot_submit(struct vmmfs_machine *machine,
	const struct vmm_cpustate *state)
{
	vmm_machine_t runtime_machine;
	uint32_t vcpu_count;
	int error;
	int release_error;

	if (machine == NULL || state == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->branch.token);
	if (machine->branch.node.dead || machine->machine == NULL) {
		lwkt_reltoken(&machine->branch.token);
		return (EPIPE);
	}
	runtime_machine = machine->machine;
	vcpu_count = machine->vcpu.count;
	lwkt_reltoken(&machine->branch.token);
	error = vmmfs_memory_snapshot(&machine->memory);
	if (error != 0)
		goto failed;
	machine->boot_state = *state;
	error = vmmfs_vcpu_start(&machine->vcpu, vcpu_count, runtime_machine,
	    state);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_COMPLETED,
	    NULL);
	return (0);

failed:
	release_error = vmmfs_machine_release_runtime(machine);
	if (release_error != 0)
		error = release_error;
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_FAILED,
	    "error=%d", error);
	return (error);
}

static int
vmmfs_machine_release_runtime(struct vmmfs_machine *machine)
{
	vmm_machine_t runtime_machine;
	int error;

	lwkt_gettoken(&machine->branch.token);
	runtime_machine = machine->machine;
	lwkt_reltoken(&machine->branch.token);
	vmmfs_boot_revoke(&machine->boot);
	(void)vmmfs_platform_x64_stop(&machine->platform);
	(void)vmmfs_serialroot_stop(&machine->serialroot);
	(void)vmmfs_pciroot_stop(&machine->pciroot);
	(void)vmmfs_rtc_stop(&machine->rtc);
	if (runtime_machine != NULL) {
		error = vmm_machine_destroy(runtime_machine);
		if (error != 0) {
			return (error);
		}
	}
	vmmfs_memory_release(&machine->memory);
	/* A pre-vCPU stop has no worker to clear these request bits. */
	lwkt_gettoken(&machine->vcpu.token);
	if (machine->vcpu.active_count == 0 && machine->vcpu.threads == NULL) {
		machine->vcpu.stop_requested = false;
		machine->vcpu.reset_requested = false;
	}
	lwkt_reltoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->branch.token);
	KKASSERT(machine->machine == runtime_machine);
	machine->machine = NULL;
	lwkt_reltoken(&machine->branch.token);
	error = vmmfs_machine_create_stopped(machine);
	if (error != 0)
		return (error);
	return (0);
}

void
vmmfs_machine_vcpu_stopped(struct vmmfs_machine *machine)
{
	int error;

	if (machine == NULL)
		return;
	error = vmmfs_machine_release_runtime(machine);
	KKASSERT(error == 0);
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
	lwkt_gettoken(&machine->branch.token);
	old_machine = machine->machine;
	lwkt_reltoken(&machine->branch.token);
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
	lwkt_gettoken(&machine->branch.token);
	KKASSERT(machine->machine == old_machine);
	machine->machine = runtime_machine;
	lwkt_reltoken(&machine->branch.token);
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
