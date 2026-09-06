/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Loader script node and one-way delivery of the common launch fd 3.
 */
#include <sys/conf.h>
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
#include <sys/systm.h>
#include <sys/sysmsg.h>
#include <sys/sysproto.h>
#include <sys/uio.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <machine/pmap.h>
#include <machine/vmparam.h>
#include "vmmfs.h"
#include "vmmfs_launch.h"
#include "vmmfs_loader.h"
#include "vmmfs_machine.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"

#define VMMFS_LOADER_MODE 0644
#define VMMFS_LOADER_SHELL "/bin/sh"

struct vmmfs_loader_process {
	char script[PAGE_SIZE];
	struct file *file;
	struct vmmfs_launch *launch;
};

static void vmmfs_loader_child(void *, struct trapframe *);
static int vmmfs_loader_exec_shell(const char *);
static int vmmfs_loader_install_fd(struct file *, int);
static void vmmfs_loader_set_process_cred(struct proc *, struct ucred *);
static void vmmfs_loader_drop(struct vmmfs_node *);

static int
vmmfs_loader_deactivate(struct vmmfs_node *node)
{
	(void)node;
	return (0);
}

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

int
vmmfs_loader_run(struct vmmfs_loader *loader, struct vnode *vnode,
	struct ucred *cred)
{
	struct vmmfs_loader_process *process;
	struct proc *child;
	struct lwp *lwp;
	int error;

	process = kmalloc(sizeof(*process), M_VMMFS, M_WAITOK | M_ZERO);
	lwkt_gettoken(&loader->node.token);
	if (loader->node.dead || loader->script[0] == '\0') {
		lwkt_reltoken(&loader->node.token);
		kfree(process, M_VMMFS);
		return (EINVAL);
	}
	bcopy(loader->script, process->script, sizeof(process->script));
	lwkt_reltoken(&loader->node.token);
	error = vmmfs_launch_open(vnode, cred, &process->file);
	if (error != 0) {
		kfree(process, M_VMMFS);
		return (error);
	}
	process->launch = vnode->v_data;
	vmmfs_node_hold(&process->launch->node);
	error = fork1(curthread->td_lwp,
	    RFFDG | RFPROC | RFPGLOCK | RFNOWAIT, &child);
	if (error != 0) {
		fp_close(process->file);
		vmmfs_node_put(&process->launch->node);
		kfree(process, M_VMMFS);
		return (error);
	}
	lwp = ONLY_LWP_IN_PROC(child);
	vmmfs_loader_set_process_cred(child, cred);
	cpu_set_fork_handler(lwp, vmmfs_loader_child, process);
	start_forked_proc(curthread->td_lwp, child);
	/* The child owns process/file now.  Only the launch reports boot success. */
	return (0);
}

static void
vmmfs_loader_child(void *argument, struct trapframe *frame)
{
	struct vmmfs_loader_process *process = argument;
	struct vmmfs_launch *launch = process->launch;
	struct lwp *lwp;
	int error;

	(void)frame;
	/* Retain caller stdio, but never lend unrelated capabilities to the script. */
	error = kern_closefrom(3);
	if (error == 0)
		error = vmmfs_loader_install_fd(process->file, 3);
	/* install_fd acquired a descriptor reference on success. */
	fp_close(process->file);
	if (error == 0)
		error = vmmfs_loader_exec_shell(process->script);
	if (error != 0) {
		int abort_error = vmmfs_machine_abort(launch);
		if (abort_error != 0)
			kprintf("vmmfs: loader abort: %d\n", abort_error);
	}
	kfree(process, M_VMMFS);
	lwp = curthread->td_lwp;
	/* Scheduling may sleep; retain the launch until it has completed. */
	lwp->lwp_proc->p_usched->acquire_curproc(lwp);
	vmmfs_node_put(&launch->node);
	if (error != 0)
		exit1(1);
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
vmmfs_loader_load(struct vmmfs_node *node, char *buffer,
	size_t capacity, size_t *length)
{
	struct vmmfs_loader *loader = (struct vmmfs_loader *)node;
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
vmmfs_loader_store(struct vmmfs_node *node, const char *buffer,
	size_t length)
{
	struct vmmfs_loader *loader = (struct vmmfs_loader *)node;

	if (length == 0)
		return (EINVAL);
	if (buffer[length - 1] == '\n')
		--length;
	if (length == 0 || length >= sizeof(loader->script))
		return (ENAMETOOLONG);
	if (loader->node.dead)
		return (ENOENT);
	lwkt_gettoken(&vmmfs_loader_machine(loader)->node.token);
	if (vmmfs_loader_machine(loader)->machine != NULL) {
		lwkt_reltoken(&vmmfs_loader_machine(loader)->node.token);
		return (EBUSY);
	}
	bcopy(buffer, loader->script, length);
	loader->script[length] = 0;
	loader->node.size = (off_t)length + 1;
	lwkt_reltoken(&vmmfs_loader_machine(loader)->node.token);
	return (0);
}

int
vmmfs_loader_init(struct vmmfs_mount *mount, struct vmmfs_node *parent,
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
	loader->node.references = 1;
	lwkt_token_init(&loader->node.token, "vmmfsnode");
	loader->node.deactivate = vmmfs_loader_deactivate;
	loader->node.drop = vmmfs_loader_drop;
	if (parent != NULL)
		vmmfs_node_hold(parent);
	loader->node.load_limit = PAGE_SIZE + 1;
	loader->node.store_limit = PAGE_SIZE - 1;
	loader->node.load = vmmfs_loader_load;
	loader->node.store = vmmfs_loader_store;
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
	vmmfs_node_put(&loader->node);
	return (error);
}

static void
vmmfs_loader_drop(struct vmmfs_node *node)
{
	struct vmmfs_loader *loader;

	loader = (struct vmmfs_loader *)node;
	KKASSERT(loader != NULL);
	loader->node.inode = 0;

}
