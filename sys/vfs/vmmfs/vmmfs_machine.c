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
#include <sys/nlookup.h>
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

static int vmmfs_machine_create_item(struct vmmfs_node *, const char *,
	size_t, struct vnode **);
static int vmmfs_machine_remove_item(struct vmmfs_node *, const char *,
	size_t, struct ucred *);
static int vmmfs_machine_get_item(struct vmmfs_node *, const char *,
	size_t, struct vnode **);
static int vmmfs_machine_read_item(struct vmmfs_node *, uint64_t,
	struct vmmfs_node_item *);
static void vmmfs_machine_drop(struct vmmfs_node *);
static bool vmmfs_machine_deactivate(struct vmmfs_node *);

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
	machine->node.read_item = vmmfs_machine_read_item;
	machine->node.get_item = vmmfs_machine_get_item;
	machine->node.create_item = vmmfs_machine_create_item;
	machine->node.remove_item = vmmfs_machine_remove_item;
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
	error = vmmfs_stopped_init(&machine->node, &machine->stopped);
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
									&parent->mount->node_vops, VDIR, &machine->node);
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
	(void)vmmfs_node_deactivate(&machine->stopped.node);
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
	(void)vmmfs_node_deactivate(&machine->stopped.node);
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

static int
vmmfs_machine_create_item(struct vmmfs_node *node, const char *name,
    size_t length, struct vnode **vnodep)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)node;

	if (length != sizeof("stopped") - 1 ||
	    bcmp(name, "stopped", sizeof("stopped") - 1) != 0)
		return (EOPNOTSUPP);
	vmmfs_vcpu_request_stop(&machine->vcpu);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOP_REQUESTED,
	    "reason=external");
	vref(machine->stopped.node.vnode);
	*vnodep = machine->stopped.node.vnode;
	return (0);
}

static int
vmmfs_machine_get_item(struct vmmfs_node *node,
					   const char *name, size_t length, struct vnode **vnodep)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)node;
	struct vnode *vnode;
	bool visible;

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
		vnode = machine->stopped.node.vnode;
	} else {
		return (ENOENT);
	}
	vref(vnode);
	*vnodep = vnode;
	return (0);
}

static int
vmmfs_machine_remove_item(struct vmmfs_node *node, const char *name,
    size_t length, struct ucred *cred)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)node;
	struct vmmfs_launch *launch;
	int error;

	if (length != sizeof("stopped") - 1 ||
		bcmp(name, "stopped", sizeof("stopped") - 1) != 0)
		return (EOPNOTSUPP);
	error = vmmfs_machine_boot(machine, vmmfs_machine_post_launch, &launch);
	if (error == 0) {
		error = vmmfs_loader_run(&machine->loader, launch, cred);
		if (error != 0)
			vmmfs_launch_cancel(launch);
		{
			int result = vmmfs_launch_wait(launch);
			if (error == 0)
				error = result;
		}
		vmmfs_launch_put(launch);
	}
	return (error);
}

static int
vmmfs_machine_read_item(struct vmmfs_node *node, uint64_t index,
						struct vmmfs_node_item *item)
{
	struct vmmfs_machine *machine = (struct vmmfs_machine *)node;

	item->name[0] = '\0';
	switch (index) {
		case 0:
			item->inode = machine->id_node.node.inode;
			bcopy("id", item->name, sizeof("id"));
			item->type = DT_REG;
			break;
		case 1:
			item->inode = machine->vcpu.node.inode;
			bcopy("vcpu", item->name, sizeof("vcpu"));
			item->type = DT_REG;
			break;
		case 2:
			item->inode = machine->memory.node.inode;
			bcopy("mem", item->name, sizeof("mem"));
			item->type = DT_REG;
			break;
		case 3:
			item->inode = machine->loader.node.inode;
			bcopy("loader", item->name, sizeof("loader"));
			item->type = DT_REG;
			break;
		case 4:
			item->inode = machine->boot.node.inode;
			bcopy("boot", item->name, sizeof("boot"));
			item->type = DT_CHR;
			break;
		case 5:
			item->inode = machine->events.node.inode;
			bcopy("events", item->name, sizeof("events"));
			item->type = DT_REG;
			break;
		case 6:
			lwkt_gettoken(&machine->vcpu.token);
			if (machine->vcpu.threads == NULL ||
				machine->vcpu.threads[0].vcpu == NULL) {
				item->inode = machine->stopped.node.inode;
				bcopy("stopped", item->name, sizeof("stopped"));
				item->type = DT_REG;
			}
			lwkt_reltoken(&machine->vcpu.token);
			break;
		case 7:
			item->inode = machine->pciroot.node.inode;
			bcopy("pci", item->name, sizeof("pci"));
			item->type = DT_DIR;
			break;
		case 8:
			item->inode = machine->serialroot.node.inode;
			bcopy("serial", item->name, sizeof("serial"));
			item->type = DT_DIR;
			break;
		default:
			return (ENOENT);
	}
	return (0);
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
vmmfs_machine_stopped(struct vmmfs_machine *machine)
{
	vmm_machine_t runtime = machine->machine;
	struct nchandle parent, stopped;
	struct nlcomponent name = {
		.nlc_nameptr = machine->name,
		.nlc_namelen = strlen(machine->name),
	};

	(void)vmmfs_platform_x64_stop(&machine->platform);
	(void)vmmfs_serialroot_stop(&machine->serialroot);
	(void)vmmfs_pciroot_stop(&machine->pciroot);
	(void)vmmfs_rtc_stop(&machine->rtc);
	/* The worker barrier has destroyed every vCPU. */
	(void)vmm_machine_destroy(runtime);
	vmmfs_memory_release(&machine->memory);
	/* The namespace is known; do not reverse-scan it through NFS helpers. */
	parent = cache_nlookup(&machine->node.mount->mount->mnt_ncmountpt, &name);
	cache_setunresolved(&parent);
	cache_setvp(&parent, machine->node.vnode);
	cache_unlock(&parent);
	name.nlc_nameptr = "stopped";
	name.nlc_namelen = sizeof("stopped") - 1;
	stopped = cache_nlookup(&parent, &name);
	cache_setunresolved(&stopped);
	cache_setvp(&stopped, machine->stopped.node.vnode);
	cache_put(&stopped);
	cache_drop(&parent);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOPPED,
	    "reason=vcpu");
	/* The BSP alone releases this runtime after all shared cleanup. */
	(void)atomic_cmpset_ptr(&machine->machine, runtime, NULL);
}

int
vmmfs_machine_reset(struct vmmfs_machine *machine)
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
