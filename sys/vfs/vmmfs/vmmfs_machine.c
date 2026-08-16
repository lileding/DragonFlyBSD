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
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"

#define VMMFS_MACHINE_MODE 0555

enum vmmfs_machine_task_type {
	VMMFS_MACHINE_TASK_STOP,
	VMMFS_MACHINE_TASK_RESET,
};

struct vmmfs_machine_task {
	struct task task;
	struct vmmfs_machine *machine;
};

struct vmmfs_machine_start_task {
	struct vmmfs_machine_task task;
	struct vmmfs_machine_spec spec;
};

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
static int vmmfs_machine_prepare_start(struct vmmfs_machine *);
static int vmmfs_machine_enqueue(struct vmmfs_machine *,
	enum vmmfs_machine_task_type);
static void vmmfs_machine_task_done(struct vmmfs_machine_task *);
static void vmmfs_machine_start(void *, int);
static void vmmfs_machine_stop(void *, int);
static void vmmfs_machine_task_reset(void *, int);

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
	state = (struct vmmfs_mount *)root->mount->mnt_data;
	machine->inode = atomic_fetchadd_int(&state->next_inode, 1);
	bcopy(name, machine->name, namelen);
	machine->name[namelen] = '\0';
	lwkt_token_init(&machine->token, "vmmfsmachine");
	machine->taskqueue = taskqueue_create("vmmfs_machine", M_WAITOK,
	    taskqueue_thread_enqueue, &machine->taskqueue);
	if (machine->taskqueue == NULL)
		goto fail_token;
	error = taskqueue_start_threads(&machine->taskqueue, 1,
	    TDPRI_KERN_DAEMON, -1, "vmmfs machine");
	if (error != 0)
		goto fail_taskqueue;
	error = vmmfs_vcpu_create(machine, &machine->vcpu);
	if (error != 0)
		goto fail_taskqueue;
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
	error = vmmfs_events_create(machine, &machine->events);
	if (error != 0)
		goto fail_pciroot;
	error = getnewvnode(VT_SYNTH, root->mount, &vnode, 0, 0);
	if (error != 0)
		goto fail_events;
	vnode->v_data = machine;
	vnode->v_ops = &state->machine_vops;
	vnode->v_type = VDIR;
	machine->vnode = vnode;
	vx_downgrade(vnode);
	vref(vnode);
	vmmfs_events_log(&machine->events, "machine created");
	vmmfs_events_log(&machine->events, "state stopped reason=create");
	return (machine);

fail_events:
	(void)vmmfs_events_destroy(&machine->events);
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
fail_taskqueue:
	taskqueue_free(machine->taskqueue);
	machine->taskqueue = NULL;
fail_token:
	lwkt_token_uninit(&machine->token);
	kfree(machine, M_VMMFS);
	return (NULL);
}

int
vmmfs_machine_destroy(struct vmmfs_machine *machine)
{
	struct taskqueue *taskqueue;
	struct vnode *vnode;
	int error;

	KKASSERT(machine->root == NULL);
	lwkt_gettoken(&machine->token);
	if (atomic_load_acq_int(&machine->pending_task_count) != 0 ||
	    !RB_EMPTY(&machine->pciroot.slots)) {
		lwkt_reltoken(&machine->token);
		return (EBUSY);
	}
	taskqueue = machine->taskqueue;
	machine->taskqueue = NULL;
	lwkt_reltoken(&machine->token);
	error = vmmfs_events_destroy(&machine->events);
	if (error != 0)
		goto fail_taskqueue;
	error = vmmfs_stopped_destroy(&machine->stopped);
	if (error != 0)
		goto fail_taskqueue;
	error = vmmfs_loader_destroy(&machine->loader);
	if (error != 0)
		goto fail_taskqueue;
	error = vmmfs_memory_destroy(&machine->memory);
	if (error != 0)
		goto fail_taskqueue;
	error = vmmfs_vcpu_destroy(&machine->vcpu);
	if (error != 0)
		goto fail_taskqueue;
	error = vmmfs_pciroot_destroy(&machine->pciroot);
	if (error != 0)
		goto fail_taskqueue;
	if (taskqueue != NULL)
		taskqueue_free(taskqueue);

	/* Release the reference retained by vmmfs_machine_create(). */
	vnode = machine->vnode;
	if (vnode != NULL)
		vrele(vnode);
	return (0);

fail_taskqueue:
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->taskqueue == NULL);
	machine->taskqueue = taskqueue;
	lwkt_reltoken(&machine->token);
	return (error);
}

void
vmmfs_machine_free(struct vmmfs_machine *machine)
{
	KKASSERT(machine->root == NULL);
	KKASSERT(machine->vnode == NULL);
	KKASSERT(machine->taskqueue == NULL);
	KKASSERT(atomic_load_acq_int(&machine->pending_task_count) == 0);
	lwkt_token_uninit(&machine->token);
	kfree(machine, M_VMMFS);
}

int
vmmfs_machine_reset(struct vmmfs_machine *machine)
{
	int error;

	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->token);
	error = vmmfs_machine_enqueue(machine, VMMFS_MACHINE_TASK_RESET);
	lwkt_reltoken(&machine->token);
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
	if (machine == NULL)
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
	if (machine == NULL)
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
	if (machine->stopped.expect_stopped) {
		lwkt_reltoken(&machine->token);
		return (EEXIST);
	}
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
	lwkt_gettoken(&machine->token);
	if (machine->stopped.expect_stopped) {
		lwkt_reltoken(&machine->token);
		vput(vnode);
		return (EEXIST);
	}
	machine->stopped.expect_stopped = true;
	error = vmmfs_machine_enqueue(machine, VMMFS_MACHINE_TASK_STOP);
	if (error != 0) {
		machine->stopped.expect_stopped = false;
		lwkt_reltoken(&machine->token);
		vput(vnode);
		return (error);
	}
	lwkt_reltoken(&machine->token);
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
	if (machine == NULL || root == NULL)
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
	error = vmmfs_machine_prepare_start(machine);
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
	if (machine == NULL)
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
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop;
	return (error);
}

static int
vmmfs_machine_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_machine *machine;
	int destroy;

	machine = ap->a_vp->v_data;
	if (machine == NULL)
		return (0);
	lwkt_gettoken(&machine->token);
	if (machine->vnode == ap->a_vp)
		machine->vnode = NULL;
	destroy = machine->root == NULL;
	lwkt_reltoken(&machine->token);
	ap->a_vp->v_data = NULL;
	if (destroy)
		vmmfs_machine_free(machine);
	return (0);
}

static int
vmmfs_machine_prepare_start(struct vmmfs_machine *machine)
{
	struct vmmfs_machine_start_task *task;
	int error;

	task = kmalloc(sizeof(*task), M_VMMFS, M_WAITOK | M_ZERO);
	TASK_INIT(&task->task.task, 0, vmmfs_machine_start, task);
	lwkt_gettoken(&machine->token);
	if (!machine->stopped.expect_stopped) {
		error = ENOENT;
		goto done;
	}
	if (machine->taskqueue == NULL) {
		error = EPIPE;
		goto done;
	}
	task->task.machine = machine;
	task->spec = machine->spec;
	atomic_add_int(&machine->pending_task_count, 1);
	machine->stopped.expect_stopped = false;
	error = taskqueue_enqueue(machine->taskqueue, &task->task.task);
	if (error != 0) {
		machine->stopped.expect_stopped = true;
		atomic_subtract_int(&machine->pending_task_count, 1);
	}

done:
	lwkt_reltoken(&machine->token);
	if (error != 0)
		kfree(task, M_VMMFS);
	return (error);
}

static int
vmmfs_machine_enqueue(struct vmmfs_machine *machine,
	enum vmmfs_machine_task_type type)
{
	struct vmmfs_machine_task *task;
	task_fn_t *handler;
	int error;

	switch (type) {
	case VMMFS_MACHINE_TASK_STOP:
		handler = vmmfs_machine_stop;
		break;
	case VMMFS_MACHINE_TASK_RESET:
		handler = vmmfs_machine_task_reset;
		break;
	default:
		return (EINVAL);
	}
	task = kmalloc(sizeof(*task), M_VMMFS, M_NOWAIT | M_ZERO);
	if (task == NULL)
		return (ENOMEM);
	TASK_INIT(&task->task, 0, handler, task);
	task->machine = machine;
	if (machine->taskqueue == NULL) {
		kfree(task, M_VMMFS);
		return (EPIPE);
	}
	atomic_add_int(&machine->pending_task_count, 1);
	error = taskqueue_enqueue(machine->taskqueue, &task->task);
	if (error != 0) {
		atomic_subtract_int(&machine->pending_task_count, 1);
		kfree(task, M_VMMFS);
	}
	return (error);
}

static void
vmmfs_machine_task_done(struct vmmfs_machine_task *task)
{
	KKASSERT(atomic_load_acq_int(&task->machine->pending_task_count) != 0);
	atomic_subtract_int(&task->machine->pending_task_count, 1);
	kfree(task, M_VMMFS);
}

static void
vmmfs_machine_start(void *arg, int pending)
{
	struct vmmfs_machine_start_task *task = arg;

	KKASSERT(pending == 1);
	vmmfs_events_log(&task->task.machine->events, "start requested");
	vmmfs_events_log(&task->task.machine->events, "start completed");
	vmmfs_machine_task_done(&task->task);
}

static void
vmmfs_machine_stop(void *arg, int pending)
{
	struct vmmfs_machine_task *task = arg;

	KKASSERT(pending == 1);
	vmmfs_events_log(&task->machine->events, "stop requested");
	vmmfs_events_log(&task->machine->events, "stop completed");
	vmmfs_machine_task_done(task);
}

static void
vmmfs_machine_task_reset(void *arg, int pending)
{
	struct vmmfs_machine_task *task = arg;

	KKASSERT(pending == 1);
	vmmfs_events_log(&task->machine->events, "reset requested");
	vmmfs_events_log(&task->machine->events, "reset completed");
	vmmfs_machine_task_done(task);
}
