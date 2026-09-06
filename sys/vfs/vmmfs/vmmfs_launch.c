/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private launch vnode: guest-memory mmap followed by one cpustate write.
 */
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/thread2.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <machine/atomic.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_page2.h>
#include <vm/vm_pager.h>
#include "vmmfs.h"
#include "vmmfs_launch.h"
#include "vmmfs_machine.h"
#include "vmmfs_root.h"

static u_int vmmfs_launch_serial;

static int vmmfs_launch_deactivate(struct vmmfs_node *);
static void vmmfs_launch_drop(struct vmmfs_node *);
static int vmmfs_launch_mmap(struct dev_mmap_single_args *);
static int vmmfs_launch_pager_ctor(void *, vm_ooffset_t, vm_prot_t,
	vm_ooffset_t, struct ucred *, u_short *);
static void vmmfs_launch_pager_dtor(void *);
static int vmmfs_launch_pager_fault(vm_object_t, vm_ooffset_t, int, vm_page_t *);
static int vmmfs_launch_write(struct file *, struct uio *, struct ucred *, int);
static int vmmfs_launch_close(struct file *);
static int vmmfs_launch_stat(struct file *, struct stat *, struct ucred *);

struct vop_ops vmmfs_launch_vops = {
	.vop_default = vop_defaultop,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_mmap = (void *)vop_null,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_pathconf = vop_stdpathconf,
};

static struct fileops vmmfs_launch_fileops = {
	.fo_read = badfo_readwrite,
	.fo_write = vmmfs_launch_write,
	.fo_ioctl = badfo_ioctl,
	.fo_kqfilter = badfo_kqfilter,
	.fo_stat = vmmfs_launch_stat,
	.fo_close = vmmfs_launch_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = badfo_seek,
};

static struct dev_ops vmmfs_launch_dev_ops = {
	{ "vmmfs_launch", 0, D_MPSAFE },
	.d_mmap_single = vmmfs_launch_mmap,
};

static struct cdev_pager_ops vmmfs_launch_pager_ops = {
	.cdev_pg_fault = vmmfs_launch_pager_fault,
	.cdev_pg_ctor = vmmfs_launch_pager_ctor,
	.cdev_pg_dtor = vmmfs_launch_pager_dtor,
};



int
vmmfs_launch_create(struct vmmfs_node *parent,
	uint64_t size, struct vnode **vnodep)
{
	struct vmmfs_launch *launch;
	u_int serial;
	int error;

	if (size == 0 || (off_t)size <= 0)
		return (EINVAL);
	*vnodep = NULL;
	launch = kmalloc(sizeof(*launch), M_VMMFS, M_WAITOK | M_ZERO);
	launch->node.parent = parent;
	launch->node.mount = parent->mount;
	launch->node.references = 1;
	lwkt_token_init(&launch->node.token, "vmmfslaunch");
	launch->node.deactivate = vmmfs_launch_deactivate;
	launch->node.drop = vmmfs_launch_drop;
	launch->node.mode = 0600;
	launch->node.size = size;
	launch->node.inode = vmmfs_root_allocate_inode(parent->mount->root_vnode->v_data);
	launch->result = EINPROGRESS;
	vmmfs_node_hold(parent);
	serial = atomic_fetchadd_int(&vmmfs_launch_serial, 1);
	launch->dev = make_only_dev(&vmmfs_launch_dev_ops, serial,
	    UID_ROOT, GID_WHEEL, 0600, "vmmfs_launch%u", serial);
	if (launch->dev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	launch->dev->si_drv1 = launch;
	error = vmmfs_vnode_create_cdev(parent->mount->mount,
	    &parent->mount->launch_vops, launch->dev, &launch->node, vnodep);
	if (error == 0)
		return (0);
fail:
	vmmfs_node_put(&launch->node);
	return (error);
}

static void
vmmfs_launch_drop(struct vmmfs_node *node)
{
	struct vmmfs_launch *launch = (struct vmmfs_launch *)node;

	KKASSERT(launch->pager_object == NULL);
	if (launch->backing_object != NULL)
		vm_object_deallocate(launch->backing_object);
	if (launch->dev != NULL) {
		launch->dev->si_drv1 = NULL;
		destroy_only_dev(launch->dev);
	}
	kfree(launch, M_VMMFS);
}

int
vmmfs_launch_map(struct vmmfs_launch *launch, struct vm_object *object)
{
	struct vm_object *pager;
	int error;

	vm_object_reference_quick(object);
	lwkt_gettoken(&launch->node.token);
	if (launch->node.dead || launch->backing_object != NULL) {
		lwkt_reltoken(&launch->node.token);
		vm_object_deallocate(object);
		return (ECANCELED);
	}
	launch->backing_object = object;
	pager = cdev_pager_allocate(launch, OBJT_MGTDEVICE,
	    &vmmfs_launch_pager_ops, launch->node.size,
	    VM_PROT_READ | VM_PROT_WRITE, 0, proc0.p_ucred);
	error = pager == NULL ? ENOMEM : 0;
	if (launch->node.dead && pager != NULL) {
		vm_object_deallocate(pager);
		error = ECANCELED;
	} else {
		launch->pager_object = pager;
	}
	lwkt_reltoken(&launch->node.token);
	return (error);
}

int
vmmfs_launch_open(struct vnode *vnode, struct ucred *cred, struct file **filep)
{
	struct vmmfs_launch *launch = vnode->v_data;
	struct file *file;
	int error;

	error = falloc(NULL, &file, NULL);
	if (error != 0)
		return (error);
	lwkt_gettoken(&launch->node.token);
	if (launch->node.dead || launch->pager_object == NULL) {
		lwkt_reltoken(&launch->node.token);
		fp_close(file);
		return (ECANCELED);
	}
	fsetcred(file, cred);
	file->f_type = DTYPE_VNODE;
	file->f_flag = FREAD | FWRITE;
	file->f_ops = &vmmfs_launch_fileops;
	file->f_data = vnode;
	vref(vnode);
	atomic_add_int(&vnode->v_opencount, 1);
	atomic_add_int(&vnode->v_writecount, 1);
	lwkt_reltoken(&launch->node.token);
	*filep = file;
	return (0);
}

static int
vmmfs_launch_write(struct file *file, struct uio *uio,
	struct ucred *cred, int flags)
{
	struct vnode *vnode = file->f_data;
	struct vmmfs_launch *launch;
	struct vmm_cpustate state;
	int error, abort_error;

	(void)cred;
	(void)flags;
	if (vnode == NULL || vnode->v_data == NULL)
		return (EBADF);
	launch = vnode->v_data;
	vmmfs_node_hold(&launch->node);
	error = EINVAL;
	if ((uio->uio_offset == -1 || uio->uio_offset == 0) &&
	    uio->uio_resid == sizeof(state)) {
		uio->uio_offset = 0;
		error = uiomove((caddr_t)&state, sizeof(state), uio);
	}
	if (error != 0) {
		abort_error = vmmfs_machine_abort(launch);
		if (abort_error != 0)
			error = abort_error;
		goto done;
	}
	lwkt_gettoken(&launch->node.token);
	if (launch->node.dead || launch->pager_object == NULL) {
		error = EPIPE;
	} else {
		launch->cpustate = state;
		/* Clear mmap admission before sleeping or accepting another writer. */
		vmmfs_launch_revoke(launch);
		error = 0;
	}
	lwkt_reltoken(&launch->node.token);
	if (error == 0)
		error = vmmfs_machine_run(launch);
done:
	vmmfs_node_put(&launch->node);
	return (error);
}

static int
vmmfs_launch_close(struct file *file)
{
	struct vnode *vnode = file->f_data;
	struct vmmfs_launch *launch;
	int error = 0;

	if (vnode != NULL && vnode->v_data != NULL) {
		launch = vnode->v_data;
		error = vmmfs_machine_abort(launch);
	}
	file->f_data = NULL;
	file->f_ops = &badfileops;
	if (vnode != NULL) {
		int close_error = vn_close(vnode, file->f_flag, file);
		if (error == 0)
			error = close_error;
	}
	return (error);
}

static int
vmmfs_launch_stat(struct file *file, struct stat *status, struct ucred *cred)
{
	struct vnode *vnode = file->f_data;
	struct vmmfs_launch *launch;

	(void)cred;
	if (vnode == NULL || vnode->v_data == NULL)
		return (EBADF);
	launch = vnode->v_data;
	bzero(status, sizeof(*status));
	status->st_mode = S_IFCHR | 0600;
	status->st_nlink = 1;
	status->st_size = launch->node.size;
	status->st_ino = launch->node.inode;
	status->st_blocks = howmany(status->st_size, S_BLKSIZE);
	status->st_blksize = PAGE_SIZE;
	status->__old_st_blksize = PAGE_SIZE;
	return (0);
}

static int
vmmfs_launch_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_launch *launch = (struct vmmfs_launch *)node;
	int error;

	error = vmmfs_machine_abort(launch);
	if (error != 0)
		kprintf("vmmfs: launch close abort: %d\n", error);
	/* Abort may have waited; cleanup errors must not reopen this handle. */
	vmmfs_launch_revoke(launch);
	return (0);
}

void
vmmfs_launch_complete(struct vmmfs_launch *launch, int error)
{
	lwkt_gettoken(&launch->node.token);
	launch->result = error;
	lwkt_reltoken(&launch->node.token);
	wakeup(launch);
}

int
vmmfs_launch_wait(struct vmmfs_launch *launch)
{
	int error, abort_error, signal_error = 0;
	int flags = PCATCH;

	for (;;) {
		tsleep_interlock(launch, flags);
		lwkt_gettoken(&launch->node.token);
		error = launch->result;
		lwkt_reltoken(&launch->node.token);
		if (error != EINPROGRESS) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			return (error == ECANCELED && signal_error != 0 ?
			    signal_error : error);
		}
		error = tsleep(launch, PINTERLOCKED | flags, "vmmfslaunch", 0);
		if (error == 0)
			continue;
		signal_error = error;
		abort_error = vmmfs_machine_abort(launch);
		if (abort_error != 0)
			return (abort_error);
		/*
		 * Abort and run compete for the launch identity.  If run already
		 * owns it, await that result rather than reporting a false cancel.
		 */
		flags = 0;
	}
}

/*
 * Page busy extends beyond the pager callback through the outer pmap_enter.
 * Restart the scan after sleeping: the object token was temporarily released.
 */
static int
vmmfs_launch_drain_page(vm_page_t page, void *argument)
{
	bool *retry = argument;

	if (vm_page_busy_try(page, TRUE)) {
		vm_page_sleep_busy(page, TRUE, "vmmlpage");
		*retry = true;
	} else {
		vm_page_wakeup(page);
	}
	return (0);
}

void
vmmfs_launch_revoke(struct vmmfs_launch *launch)
{
	struct vm_object *object;
	bool retry;

	lwkt_gettoken(&launch->node.token);
	object = launch->pager_object;
	launch->pager_object = NULL;
	lwkt_reltoken(&launch->node.token);
	if (object == NULL)
		return;
	/*
	 * Drain old faults after closing admission.  The MGTDEVICE backing
	 * list, not the RAM object's page list, owns the loader mappings.
	 * Removing those mappings preserves RAM contents for VMM.
	 */
	VM_OBJECT_LOCK(object);
	vm_object_pip_wait(object, "vmmlfault");
	VM_OBJECT_UNLOCK(object);
	/*
	 * Accepted faults return RAM pages outside the pager's rb_memq.
	 * Drain those pages before removing mappings; PIP alone ends before
	 * pmap_enter.  Do not invalidate the RAM contents needed by VMM.
	 */
	VM_OBJECT_LOCK(launch->backing_object);
	do {
		retry = false;
		vm_page_rb_tree_RB_SCAN(&launch->backing_object->rb_memq,
		    NULL, vmmfs_launch_drain_page, &retry);
	} while (retry);
	VM_OBJECT_UNLOCK(launch->backing_object);
	vm_object_page_remove(object, 0, 0, FALSE);
	vm_object_deallocate(object);
}

static int
vmmfs_launch_mmap(struct dev_mmap_single_args *ap)
{
	struct vmmfs_launch *launch = ap->a_head.a_dev->si_drv1;
	struct vm_object *object;
	vm_ooffset_t offset = *ap->a_offset;

	if (launch == NULL || (ap->a_nprot & VM_PROT_EXECUTE) != 0)
		return (EINVAL);
	lwkt_gettoken(&launch->node.token);
	object = launch->pager_object;
	if (launch->node.dead || object == NULL)
		goto closed;
	if (offset < 0 || offset >= launch->node.size ||
	    ap->a_size > launch->node.size - offset) {
		lwkt_reltoken(&launch->node.token);
		return (EINVAL);
	}
	vm_object_reference_quick(object);
	*ap->a_object = object;
	lwkt_reltoken(&launch->node.token);
	return (0);
closed:
	lwkt_reltoken(&launch->node.token);
	return (EBADF);
}

static int
vmmfs_launch_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
	vm_ooffset_t offset, struct ucred *cred, u_short *color)
{
	struct vmmfs_launch *launch = handle;

	(void)cred;
	if (offset != 0 || size != launch->node.size ||
	    (prot & VM_PROT_EXECUTE) != 0)
		return (EINVAL);
	vmmfs_node_hold(&launch->node);
	*color = 0;
	return (0);
}

static void
vmmfs_launch_pager_dtor(void *handle)
{
	struct vmmfs_launch *launch = handle;

	vmmfs_node_put(&launch->node);
}

static int
vmmfs_launch_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
	vm_page_t *page_result)
{
	struct vmmfs_launch *launch = object->handle;
	struct vm_object *backing;
	vm_page_t page;

	if (launch == NULL || offset < 0 || offset >= launch->node.size ||
	    (prot & VM_PROT_EXECUTE) != 0)
		return (VM_PAGER_ERROR);
	lwkt_gettoken(&launch->node.token);
	if (launch->node.dead || launch->pager_object != object)
		goto failed;
	backing = launch->backing_object;
	page = vm_page_grab(backing, OFF_TO_IDX(offset),
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	/* vm_page_grab may sleep and temporarily release the token. */
	if (page == NULL)
		goto failed;
	if (launch->node.dead || launch->pager_object != object) {
		vm_page_wakeup(page);
		goto failed;
	}
	if (page->valid != VM_PAGE_BITS_ALL)
		vm_page_zero_invalid(page, TRUE);
	*page_result = page;
	lwkt_reltoken(&launch->node.token);
	return (VM_PAGER_OK);
failed:
	lwkt_reltoken(&launch->node.token);
	return (VM_PAGER_ERROR);
}
