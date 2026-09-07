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

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>

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
static int vmmfs_machine_nremove(struct vop_nremove_args *);
static int vmmfs_machine_nresolve(struct vop_nresolve_args *);
static int vmmfs_machine_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_machine_prepare_stopped(struct vmmfs_machine *);
static int vmmfs_machine_readdir(struct vop_readdir_args *);
static int vmmfs_machine_release_runtime(struct vmmfs_machine *);
static int vmmfs_machine_release_to_stopped(struct vmmfs_machine *);
static int vmmfs_machine_create_stopped(struct vmmfs_machine *);
static void vmmfs_machine_runtime_put(struct vmmfs_machine *);
static void vmmfs_machine_runtime_wait(struct vmmfs_machine *);
static void vmmfs_machine_cleanup_stopped(struct vmmfs_machine *);
static void vmmfs_machine_drop(struct vmmfs_node *);
static bool vmmfs_machine_deactivate(struct vmmfs_node *);

struct vop_ops vmmfs_machine_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_ncreate = vmmfs_machine_ncreate,
	.vop_nlookupdotdot = vmmfs_node_nlookupdotdot,
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
vmmfs_machine_create(struct vmmfs_node *parent,
	const char *name, size_t namelen, struct vmmfs_machine **objectp)
{
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	int error;

	if (parent == NULL || name == NULL || objectp == NULL || namelen == 0 ||
	    namelen > NAME_MAX)
		return (EINVAL);
	root = (struct vmmfs_root *)parent;
	*objectp = NULL;
	machine = kmalloc(sizeof(*machine), M_VMMFS, M_WAITOK | M_ZERO);
	machine->node.parent = parent;
	machine->node.mount = parent->mount;
	machine->node.references = 1;
	lwkt_token_init(&machine->token, "vmmfsnode");
	lockinit(&machine->node.lock, "vmmfsnode", 0, 0);
	machine->node.drop = vmmfs_machine_drop;
	vmmfs_node_hold(parent);
	machine->node.inode = vmmfs_root_allocate_inode(root);
	machine->node.mode = VMMFS_MACHINE_MODE;
	machine->node.size = 0;
	bcopy(name, machine->name, namelen);
	machine->name[namelen] = 0;
	error = vmmfs_machine_id_init(&machine->node, &machine->id_node);
	if (error != 0)
		goto fail;
	error = vmmfs_vcpu_init(&machine->node, &machine->vcpu);
	if (error != 0)
		goto fail;
	error = vmmfs_memory_init(&machine->node, &machine->memory);
	if (error != 0)
		goto fail;
	error = vmmfs_loader_init(&machine->node, &machine->loader);
	if (error != 0)
		goto fail;
	error = vmmfs_boot_init(&machine->node, &machine->boot);
	if (error != 0)
		goto fail;
	error = vmmfs_machine_create_stopped(machine);
	if (error != 0)
		goto fail;
	error = vmmfs_pciroot_init(&machine->node, &machine->pciroot);
	if (error != 0)
		goto fail;
	error = vmmfs_platform_x64_init(machine, &machine->platform);
	if (error != 0)
		goto fail;
	error = vmmfs_rtc_init(machine, &machine->rtc);
	if (error != 0)
		goto fail;
	error = vmmfs_serialroot_init(&machine->node, &machine->serialroot);
	if (error != 0)
		goto fail;
	error = vmmfs_events_init(&machine->node, &machine->events);
	if (error != 0)
		goto fail;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->machine_vops, VDIR, &machine->node);
	if (error != 0)
		goto fail;
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_CREATED, NULL);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOPPED,
	    "reason=create");
	machine->node.deactivate = vmmfs_machine_deactivate;
	*objectp = machine;
	return (0);

fail:
	(void)vmmfs_node_deactivate(&machine->events.node);
	(void)vmmfs_node_deactivate(&machine->serialroot.node);
	if (machine->rtc.machine != NULL)
		vmmfs_rtc_fini(&machine->rtc);
	if (machine->platform.machine != NULL)
		vmmfs_platform_x64_fini(&machine->platform);
	(void)vmmfs_node_deactivate(&machine->pciroot.node);
	vmmfs_machine_cleanup_stopped(machine);
	(void)vmmfs_node_deactivate(&machine->boot.node);
	(void)vmmfs_node_deactivate(&machine->loader.node);
	(void)vmmfs_node_deactivate(&machine->memory.node);
	(void)vmmfs_node_deactivate(&machine->vcpu.node);
	(void)vmmfs_node_deactivate(&machine->id_node.node);
	vmmfs_node_put(&machine->node);
	return (error);
}

static int
vmmfs_machine_prepare_stopped(struct vmmfs_machine *machine)
{
	struct vmmfs_stopped *stopped;
	int error;

	error = vmmfs_stopped_create(&machine->node,
	    &stopped);
	if (error != 0)
		return (error);
	lwkt_gettoken(&machine->token);
	if (machine->runtime_releasing) {
		lwkt_reltoken(&machine->token);
		(void)vmmfs_node_deactivate(&stopped->node);
		return (EBUSY);
	}
	if (machine->stopped != NULL) {
		lwkt_reltoken(&machine->token);
		(void)vmmfs_node_deactivate(&stopped->node);
		return (0);
	}
	machine->stopped = stopped;
	lwkt_reltoken(&machine->token);
	return (0);
}

static int
vmmfs_machine_create_stopped(struct vmmfs_machine *machine)
{
	struct vmmfs_stopped *stopped;
	struct vnode *vnode;
	int error;

	/* Cleanup may finish during a veto, but must exclude successful close. */
	error = lockmgr(&machine->node.lock, LK_SHARED);
	if (error != 0)
		return (error);
	lwkt_gettoken(&machine->token);
	if (machine->node.dead && !machine->runtime_released) {
		lwkt_reltoken(&machine->token);
		lockmgr(&machine->node.lock, LK_RELEASE);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->token);
	error = vmmfs_stopped_create(&machine->node, &stopped);
	if (error != 0) {
		lockmgr(&machine->node.lock, LK_RELEASE);
		return (error);
	}
	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->token);
	if (machine->runtime_releasing ||
	    (machine->machine != NULL && !machine->runtime_released)) {
		error = EBUSY;
		goto done;
	}
	if (machine->stopped == NULL) {
		machine->stopped = stopped;
		stopped = NULL;
	}
	/* A stop before VCPU startup has no worker to consume requests. */
	if (machine->vcpu.threads == NULL) {
		machine->vcpu.stop_requested = false;
		machine->vcpu.reset_requested = false;
	}
	machine->machine = NULL;
	machine->runtime_releasing = false;
	machine->runtime_released = false;
done:
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&machine->vcpu.token);
	(void)vmmfs_node_deactivate((struct vmmfs_node *)stopped);
	/* Namecache invalidation must not retain the lifecycle lock. */
	lockmgr(&machine->node.lock, LK_RELEASE);
	if (error == 0) {
		lwkt_gettoken(&machine->token);
		vnode = machine->node.vnode;
		if (vnode != NULL)
			vhold(vnode);
		lwkt_reltoken(&machine->token);
		if (vnode != NULL) {
			cache_inval_vp(vnode, CINV_CHILDREN);
			vdrop(vnode);
		}
	}
	return (error);
}

static void
vmmfs_machine_cleanup_stopped(struct vmmfs_machine *machine)
{
	struct vmmfs_stopped *stopped;

	lwkt_gettoken(&machine->token);
	stopped = machine->stopped;
	machine->stopped = NULL;
	lwkt_reltoken(&machine->token);
	(void)vmmfs_node_deactivate((struct vmmfs_node *)stopped);
}

static bool
vmmfs_machine_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)node;

	if (machine->machine != NULL) {
		return (false);
	}

	/* With no runtime, child nodes complete closure without veto. */
	(void)vmmfs_node_deactivate(&machine->id_node.node);

	(void)vmmfs_node_deactivate(&machine->vcpu.node);

	(void)vmmfs_node_deactivate(&machine->memory.node);

	(void)vmmfs_node_deactivate(&machine->loader.node);

	(void)vmmfs_node_deactivate(&machine->boot.node);

	(void)vmmfs_node_deactivate(&machine->stopped->node);

	(void)vmmfs_node_deactivate(&machine->pciroot.node);

	(void)vmmfs_node_deactivate(&machine->serialroot.node);

	(void)vmmfs_node_deactivate(&machine->events.node);
	return (true);
}

static void
vmmfs_machine_drop(struct vmmfs_node *node)
{
	struct vmmfs_machine *machine;

	machine = (struct vmmfs_machine *)node;
	KKASSERT(machine != NULL);
	KKASSERT(machine->node.references == 0);
	KKASSERT(machine->id_node.node.drop == NULL);
	KKASSERT(machine->vcpu.node.drop == NULL);
	KKASSERT(machine->memory.node.drop == NULL);
	KKASSERT(machine->loader.node.drop == NULL);
	KKASSERT(machine->boot.node.drop == NULL);
	KKASSERT(machine->events.node.drop == NULL);
	KKASSERT(machine->pciroot.node.drop == NULL);
	KKASSERT(machine->serialroot.node.drop == NULL);
	KKASSERT(machine->pciroot.runtime_machine == NULL);
	KKASSERT(machine->node.vnode == NULL);
	if (machine->rtc.machine != NULL)
		vmmfs_rtc_fini(&machine->rtc);
	if (machine->platform.machine != NULL)
		vmmfs_platform_x64_fini(&machine->platform);
	lwkt_token_uninit(&machine->token);
	kfree(machine, M_VMMFS);
}

static void
vmmfs_machine_runtime_put(struct vmmfs_machine *machine)
{
	bool wake;

	KKASSERT(machine != NULL);
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->runtime_references != 0);
	wake = --machine->runtime_references == 0;
	lwkt_reltoken(&machine->token);
	if (wake)
		wakeup(machine);
}

static void
vmmfs_machine_runtime_wait(struct vmmfs_machine *machine)
{
	KKASSERT(machine != NULL);
	for (;;) {
		tsleep_interlock(machine, 0);
		lwkt_gettoken(&machine->token);
		if (machine->runtime_references == 0) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			lwkt_reltoken(&machine->token);
			return;
		}
		lwkt_reltoken(&machine->token);
		(void)tsleep(machine, PINTERLOCKED, "vmmfsrt", 0);
	}
}

int
vmmfs_machine_request_stop(struct vmmfs_machine *machine, const char *reason)
{
	struct vnode *vnode;
	bool running, released;
	int error = 0;

	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->token);
	vnode = machine->launch != NULL ? machine->launch->node.vnode : NULL;
	if (vnode != NULL)
		vref(vnode);
	running = vnode == NULL && machine->machine != NULL &&
	    !machine->runtime_releasing && !machine->runtime_released;
	/* No launch exists yet: PREPARE consumes the cancellation request. */
	if (running && machine->vcpu.threads == NULL) {
		machine->vcpu.stop_requested = true;
		machine->vcpu.reset_requested = false;
		running = false;
	}
	released = machine->runtime_released;
	/* Keep this request attached to the runtime admitted above. */
	if (running)
		++machine->runtime_references;
	vmmfs_node_hold(&machine->vcpu.node);
	vmmfs_node_hold(&machine->events.node);
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&machine->vcpu.token);
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
	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->token);
	if (machine->vcpu.threads == NULL || machine->machine == NULL ||
	    machine->launch != NULL || machine->runtime_releasing ||
	    machine->runtime_released) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&machine->vcpu.token);
		return (EBUSY);
	}
	++machine->runtime_references;
	vmmfs_node_hold(&machine->vcpu.node);
	vmmfs_node_hold(&machine->events.node);
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&machine->vcpu.token);
	vmmfs_vcpu_request_reset(&machine->vcpu);
	vmmfs_machine_runtime_put(machine);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_RESET_REQUESTED,
	    "reason=external");
	vmmfs_node_put(&machine->vcpu.node);
	vmmfs_node_put(&machine->events.node);
	return (0);
}


static int
vmmfs_machine_touch_stopped(struct vmmfs_machine *machine,
	struct vnode **vnodep)
{
	struct vnode *vnode;
	int error;

	error = vmmfs_machine_prepare_stopped(machine);
	if (error != 0)
		return (error);
	error = vmmfs_machine_request_stop(machine, "external");
	if (error != 0)
		return (error);
	lwkt_gettoken(&machine->token);
	vnode = machine->stopped != NULL ? machine->stopped->node.vnode : NULL;
	if (vnode != NULL)
		vref(vnode);
	lwkt_reltoken(&machine->token);
	if (vnode == NULL)
		return (ENOENT);
	*vnodep = vnode;
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
	error = VMMFS_WORK(machine,
	    vmmfs_machine_touch_stopped(machine, &vnode));
	if (error != 0)
		return (error);
	error = vn_lock(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		vrele(vnode);
		return (error);
	}
	cache_setunresolved(ap->a_nch);
	*ap->a_vpp = vnode;
	return (0);
}

static int
vmmfs_machine_get_item(struct vmmfs_machine *machine,
	const char *name, size_t length, struct vnode **vnodep)
{
	struct vnode *vnode;

	if (length == sizeof("id") - 1 &&
	    bcmp(name, "id", sizeof("id") - 1) == 0)
		vnode = machine->id_node.node.vnode;
	else if (length == sizeof("vcpu") - 1 &&
	    bcmp(name, "vcpu", sizeof("vcpu") - 1) == 0)
		vnode = machine->vcpu.node.vnode;
	else if (length == sizeof("mem") - 1 &&
	    bcmp(name, "mem", sizeof("mem") - 1) == 0)
		vnode = machine->memory.node.vnode;
	else if (length == sizeof("loader") - 1 &&
	    bcmp(name, "loader", sizeof("loader") - 1) == 0)
		vnode = machine->loader.node.vnode;
	else if (length == sizeof("boot") - 1 &&
	    bcmp(name, "boot", sizeof("boot") - 1) == 0)
		vnode = machine->boot.node.vnode;
	else if (length == sizeof("events") - 1 &&
	    bcmp(name, "events", sizeof("events") - 1) == 0)
		vnode = machine->events.node.vnode;
	else if (length == sizeof("pci") - 1 &&
	    bcmp(name, "pci", sizeof("pci") - 1) == 0)
		vnode = machine->pciroot.node.vnode;
	else if (length == sizeof("serial") - 1 &&
	    bcmp(name, "serial", sizeof("serial") - 1) == 0)
		vnode = machine->serialroot.node.vnode;
	else if (length == sizeof("stopped") - 1 &&
	    bcmp(name, "stopped", sizeof("stopped") - 1) == 0) {
		lwkt_gettoken(&machine->token);
		vnode = machine->machine == NULL || machine->launch != NULL ?
		    (machine->stopped != NULL ? machine->stopped->node.vnode : NULL) : NULL;
		if (vnode != NULL)
			vref(vnode);
		lwkt_reltoken(&machine->token);
		if (vnode == NULL)
			return (ENOENT);
		*vnodep = vnode;
		return (0);
	} else {
		return (ENOENT);
	}
	vref(vnode);
	*vnodep = vnode;
	return (0);
}

static int
vmmfs_machine_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_machine *machine = ap->a_dvp->v_data;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vnode *vnode;
	int error;

	error = VMMFS_WORK(machine, vmmfs_machine_get_item(machine,
	    ncp->nc_name, ncp->nc_nlen, &vnode));
	if (error != 0) {
		cache_setvp(ap->a_nch, NULL);
		return (error);
	}
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
	struct vmmfs_launch *launch;
	int error, abort_error;

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
	error = VMMFS_WORK(machine, vmmfs_machine_boot(machine, &launch));
	if (error == 0) {
		error = vmmfs_loader_run(&machine->loader, launch, ap->a_cred);
		if (error == 0)
			error = vmmfs_launch_wait(launch);
		else {
			abort_error = vmmfs_machine_abort(launch);
			if (abort_error != 0)
				error = abort_error;
		}
		vrele(launch->node.vnode);
	}
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

struct vmmfs_machine_item {
	ino_t inode;
	uint8_t type;
	const char *name;
};

static int
vmmfs_machine_read_item(struct vmmfs_machine *machine, uint64_t index,
	struct vmmfs_machine_item *item)
{
	struct vmmfs_node *stopped;

	lwkt_gettoken(&machine->token);
	item->name = NULL;
	switch (index) {
	case 0:
		item->inode = machine->id_node.node.inode;
		item->name = "id";
		item->type = DT_REG;
		break;
	case 1:
		item->inode = machine->vcpu.node.inode;
		item->name = "vcpu";
		item->type = DT_REG;
		break;
	case 2:
		item->inode = machine->memory.node.inode;
		item->name = "mem";
		item->type = DT_REG;
		break;
	case 3:
		item->inode = machine->loader.node.inode;
		item->name = "loader";
		item->type = DT_REG;
		break;
	case 4:
		item->inode = machine->boot.node.inode;
		item->name = "boot";
		item->type = DT_CHR;
		break;
	case 5:
		item->inode = machine->events.node.inode;
		item->name = "events";
		item->type = DT_REG;
		break;
	case 6:
		if (machine->stopped != NULL &&
		    (machine->machine == NULL || machine->launch != NULL)) {
			stopped = &machine->stopped->node;
			item->inode = stopped->inode;
			item->name = "stopped";
			item->type = DT_REG;
		}
		break;
	case 7:
		item->inode = machine->pciroot.node.inode;
		item->name = "pci";
		item->type = DT_DIR;
		break;
	case 8:
		item->inode = machine->serialroot.node.inode;
		item->name = "serial";
		item->type = DT_DIR;
		break;
	default:
		lwkt_reltoken(&machine->token);
		return (ENOENT);
	}
	lwkt_reltoken(&machine->token);
	return (0);
}

static int
vmmfs_machine_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_machine *machine;
	struct vmmfs_machine_item item;
	struct vmmfs_mount *mount;
	struct uio *uio;
	off_t offset;
	int error;
	int stop;

	machine = ap->a_vp->v_data;
	mount = (struct vmmfs_mount *)ap->a_vp->v_mount->mnt_data;
	if (machine == NULL || mount == NULL || mount->root_inode == 0)
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
	while (!stop) {
		error = VMMFS_WORK(machine,
		    vmmfs_machine_read_item(machine, offset - 2, &item));
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		if (item.name != NULL)
			stop = vop_write_dirent(&error, uio, item.inode,
			    item.type, (uint16_t)strlen(item.name), item.name);
		if (!stop)
			++offset;
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop;
	return (error);
}

int
vmmfs_machine_boot(struct vmmfs_machine *machine, struct vmmfs_launch **launchp)
{
	struct vmspace *vmspace;
	struct vmmfs_launch *launch;
	struct vnode *vnode;
	vmm_machine_t runtime;
	int error, cleanup_error;
	bool current;

	*launchp = NULL;
	/* Create an empty GPA namespace before reading the machine topology. */
	vmspace = vmspace_alloc(VM_MIN_USER_ADDRESS, VMMFS_GPA_MAX);
	if (vmspace == NULL)
		return (ENOMEM);
	error = vmm_machine_create(vmspace, &runtime);
	if (error != 0)
		goto free_vmspace;

	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->token);
	if (!atomic_cmpset_ptr(&machine->machine, NULL, runtime)) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&machine->vcpu.token);
		error = EBUSY;
		goto rejected;
	}
	machine->memory.run_vmspace = vmspace;
	/* Keep cancellation from releasing resources while PREPARE builds them. */
	++machine->runtime_references;
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&machine->vcpu.token);

	error = vmmfs_launch_create(&machine->node, machine->memory.size, &launch);
	if (error != 0) {
		vmmfs_machine_runtime_put(machine);
		cleanup_error = vmmfs_machine_release_to_stopped(machine);
		if (cleanup_error != 0)
			error = cleanup_error;
		return (error);
	}
	vnode = launch->node.vnode;
	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->token);
	machine->launch = launch;
	vref(vnode);
	error = machine->vcpu.stop_requested ? ECANCELED : 0;
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&machine->vcpu.token);

	if (error == 0)
		error = vmmfs_memory_prepare(&machine->memory, machine->memory.size);
	if (error == 0)
		error = vmmfs_memory_map(&machine->memory);
	if (error == 0)
		error = vmmfs_launch_map(launch, machine->memory.object);
	if (error == 0)
		error = vmm_machine_create_irqchip(runtime);
	if (error == 0)
		error = vmm_machine_create_pit(runtime);
	if (error == 0)
		error = vmmfs_platform_x64_prepare(&machine->platform,
		    &machine->memory, machine->vcpu.count, &machine->pciroot,
		    &machine->serialroot);
	if (error == 0)
		error = vmmfs_rtc_start(&machine->rtc, runtime);
	if (error == 0)
		error = vmmfs_pciroot_start(&machine->pciroot, runtime);
	if (error == 0)
		error = vmmfs_serialroot_start(&machine->serialroot, runtime);
	if (error == 0)
		error = vmmfs_platform_x64_start(&machine->platform, runtime);
	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->token);
	current = machine->launch == launch && !machine->vcpu.stop_requested;
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&machine->vcpu.token);
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
	*launchp = launch;
	goto finished;

rejected:
	/* The losing candidate never owned any shared machine resources. */
	cleanup_error = vmm_machine_destroy(runtime);
	if (cleanup_error != 0)
		panic("vmmfs: empty candidate destroy: %d", cleanup_error);
free_vmspace:
	pmap_del_all_cpus(vmspace);
	vmspace_rel(vmspace);
finished:
	return (error);
}

int
vmmfs_machine_abort(struct vmmfs_launch *launch)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)launch->node.parent;
	struct vnode *vnode;
	int error;

	lwkt_gettoken(&launch->token);
	lwkt_gettoken(&machine->token);
	if (machine->launch != launch) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&launch->token);
		return (0);
	}
	vnode = launch->node.vnode;
	vmmfs_node_hold(&machine->events.node);
	machine->launch = NULL;
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&launch->token);
	/* PREPARE has no fd user yet, but may still be constructing devices. */
	vmmfs_machine_runtime_wait(machine);
	vmmfs_launch_revoke(launch);
	error = vmmfs_machine_release_to_stopped(machine);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_FAILED,
	    "error=%d", error == 0 ? ECANCELED : error);
	vmmfs_launch_complete(launch, error == 0 ? ECANCELED : error);
	vmmfs_node_put(&machine->events.node);
	/* Recursive abort during closure leaves this reference to its owner. */
	if (!vmmfs_node_deactivate(&launch->node))
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

	lwkt_gettoken(&launch->token);
	lwkt_gettoken(&machine->token);
	if (machine->launch != launch) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&launch->token);
		return (EPIPE);
	}
	vnode = launch->node.vnode;
	machine->launch = NULL;
	runtime = machine->machine;
	count = machine->vcpu.count;
	state = launch->cpustate;
	++machine->runtime_references;
	vmmfs_node_hold(&machine->events.node);
	lwkt_reltoken(&machine->token);
	/* Consuming the launch identity is the run/abort linearization point. */
	vmmfs_launch_revoke(launch);
	lwkt_reltoken(&launch->token);
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
	vmmfs_events_log(&machine->events, error == 0 ?
	    VMMFS_MACHINE_EVENT_BOOT_COMPLETED : VMMFS_MACHINE_EVENT_BOOT_FAILED,
	    "error=%d", error);
	vmmfs_launch_complete(launch, error);
	vmmfs_node_put(&machine->events.node);
	/* Recursive abort during closure leaves this reference to its owner. */
	if (!vmmfs_node_deactivate(&launch->node))
		vrele(vnode);
	return (error);
}

static int
vmmfs_machine_release_runtime(struct vmmfs_machine *machine)
{
	vmm_machine_t runtime_machine;
	int error;

	lwkt_gettoken(&machine->token);
	runtime_machine = machine->machine;
	if (runtime_machine == NULL) {
		lwkt_reltoken(&machine->token);
		return (0);
	}
	if (machine->runtime_releasing) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	if (machine->runtime_released) {
		lwkt_reltoken(&machine->token);
		return (0);
	}
	machine->runtime_releasing = true;
	lwkt_reltoken(&machine->token);
	/* New users are excluded before waiting for borrowed runtime handles. */
	vmmfs_machine_runtime_wait(machine);
	(void)vmmfs_platform_x64_stop(&machine->platform);
	(void)vmmfs_serialroot_stop(&machine->serialroot);
	(void)vmmfs_pciroot_stop(&machine->pciroot);
	(void)vmmfs_rtc_stop(&machine->rtc);
	error = vmm_machine_destroy(runtime_machine);
	if (error != 0) {
		lwkt_gettoken(&machine->token);
		machine->runtime_releasing = false;
		lwkt_reltoken(&machine->token);
		return (error);
	}
	vmmfs_memory_release(&machine->memory);
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->machine == runtime_machine);
	machine->runtime_releasing = false;
	machine->runtime_released = true;
	lwkt_reltoken(&machine->token);
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
	lwkt_gettoken(&machine->token);
	old_machine = machine->machine;
	lwkt_reltoken(&machine->token);
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
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->machine == old_machine);
	machine->machine = runtime_machine;
	lwkt_reltoken(&machine->token);
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
