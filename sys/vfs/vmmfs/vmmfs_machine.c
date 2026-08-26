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
#include "vmmfs_platform_x64.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_serialport.h"

#define VMMFS_MACHINE_MODE 0555

static int vmmfs_machine_access(struct vop_access_args *);
static int vmmfs_machine_getattr(struct vop_getattr_args *);
static int vmmfs_machine_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_machine_ncreate(struct vop_ncreate_args *);
static int vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_machine_nremove(struct vop_nremove_args *);
static int vmmfs_machine_nresolve(struct vop_nresolve_args *);
static int vmmfs_machine_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_machine_open(struct vop_open_args *);
static int vmmfs_machine_readdir(struct vop_readdir_args *);
static int vmmfs_machine_inactive(struct vop_inactive_args *);
static int vmmfs_machine_reclaim(struct vop_reclaim_args *);
static int vmmfs_machine_start(struct vmmfs_machine *, struct ucred *);
static int vmmfs_machine_prepare_start(struct vmmfs_machine *, char *,
	uint32_t *);
static void vmmfs_machine_wake_waiters(struct vmmfs_machine *);
static int vmmfs_machine_release_runtime(struct vmmfs_machine *);

struct vop_ops vmmfs_machine_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_machine_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_machine_getattr,
	.vop_getattr_lite = vmmfs_machine_getattr_lite,
	.vop_ncreate = vmmfs_machine_ncreate,
	.vop_nlookupdotdot = vmmfs_machine_nlookupdotdot,
	.vop_nremove = vmmfs_machine_nremove,
	.vop_nresolve = vmmfs_machine_nresolve,
	.vop_nrmdir = vmmfs_machine_nrmdir,
	.vop_open = vmmfs_machine_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_machine_readdir,
	.vop_inactive = vmmfs_machine_inactive,
	.vop_reclaim = vmmfs_machine_reclaim,
};

int
vmmfs_machine_compare(struct vmmfs_machine *left,
	struct vmmfs_machine *right)
{
	return (strcmp(left->name, right->name));
}

RB_GENERATE(vmmfs_machine_tree, vmmfs_machine, entry, vmmfs_machine_compare);

int
vmmfs_machine_create(struct vmmfs_root *root, const char *name,
	size_t namelen, struct vmmfs_machine **machinep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	if (root == NULL || name == NULL || machinep == NULL || namelen == 0 ||
	    namelen > NAME_MAX)
		return (EINVAL);
	*machinep = NULL;
	machine = kmalloc(sizeof(*machine), M_VMMFS, M_WAITOK | M_ZERO);
	machine->root = root;
	machine->references = 1;
	state = (struct vmmfs_mount *)root->mount->mnt_data;
	machine->inode = atomic_fetchadd_int(&state->next_inode, 1);
	bcopy(name, machine->name, namelen);
	machine->name[namelen] = '\0';
	lwkt_token_init(&machine->token, "vmmfsmachine");
	error = vmmfs_machine_id_init(machine, &machine->id_node);
	if (error != 0)
		goto fail_token;
	error = vmmfs_vcpu_init(machine, &machine->vcpu);
	if (error != 0)
		goto fail_id;
	error = vmmfs_memory_init(machine, &machine->memory);
	if (error != 0)
		goto fail_vcpu;
	error = vmmfs_loader_init(machine, &machine->loader);
	if (error != 0)
		goto fail_memory;
	error = vmmfs_boot_init(machine, &machine->boot);
	if (error != 0)
		goto fail_loader;
	error = vmmfs_stopped_init(machine, &machine->stopped);
	if (error != 0)
		goto fail_boot;
	error = vmmfs_pciroot_init(machine, &machine->pciroot);
	if (error != 0)
		goto fail_stopped;
	error = vmmfs_platform_x64_init(machine, &machine->platform);
	if (error != 0)
		goto fail_pciroot;
	error = vmmfs_rtc_init(machine, &machine->rtc);
	if (error != 0)
		goto fail_platform;
	error = vmmfs_serialroot_init(machine, &machine->serialroot);
	if (error != 0)
		goto fail_rtc;
	error = vmmfs_events_init(machine, &machine->events);
	if (error != 0)
		goto fail_serialroot;
	error = getnewvnode(VT_SYNTH, root->mount, &vnode, 0, 0);
	if (error != 0)
		goto fail_events;
	vnode->v_data = machine;
	vnode->v_ops = &state->machine_vops;
	vnode->v_type = VDIR;
	machine->vnode = vnode;
	vmmfs_machine_hold(machine);
	vx_downgrade(vnode);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_CREATED, NULL);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_STOPPED,
	    "reason=create");
	*machinep = machine;
	return (0);

fail_events:
	vmmfs_vnode_discard(machine->events.vnode);
	(void)vmmfs_events_fini(&machine->events);
fail_serialroot:
	vmmfs_vnode_discard(machine->serialroot.vnode);
	(void)vmmfs_serialroot_fini(&machine->serialroot);
fail_rtc:
	(void)vmmfs_rtc_fini(&machine->rtc);
fail_platform:
	(void)vmmfs_platform_x64_fini(&machine->platform);
fail_pciroot:
	vmmfs_vnode_discard(machine->pciroot.vnode);
	(void)vmmfs_pciroot_fini(&machine->pciroot);
fail_stopped:
	vmmfs_vnode_discard(machine->stopped.vnode);
	(void)vmmfs_stopped_fini(&machine->stopped);
fail_boot:
	vmmfs_vnode_discard(machine->boot.vnode);
	(void)vmmfs_boot_fini(&machine->boot);
fail_loader:
	vmmfs_vnode_discard(machine->loader.vnode);
	(void)vmmfs_loader_fini(&machine->loader);
fail_memory:
	vmmfs_vnode_discard(machine->memory.vnode);
	(void)vmmfs_memory_fini(&machine->memory);
fail_vcpu:
	vmmfs_vnode_discard(machine->vcpu.vnode);
	(void)vmmfs_vcpu_fini(&machine->vcpu);
fail_id:
	vmmfs_vnode_discard(machine->id_node.vnode);
	(void)vmmfs_machine_id_fini(&machine->id_node);
fail_token:
	lwkt_token_uninit(&machine->token);
	kfree(machine, M_VMMFS);
	return (error);
}

void
vmmfs_machine_abort_create(struct vmmfs_machine *machine)
{
	struct vnode *vnode;

	if (machine == NULL)
		return;
	vmmfs_vnode_discard(machine->events.vnode);
	vmmfs_vnode_discard(machine->serialroot.vnode);
	vmmfs_vnode_discard(machine->pciroot.vnode);
	vmmfs_vnode_discard(machine->stopped.vnode);
	vmmfs_vnode_discard(machine->boot.vnode);
	vmmfs_vnode_discard(machine->loader.vnode);
	vmmfs_vnode_discard(machine->memory.vnode);
	vmmfs_vnode_discard(machine->vcpu.vnode);
	vmmfs_vnode_discard(machine->id_node.vnode);
	vnode = machine->vnode;
	if (vnode != NULL) {
		vx_downgrade(vnode);
		vn_unlock(vnode);
		vmmfs_vnode_discard(vnode);
	}
}

int
vmmfs_machine_begin_destroy(struct vmmfs_machine *machine)
{
	int runtime_active;

	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->token);
	runtime_active = machine->machine != NULL;
	if (runtime_active) {
		lwkt_reltoken(&machine->token);
		vmmfs_events_log(&machine->events,
		    VMMFS_MACHINE_EVENT_DESTROY_REFUSED, "runtime=%d",
		    runtime_active);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->token);
	vmmfs_machine_wake_waiters(machine);
	vmmfs_pciroot_release_vnodes(&machine->pciroot);
	vmmfs_serialroot_release_vnodes(&machine->serialroot);
	vmmfs_vnode_discard(machine->events.vnode);
	vmmfs_vnode_discard(machine->stopped.vnode);
	vmmfs_vnode_discard(machine->boot.vnode);
	vmmfs_vnode_discard(machine->loader.vnode);
	vmmfs_vnode_discard(machine->memory.vnode);
	vmmfs_vnode_discard(machine->vcpu.vnode);
	vmmfs_vnode_discard(machine->id_node.vnode);
	vmmfs_vnode_discard(machine->vnode);
	return (0);
}

static void
vmmfs_machine_destroy(struct vmmfs_machine *machine)
{
	struct vmmfs_root *root;
	bool root_counted;
	int error;

	KKASSERT(machine != NULL);
	KKASSERT(machine->dead);
	KKASSERT(machine->references == 0);
	KKASSERT(machine->vnode == NULL);
	KKASSERT(machine->id_node.vnode == NULL);
	KKASSERT(machine->vcpu.vnode == NULL);
	KKASSERT(machine->memory.vnode == NULL);
	KKASSERT(machine->loader.vnode == NULL);
	KKASSERT(machine->boot.vnode == NULL);
	KKASSERT(machine->stopped.vnode == NULL);
	KKASSERT(machine->events.vnode == NULL);
	KKASSERT(machine->pciroot.vnode == NULL);
	KKASSERT(machine->serialroot.vnode == NULL);
	root = machine->root;
	root_counted = machine->root_counted;
	error = vmmfs_stopped_fini(&machine->stopped);
	KKASSERT(error == 0);
	error = vmmfs_boot_fini(&machine->boot);
	KKASSERT(error == 0);
	error = vmmfs_loader_fini(&machine->loader);
	KKASSERT(error == 0);
	error = vmmfs_memory_fini(&machine->memory);
	KKASSERT(error == 0);
	error = vmmfs_vcpu_fini(&machine->vcpu);
	KKASSERT(error == 0);
	error = vmmfs_serialroot_fini(&machine->serialroot);
	KKASSERT(error == 0);
	error = vmmfs_rtc_fini(&machine->rtc);
	KKASSERT(error == 0);
	error = vmmfs_platform_x64_fini(&machine->platform);
	KKASSERT(error == 0);
	error = vmmfs_pciroot_fini(&machine->pciroot);
	KKASSERT(error == 0);
	error = vmmfs_events_fini(&machine->events);
	KKASSERT(error == 0);
	error = vmmfs_machine_id_fini(&machine->id_node);
	KKASSERT(error == 0);
	machine->root = NULL;
	lwkt_token_uninit(&machine->token);
	kfree(machine, M_VMMFS);
	if (root_counted) {
		lwkt_gettoken(&root->token);
		KKASSERT(root->machine_count != 0);
		--root->machine_count;
		lwkt_reltoken(&root->token);
	}
}

void
vmmfs_machine_hold(struct vmmfs_machine *machine)
{
	KKASSERT(machine != NULL);
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->references != UINT_MAX);
	++machine->references;
	lwkt_reltoken(&machine->token);
}

void
vmmfs_machine_put(struct vmmfs_machine *machine)
{
	bool free_machine;

	KKASSERT(machine != NULL);
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->references != 0);
	--machine->references;
	free_machine = machine->references == 0;
	if (free_machine)
		KKASSERT(machine->dead);
	lwkt_reltoken(&machine->token);
	if (free_machine)
		vmmfs_machine_destroy(machine);
}

bool
vmmfs_machine_is_dead(struct vmmfs_machine *machine)
{
	bool dead;

	if (machine == NULL)
		return (true);
	lwkt_gettoken(&machine->token);
	dead = machine->dead;
	lwkt_reltoken(&machine->token);
	return (dead);
}

bool
vmmfs_machine_vnode_detach(struct vmmfs_machine *machine,
	struct vnode **vnodep, struct vnode *vnode)
{
	bool detached;

	if (machine == NULL || vnodep == NULL || vnode == NULL)
		return (false);
	lwkt_gettoken(&machine->token);
	detached = machine->dead && *vnodep == vnode;
	if (detached)
		*vnodep = NULL;
	lwkt_reltoken(&machine->token);
	return (detached);
}

static void
vmmfs_machine_wake_waiters(struct vmmfs_machine *machine)
{
	struct vmmfs_pcislot *slot;

	vmmfs_boot_revoke(&machine->boot);
	vmmfs_events_revoke(&machine->events);
	RB_FOREACH(slot, vmmfs_pcislot_tree, &machine->pciroot.slots) {
		vmmfs_pcislot_config_revoke(&slot->config);
		vmmfs_pcislot_events_revoke(&slot->events);
	}
}

int
vmmfs_machine_stop_request(struct vmmfs_machine *machine, const char *reason)
{
	bool running;
	bool boot_pending;

	if (machine == NULL || reason == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->token);
	if (machine->dead) {
		lwkt_reltoken(&machine->token);
		return (ENOENT);
	}
	running = machine->machine != NULL;
	lwkt_reltoken(&machine->token);
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
	lwkt_gettoken(&machine->token);
	if (machine->dead) {
		lwkt_reltoken(&machine->token);
		return (ENOENT);
	}
	running = machine->machine != NULL;
	lwkt_reltoken(&machine->token);
	if (!running)
		return (EBUSY);
	vmmfs_vcpu_request_reset(&machine->vcpu);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_RESET_REQUESTED,
	    "reason=external");
	return (0);
}

static int
vmmfs_machine_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_MACHINE_MODE, 0));
}

static int
vmmfs_machine_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_machine *machine;
	struct vattr *vattr;

	machine = ap->a_vp->v_data;
	if (machine == NULL || vmmfs_machine_is_dead(machine))
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_MACHINE_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = machine->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

static int
vmmfs_machine_getattr_lite(struct vop_getattr_lite_args *ap)
{
	struct vmmfs_machine *machine;
	struct vattr_lite *vattr;
	machine = ap->a_vp->v_data;
	if (machine == NULL || vmmfs_machine_is_dead(machine))
		return (ENOENT);
	vattr = ap->a_lvap;
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_MACHINE_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_size = 0;
	vattr->va_flags = 0;
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
	error = vmmfs_machine_stop_request(machine, "external");
	if (error != 0)
		return (error);
	lwkt_gettoken(&machine->token);
	vnode = machine->stopped.vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->token);
	if (vnode == NULL)
		return (ENOENT);
	error = vget(vnode, LK_EXCLUSIVE);
	vdrop(vnode);
	if (error != 0)
		return (error);
	*ap->a_vpp = vnode;
	/*
	 * touch(1) needs a vnode for this operation, but stopped is a projection
	 * of machine->machine rather than a persistent file creation.  Leave the
	 * name unresolved so a subsequent lookup stays hidden until the BSP has
	 * released the runtime and made the machine genuinely stopped.
	 */
	cache_setunresolved(ap->a_nch);
	return (0);
}

static int
vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	struct vnode *vnode;

	machine = ap->a_dvp->v_data;
	root = ((struct vmmfs_mount *)ap->a_dvp->v_mount->mnt_data)->root;
	if (machine == NULL || root == NULL || vmmfs_machine_is_dead(machine))
		return (ENOENT);
	lwkt_gettoken(&machine->token);
	if (machine->root != root) {
		lwkt_reltoken(&machine->token);
		return (ENOENT);
	}
	lwkt_reltoken(&machine->token);
	lwkt_gettoken(&root->token);
	vnode = root->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&root->token);
	if (vnode == NULL)
		return (ENOENT);
	if (vget(vnode, LK_EXCLUSIVE | LK_RETRY) != 0) {
		vdrop(vnode);
		return (ENOENT);
	}
	vdrop(vnode);
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
	lwkt_gettoken(&machine->token);
	if (machine->dead) {
		lwkt_reltoken(&machine->token);
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	if (ncp->nc_nlen == sizeof("id") - 1 &&
	    bcmp(ncp->nc_name, "id", sizeof("id") - 1) == 0)
		vnode = machine->id_node.vnode;
	else if (ncp->nc_nlen == sizeof("vcpu") - 1 &&
	    bcmp(ncp->nc_name, "vcpu", sizeof("vcpu") - 1) == 0)
		vnode = machine->vcpu.vnode;
	else if (ncp->nc_nlen == sizeof("mem") - 1 &&
	    bcmp(ncp->nc_name, "mem", sizeof("mem") - 1) == 0)
		vnode = machine->memory.vnode;
	else if (ncp->nc_nlen == sizeof("loader") - 1 &&
	    bcmp(ncp->nc_name, "loader", sizeof("loader") - 1) == 0)
		vnode = machine->loader.vnode;
	else if (ncp->nc_nlen == sizeof("boot") - 1 &&
	    bcmp(ncp->nc_name, "boot", sizeof("boot") - 1) == 0)
		vnode = machine->boot.vnode;
	else if (ncp->nc_nlen == sizeof("events") - 1 &&
	    bcmp(ncp->nc_name, "events", sizeof("events") - 1) == 0)
		vnode = machine->events.vnode;
	else if (ncp->nc_nlen == sizeof("pci") - 1 &&
	    bcmp(ncp->nc_name, "pci", sizeof("pci") - 1) == 0)
		vnode = machine->pciroot.vnode;
	else if (ncp->nc_nlen == sizeof("serial") - 1 &&
	    bcmp(ncp->nc_name, "serial", sizeof("serial") - 1) == 0)
		vnode = machine->serialroot.vnode;
	else if (ncp->nc_nlen == sizeof("stopped") - 1 &&
	    bcmp(ncp->nc_name, "stopped", sizeof("stopped") - 1) == 0)
		vnode = machine->machine == NULL ? machine->stopped.vnode : NULL;
	else {
		lwkt_reltoken(&machine->token);
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->token);
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
	lwkt_gettoken(&machine->token);
	if (machine->dead || machine->machine != NULL) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->token);
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
vmmfs_machine_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_machine_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_machine *machine;
	struct uio *uio;
	off_t offset;
	ino_t inode;
	int error;
	int present;
	int stop;

	machine = ap->a_vp->v_data;
	if (machine == NULL || vmmfs_machine_is_dead(machine))
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
		stop = vop_write_dirent(&error, uio, machine->inode, DT_DIR, 1,
		    ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, VMMFS_ROOT_INO, DT_DIR, 2,
		    "..");
		if (!stop)
			offset = 2;
	}
	if (!stop && offset == 2) {
		stop = vop_write_dirent(&error, uio, machine->id_node.inode,
		    DT_REG, sizeof("id") - 1, "id");
		if (!stop)
			offset = 3;
	}
	if (!stop && offset == 3) {
		stop = vop_write_dirent(&error, uio, machine->vcpu.inode,
		    DT_REG, sizeof("vcpu") - 1, "vcpu");
		if (!stop)
			offset = 4;
	}
	if (!stop && offset == 4) {
		stop = vop_write_dirent(&error, uio, machine->memory.inode,
		    DT_REG, sizeof("mem") - 1, "mem");
		if (!stop)
			offset = 5;
	}
	if (!stop && offset == 5) {
		stop = vop_write_dirent(&error, uio, machine->loader.inode,
		    DT_REG, sizeof("loader") - 1, "loader");
		if (!stop)
			offset = 6;
	}
	if (!stop && offset == 6) {
		stop = vop_write_dirent(&error, uio, machine->boot.inode,
		    DT_CHR, sizeof("boot") - 1, "boot");
		if (!stop)
			offset = 7;
	}
	if (!stop && offset == 7) {
		stop = vop_write_dirent(&error, uio, machine->events.inode,
		    DT_REG, sizeof("events") - 1, "events");
		if (!stop)
			offset = 8;
	}
	if (!stop && offset == 8) {
		lwkt_gettoken(&machine->token);
		present = machine->machine == NULL;
		inode = machine->stopped.inode;
		lwkt_reltoken(&machine->token);
		if (present) {
			stop = vop_write_dirent(&error, uio, inode, DT_REG,
			    sizeof("stopped") - 1, "stopped");
		}
		if (!stop)
			offset = 9;
	}
	if (!stop && offset == 9) {
		lwkt_gettoken(&machine->token);
		inode = machine->pciroot.inode;
		lwkt_reltoken(&machine->token);
		stop = vop_write_dirent(&error, uio, inode, DT_DIR,
		    sizeof("pci") - 1, "pci");
		if (!stop)
			offset = 10;
	}
	if (!stop && offset == 10) {
		lwkt_gettoken(&machine->token);
		inode = machine->serialroot.inode;
		lwkt_reltoken(&machine->token);
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
vmmfs_machine_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_machine *machine;

	machine = ap->a_vp->v_data;
	if (machine == NULL || !vmmfs_machine_vnode_detach(machine,
	    &machine->vnode, ap->a_vp))
		return (0);
	ap->a_vp->v_data = NULL;
	vmmfs_machine_put(machine);
	vrecycle(ap->a_vp);
	return (0);
}

static int
vmmfs_machine_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_machine *machine;

	machine = ap->a_vp->v_data;
	if (machine == NULL)
		return (0);
	lwkt_gettoken(&machine->token);
	if (machine->vnode == ap->a_vp)
		machine->vnode = NULL;
	lwkt_reltoken(&machine->token);
	ap->a_vp->v_data = NULL;
	vmmfs_machine_put(machine);
	return (0);
}

static int
vmmfs_machine_prepare_start(struct vmmfs_machine *machine,
	char *loader_script, uint32_t *vcpu_countp)
{
	vmm_machine_t runtime_machine;
	uint64_t memory_size;
	uint32_t vcpu_count;
	bool stop_requested;
	int error;
	int release_error;

	if (machine == NULL)
		return (EINVAL);
	runtime_machine = NULL;
	lwkt_gettoken(&machine->token);
	if (machine->dead || machine->machine != NULL) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	memory_size = machine->memory.size;
	vcpu_count = machine->vcpu.count;
	if (loader_script != NULL)
		bcopy(machine->loader.script, loader_script,
		    sizeof(machine->loader.script));
	if (memory_size == 0 || vcpu_count == 0 ||
	    (loader_script != NULL && loader_script[0] == '\0')) {
		lwkt_reltoken(&machine->token);
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
	lwkt_reltoken(&machine->token);

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
	lwkt_gettoken(&machine->token);
	lwkt_gettoken(&machine->vcpu.token);
	stop_requested = machine->vcpu.stop_requested;
	lwkt_reltoken(&machine->vcpu.token);
	if (!stop_requested) {
		error = vmmfs_boot_arm_locked(&machine->boot,
		    machine->memory.object, memory_size);
	}
	lwkt_reltoken(&machine->token);
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
	lwkt_reltoken(&machine->token);
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
	lwkt_gettoken(&machine->token);
	if (machine->dead) {
		lwkt_reltoken(&machine->token);
		return (ENOENT);
	}
	running = machine->machine != NULL;
	lwkt_reltoken(&machine->token);
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
	lwkt_gettoken(&machine->token);
	if (machine->dead || machine->machine == NULL) {
		lwkt_reltoken(&machine->token);
		return (EPIPE);
	}
	runtime_machine = machine->machine;
	vcpu_count = machine->vcpu.count;
	lwkt_reltoken(&machine->token);
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

	lwkt_gettoken(&machine->token);
	runtime_machine = machine->machine;
	lwkt_reltoken(&machine->token);
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
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->machine == runtime_machine);
	machine->machine = NULL;
	lwkt_reltoken(&machine->token);
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
