/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs machine loader declaration node.
 */
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/kern_syscall.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/spinlock.h>
#include <sys/spinlock2.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/sysmsg.h>
#include <sys/sysproto.h>
#include <sys/uio.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/pmap.h>
#include <machine/vmparam.h>

#include "vmmfs.h"
#include "vmmfs_boot.h"
#include "vmmfs_loader.h"
#include "vmmfs_machine.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"

#define VMMFS_LOADER_MODE 0644
#define VMMFS_LOADER_WAIT_TICKS (hz * 10)
#define VMMFS_LOADER_SHELL "/bin/sh"

#define VMMFS_LOADER_INITING -3
#define VMMFS_LOADER_PAUSED -2
#define VMMFS_LOADER_RUNNING -1
#define VMMFS_LOADER_OK 0
#define VMMFS_LOADER_FAILED 1

enum vmmfs_loader_file_state {
	VMMFS_LOADER_FILE_ACTIVE,
	VMMFS_LOADER_FILE_SUBMITTING,
	VMMFS_LOADER_FILE_SUBMITTED,
	VMMFS_LOADER_FILE_FAILED,
};

struct vmmfs_loader_file;

struct vmmfs_loader_process {
	const char *script;
	struct vmmfs_events *events;
	struct vmmfs_boot *boot;
	struct file *file;
	struct vnode *vnode;
	struct vmmfs_loader_file *control;
	pid_t pid;
	int error;
	int state;
	int file_installed;
};

struct vmmfs_loader_file {
	struct vmmfs_loader_process *process;
	struct vmmfs_boot *boot;
	struct spinlock lock;
	enum vmmfs_loader_file_state state;
	int references;
	int counted;
};

static int vmmfs_loader_file_count;

static void vmmfs_loader_process_complete(struct vmmfs_loader_process *, int);
static int vmmfs_loader_process_init(struct vmmfs_loader_process *,
	const char *, struct vmmfs_boot *, struct ucred *,
	struct vmmfs_events *);
static int vmmfs_loader_process_install(struct vmmfs_loader_process *);
static int vmmfs_loader_process_resume(struct vmmfs_loader_process *);
static int vmmfs_loader_process_wait(struct vmmfs_loader_process *);
static void vmmfs_loader_process_finish(struct vmmfs_loader_process *);
static void vmmfs_loader_process_kill(struct vmmfs_loader_process *);
static void vmmfs_loader_child(void *, struct trapframe *);
static void vmmfs_loader_child_exit(int);
static int vmmfs_loader_install_fd(struct file *, int);
static int vmmfs_loader_exec_shell(const char *);
static void vmmfs_loader_set_process_cred(struct proc *, struct ucred *);
static int vmmfs_loader_open_file(struct vmmfs_loader_process *);
static int vmmfs_loader_make_vnode(cdev_t, struct vnode **);
static void vmmfs_loader_revoke_vnode(struct vnode *);
static void vmmfs_loader_file_detach(struct vmmfs_loader_file *);
static void vmmfs_loader_file_vnode_destroy(struct vnode *);
static void vmmfs_loader_file_hold(struct vmmfs_loader_file *);
static void vmmfs_loader_file_put(struct vmmfs_loader_file *);
static void vmmfs_loader_file_free(void *);
static int vmmfs_loader_file_getattr(struct vop_getattr_args *);
static int vmmfs_loader_file_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_loader_file_write(struct file *, struct uio *,
	struct ucred *, int);
static int vmmfs_loader_file_ioctl(struct file *, u_long, caddr_t,
	struct ucred *, struct sysmsg *);
static int vmmfs_loader_file_kqfilter(struct file *, struct knote *);
static int vmmfs_loader_file_stat(struct file *, struct stat *,
	struct ucred *);
static int vmmfs_loader_file_close(struct file *);
static int vmmfs_loader_file_seek(struct file *, off_t, int, off_t *);
static void vmmfs_loader_file_disarm(struct file *);

static void vmmfs_loader_drop(struct vmmfs_node *);
static int vmmfs_loader_node_load(struct vmmfs_node *, char *, size_t, size_t *);
static int vmmfs_loader_node_store(struct vmmfs_node *, const char *, size_t);

struct vop_ops vmmfs_loader_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_node_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_setattr = vmmfs_node_setattr,
	.vop_write = vmmfs_node_write,
};

static struct vop_ops vmmfs_loader_file_vops = {
	.vop_default = vop_defaultop,
	.vop_close = vop_stdclose,
	.vop_advlock = (void *)vop_null,
	.vop_getattr = vmmfs_loader_file_getattr,
	.vop_getattr_lite = vmmfs_loader_file_getattr_lite,
	.vop_inactive = (void *)vop_null,
	.vop_mmap = (void *)vop_null,
	.vop_reclaim = (void *)vop_null,
	.vop_pathconf = vop_stdpathconf,
};

static struct vop_ops *vmmfs_loader_file_vops_pointer =
	&vmmfs_loader_file_vops;

int vmmfs_loader_module_init(void);
int vmmfs_loader_module_fini(void);

static struct fileops vmmfs_loader_fileops = {
	.fo_read = badfo_readwrite,
	.fo_write = vmmfs_loader_file_write,
	.fo_ioctl = vmmfs_loader_file_ioctl,
	.fo_kqfilter = vmmfs_loader_file_kqfilter,
	.fo_stat = vmmfs_loader_file_stat,
	.fo_close = vmmfs_loader_file_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = vmmfs_loader_file_seek,
};

int
vmmfs_loader_module_fini(void)
{
	return (atomic_fetchadd_int(&vmmfs_loader_file_count, 0) == 0 ?
	    0 : EBUSY);
}

int
vmmfs_loader_run(struct vmmfs_loader *loader, const char *script,
	struct vmmfs_boot *boot, struct ucred *cred)
{
	struct vmmfs_loader_process process;
	int error;

	if (loader == NULL || script == NULL || boot == NULL || cred == NULL ||
	    vmmfs_loader_machine(loader) == NULL || vmmfs_boot_machine(boot) != vmmfs_loader_machine(loader) ||
	    script[0] == '\0')
		return (EINVAL);
	bzero(&process, sizeof(process));
	error = vmmfs_loader_process_init(&process, script, boot, cred,
	    &vmmfs_loader_machine(loader)->events);
	if (error == 0)
		error = vmmfs_loader_process_install(&process);
	if (error == 0)
		error = vmmfs_loader_process_resume(&process);
	if (error == 0)
		error = vmmfs_loader_process_wait(&process);
	if (error == 0)
		vmmfs_events_log(&vmmfs_loader_machine(loader)->events,
		    VMMFS_MACHINE_EVENT_LOADER_SUBMITTED_CPUSTATE, NULL);
	else
		vmmfs_events_log(&vmmfs_loader_machine(loader)->events,
		    VMMFS_MACHINE_EVENT_LOADER_FAILED, "error=%d", error);
	vmmfs_loader_process_finish(&process);
	return (error);
}

static void
vmmfs_loader_process_complete(struct vmmfs_loader_process *process, int error)
{
	int state;

	if (process == NULL)
		return;
	for (;;) {
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_OK || state == VMMFS_LOADER_FAILED)
			return;
		if (atomic_cmpset_int(&process->state, state,
		    error == 0 ? VMMFS_LOADER_OK : VMMFS_LOADER_FAILED)) {
			atomic_store_rel_int(&process->error, error);
			wakeup(process);
			return;
		}
	}
}

static int
vmmfs_loader_process_init(struct vmmfs_loader_process *process,
	const char *script, struct vmmfs_boot *boot, struct ucred *cred,
	struct vmmfs_events *events)
{
	struct proc *child;
	struct lwp *child_lwp;
	int error;
	int state;

	if (process == NULL || script == NULL || script[0] == '\0' ||
	    boot == NULL || vmmfs_boot_machine(boot) == NULL || cred == NULL ||
	    events == NULL)
		return (EINVAL);
	process->script = script;
	process->boot = boot;
	process->events = events;
	process->state = VMMFS_LOADER_INITING;
	error = fork1(curthread->td_lwp,
	    RFFDG | RFPROC | RFPGLOCK | RFNOWAIT, &child);
	if (error != 0)
		return (error);
	process->pid = child->p_pid;
	child_lwp = ONLY_LWP_IN_PROC(child);
	vmmfs_loader_set_process_cred(child, cred);
	cpu_set_fork_handler(child_lwp, vmmfs_loader_child, process);
	PHOLD(child);
	start_forked_proc(curthread->td_lwp, child);
	PRELE(child);
	for (;;) {
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_PAUSED)
			return (0);
		if (state == VMMFS_LOADER_FAILED)
			return (atomic_load_acq_int(&process->error));
		tsleep_interlock(process, PCATCH);
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_PAUSED ||
		    state == VMMFS_LOADER_FAILED)
			continue;
		error = tsleep(process, PINTERLOCKED | PCATCH, "vmmfsldi",
		    VMMFS_LOADER_WAIT_TICKS);
		if (error == 0)
			continue;
		if (error == EWOULDBLOCK)
			error = ETIMEDOUT;
		vmmfs_loader_process_complete(process, error);
		vmmfs_loader_process_kill(process);
		return (error);
	}
}

static int
vmmfs_loader_process_install(struct vmmfs_loader_process *process)
{
	if (process == NULL || process->boot == NULL ||
	    atomic_fetchadd_int(&process->state, 0) != VMMFS_LOADER_PAUSED)
		return (EINVAL);
	return (vmmfs_loader_open_file(process));
}

static int
vmmfs_loader_process_resume(struct vmmfs_loader_process *process)
{
	struct file *file;
	int error;
	int state;

	if (process == NULL || process->file == NULL ||
	    !atomic_cmpset_int(&process->state, VMMFS_LOADER_PAUSED,
	    VMMFS_LOADER_RUNNING))
		return (ECANCELED);
	wakeup(process);
	for (;;) {
		if (atomic_load_acq_int(&process->file_installed) != 0) {
			file = process->file;
			process->file = NULL;
			fp_close(file);
			return (0);
		}
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_FAILED) {
			error = atomic_load_acq_int(&process->error);
			return (error == 0 ? EPROTO : error);
		}
		tsleep_interlock(process, PCATCH);
		if (atomic_load_acq_int(&process->file_installed) != 0 ||
		    atomic_fetchadd_int(&process->state, 0) ==
		    VMMFS_LOADER_FAILED)
			continue;
		error = tsleep(process, PINTERLOCKED | PCATCH, "vmmfsldf",
		    VMMFS_LOADER_WAIT_TICKS);
		if (error == 0)
			continue;
		if (error == EWOULDBLOCK)
			error = ETIMEDOUT;
		vmmfs_loader_process_complete(process, error);
		vmmfs_loader_process_kill(process);
		return (error);
	}
}

static int
vmmfs_loader_process_wait(struct vmmfs_loader_process *process)
{
	int error;
	int state;

	if (process == NULL)
		return (EINVAL);
	for (;;) {
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_OK)
			return (0);
		if (state == VMMFS_LOADER_FAILED) {
			error = atomic_load_acq_int(&process->error);
			return (error == 0 ? EPROTO : error);
		}
		tsleep_interlock(process, PCATCH);
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_OK || state == VMMFS_LOADER_FAILED)
			continue;
		error = tsleep(process, PINTERLOCKED | PCATCH, "vmmfsld",
		    VMMFS_LOADER_WAIT_TICKS);
		if (error == 0)
			continue;
		if (error == EWOULDBLOCK)
			error = ETIMEDOUT;
		vmmfs_loader_process_complete(process, error);
		vmmfs_loader_process_kill(process);
		return (error);
	}
}

static void
vmmfs_loader_process_finish(struct vmmfs_loader_process *process)
{
	int state;

	if (process == NULL)
		return;
	state = atomic_fetchadd_int(&process->state, 0);
	if (state != VMMFS_LOADER_OK)
		vmmfs_loader_process_kill(process);
	vmmfs_loader_file_detach(process->control);
	vmmfs_loader_revoke_vnode(process->vnode);
	if (process->file != NULL) {
		fp_close(process->file);
		process->file = NULL;
	}
	if (process->vnode != NULL) {
		vmmfs_loader_file_vnode_destroy(process->vnode);
		process->vnode = NULL;
	}
	vmmfs_loader_file_put(process->control);
	process->control = NULL;
	process->pid = 0;
}

static void
vmmfs_loader_process_kill(struct vmmfs_loader_process *process)
{
	struct proc *target;

	if (process == NULL || process->pid <= 0)
		return;
	target = pfind(process->pid);
	if (target != NULL) {
		ksignal(target, SIGKILL);
		PRELE(target);
	}
}

static void
vmmfs_loader_child(void *argument, struct trapframe *frame)
{
	struct vmmfs_loader_process *process;
	struct lwp *lwp;
	int error;
	int state;

	(void)frame;
	process = argument;
	if (process == NULL)
		vmmfs_loader_child_exit(1);
	if (!atomic_cmpset_int(&process->state, VMMFS_LOADER_INITING,
	    VMMFS_LOADER_PAUSED))
		vmmfs_loader_child_exit(1);
	wakeup(process);
	for (;;) {
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_RUNNING)
			break;
		if (state == VMMFS_LOADER_OK || state == VMMFS_LOADER_FAILED)
			vmmfs_loader_child_exit(1);
		tsleep_interlock(process, PCATCH);
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_RUNNING || state == VMMFS_LOADER_OK ||
		    state == VMMFS_LOADER_FAILED)
			continue;
		error = tsleep(process, PINTERLOCKED | PCATCH, "vmmfsldp", 0);
		if (error != 0)
			vmmfs_loader_child_exit(1);
	}
	error = vmmfs_loader_install_fd(process->file, 3);
	if (error == 0) {
		atomic_store_rel_int(&process->file_installed, 1);
		wakeup(process);
	}
	if (error == 0)
		error = vmmfs_loader_exec_shell(process->script);
	if (error == 0) {
		lwp = curthread->td_lwp;
		lwp->lwp_proc->p_usched->acquire_curproc(lwp);
		return;
	}
	vmmfs_loader_process_complete(process, error);
	vmmfs_loader_child_exit(1);
}

static void
vmmfs_loader_child_exit(int code)
{
	struct lwp *lwp;

	lwp = curthread->td_lwp;
	lwp->lwp_proc->p_usched->acquire_curproc(lwp);
	exit1(code);
}

static int
vmmfs_loader_install_fd(struct file *file, int target_fd)
{
	int fd;
	int error;

	error = kern_close(target_fd);
	if (error != 0 && error != EBADF)
		return (error);
	error = fdalloc(curproc, target_fd, &fd);
	if (error != 0)
		return (error);
	if (fd != target_fd) {
		fsetfd(curproc->p_fd, NULL, fd);
		return (EBUSY);
	}
	fsetfd(curproc->p_fd, file, target_fd);
	return (0);
}

static int
vmmfs_loader_exec_shell(const char *script)
{
	struct sysmsg message;
	struct execve_args arguments;
	char *shell_argument;
	char *option_argument;
	char *script_argument;
	char *user_pointer;
	char **user_argv;
	size_t index;
	size_t script_length;
	size_t shell_length;

	if (script == NULL)
		return (EINVAL);
	script_length = strnlen(script, PAGE_SIZE);
	if (script_length == 0 || script_length >= PAGE_SIZE)
		return (EINVAL);
	shell_length = sizeof(VMMFS_LOADER_SHELL) - 1;
	user_pointer = (char *)USRSTACK;
	if (subyte(--user_pointer, 0) != 0)
		return (EFAULT);
	for (index = script_length; index > 0; --index) {
		if (subyte(--user_pointer, script[index - 1]) != 0)
			return (EFAULT);
	}
	script_argument = user_pointer;
	if (subyte(--user_pointer, 0) != 0 ||
	    subyte(--user_pointer, 'c') != 0 ||
	    subyte(--user_pointer, '-') != 0)
		return (EFAULT);
	option_argument = user_pointer;
	if (subyte(--user_pointer, 0) != 0)
		return (EFAULT);
	for (index = shell_length; index > 0; --index) {
		if (subyte(--user_pointer, VMMFS_LOADER_SHELL[index - 1]) != 0)
			return (EFAULT);
	}
	shell_argument = user_pointer;
	user_argv = (char **)rounddown2((intptr_t)user_pointer,
	    sizeof(intptr_t));
	if (suword64((uint64_t *)(caddr_t)--user_argv, 0) != 0)
		return (EFAULT);
	if (suword64((uint64_t *)(caddr_t)--user_argv,
	    (uint64_t)(intptr_t)script_argument) != 0)
		return (EFAULT);
	if (suword64((uint64_t *)(caddr_t)--user_argv,
	    (uint64_t)(intptr_t)option_argument) != 0)
		return (EFAULT);
	if (suword64((uint64_t *)(caddr_t)--user_argv,
	    (uint64_t)(intptr_t)shell_argument) != 0)
		return (EFAULT);
	bzero(&message, sizeof(message));
	arguments.fname = shell_argument;
	arguments.argv = user_argv;
	arguments.envv = NULL;
	return (sys_execve(&message, &arguments));
}

static void
vmmfs_loader_file_vnode_destroy(struct vnode *vnode)
{
	if (vnode == NULL)
		return;
	cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	vfinalize(vnode);
	vrele(vnode);
}

static void
vmmfs_loader_set_process_cred(struct proc *process, struct ucred *cred)
{
	struct lwp *lwp;
	struct ucred *old_cred;

	old_cred = process->p_ucred;
	process->p_ucred = crhold(cred);
	crfree(old_cred);
	lwp = ONLY_LWP_IN_PROC(process);
	old_cred = lwp->lwp_thread->td_ucred;
	lwp->lwp_thread->td_ucred = crhold(cred);
	crfree(old_cred);
}

static int
vmmfs_loader_make_vnode(cdev_t dev, struct vnode **vnodep)
{
	struct vnode *vnode;
	int error;

	if (dev == NULL || vnodep == NULL)
		return (EINVAL);
	error = getspecialvnode(VT_NON, NULL, &vmmfs_loader_file_vops_pointer,
	    &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_type = VCHR;
	error = v_associate_rdev(vnode, dev);
	if (error != 0) {
		vx_downgrade(vnode);
		vn_unlock(vnode);
		vmmfs_loader_file_vnode_destroy(vnode);
		return (error);
	}
	vnode->v_umajor = dev->si_umajor;
	vnode->v_uminor = dev->si_uminor;
	vx_unlock(vnode);
	*vnodep = vnode;
	return (0);
}

static int
vmmfs_loader_open_file(struct vmmfs_loader_process *process)
{
	struct vmmfs_loader_file *control;
	struct file *file;
	struct vnode *vnode;
	int error;

	if (process == NULL || process->boot == NULL ||
	    vmmfs_boot_machine(process->boot) == NULL || process->boot->dev == NULL)
		return (EINVAL);
	control = kmalloc(sizeof(*control), M_VMMFS, M_WAITOK | M_ZERO);
	control->process = process;
	control->boot = process->boot;
	control->state = VMMFS_LOADER_FILE_ACTIVE;
	control->references = 1;
	spin_init(&control->lock, "vmmfsldfd");
	vmmfs_branch_hold(&vmmfs_boot_machine(process->boot)->branch);
	error = vmmfs_loader_make_vnode(process->boot->dev, &vnode);
	if (error != 0)
		goto fail_control;
	error = falloc(NULL, &file, NULL);
	if (error != 0)
		goto fail_vnode;
	fsetcred(file, proc0.p_ucred);
	file->f_type = DTYPE_VNODE;
	file->f_flag = FREAD | FWRITE;
	file->f_ops = &vmmfs_loader_fileops;
	file->f_data = vnode;
	vref(vnode);
	atomic_add_int(&vnode->v_opencount, 1);
	atomic_add_int(&vnode->v_writecount, 1);
	error = devfs_set_cdevpriv(file, control, vmmfs_loader_file_free);
	if (error != 0) {
		fp_close(file);
		goto fail_control;
	}
	vmmfs_loader_file_hold(control);
	control->counted = 1;
	atomic_add_int(&vmmfs_loader_file_count, 1);
	process->file = file;
	process->vnode = vnode;
	process->control = control;
	return (0);

fail_vnode:
	vmmfs_loader_file_vnode_destroy(vnode);
fail_control:
	vmmfs_loader_file_put(control);
	return (error);
}

static void
vmmfs_loader_file_detach(struct vmmfs_loader_file *control)
{
	if (control == NULL)
		return;
	spin_lock(&control->lock);
	control->process = NULL;
	spin_unlock(&control->lock);
}

static void
vmmfs_loader_revoke_vnode(struct vnode *vnode)
{
	if (vnode == NULL)
		return;
	(void)fdrevoke(vnode, DTYPE_VNODE, proc0.p_ucred);
}

static void
vmmfs_loader_file_hold(struct vmmfs_loader_file *control)
{
	if (control != NULL)
		atomic_add_int(&control->references, 1);
}

static void
vmmfs_loader_file_put(struct vmmfs_loader_file *control)
{
	int references;

	if (control == NULL)
		return;
	references = atomic_fetchadd_int(&control->references, -1);
	KKASSERT(references > 0);
	if (references != 1)
		return;
	vmmfs_branch_put(&vmmfs_boot_machine(control->boot)->branch);
	spin_uninit(&control->lock);
	if (control->counted != 0)
		atomic_add_int(&vmmfs_loader_file_count, -1);
	kfree(control, M_VMMFS);
}

static void
vmmfs_loader_file_free(void *argument)
{
	struct vmmfs_loader_file *control;

	control = argument;
	vmmfs_loader_file_put(control);
}

static int
vmmfs_loader_file_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_boot *boot;
	struct vattr *vattr;
	uint64_t size;

	if (ap == NULL || ap->a_vp == NULL || ap->a_vp->v_rdev == NULL)
		return (EBADF);
	boot = ap->a_vp->v_rdev->si_drv1;
	if (boot == NULL || vmmfs_boot_machine(boot) == NULL)
		return (EBADF);
	lwkt_gettoken(&vmmfs_boot_machine(boot)->branch.token);
	size = vmmfs_boot_machine(boot)->memory.size;
	lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VCHR;
	vattr->va_mode = 0600;
	vattr->va_flags = 0;
	vattr->va_nlink = 1;
	vattr->va_uid = UID_ROOT;
	vattr->va_gid = GID_WHEEL;
	vattr->va_size = (off_t)size;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = size;
	return (0);
}

static int
vmmfs_loader_file_getattr_lite(struct vop_getattr_lite_args *ap)
{
	struct vmmfs_boot *boot;
	uint64_t size;

	if (ap == NULL || ap->a_vp == NULL || ap->a_vp->v_rdev == NULL)
		return (EBADF);
	boot = ap->a_vp->v_rdev->si_drv1;
	if (boot == NULL || vmmfs_boot_machine(boot) == NULL)
		return (EBADF);
	lwkt_gettoken(&vmmfs_boot_machine(boot)->branch.token);
	size = vmmfs_boot_machine(boot)->memory.size;
	lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
	ap->a_lvap->va_type = VCHR;
	ap->a_lvap->va_mode = 0600;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = UID_ROOT;
	ap->a_lvap->va_gid = GID_WHEEL;
	ap->a_lvap->va_size = (off_t)size;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_loader_file_write(struct file *file, struct uio *uio,
	struct ucred *cred, int flags)
{
	struct vmmfs_loader_file *control;
	struct vmmfs_loader_process *process;
	struct vmm_cpustate state;
	int error;

	(void)cred;
	(void)flags;
	if (file == NULL || uio == NULL || uio->uio_rw != UIO_WRITE ||
	    (uio->uio_offset != -1 && uio->uio_offset != 0) ||
	    uio->uio_resid != sizeof(state))
		return (EINVAL);
	uio->uio_offset = 0;
	error = devfs_get_cdevpriv(file, (void **)&control);
	if (error != 0 || control == NULL)
		return (error == 0 ? EBADF : error);
	error = uiomove((caddr_t)&state, sizeof(state), uio);
	if (error != 0)
		return (error);
	spin_lock(&control->lock);
	if (control->process == NULL)
		error = EBADF;
	else if (control->state != VMMFS_LOADER_FILE_ACTIVE)
		error = EBUSY;
	else {
		control->state = VMMFS_LOADER_FILE_SUBMITTING;
		process = control->process;
		error = 0;
	}
	spin_unlock(&control->lock);
	if (error != 0)
		return (error);
	error = vmmfs_boot_submit(control->boot, &state);
	spin_lock(&control->lock);
	control->state = error == 0 ? VMMFS_LOADER_FILE_SUBMITTED :
	    VMMFS_LOADER_FILE_FAILED;
	spin_unlock(&control->lock);
	vmmfs_loader_process_complete(process, error);
	return (error);
}

static int
vmmfs_loader_file_ioctl(struct file *file, u_long command, caddr_t data,
	struct ucred *cred, struct sysmsg *message)
{
	(void)file;
	(void)command;
	(void)data;
	(void)cred;
	(void)message;
	return (EOPNOTSUPP);
}

static int
vmmfs_loader_file_kqfilter(struct file *file, struct knote *knote)
{
	(void)file;
	(void)knote;
	return (EOPNOTSUPP);
}

static int
vmmfs_loader_file_stat(struct file *file, struct stat *status,
	struct ucred *cred)
{
	struct vmmfs_loader_file *control;
	uint64_t size;
	int error;

	(void)cred;
	error = devfs_get_cdevpriv(file, (void **)&control);
	if (error != 0 || control == NULL)
		return (error == 0 ? EBADF : error);
	lwkt_gettoken(&vmmfs_boot_machine(control->boot)->branch.token);
	size = vmmfs_boot_machine(control->boot)->memory.size;
	lwkt_reltoken(&vmmfs_boot_machine(control->boot)->branch.token);
	bzero(status, sizeof(*status));
	status->st_nlink = 1;
	status->st_mode = S_IFCHR | 0600;
	status->st_uid = UID_ROOT;
	status->st_gid = GID_WHEEL;
	status->st_size = (off_t)size;
	status->st_blocks = howmany(size, S_BLKSIZE);
	status->st_blksize = PAGE_SIZE;
	status->__old_st_blksize = status->st_blksize;
	return (0);
}

static int
vmmfs_loader_file_close(struct file *file)
{
	struct vmmfs_loader_file *control;
	struct vmmfs_loader_process *process;
	bool failed;

	process = NULL;
	failed = false;
	if (file != NULL &&
	    devfs_get_cdevpriv(file, (void **)&control) == 0 &&
	    control != NULL) {
		spin_lock(&control->lock);
		if (control->state == VMMFS_LOADER_FILE_ACTIVE) {
			control->state = VMMFS_LOADER_FILE_FAILED;
			process = control->process;
			failed = true;
		}
		spin_unlock(&control->lock);
	}
	if (failed)
		vmmfs_loader_process_complete(process, EPIPE);
	vmmfs_loader_file_disarm(file);
	return (0);
}

static int
vmmfs_loader_file_seek(struct file *file, off_t offset, int whence,
	off_t *result)
{
	(void)file;
	(void)offset;
	(void)whence;
	(void)result;
	return (ESPIPE);
}

static void
vmmfs_loader_file_disarm(struct file *file)
{
	struct vnode *vnode;

	if (file == NULL || file->f_ops == &badfileops)
		return;
	vnode = file->f_data;
	file->f_data = NULL;
	atomic_clear_int(&file->f_flag, FHASLOCK);
	file->f_ops = &badfileops;
	if (vnode != NULL)
		(void)vn_close(vnode, file->f_flag, file);
	devfs_clear_cdevpriv(file);
}

static int
vmmfs_loader_load(struct vmmfs_loader *loader, char *buffer,
	size_t capacity, size_t *length)
{
	int result;

	if (loader == NULL || loader->node.dead)
		return (ENOENT);
	result = ksnprintf(buffer, capacity, "%s\n", loader->script);
	if (result < 0 || (size_t)result >= capacity)
		return (EOVERFLOW);
	*length = (size_t)result;
	return (0);
}

static int
vmmfs_loader_store(struct vmmfs_loader *loader, const char *buffer,
	size_t length)
{
	if (length == 0)
		return (EINVAL);
	if (buffer[length - 1] == '\n')
		--length;
	if (length == 0 || length >= sizeof(loader->script))
		return (ENAMETOOLONG);
	if (loader->node.dead)
		return (ENOENT);
	lwkt_gettoken(&vmmfs_loader_machine(loader)->branch.token);
	if (vmmfs_loader_machine(loader)->machine != NULL) {
		lwkt_reltoken(&vmmfs_loader_machine(loader)->branch.token);
		return (EBUSY);
	}
	bcopy(buffer, loader->script, length);
	loader->script[length] = 0;
	loader->node.size = (off_t)length + 1;
	lwkt_reltoken(&vmmfs_loader_machine(loader)->branch.token);
	return (0);
}

static int
vmmfs_loader_node_load(struct vmmfs_node *node, char *buffer,
	size_t capacity, size_t *length)
{
	return (vmmfs_loader_load((struct vmmfs_loader *)node, buffer,
	    capacity, length));
}

static int
vmmfs_loader_node_store(struct vmmfs_node *node, const char *buffer,
	size_t length)
{
	return (vmmfs_loader_store((struct vmmfs_loader *)node, buffer, length));
}

int
vmmfs_loader_init(struct vmmfs_mount *mount, struct vmmfs_branch *parent,
	struct vmmfs_loader *loader,
	struct vnode **vnodep)
{
	struct vmmfs_root *root;
	int error;

	if (mount == NULL || parent == NULL || loader == NULL || vnodep == NULL)
		return (EINVAL);
	root = mount->root_vnode == NULL ? NULL : mount->root_vnode->v_data;
	if (root == NULL)
		return (ENXIO);
	*vnodep = NULL;
	bzero(loader, sizeof(*loader));
	loader->node.parent = parent;
	loader->node.dead = false;
	loader->node.deactivate = vmmfs_node_default_deactivate;
	loader->node.drop = vmmfs_loader_drop;
	if (parent != NULL)
		vmmfs_branch_hold(parent);
	loader->node.load_limit = PAGE_SIZE + 1;
	loader->node.store_limit = PAGE_SIZE - 1;
	loader->node.load = vmmfs_loader_node_load;
	loader->node.store = vmmfs_loader_node_store;
	if (mount == NULL || mount->loader_vops == NULL) {
		error = ENXIO;
		goto fail;
	}
	loader->node.inode = vmmfs_root_allocate_inode(root);
	loader->node.mode = VMMFS_LOADER_MODE;
	loader->node.size = 1;
	error = vmmfs_vnode_create_regular(mount->mount,
	    &mount->loader_vops, VREG, &loader->node, vnodep);
	if (error == 0)
		return (0);

fail:
	vmmfs_node_drop(&loader->node);
	return (error);
}

static void
vmmfs_loader_drop(struct vmmfs_node *node)
{
	struct vmmfs_loader *loader;

	loader = (struct vmmfs_loader *)node;
	KKASSERT(loader != NULL);
	loader->node.inode = 0;
	vmmfs_node_parent_put(node);
}
