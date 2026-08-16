/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs machine loader declaration node.
 */
#include <sys/dirent.h>
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
#include <sys/resource.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/spinlock.h>
#include <sys/spinlock2.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <sys/systm.h>
#include <sys/sysmsg.h>
#include <sys/sysproto.h>
#include <sys/uio.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include <machine/atomic.h>
#include <machine/vmparam.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include "vmmfs.h"

#define VMMFS_LOADER_MODE 0644
#define VMMFS_LOADER_WAIT_TICKS (hz * 10)

#define VMMFS_LOADER_INITING -3
#define VMMFS_LOADER_PAUSED -2
#define VMMFS_LOADER_RUNNING -1
#define VMMFS_LOADER_OK 0
#define VMMFS_LOADER_FAILED 1

struct vmmfs_loader_handler {
	RB_ENTRY(vmmfs_loader_handler) entry;
	pid_t pid;
	void (*exit_callback)(void *, int);
	void *argument;
	void *wait_channel;
};

struct vmmfs_loader_domain {
	struct spinlock spin;
	RB_HEAD(vmmfs_loader_handler_tree, vmmfs_loader_handler) handlers;
	bool initialized;
};

struct vmmfs_loader_fd {
	cdev_t dev;
	struct vnode *vnode;
	struct vm_object *object;
	struct vm_object *backing_object;
	vm_size_t size;
	int references;
	int revoked;
};

struct vmmfs_loader_process {
	const char *path;
	pid_t pid;
	struct vmmfs_loader_handler handler;
	struct file *memory_file;
	int state;
};

static struct vmmfs_loader_domain vmmfs_loader_domain;
static int vmmfs_loader_mmap_object_count;
static uint32_t vmmfs_loader_fd_serial;

static int vmmfs_loader_handler_compare(struct vmmfs_loader_handler *,
	struct vmmfs_loader_handler *);
static void vmmfs_loader_process_exit(struct thread *);
static int vmmfs_loader_handler_register(struct vmmfs_loader_handler *);
static void vmmfs_loader_handler_unregister(struct vmmfs_loader_handler *);
static void vmmfs_loader_exit_callback(void *, int);
static int vmmfs_loader_process_init(struct vmmfs_loader_process *,
	const char *, struct ucred *);
static int vmmfs_loader_process_install(struct vmmfs_loader_process *,
	struct vm_object *, uint64_t);
static int vmmfs_loader_process_resume(struct vmmfs_loader_process *);
static int vmmfs_loader_process_wait(struct vmmfs_loader_process *);
static void vmmfs_loader_process_finish(struct vmmfs_loader_process *);
static void vmmfs_loader_process_kill(struct vmmfs_loader_process *);
static void vmmfs_loader_child(void *, struct trapframe *);
static void vmmfs_loader_child_exit(int);
static int vmmfs_loader_install_fd(struct file *, int);
static int vmmfs_loader_exec_path(const char *);
static void vmmfs_loader_set_process_cred(struct proc *, struct ucred *);
static int vmmfs_loader_open_memory_fd(struct vm_object *, vm_size_t,
	struct file **);
static int vmmfs_loader_open_fd(struct vmmfs_loader_fd *, struct file **);
static int vmmfs_loader_make_vnode(cdev_t, struct vnode **);
static void vmmfs_loader_revoke(struct vmmfs_loader_process *);
static void vmmfs_loader_revoke_file(struct file *);
static void vmmfs_loader_close_files(struct vmmfs_loader_process *);
static void vmmfs_loader_fd_revoke(struct vmmfs_loader_fd *);
static void vmmfs_loader_fd_free(void *);
static void vmmfs_loader_fd_ref(struct vmmfs_loader_fd *);
static void vmmfs_loader_fd_put(struct vmmfs_loader_fd *);
static int vmmfs_loader_fd_open(struct dev_open_args *);
static int vmmfs_loader_fd_close(struct dev_close_args *);
static int vmmfs_loader_fd_mmap_single(struct dev_mmap_single_args *);
static int vmmfs_loader_fd_getattr(struct vop_getattr_args *);
static int vmmfs_loader_file_readwrite(struct file *, struct uio *,
	struct ucred *, int);
static int vmmfs_loader_file_ioctl(struct file *, u_long, caddr_t,
	struct ucred *, struct sysmsg *);
static int vmmfs_loader_file_kqfilter(struct file *, struct knote *);
static int vmmfs_loader_file_stat(struct file *, struct stat *,
	struct ucred *);
static int vmmfs_loader_file_close(struct file *);
static int vmmfs_loader_file_seek(struct file *, off_t, int, off_t *);
static void vmmfs_loader_file_disarm(struct file *);
static int vmmfs_loader_pager_fault(vm_object_t, vm_ooffset_t, int,
	vm_page_t *);
static int vmmfs_loader_pager_ctor(void *, vm_ooffset_t, vm_prot_t,
	vm_ooffset_t, struct ucred *, u_short *);
static void vmmfs_loader_pager_dtor(void *);

RB_PROTOTYPE_STATIC(vmmfs_loader_handler_tree, vmmfs_loader_handler, entry,
	vmmfs_loader_handler_compare);
RB_GENERATE_STATIC(vmmfs_loader_handler_tree, vmmfs_loader_handler, entry,
	vmmfs_loader_handler_compare);

static int vmmfs_loader_access(struct vop_access_args *);
static int vmmfs_loader_getattr(struct vop_getattr_args *);
static int vmmfs_loader_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_loader_open(struct vop_open_args *);
static int vmmfs_loader_read(struct vop_read_args *);
static int vmmfs_loader_setattr(struct vop_setattr_args *);
static int vmmfs_loader_write(struct vop_write_args *);
static int vmmfs_loader_reclaim(struct vop_reclaim_args *);

struct vop_ops vmmfs_loader_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_loader_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_loader_getattr,
	.vop_getattr_lite = vmmfs_loader_getattr_lite,
	.vop_open = vmmfs_loader_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_loader_read,
	.vop_reclaim = vmmfs_loader_reclaim,
	.vop_setattr = vmmfs_loader_setattr,
	.vop_write = vmmfs_loader_write,
};

static struct dev_ops vmmfs_loader_fd_ops = {
	{ "vmmfs_loader_fd", 0, D_MPSAFE },
	.d_open = vmmfs_loader_fd_open,
	.d_close = vmmfs_loader_fd_close,
	.d_mmap_single = vmmfs_loader_fd_mmap_single,
};

static struct cdev_pager_ops vmmfs_loader_pager_ops = {
	.cdev_pg_fault = vmmfs_loader_pager_fault,
	.cdev_pg_ctor = vmmfs_loader_pager_ctor,
	.cdev_pg_dtor = vmmfs_loader_pager_dtor,
};

static struct vop_ops vmmfs_loader_fd_vops = {
	.vop_default = vop_defaultop,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_loader_fd_getattr,
	.vop_advlock = (void *)vop_null,
	.vop_inactive = (void *)vop_null,
	.vop_reclaim = (void *)vop_null,
	.vop_pathconf = vop_stdpathconf,
};

static struct vop_ops *vmmfs_loader_fd_vops_pointer = &vmmfs_loader_fd_vops;

static struct fileops vmmfs_loader_fileops = {
	.fo_read = vmmfs_loader_file_readwrite,
	.fo_write = vmmfs_loader_file_readwrite,
	.fo_ioctl = vmmfs_loader_file_ioctl,
	.fo_kqfilter = vmmfs_loader_file_kqfilter,
	.fo_stat = vmmfs_loader_file_stat,
	.fo_close = vmmfs_loader_file_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = vmmfs_loader_file_seek,
};

int
vmmfs_loader_init(void)
{
	int error;

	spin_init(&vmmfs_loader_domain.spin, "vmmfsload");
	RB_INIT(&vmmfs_loader_domain.handlers);
	error = at_exit(vmmfs_loader_process_exit);
	if (error != 0) {
		spin_uninit(&vmmfs_loader_domain.spin);
		return (error);
	}
	vmmfs_loader_domain.initialized = true;
	return (0);
}

int
vmmfs_loader_uninit(void)
{
	if (!vmmfs_loader_domain.initialized)
		return (0);
	if (!RB_EMPTY(&vmmfs_loader_domain.handlers) ||
	    atomic_fetchadd_int(&vmmfs_loader_mmap_object_count, 0) != 0)
		return (EBUSY);
	rm_at_exit(vmmfs_loader_process_exit);
	spin_uninit(&vmmfs_loader_domain.spin);
	vmmfs_loader_domain.initialized = false;
	return (0);
}

int
vmmfs_loader_run(struct vmmfs_loader *loader, struct vmmfs_memory *memory,
	struct ucred *cred)
{
	char event[96];
	struct vmmfs_loader_process process;
	int error;

	if (loader == NULL || memory == NULL || cred == NULL ||
	    loader->machine == NULL || memory->object == NULL ||
	    memory->machine != loader->machine || memory->machine->spec.loader.path[0] == '\0')
		return (EINVAL);
	bzero(&process, sizeof(process));
	error = vmmfs_loader_process_init(&process,
	    loader->machine->spec.loader.path, cred);
	if (error != 0) {
		ksnprintf(event, sizeof(event),
		    "loader initialization failed error=%d", error);
		vmmfs_events_log(&loader->machine->events, event);
		return (error);
	}
	ksnprintf(event, sizeof(event), "loader started pid=%d", process.pid);
	vmmfs_events_log(&loader->machine->events, event);
	error = vmmfs_loader_process_install(&process, memory->object,
	    memory->machine->spec.memory.size);
	if (error == 0)
		error = vmmfs_loader_process_resume(&process);
	if (error == 0)
		error = vmmfs_loader_process_wait(&process);
	if (error == 0)
		ksnprintf(event, sizeof(event), "loader completed");
	else
		ksnprintf(event, sizeof(event), "loader failed error=%d", error);
	vmmfs_events_log(&loader->machine->events, event);
	vmmfs_loader_process_finish(&process);
	return (error);
}

static int
vmmfs_loader_handler_compare(struct vmmfs_loader_handler *left,
	struct vmmfs_loader_handler *right)
{
	if (left->pid < right->pid)
		return (-1);
	if (left->pid > right->pid)
		return (1);
	return (0);
}

static int
vmmfs_loader_handler_register(struct vmmfs_loader_handler *handler)
{
	struct vmmfs_loader_handler *old;

	if (!vmmfs_loader_domain.initialized || handler == NULL ||
	    handler->pid <= 0 || handler->exit_callback == NULL)
		return (EINVAL);
	spin_lock(&vmmfs_loader_domain.spin);
	old = RB_INSERT(vmmfs_loader_handler_tree,
	    &vmmfs_loader_domain.handlers, handler);
	spin_unlock(&vmmfs_loader_domain.spin);
	return (old == NULL ? 0 : EEXIST);
}

static void
vmmfs_loader_handler_unregister(struct vmmfs_loader_handler *handler)
{
	struct vmmfs_loader_handler key;
	struct vmmfs_loader_handler *found;

	if (!vmmfs_loader_domain.initialized || handler == NULL ||
	    handler->pid <= 0)
		return;
	key.pid = handler->pid;
	spin_lock(&vmmfs_loader_domain.spin);
	found = RB_FIND(vmmfs_loader_handler_tree,
	    &vmmfs_loader_domain.handlers, &key);
	if (found == handler)
		RB_REMOVE(vmmfs_loader_handler_tree,
		    &vmmfs_loader_domain.handlers, handler);
	spin_unlock(&vmmfs_loader_domain.spin);
}

static void
vmmfs_loader_process_exit(struct thread *thread)
{
	struct vmmfs_loader_handler key;
	struct vmmfs_loader_handler *handler;
	void *wait_channel;
	struct proc *process;

	if (thread == NULL || thread->td_proc == NULL)
		return;
	process = thread->td_proc;
	key.pid = process->p_pid;
	wait_channel = NULL;
	spin_lock(&vmmfs_loader_domain.spin);
	handler = RB_FIND(vmmfs_loader_handler_tree,
	    &vmmfs_loader_domain.handlers, &key);
	if (handler != NULL) {
		handler->exit_callback(handler->argument, process->p_xstat);
		wait_channel = handler->wait_channel;
	}
	spin_unlock(&vmmfs_loader_domain.spin);
	if (wait_channel != NULL)
		wakeup(wait_channel);
}

static int
vmmfs_loader_process_init(struct vmmfs_loader_process *process,
	const char *path, struct ucred *cred)
{
	struct proc *child;
	struct lwp *child_lwp;
	int error;
	int state;

	if (process == NULL || path == NULL || path[0] == '\0' || cred == NULL)
		return (EINVAL);
	process->path = path;
	process->state = VMMFS_LOADER_INITING;
	error = fork1(curthread->td_lwp,
	    RFFDG | RFPROC | RFPGLOCK | RFNOWAIT, &child);
	if (error != 0)
		return (error);
	process->pid = child->p_pid;
	process->handler.pid = process->pid;
	process->handler.exit_callback = vmmfs_loader_exit_callback;
	process->handler.argument = process;
	process->handler.wait_channel = &process->handler;
	error = vmmfs_loader_handler_register(&process->handler);
	if (error != 0) {
		ksignal(child, SIGKILL);
		wakeup(process);
		process->pid = 0;
		return (error);
	}
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
		if (state == VMMFS_LOADER_OK || state == VMMFS_LOADER_FAILED)
			break;
		tsleep_interlock(&process->handler, PCATCH);
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_PAUSED || state == VMMFS_LOADER_OK ||
		    state == VMMFS_LOADER_FAILED)
			continue;
		error = tsleep(&process->handler, PINTERLOCKED | PCATCH,
		    "vmmfsldi", VMMFS_LOADER_WAIT_TICKS);
		if (error == 0)
			continue;
		vmmfs_loader_process_kill(process);
		break;
	}
	vmmfs_loader_process_finish(process);
	return (error == 0 ? ENOEXEC : error);
}

static int
vmmfs_loader_process_install(struct vmmfs_loader_process *process,
	struct vm_object *object, uint64_t size)
{
	if (process == NULL || object == NULL || size == 0 ||
	    atomic_fetchadd_int(&process->state, 0) != VMMFS_LOADER_PAUSED)
		return (EINVAL);
	return (vmmfs_loader_open_memory_fd(object, (vm_size_t)size,
	    &process->memory_file));
}

static int
vmmfs_loader_process_resume(struct vmmfs_loader_process *process)
{
	if (process == NULL || process->memory_file == NULL ||
	    !atomic_cmpset_int(&process->state, VMMFS_LOADER_PAUSED,
	    VMMFS_LOADER_RUNNING))
		return (ECANCELED);
	wakeup(process);
	return (0);
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
		if (state != VMMFS_LOADER_RUNNING)
			return (ENOEXEC);
		tsleep_interlock(&process->handler, PCATCH);
		state = atomic_fetchadd_int(&process->state, 0);
		if (state != VMMFS_LOADER_RUNNING)
			continue;
		error = tsleep(&process->handler, PINTERLOCKED | PCATCH,
		    "vmmfsld", VMMFS_LOADER_WAIT_TICKS);
		if (error == 0)
			continue;
		vmmfs_loader_process_kill(process);
		return (error == EWOULDBLOCK ? ENOEXEC : error);
	}
}

static void
vmmfs_loader_process_finish(struct vmmfs_loader_process *process)
{
	int state;

	if (process == NULL)
		return;
	state = atomic_fetchadd_int(&process->state, 0);
	if (state != VMMFS_LOADER_OK && state != VMMFS_LOADER_FAILED)
		vmmfs_loader_process_kill(process);
	for (;;) {
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_OK || state == VMMFS_LOADER_FAILED)
			break;
		tsleep_interlock(&process->handler, 0);
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_OK || state == VMMFS_LOADER_FAILED)
			continue;
		(void)tsleep(&process->handler, PINTERLOCKED, "vmmfsldx",
		    VMMFS_LOADER_WAIT_TICKS);
		vmmfs_loader_process_kill(process);
	}
	vmmfs_loader_handler_unregister(&process->handler);
	vmmfs_loader_revoke(process);
	vmmfs_loader_close_files(process);
	process->pid = 0;
}

static void
vmmfs_loader_process_kill(struct vmmfs_loader_process *process)
{
	struct proc *target;

	if (process == NULL)
		return;
	if (process->pid > 0) {
		target = pfind(process->pid);
		if (target != NULL) {
			ksignal(target, SIGKILL);
			PRELE(target);
		}
	}
	wakeup(process);
	vmmfs_loader_revoke(process);
}

static void
vmmfs_loader_exit_callback(void *argument, int exit_code)
{
	struct vmmfs_loader_process *process;
	int state;
	int old;

	process = argument;
	if (process == NULL)
		return;
	state = WIFEXITED(exit_code) && WEXITSTATUS(exit_code) == 0 ?
	    VMMFS_LOADER_OK : VMMFS_LOADER_FAILED;
	for (;;) {
		old = atomic_fetchadd_int(&process->state, 0);
		if (old == VMMFS_LOADER_OK || old == VMMFS_LOADER_FAILED)
			return;
		if (atomic_cmpset_int(&process->state, old, state))
			return;
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
		vmmfs_loader_child_exit(W_EXITCODE(127, SIGKILL));
	if (!atomic_cmpset_int(&process->state, VMMFS_LOADER_INITING,
	    VMMFS_LOADER_PAUSED))
		vmmfs_loader_child_exit(W_EXITCODE(127, SIGKILL));
	wakeup(&process->handler);
	for (;;) {
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_RUNNING)
			break;
		if (state == VMMFS_LOADER_OK || state == VMMFS_LOADER_FAILED)
			vmmfs_loader_child_exit(W_EXITCODE(127, SIGKILL));
		tsleep_interlock(process, PCATCH);
		state = atomic_fetchadd_int(&process->state, 0);
		if (state == VMMFS_LOADER_RUNNING || state == VMMFS_LOADER_OK ||
		    state == VMMFS_LOADER_FAILED)
			continue;
		error = tsleep(process, PINTERLOCKED | PCATCH, "vmmfsldp", 0);
		if (error != 0)
			vmmfs_loader_child_exit(W_EXITCODE(127, SIGKILL));
	}
	error = vmmfs_loader_install_fd(process->memory_file, 3);
	if (error == 0)
		error = vmmfs_loader_exec_path(process->path);
	if (error == 0) {
		lwp = curthread->td_lwp;
		lwp->lwp_proc->p_usched->acquire_curproc(lwp);
		return;
	}
	if (error < 0)
		vmmfs_loader_child_exit(W_EXITCODE(127, SIGABRT));
	vmmfs_loader_child_exit(W_EXITCODE(127, 0));
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
vmmfs_loader_exec_path(const char *path)
{
	struct sysmsg message;
	struct execve_args arguments;
	char *argument;
	char *user_pointer;
	char **user_argv;
	size_t index;
	size_t length;

	if (path == NULL)
		return (EINVAL);
	length = strlen(path);
	if (length == 0 || length >= PATH_MAX)
		return (EINVAL);
	user_pointer = (char *)USRSTACK;
	if (subyte(--user_pointer, 0) != 0)
		return (EFAULT);
	for (index = length; index > 0; --index) {
		if (subyte(--user_pointer, path[index - 1]) != 0)
			return (EFAULT);
	}
	argument = user_pointer;
	user_argv = (char **)rounddown2((intptr_t)user_pointer,
	    sizeof(intptr_t));
	if (suword64((uint64_t *)(caddr_t)--user_argv, 0) != 0)
		return (EFAULT);
	if (suword64((uint64_t *)(caddr_t)--user_argv,
	    (uint64_t)(intptr_t)argument) != 0)
		return (EFAULT);
	bzero(&message, sizeof(message));
	arguments.fname = argument;
	arguments.argv = user_argv;
	arguments.envv = NULL;
	return (sys_execve(&message, &arguments));
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
vmmfs_loader_fd_open(struct dev_open_args *arguments)
{
	(void)arguments;
	return (0);
}

static int
vmmfs_loader_fd_close(struct dev_close_args *arguments)
{
	(void)arguments;
	return (0);
}

static int
vmmfs_loader_fd_getattr(struct vop_getattr_args *arguments)
{
	struct vmmfs_loader_fd *fd;
	struct vattr *attributes;

	fd = arguments->a_vp->v_rdev->si_drv1;
	if (fd == NULL)
		return (ENOENT);
	attributes = arguments->a_vap;
	VATTR_NULL(attributes);
	attributes->va_type = VCHR;
	attributes->va_mode = 0600;
	attributes->va_nlink = 1;
	attributes->va_uid = UID_ROOT;
	attributes->va_gid = GID_WHEEL;
	attributes->va_size = fd->size;
	attributes->va_blocksize = PAGE_SIZE;
	attributes->va_bytes = fd->size;
	attributes->va_fileid = 0;
	attributes->va_atime.tv_sec = 0;
	attributes->va_atime.tv_nsec = 0;
	attributes->va_mtime = attributes->va_atime;
	attributes->va_ctime = attributes->va_atime;
	attributes->va_flags = 0;
	attributes->va_gen = 1;
	attributes->va_filerev = 0;
	return (0);
}

static int
vmmfs_loader_file_readwrite(struct file *file, struct uio *uio,
	struct ucred *cred, int flags)
{
	(void)file;
	(void)uio;
	(void)cred;
	(void)flags;
	return (EOPNOTSUPP);
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
	struct vmmfs_loader_fd *fd;
	int error;

	(void)cred;
	error = devfs_get_cdevpriv(file, (void **)&fd);
	if (error != 0)
		return (error);
	bzero(status, sizeof(*status));
	status->st_nlink = 1;
	status->st_mode = S_IFCHR | 0600;
	status->st_uid = UID_ROOT;
	status->st_gid = GID_WHEEL;
	status->st_size = (off_t)fd->size;
	status->st_blocks = howmany(fd->size, S_BLKSIZE);
	status->st_blksize = PAGE_SIZE;
	status->__old_st_blksize = status->st_blksize;
	return (0);
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
vmmfs_loader_file_close(struct file *file)
{
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
vmmfs_loader_fd_ref(struct vmmfs_loader_fd *fd)
{
	atomic_add_int(&fd->references, 1);
}

static void
vmmfs_loader_fd_put(struct vmmfs_loader_fd *fd)
{
	if (atomic_fetchadd_int(&fd->references, -1) == 1) {
		atomic_add_int(&vmmfs_loader_mmap_object_count, -1);
		kfree(fd, M_VMMFS);
	}
}

static int
vmmfs_loader_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
	vm_ooffset_t offset, struct ucred *cred, u_short *color)
{
	struct vmmfs_loader_fd *fd;

	(void)cred;
	fd = handle;
	if (fd == NULL || color == NULL)
		return (EINVAL);
	if ((prot & VM_PROT_EXECUTE) != 0 || offset < 0 || offset > fd->size ||
	    size > fd->size - offset || fd->revoked ||
	    fd->backing_object == NULL)
		return (EINVAL);
	*color = 0;
	vmmfs_loader_fd_ref(fd);
	return (0);
}

static void
vmmfs_loader_pager_dtor(void *handle)
{
	struct vmmfs_loader_fd *fd;
	struct vm_object *backing_object;

	fd = handle;
	if (fd == NULL)
		return;
	backing_object = fd->backing_object;
	fd->backing_object = NULL;
	if (backing_object != NULL)
		vm_object_deallocate(backing_object);
	vmmfs_loader_fd_put(fd);
}

static int
vmmfs_loader_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
	vm_page_t *page_result)
{
	struct vmmfs_loader_fd *fd;
	struct vm_object *backing_object;
	vm_page_t page;

	fd = object->handle;
	if (fd == NULL || page_result == NULL || offset < 0 ||
	    offset >= fd->size || (prot & VM_PROT_EXECUTE) != 0)
		return (VM_PAGER_ERROR);
	VM_OBJECT_LOCK(object);
	if (fd->revoked || fd->backing_object == NULL) {
		VM_OBJECT_UNLOCK(object);
		return (VM_PAGER_ERROR);
	}
	backing_object = fd->backing_object;
	vm_object_reference_quick(backing_object);
	VM_OBJECT_UNLOCK(object);
	page = vm_page_grab(backing_object, OFF_TO_IDX(offset),
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	vm_object_deallocate(backing_object);
	if (page == NULL)
		return (VM_PAGER_ERROR);
	if (page->valid != VM_PAGE_BITS_ALL)
		vm_page_zero_invalid(page, TRUE);
	*page_result = page;
	return (VM_PAGER_OK);
}

static void
vmmfs_loader_fd_revoke(struct vmmfs_loader_fd *fd)
{
	struct vm_object *backing_object;
	struct vm_object *object;
	int remove_pages;

	backing_object = NULL;
	remove_pages = 0;
	object = fd->object;
	if (object != NULL) {
		VM_OBJECT_LOCK(object);
		if (!fd->revoked) {
			fd->revoked = 1;
			remove_pages = 1;
		}
		VM_OBJECT_UNLOCK(object);
		if (remove_pages)
			vm_object_page_remove(object, 0, 0, FALSE);
	} else if (!fd->revoked) {
		fd->revoked = 1;
		backing_object = fd->backing_object;
		fd->backing_object = NULL;
	}
	if (backing_object != NULL)
		vm_object_deallocate(backing_object);
}

static void
vmmfs_loader_fd_free(void *argument)
{
	struct vmmfs_loader_fd *fd;
	struct vnode *vnode;

	fd = argument;
	if (fd == NULL)
		return;
	vmmfs_loader_fd_revoke(fd);
	vnode = fd->vnode;
	if (vnode != NULL) {
		fd->vnode = NULL;
		vx_get(vnode);
		vgone_vxlocked(vnode);
		vx_put(vnode);
		vrele(vnode);
	}
	if (fd->dev != NULL) {
		fd->dev->si_drv1 = NULL;
		destroy_only_dev(fd->dev);
		fd->dev = NULL;
	}
	if (fd->object != NULL) {
		vm_object_deallocate(fd->object);
		fd->object = NULL;
	}
	vmmfs_loader_fd_put(fd);
}

static int
vmmfs_loader_fd_mmap_single(struct dev_mmap_single_args *arguments)
{
	struct vmmfs_loader_fd *fd;
	struct vm_object *object;
	vm_ooffset_t offset;

	if (arguments == NULL || arguments->a_head.a_dev == NULL)
		return (EINVAL);
	fd = arguments->a_head.a_dev->si_drv1;
	if (fd == NULL || fd->object == NULL)
		return (EINVAL);
	object = fd->object;
	if ((arguments->a_nprot & VM_PROT_EXECUTE) != 0)
		return (EACCES);
	offset = *arguments->a_offset;
	if (offset < 0 || offset > fd->size ||
	    arguments->a_size > fd->size - offset)
		return (EINVAL);
	VM_OBJECT_LOCK(object);
	if (fd->revoked || fd->backing_object == NULL) {
		VM_OBJECT_UNLOCK(object);
		return (EINVAL);
	}
	vm_object_reference_locked(object);
	VM_OBJECT_UNLOCK(object);
	*arguments->a_object = object;
	return (0);
}

static int
vmmfs_loader_make_vnode(cdev_t dev, struct vnode **vnode_pointer)
{
	struct vnode *vnode;
	int error;

	error = getspecialvnode(VT_NON, NULL, &vmmfs_loader_fd_vops_pointer,
	    &vnode, 0, 0);
	if (error != 0) {
		*vnode_pointer = NULL;
		return (error);
	}
	vnode->v_type = VCHR;
	error = v_associate_rdev(vnode, dev);
	if (error != 0) {
		vgone_vxlocked(vnode);
		vx_put(vnode);
		*vnode_pointer = NULL;
		return (error);
	}
	vnode->v_umajor = dev->si_umajor;
	vnode->v_uminor = dev->si_uminor;
	vx_unlock(vnode);
	*vnode_pointer = vnode;
	return (0);
}

static int
vmmfs_loader_open_fd(struct vmmfs_loader_fd *fd, struct file **file_pointer)
{
	struct file *file;
	struct vnode *vnode;
	int error;

	error = vmmfs_loader_make_vnode(fd->dev, &vnode);
	if (error != 0)
		goto fail;
	fd->vnode = vnode;
	error = falloc(NULL, &file, NULL);
	if (error != 0)
		goto fail;
	fsetcred(file, proc0.p_ucred);
	file->f_type = DTYPE_VNODE;
	file->f_flag = FREAD | FWRITE;
	file->f_ops = &vmmfs_loader_fileops;
	file->f_data = vnode;
	vref(vnode);
	atomic_add_int(&vnode->v_opencount, 1);
	atomic_add_int(&vnode->v_writecount, 1);
	error = devfs_set_cdevpriv(file, fd, vmmfs_loader_fd_free);
	if (error != 0) {
		fp_close(file);
		vmmfs_loader_fd_free(fd);
		return (error);
	}
	*file_pointer = file;
	return (0);

fail:
	vmmfs_loader_fd_free(fd);
	return (error);
}

static int
vmmfs_loader_open_memory_fd(struct vm_object *object, vm_size_t size,
	struct file **file_pointer)
{
	struct vmmfs_loader_fd *fd;
	uint32_t serial;

	if (object == NULL || size == 0)
		return (EINVAL);
	fd = kmalloc(sizeof(*fd), M_VMMFS, M_WAITOK | M_ZERO);
	vm_object_reference_quick(object);
	fd->backing_object = object;
	fd->size = round_page(size);
	fd->references = 1;
	atomic_add_int(&vmmfs_loader_mmap_object_count, 1);
	fd->object = cdev_pager_allocate(fd, OBJT_MGTDEVICE,
	    &vmmfs_loader_pager_ops, fd->size,
	    VM_PROT_READ | VM_PROT_WRITE, 0, proc0.p_ucred);
	if (fd->object == NULL) {
		vmmfs_loader_fd_free(fd);
		return (ENOMEM);
	}
	serial = atomic_fetchadd_int(&vmmfs_loader_fd_serial, 1);
	fd->dev = make_only_dev(&vmmfs_loader_fd_ops, serial, UID_ROOT,
	    GID_WHEEL, 0600, "vmmfsld%d", serial);
	if (fd->dev == NULL) {
		vmmfs_loader_fd_free(fd);
		return (ENXIO);
	}
	fd->dev->si_drv1 = fd;
	return (vmmfs_loader_open_fd(fd, file_pointer));
}

static void
vmmfs_loader_revoke_file(struct file *file)
{
	struct vmmfs_loader_fd *fd;

	if (file == NULL || file->f_type != DTYPE_VNODE || file->f_data == NULL)
		return;
	if (devfs_get_cdevpriv(file, (void **)&fd) == 0)
		vmmfs_loader_fd_revoke(fd);
	(void)fdrevoke(file->f_data, DTYPE_VNODE, proc0.p_ucred);
}

static void
vmmfs_loader_revoke(struct vmmfs_loader_process *process)
{
	vmmfs_loader_revoke_file(process->memory_file);
}

static void
vmmfs_loader_close_files(struct vmmfs_loader_process *process)
{
	if (process->memory_file != NULL) {
		fp_close(process->memory_file);
		process->memory_file = NULL;
	}
}

static int
vmmfs_loader_load(struct vmmfs_loader *loader, char *buffer, size_t capacity, size_t *length)
{
	int result;
	result = ksnprintf(buffer, capacity, "%s\n",
	    loader->machine->spec.loader.path);
	if (result < 0 || (size_t)result >= capacity)
		return (EOVERFLOW);
	*length = (size_t)result;
	return (0);
}

static int
vmmfs_loader_store(struct vmmfs_loader *loader, const char *buffer, size_t length)
{
	if (length == 0)
		return (EINVAL);
	if (buffer[length - 1] == 10)
		--length;
	if (length == 0 || length >= MAXPATHLEN)
		return (ENAMETOOLONG);
	lwkt_gettoken(&loader->machine->token);
	if (!loader->machine->stopped.expect_stopped ||
	    loader->machine->machine != NULL) {
		lwkt_reltoken(&loader->machine->token);
		return (EBUSY);
	}
	bcopy(buffer, loader->machine->spec.loader.path, length);
	loader->machine->spec.loader.path[length] = 0;
	lwkt_reltoken(&loader->machine->token);
	return (0);
}

int
vmmfs_loader_create(struct vmmfs_machine *machine, struct vmmfs_loader *loader)
{
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	bzero(loader, sizeof(*loader));
	loader->machine = machine;
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
    loader->inode = atomic_fetchadd_int(&state->next_inode, 1);
	if (state->loader_vops == NULL)
		return (ENXIO);

	error = getnewvnode(VT_SYNTH, machine->root->mount, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = loader;
	vnode->v_ops = &state->loader_vops;
	vnode->v_type = VREG;
	loader->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_loader_destroy(struct vmmfs_loader *loader)
{
	struct vnode *vnode;

	if (loader == NULL)
		return (EINVAL);

	vnode = loader->vnode;
	if (vnode != NULL) {
		vx_get(vnode);
		vgone_vxlocked(vnode);
		vx_put(vnode);
		vrele(vnode);
	}
	KKASSERT(loader->vnode == NULL);
	loader->machine = NULL;
	return (0);
}

static int
vmmfs_loader_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_LOADER_MODE, 0));
}

static int
vmmfs_loader_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_loader *loader;
	struct vattr *vattr;
	char buffer[32];
	size_t length;
	int error;

	loader = ap->a_vp->v_data;
	if (loader == NULL)
		return (ENOENT);
	error = vmmfs_loader_load(loader, buffer, sizeof(buffer), &length);
	if (error != 0)
		return (error);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_LOADER_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = loader->inode;
	vattr->va_size = length;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = length;
	return (0);
}

static int
vmmfs_loader_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_LOADER_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_loader_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_loader_read(struct vop_read_args *ap)
{
	struct vmmfs_loader *loader;
	struct uio *uio;
	char buffer[32];
	size_t length;
	off_t offset;
	int error;

	loader = ap->a_vp->v_data;
	if (loader == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	error = vmmfs_loader_load(loader, buffer, sizeof(buffer), &length);
	if (error != 0)
		return (error);
	offset = uio->uio_offset;
	if ((size_t)offset >= length)
		return (0);
	return (uiomove(buffer + offset, length - (size_t)offset, uio));
}

static int
vmmfs_loader_setattr(struct vop_setattr_args *ap)
{
	/* Accept the O_TRUNC size update performed before a control write. */
	(void)ap;
	return (0);
}

static int
vmmfs_loader_write(struct vop_write_args *ap)
{
	struct vmmfs_loader *loader;
	struct uio *uio;
	char buffer[32];
	size_t length;
	int error;

	loader = ap->a_vp->v_data;
	if (loader == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset != 0 || uio->uio_resid == 0 ||
	    (size_t)uio->uio_resid >= sizeof(buffer))
		return (EINVAL);
	length = (size_t)uio->uio_resid;
	error = uiomove(buffer, length, uio);
	if (error != 0)
		return (error);
	return (vmmfs_loader_store(loader, buffer, length));
}

static int
vmmfs_loader_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_loader *loader;

	loader = ap->a_vp->v_data;
	if (loader != NULL && loader->vnode == ap->a_vp)
		loader->vnode = NULL;
	ap->a_vp->v_data = NULL;
	return (0);
}
