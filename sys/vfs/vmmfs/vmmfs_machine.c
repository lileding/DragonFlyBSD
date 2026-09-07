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
static int vmmfs_machine_readdir(struct vop_readdir_args *);
static void vmmfs_machine_runtime_put(struct vmmfs_machine *);
static void vmmfs_machine_runtime_wait(struct vmmfs_machine *);
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
	machine->stopped_inode = vmmfs_root_allocate_inode(root);
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
	(void)vmmfs_node_deactivate(&machine->boot.node);
	(void)vmmfs_node_deactivate(&machine->loader.node);
	(void)vmmfs_node_deactivate(&machine->memory.node);
	(void)vmmfs_node_deactivate(&machine->vcpu.node);
	(void)vmmfs_node_deactivate(&machine->id_node.node);
	vmmfs_node_put(&machine->node);
	return (error);
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
	bool running;

	lwkt_gettoken(&machine->vcpu.token);
	lwkt_gettoken(&machine->token);
	running = machine->vcpu.threads != NULL &&
		!machine->runtime_releasing;
	if (running)
		++machine->runtime_references;
	lwkt_reltoken(&machine->token);
	if (running)
		vmmfs_vcpu_request_stop(&machine->vcpu);
	lwkt_reltoken(&machine->vcpu.token);
	if (running)
		vmmfs_machine_runtime_put(machine);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOP_REQUESTED,
				  "reason=%s", reason);
	return (0);
}

int
vmmfs_machine_reset(struct vmmfs_machine *machine)
{
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_RESET_REQUESTED,
				  "reason=external");
	vmmfs_vcpu_request_reset(&machine->vcpu);
	return (0);
}


static int
vmmfs_machine_touch_stopped(struct vmmfs_machine *machine,
							struct vnode **vnodep)
{
	struct vmmfs_stopped *stopped;
	int error;

	error = vmmfs_stopped_create(&machine->node, &stopped);
	if (error != 0)
		return (error);
	error = vmmfs_machine_request_stop(machine, "external");
	if (error != 0) {
		vrele(stopped->node.vnode);
		return (error);
	}
	*vnodep = stopped->node.vnode;
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
	struct vmmfs_stopped *stopped;
	bool visible;
	int error;

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
	lwkt_gettoken(&machine->vcpu.token);
	visible = machine->vcpu.threads == NULL ||
		machine->vcpu.threads[0].vcpu == NULL;
	lwkt_reltoken(&machine->vcpu.token);
	if (!visible)
		return (ENOENT);
	error = vmmfs_stopped_create(&machine->node, &stopped);
	if (error != 0)
		return (error);
	/* Creation may sleep while the BSP is published. */
	lwkt_gettoken(&machine->vcpu.token);
	visible = machine->vcpu.threads == NULL ||
		machine->vcpu.threads[0].vcpu == NULL;
	lwkt_reltoken(&machine->vcpu.token);
	if (!visible) {
		vrele(stopped->node.vnode);
		return (ENOENT);
	}
	*vnodep = stopped->node.vnode;
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
	struct vmmfs_launch *launch;
	int error;

	if (ncp->nc_nlen != sizeof("stopped") - 1 ||
		bcmp(ncp->nc_name, "stopped", sizeof("stopped") - 1) != 0)
		return (EOPNOTSUPP);
	/* post_launch unlinks the old stopped before waking this waiter. */
	cache_unlock(ap->a_nch);
	error = VMMFS_WORK(machine, vmmfs_machine_boot(machine,
												vmmfs_machine_post_launch, &launch));
	if (error == 0) {
		error = vmmfs_loader_run(&machine->loader, launch, ap->a_cred);
		if (error != 0)
			vmmfs_launch_cancel(launch);
		{
			int result = vmmfs_launch_wait(launch);
			if (error == 0)
				error = result;
		}
		vmmfs_launch_put(launch);
	}
	cache_lock(ap->a_nch);
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
			lwkt_gettoken(&machine->vcpu.token);
			if (machine->vcpu.threads == NULL ||
				machine->vcpu.threads[0].vcpu == NULL) {
				item->inode = machine->stopped_inode;
				item->name = "stopped";
				item->type = DT_REG;
			}
			lwkt_reltoken(&machine->vcpu.token);
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
			return (ENOENT);
	}
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
vmmfs_machine_boot(struct vmmfs_machine *machine,
				   void (*post_launch)(struct vmmfs_launch *), struct vmmfs_launch **launchp)
{
	struct vmspace *vmspace;
	struct vmmfs_launch *launch;
	vmm_machine_t runtime;
	int error;

	*launchp = NULL;
	/* Create an empty GPA namespace before reading the machine topology. */
	vmspace = vmspace_alloc(VM_MIN_USER_ADDRESS, VMMFS_GPA_MAX);
	if (vmspace == NULL)
		return (ENOMEM);
	error = vmm_machine_create(vmspace, &runtime);
	if (error != 0)
		goto free_vmspace;

	if (!atomic_cmpset_ptr(&machine->machine, NULL, runtime)) {
		error = EBUSY;
		goto rejected;
	}
	machine->memory.run_vmspace = vmspace;

	error = vmmfs_memory_prepare(&machine->memory, machine->memory.size);
	if (error == 0)
		error = vmmfs_memory_map(&machine->memory);
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
	if (error == 0)
		error = vmmfs_launch_create(machine, post_launch, &launch);
	if (error != 0) {
		(void)vmmfs_platform_x64_stop(&machine->platform);
		(void)vmmfs_serialroot_stop(&machine->serialroot);
		(void)vmmfs_pciroot_stop(&machine->pciroot);
		(void)vmmfs_rtc_stop(&machine->rtc);
		/* No vCPU was created for this candidate. */
		(void)vmm_machine_destroy(runtime);
		vmmfs_memory_release(&machine->memory);
		machine->machine = NULL;
		return (error);
	}
	*launchp = launch;
	goto finished;

rejected:
	/* The losing candidate never owned any shared machine resources. */
	(void)vmm_machine_destroy(runtime);
free_vmspace:
	pmap_del_all_cpus(vmspace);
	vmspace_rel(vmspace);
finished:
	return (error);
}

void
vmmfs_machine_post_launch(struct vmmfs_launch *launch)
{
	struct vmmfs_machine *machine = launch->machine;

	if (launch->result == 0) {
		cache_inval_vp(machine->node.vnode, CINV_CHILDREN);
		vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_COMPLETED,
				   "error=0");
	} else {
		vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_BOOT_FAILED,
				   "error=%d", launch->result);
		(void)vmmfs_platform_x64_stop(&machine->platform);
		(void)vmmfs_serialroot_stop(&machine->serialroot);
		(void)vmmfs_pciroot_stop(&machine->pciroot);
		(void)vmmfs_rtc_stop(&machine->rtc);
		/* Failed vCPU preparation has already destroyed its candidates. */
		(void)vmm_machine_destroy(machine->machine);
		vmmfs_memory_release(&machine->memory);
		machine->machine = NULL;
	}
}

void
vmmfs_machine_vcpu_stopped(struct vmmfs_machine *machine)
{
	if (machine == NULL)
		return;
	lwkt_gettoken(&machine->token);
	machine->runtime_releasing = true;
	lwkt_reltoken(&machine->token);
	vmmfs_machine_runtime_wait(machine);
	(void)vmmfs_platform_x64_stop(&machine->platform);
	(void)vmmfs_serialroot_stop(&machine->serialroot);
	(void)vmmfs_pciroot_stop(&machine->pciroot);
	(void)vmmfs_rtc_stop(&machine->rtc);
	/* The worker barrier has destroyed every vCPU. */
	(void)vmm_machine_destroy(machine->machine);
	vmmfs_memory_release(&machine->memory);
	cache_inval_vp(machine->node.vnode, CINV_CHILDREN);
	lwkt_gettoken(&machine->token);
	machine->runtime_releasing = false;
	machine->machine = NULL;
	lwkt_reltoken(&machine->token);
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
