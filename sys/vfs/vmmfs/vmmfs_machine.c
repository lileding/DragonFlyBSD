/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine directory object.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
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
static int vmmfs_machine_reclaim(struct vop_reclaim_args *);
static int vmmfs_machine_start(struct vmmfs_machine *, struct ucred *);
static int vmmfs_machine_stop(struct vmmfs_machine *);
static void vmmfs_machine_close_vnodes(struct vmmfs_machine *);

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
	.vop_reclaim = vmmfs_machine_reclaim,
};

int
vmmfs_machine_compare(struct vmmfs_machine *left,
	struct vmmfs_machine *right)
{
	return (strcmp(left->name, right->name));
}

RB_GENERATE(vmmfs_machine_tree, vmmfs_machine, entry, vmmfs_machine_compare);

struct vmmfs_machine *
vmmfs_machine_create(struct vmmfs_root *root, const char *name,
	size_t namelen)
{
	struct vmmfs_machine *machine;
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	if (namelen == 0 || namelen > NAME_MAX)
		return (NULL);
	machine = kmalloc(sizeof(*machine), M_VMMFS, M_WAITOK | M_ZERO);
	machine->root = root;
	machine->references = 1;
	state = (struct vmmfs_mount *)root->mount->mnt_data;
	machine->inode = atomic_fetchadd_int(&state->next_inode, 1);
	bcopy(name, machine->name, namelen);
	machine->name[namelen] = '\0';
	lwkt_token_init(&machine->token, "vmmfsmachine");
	error = vmmfs_vcpu_create(machine, &machine->vcpu);
	if (error != 0)
		goto fail_token;
	error = vmmfs_memory_create(machine, &machine->memory);
	if (error != 0)
		goto fail_vcpu;
	error = vmmfs_loader_create(machine, &machine->loader);
	if (error != 0)
		goto fail_memory;
	error = vmmfs_stopped_create(machine, &machine->stopped);
	if (error != 0)
		goto fail_loader;
	machine->stopped.expect_stopped = true;
	error = vmmfs_pciroot_create(machine, &machine->pciroot);
	if (error != 0)
		goto fail_stopped;
	error = vmmfs_platform_x64_create(machine, &machine->platform);
	if (error != 0)
		goto fail_pciroot;
	error = vmmfs_rtc_create(machine, &machine->rtc);
	if (error != 0)
		goto fail_platform;
	error = vmmfs_serialroot_create(machine, &machine->serialroot);
	if (error != 0)
		goto fail_rtc;
	error = vmmfs_events_create(machine, &machine->events);
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
	vmmfs_events_log(&machine->events, "machine created");
	vmmfs_events_log(&machine->events, "state stopped reason=create");
	return (machine);

fail_events:
	(void)vmmfs_events_destroy(&machine->events);
fail_serialroot:
	(void)vmmfs_serialroot_destroy(&machine->serialroot);
fail_rtc:
	(void)vmmfs_rtc_destroy(&machine->rtc);
fail_platform:
	(void)vmmfs_platform_x64_destroy(&machine->platform);
fail_pciroot:
	(void)vmmfs_pciroot_destroy(&machine->pciroot);
fail_stopped:
	(void)vmmfs_stopped_destroy(&machine->stopped);
fail_loader:
	(void)vmmfs_loader_destroy(&machine->loader);
fail_memory:
	(void)vmmfs_memory_destroy(&machine->memory);
fail_vcpu:
	(void)vmmfs_vcpu_destroy(&machine->vcpu);
fail_token:
	lwkt_token_uninit(&machine->token);
	kfree(machine, M_VMMFS);
	return (NULL);
}

int
vmmfs_machine_destroy(struct vmmfs_machine *machine)
{
	int expected_stopped;
	int runtime_active;

	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->token);
	expected_stopped = machine->stopped.expect_stopped;
	runtime_active = machine->machine != NULL;
	if (!expected_stopped || runtime_active) {
		lwkt_reltoken(&machine->token);
		vmmfs_events_log(&machine->events,
		    "destroy refused stopped=%d runtime=%d", expected_stopped,
		    runtime_active);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->token);
	vmmfs_machine_close_vnodes(machine);
	return (0);
}

void
vmmfs_machine_free(struct vmmfs_machine *machine)
{
	struct vmmfs_root *root;
	bool root_counted;

	KKASSERT(machine != NULL);
	KKASSERT(machine->dead);
	KKASSERT(machine->references == 0);
	KKASSERT(machine->vnode == NULL);
	KKASSERT(machine->vcpu.vnode == NULL);
	KKASSERT(machine->memory.vnode == NULL);
	KKASSERT(machine->loader.vnode == NULL);
	KKASSERT(machine->stopped.vnode == NULL);
	KKASSERT(machine->events.vnode == NULL);
	KKASSERT(machine->pciroot.vnode == NULL);
	KKASSERT(machine->serialroot.vnode == NULL);
	root = machine->root;
	root_counted = machine->root_counted;
	KKASSERT(vmmfs_stopped_destroy(&machine->stopped) == 0);
	KKASSERT(vmmfs_loader_destroy(&machine->loader) == 0);
	KKASSERT(vmmfs_memory_destroy(&machine->memory) == 0);
	KKASSERT(vmmfs_vcpu_destroy(&machine->vcpu) == 0);
	KKASSERT(vmmfs_serialroot_destroy(&machine->serialroot) == 0);
	KKASSERT(vmmfs_rtc_destroy(&machine->rtc) == 0);
	KKASSERT(vmmfs_platform_x64_destroy(&machine->platform) == 0);
	KKASSERT(vmmfs_pciroot_destroy(&machine->pciroot) == 0);
	KKASSERT(vmmfs_events_destroy(&machine->events) == 0);
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
		vmmfs_machine_free(machine);
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

static void
vmmfs_machine_close_vnodes(struct vmmfs_machine *machine)
{
	struct vmmfs_pcislot *slot;
	struct vmmfs_serialport *port;

	/* Close children before their directory vnode can be reclaimed. */
	RB_FOREACH(slot, vmmfs_pcislot_tree, &machine->pciroot.slots) {
		vmmfs_pcislot_config_revoke(&slot->config);
		vmmfs_pcislot_events_revoke(&slot->events);
		vmmfs_vnode_close(slot->descriptor.vnode);
		vmmfs_vnode_close(slot->config.vnode);
		vmmfs_vnode_close(slot->events.vnode);
		vmmfs_vnode_close(slot->vnode);
	}
	RB_FOREACH(port, vmmfs_serialport_tree, &machine->serialroot.ports)
		vmmfs_vnode_close(port->vnode);
	vmmfs_vnode_close(machine->pciroot.vnode);
	vmmfs_vnode_close(machine->serialroot.vnode);
	vmmfs_vnode_close(machine->vcpu.vnode);
	vmmfs_vnode_close(machine->memory.vnode);
	vmmfs_vnode_close(machine->loader.vnode);
	vmmfs_vnode_close(machine->stopped.vnode);
	vmmfs_events_revoke(&machine->events);
	vmmfs_vnode_close(machine->events.vnode);
	vmmfs_vnode_close(machine->vnode);
}

int
vmmfs_machine_reset(struct vmmfs_machine *machine, struct ucred *cred)
{
	int was_stopped;
	int error;

	if (machine == NULL || cred == NULL)
		return (EINVAL);
	was_stopped = 0;
	vmmfs_events_log(&machine->events, "reset requested");
	lwkt_gettoken(&machine->token);
	if (machine->dead) {
		lwkt_reltoken(&machine->token);
		return (ENOENT);
	}
	if (!machine->stopped.expect_stopped && machine->machine != NULL) {
		machine->stopped.expect_stopped = true;
		lwkt_reltoken(&machine->token);
		error = vmmfs_machine_stop(machine);
		if (error != 0)
			return (error);
		lwkt_gettoken(&machine->token);
		KKASSERT(machine->stopped.expect_stopped &&
		    machine->machine == NULL);
		machine->stopped.expect_stopped = false;
		lwkt_reltoken(&machine->token);
	} else if (machine->stopped.expect_stopped && machine->machine == NULL) {
		machine->stopped.expect_stopped = false;
		was_stopped = 1;
		lwkt_reltoken(&machine->token);
	} else {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	error = vmmfs_machine_start(machine, cred);
	if (error != 0) {
		lwkt_gettoken(&machine->token);
		if (!machine->stopped.expect_stopped && machine->machine == NULL)
			machine->stopped.expect_stopped = true;
		lwkt_reltoken(&machine->token);
	} else {
		if (was_stopped && machine->stopped.vnode != NULL)
			cache_inval_vp(machine->stopped.vnode, CINV_DESTROY);
		vmmfs_events_log(&machine->events, "reset completed");
	}
	return (error);
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
	lwkt_gettoken(&machine->token);
	if (machine->dead || machine->stopped.expect_stopped ||
	    machine->machine == NULL) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	machine->stopped.expect_stopped = true;
	lwkt_reltoken(&machine->token);
	error = vmmfs_machine_stop(machine);
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
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
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
	if (ncp->nc_nlen == sizeof("vcpu") - 1 &&
	    bcmp(ncp->nc_name, "vcpu", sizeof("vcpu") - 1) == 0)
		vnode = machine->vcpu.vnode;
	else if (ncp->nc_nlen == sizeof("mem") - 1 &&
	    bcmp(ncp->nc_name, "mem", sizeof("mem") - 1) == 0)
		vnode = machine->memory.vnode;
	else if (ncp->nc_nlen == sizeof("loader") - 1 &&
	    bcmp(ncp->nc_name, "loader", sizeof("loader") - 1) == 0)
		vnode = machine->loader.vnode;
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
		vnode = machine->stopped.expect_stopped ? machine->stopped.vnode : NULL;
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
	if (machine->dead || !machine->stopped.expect_stopped) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	machine->stopped.expect_stopped = false;
	lwkt_reltoken(&machine->token);
	error = vmmfs_machine_start(machine, ap->a_cred);
	if (error != 0) {
		lwkt_gettoken(&machine->token);
		if (!machine->stopped.expect_stopped && machine->machine == NULL)
			machine->stopped.expect_stopped = true;
		lwkt_reltoken(&machine->token);
		return (error);
	}
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
		stop = vop_write_dirent(&error, uio, machine->vcpu.inode,
		    DT_REG, sizeof("vcpu") - 1, "vcpu");
		if (!stop)
			offset = 3;
	}
	if (!stop && offset == 3) {
		stop = vop_write_dirent(&error, uio, machine->memory.inode,
		    DT_REG, sizeof("mem") - 1, "mem");
		if (!stop)
			offset = 4;
	}
	if (!stop && offset == 4) {
		stop = vop_write_dirent(&error, uio, machine->loader.inode,
		    DT_REG, sizeof("loader") - 1, "loader");
		if (!stop)
			offset = 5;
	}
	if (!stop && offset == 5) {
		stop = vop_write_dirent(&error, uio, machine->events.inode,
		    DT_REG, sizeof("events") - 1, "events");
		if (!stop)
			offset = 6;
	}
	if (!stop && offset == 6) {
		lwkt_gettoken(&machine->token);
		present = machine->stopped.expect_stopped;
		inode = machine->stopped.inode;
		lwkt_reltoken(&machine->token);
		if (present) {
			stop = vop_write_dirent(&error, uio, inode, DT_REG,
			    sizeof("stopped") - 1, "stopped");
		}
		if (!stop)
			offset = 7;
	}
	if (!stop && offset == 7) {
		lwkt_gettoken(&machine->token);
		inode = machine->pciroot.inode;
		lwkt_reltoken(&machine->token);
		stop = vop_write_dirent(&error, uio, inode, DT_DIR,
		    sizeof("pci") - 1, "pci");
		if (!stop)
			offset = 8;
	}
	if (!stop && offset == 8) {
		lwkt_gettoken(&machine->token);
		inode = machine->serialroot.inode;
		lwkt_reltoken(&machine->token);
		stop = vop_write_dirent(&error, uio, inode, DT_DIR,
		    sizeof("serial") - 1, "serial");
		if (!stop)
			offset = 9;
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop;
	return (error);
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
vmmfs_machine_start(struct vmmfs_machine *machine, struct ucred *cred)
{
	struct vmm_cpustate state;
	vmm_machine_t runtime_machine;
	int cleanup_error;
	int error;

	if (machine == NULL || cred == NULL)
		return (EINVAL);
	runtime_machine = NULL;
	lwkt_gettoken(&machine->token);
	if (machine->dead || machine->stopped.expect_stopped ||
	    machine->machine != NULL) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	lwkt_reltoken(&machine->token);
	vmmfs_events_log(&machine->events, "start requested");
	if (machine->spec.memory.size == 0 ||
	    machine->spec.loader.script[0] == '\0') {
		error = EINVAL;
		goto failed;
	}
	vmmfs_events_log(&machine->events, "memory prepare begin");
	error = vmmfs_memory_prepare(&machine->memory);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "memory prepare completed");
	vmmfs_events_log(&machine->events, "machine create begin");
	error = vmm_machine_create(machine->memory.run_vmspace, &runtime_machine);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "machine create completed");
	vmmfs_events_log(&machine->events, "memory map begin");
	error = vmmfs_memory_map(&machine->memory);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "memory map completed");
	vmmfs_events_log(&machine->events, "irqchip create begin");
	error = vmm_machine_create_irqchip(runtime_machine);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "irqchip create completed");
	vmmfs_events_log(&machine->events, "pit create begin");
	error = vmm_machine_create_pit(runtime_machine);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "pit create completed");
	vmmfs_events_log(&machine->events, "platform prepare begin");
	error = vmmfs_platform_x64_prepare(&machine->platform,
	    &machine->memory, machine->spec.vcpu.count, &machine->pciroot,
	    &machine->serialroot);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "platform prepare completed");
	vmmfs_events_log(&machine->events, "rtc start begin");
	error = vmmfs_rtc_start(&machine->rtc, runtime_machine);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "rtc start completed");
	vmmfs_events_log(&machine->events, "pci root start begin");
	error = vmmfs_pciroot_start(&machine->pciroot, runtime_machine);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "pci root start completed");
	vmmfs_events_log(&machine->events, "serial start begin");
	error = vmmfs_serialroot_start(&machine->serialroot, runtime_machine);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "serial start completed");
	/* Its whole-legacy-PIO fallback must follow every concrete device. */
	vmmfs_events_log(&machine->events, "platform start begin");
	error = vmmfs_platform_x64_start(&machine->platform, runtime_machine);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "platform start completed");
	vmmfs_events_log(&machine->events, "loader start begin");
	error = vmmfs_loader_run(&machine->loader, &machine->memory, cred,
	    &state);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "memory snapshot begin");
	error = vmmfs_memory_snapshot(&machine->memory);
	if (error != 0)
		goto failed;
	vmmfs_events_log(&machine->events, "memory snapshot completed");
	vmmfs_events_log(&machine->events, "vcpu start begin");
	error = vmmfs_vcpu_start(&machine->vcpu, runtime_machine, &state);
	if (error != 0)
		goto failed;
	lwkt_gettoken(&machine->token);
	if (machine->dead || machine->stopped.expect_stopped ||
	    machine->machine != NULL) {
		lwkt_reltoken(&machine->token);
		error = EBUSY;
		goto failed;
	}
	machine->machine = runtime_machine;
	lwkt_reltoken(&machine->token);
	vmmfs_events_log(&machine->events, "start completed");
	return (0);

failed:
	/* Each stop is idempotent and also unwinds a partially started object. */
	(void)vmmfs_platform_x64_stop(&machine->platform);
	(void)vmmfs_serialroot_stop(&machine->serialroot);
	(void)vmmfs_pciroot_stop(&machine->pciroot);
	(void)vmmfs_rtc_stop(&machine->rtc);
	(void)vmmfs_vcpu_stop(&machine->vcpu);
	if (runtime_machine != NULL) {
		cleanup_error = vmm_machine_destroy(runtime_machine);
		if (cleanup_error != 0) {
			lwkt_gettoken(&machine->token);
			machine->machine = runtime_machine;
			machine->stopped.expect_stopped = true;
			lwkt_reltoken(&machine->token);
			vmmfs_events_log(&machine->events,
			    "start rollback incomplete error=%d cleanup=%d", error,
			    cleanup_error);
			return (error);
		}
	}
	vmmfs_memory_release(&machine->memory);
	lwkt_gettoken(&machine->token);
	if (machine->machine == NULL)
		machine->stopped.expect_stopped = true;
	lwkt_reltoken(&machine->token);
	vmmfs_events_log(&machine->events, "start failed error=%d", error);
	return (error);
}

static int
vmmfs_machine_stop(struct vmmfs_machine *machine)
{
	vmm_machine_t runtime_machine;
	int error;
	int result;

	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->token);
	if (!machine->stopped.expect_stopped || machine->machine == NULL) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	runtime_machine = machine->machine;
	lwkt_reltoken(&machine->token);
	vmmfs_events_log(&machine->events, "stop requested");
	result = 0;
	error = vmmfs_vcpu_stop(&machine->vcpu);
	if (result == 0)
		result = error;
	error = vmmfs_platform_x64_stop(&machine->platform);
	if (result == 0)
		result = error;
	error = vmmfs_serialroot_stop(&machine->serialroot);
	if (result == 0)
		result = error;
	error = vmmfs_pciroot_stop(&machine->pciroot);
	if (result == 0)
		result = error;
	error = vmmfs_rtc_stop(&machine->rtc);
	if (result == 0)
		result = error;
	error = vmm_machine_destroy(runtime_machine);
	if (result == 0)
		result = error;
	if (error != 0)
		return (result);
	vmmfs_memory_release(&machine->memory);
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->stopped.expect_stopped && machine->machine != NULL);
	machine->machine = NULL;
	lwkt_reltoken(&machine->token);
	vmmfs_events_log(&machine->events, "stop completed");
	return (result);
}
