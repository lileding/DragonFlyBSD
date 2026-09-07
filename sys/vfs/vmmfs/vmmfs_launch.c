/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private launch file: guest-memory mmap followed by one cpustate write.
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
#include <sys/signalvar.h>
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

static int vmmfs_launch_getattr(struct vop_getattr_args *);
static int vmmfs_launch_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_launch_reclaim(struct vop_reclaim_args *);
static int vmmfs_launch_inactive(struct vop_inactive_args *);
static int vmmfs_launch_map(struct vmmfs_launch *, struct vm_object *);
static void vmmfs_launch_revoke(struct vmmfs_launch *);
static void vmmfs_launch_complete(struct vmmfs_launch *, int);
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
	.vop_getattr = vmmfs_launch_getattr,
	.vop_getattr_lite = vmmfs_launch_getattr_lite,
	.vop_mmap = (void *)vop_null,
	.vop_inactive = vmmfs_launch_inactive,
	.vop_reclaim = vmmfs_launch_reclaim,
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
vmmfs_launch_create(struct vmmfs_machine *machine,
	void (*post_launch)(struct vmmfs_launch *), struct vmmfs_launch **objectp)
{
	struct vmmfs_launch *launch;
	struct vmmfs_mount *mount = machine->node.mount;
	struct vnode *vnode;
	u_int serial;
	int error;

	*objectp = NULL;
	launch = kmalloc(sizeof(*launch), M_VMMFS, M_WAITOK | M_ZERO);
	launch->references = 1;
	launch->machine = machine;
	launch->post_launch = post_launch;
	launch->size = machine->memory.size;
	launch->inode = vmmfs_root_allocate_inode((struct vmmfs_root *)mount->root);
	lwkt_token_init(&launch->token, "vmmfslaunch");
	vmmfs_node_hold(&machine->node);
	serial = atomic_fetchadd_int(&vmmfs_launch_serial, 1);
	launch->dev = make_only_dev(&vmmfs_launch_dev_ops, serial,
	    UID_ROOT, GID_WHEEL, 0600, "vmmfs_launch%u", serial);
	if (launch->dev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	launch->dev->si_drv1 = launch;
	error = getspecialvnode(VT_SYNTH, mount->mount, &mount->launch_vops,
	    &vnode, 0, 0);
	if (error != 0)
		goto fail;
	vnode->v_ops = &mount->launch_vops;
	vnode->v_type = VCHR;
	error = v_associate_rdev(vnode, launch->dev);
	if (error == 0) {
		vnode->v_data = launch;
		vnode->v_umajor = launch->dev->si_umajor;
		vnode->v_uminor = launch->dev->si_uminor;
		launch->vnode = vnode;
		vmmfs_launch_hold(launch); /* The private carrier's reclaim reference. */
	} else {
		vnode->v_type = VBAD;
	}
	vx_downgrade(vnode);
	vn_unlock(vnode);
	if (error != 0) {
		vrele(vnode);
		goto fail;
	}
	error = vmmfs_launch_map(launch, machine->memory.object);
	if (error != 0) {
		vrele(vnode);
		goto fail;
	}
	*objectp = launch;
	return (0);
fail:
	vmmfs_launch_put(launch);
	return (error);
}

void
vmmfs_launch_hold(struct vmmfs_launch *launch)
{
	atomic_add_int(&launch->references, 1);
}

void
vmmfs_launch_put(struct vmmfs_launch *launch)
{
	if (atomic_fetchadd_int(&launch->references, -1) != 1)
		return;
	KKASSERT(launch->pager_object == NULL);
	if (launch->backing_object != NULL)
		vm_object_deallocate(launch->backing_object);
	if (launch->dev != NULL) {
		launch->dev->si_drv1 = NULL;
		destroy_only_dev(launch->dev);
	}
	lwkt_gettoken(&sigio_token);
	funsetown(&launch->loader_signal);
	lwkt_reltoken(&sigio_token);
	vmmfs_node_put(&launch->machine->node);
	lwkt_token_uninit(&launch->token);
	kfree(launch, M_VMMFS);
}

static int
vmmfs_launch_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_launch *launch = ap->a_vp->v_data;

	ap->a_vp->v_data = NULL;
	if (launch != NULL)
		vmmfs_launch_put(launch);
	return (0);
}

static int
vmmfs_launch_inactive(struct vop_inactive_args *ap)
{
	(void)vrecycle(ap->a_vp);
	return (0);
}

static int
vmmfs_launch_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_launch *launch = ap->a_vp->v_data;
	struct vattr *attr = ap->a_vap;

	VATTR_NULL(attr);
	attr->va_type = VCHR;
	attr->va_mode = 0600;
	attr->va_nlink = 1;
	attr->va_uid = 0;
	attr->va_gid = 0;
	attr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	attr->va_fileid = launch->inode;
	attr->va_size = launch->size;
	attr->va_bytes = launch->size;
	attr->va_blocksize = PAGE_SIZE;
	attr->va_flags = 0;
	attr->va_filerev = 0;
	return (0);
}

static int
vmmfs_launch_getattr_lite(struct vop_getattr_lite_args *ap)
{
	struct vmmfs_launch *launch = ap->a_vp->v_data;
	struct vattr_lite *attr = ap->a_lvap;

	attr->va_type = VCHR;
	attr->va_mode = 0600;
	attr->va_nlink = 1;
	attr->va_uid = 0;
	attr->va_gid = 0;
	attr->va_size = launch->size;
	attr->va_flags = 0;
	return (0);
}

static int
vmmfs_launch_map(struct vmmfs_launch *launch, struct vm_object *object)
{
	vm_object_reference_quick(object);
	launch->backing_object = object;
	launch->pager_object = cdev_pager_allocate(launch, OBJT_MGTDEVICE,
	    &vmmfs_launch_pager_ops, launch->size,
	    VM_PROT_READ | VM_PROT_WRITE, 0, proc0.p_ucred);
	return (launch->pager_object == NULL ? ENOMEM : 0);
}

static int
vmmfs_launch_attach(struct vmmfs_launch *launch, struct vnode *vnode,
	struct ucred *cred, struct file *file)
{
	lwkt_gettoken(&launch->token);
	if (launch->pager_object == NULL) {
		lwkt_reltoken(&launch->token);
		return (ECANCELED);
	}
	fsetcred(file, cred);
	file->f_type = DTYPE_VNODE;
	file->f_flag = FREAD | FWRITE;
	file->f_ops = &vmmfs_launch_fileops;
	file->f_data = vnode;
	vmmfs_launch_hold(launch);
	vref(vnode);
	atomic_add_int(&vnode->v_opencount, 1);
	atomic_add_int(&vnode->v_writecount, 1);
	lwkt_reltoken(&launch->token);
	return (0);
}

int
vmmfs_launch_open(struct vmmfs_launch *launch, struct ucred *cred, struct file **filep)
{
	struct vnode *vnode = launch->vnode;
	struct file *file;
	int error;

	error = falloc(NULL, &file, NULL);
	if (error != 0)
		return (error);
	error = vmmfs_launch_attach(launch, vnode, cred, file);
	if (error != 0)
		fp_close(file);
	else
		*filep = file;
	return (error);
}

static int
vmmfs_launch_submit(struct vmmfs_launch *launch, struct uio *uio)
{
	struct vmmfs_machine *machine = launch->machine;
	int error = EINVAL;

	if (!atomic_cmpset_int(&launch->claimed, 0, 1))
		return (EPIPE);
	if ((uio->uio_offset == -1 || uio->uio_offset == 0) &&
	    uio->uio_resid == sizeof(launch->cpustate)) {
		uio->uio_offset = 0;
		error = uiomove((caddr_t)&launch->cpustate,
		    sizeof(launch->cpustate), uio);
	}
	vmmfs_launch_revoke(launch);
	if (error == 0)
		error = vmmfs_memory_snapshot(&machine->memory);
	if (error == 0) {
		machine->boot_state = launch->cpustate;
		error = vmmfs_vcpu_prepare(&machine->vcpu, machine->vcpu.count,
		    machine->machine, &launch->cpustate);
	}
	vmmfs_launch_complete(launch, error);
	return (error);
}

static int
vmmfs_launch_write(struct file *file, struct uio *uio,
	struct ucred *cred, int flags)
{
	struct vnode *vnode = file->f_data;
	struct vmmfs_launch *launch;
	int error;

	(void)cred;
	(void)flags;
	if (vnode == NULL)
		return (EBADF);
	launch = vnode->v_data;
	vmmfs_launch_hold(launch);
	error = vmmfs_launch_submit(launch, uio);
	vmmfs_launch_put(launch);
	return (error);
}

static int
vmmfs_launch_close(struct file *file)
{
	struct vnode *vnode = file->f_data;
	struct vmmfs_launch *launch = vnode->v_data;
	int error;

	/* The file's reference covers cancellation and recursive fd revocation. */
	vmmfs_launch_cancel(launch);
	file->f_data = NULL;
	file->f_ops = &badfileops;
	error = vn_close(vnode, file->f_flag, file);
	vmmfs_launch_put(launch);
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
	status->st_size = launch->size;
	status->st_ino = launch->inode;
	status->st_blocks = howmany(status->st_size, S_BLKSIZE);
	status->st_blksize = PAGE_SIZE;
	status->__old_st_blksize = PAGE_SIZE;
	return (0);
}

void
vmmfs_launch_cancel(struct vmmfs_launch *launch)
{
	if (!atomic_cmpset_int(&launch->claimed, 0, 1))
		return;
	lwkt_gettoken(&sigio_token);
	pgsigio(launch->loader_signal, SIGKILL, 0);
	funsetown(&launch->loader_signal);
	lwkt_reltoken(&sigio_token);
	vmmfs_launch_revoke(launch);
	vmmfs_launch_complete(launch, ECANCELED);
}

static void
vmmfs_launch_complete(struct vmmfs_launch *launch, int result)
{
	struct vnode *vnode = launch->vnode;
	int error;

	/* claimed excludes subsequent submit/close while revoke drops files. */
	error = fdrevoke(vnode, DTYPE_VNODE, proc0.p_ucred);
	if (error != 0)
		kprintf("vmmfs: launch fd revoke: %d\n", error);
	lwkt_gettoken(&sigio_token);
	funsetown(&launch->loader_signal);
	lwkt_reltoken(&sigio_token);
	launch->dev->si_drv1 = NULL;
	destroy_only_dev(launch->dev);
	launch->dev = NULL;
	launch->result = result;
	launch->post_launch(launch);
	/* post_launch owns stopped publication; ready includes that work. */
	atomic_store_rel_int(&launch->ready, 1);
	wakeup(launch);
	if (launch->result == 0)
		vmmfs_vcpu_run(&launch->machine->vcpu);
	vrele(vnode);
}

int
vmmfs_launch_wait(struct vmmfs_launch *launch)
{
	int error = 0;

	tsleep_interlock(launch, PCATCH);
	if (atomic_load_acq_int(&launch->ready)) {
		crit_enter();
		tsleep_remove(curthread);
		crit_exit();
	} else {
		error = tsleep(launch, PINTERLOCKED | PCATCH, "vmmfslaunch", 0);
	}
	if (error != 0) {
		vmmfs_launch_cancel(launch);
		/* Cancellation may lose; wait for the sole completion wakeup. */
		tsleep_interlock(launch, 0);
		if (atomic_load_acq_int(&launch->ready)) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
		} else {
			(void)tsleep(launch, PINTERLOCKED, "vmmfslaunch", 0);
		}
	}
	return (launch->result == ECANCELED && error != 0 ?
	    error : launch->result);
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

static void
vmmfs_launch_revoke(struct vmmfs_launch *launch)
{
	struct vm_object *object;
	bool retry;

	lwkt_gettoken(&launch->token);
	object = launch->pager_object;
	launch->pager_object = NULL;
	lwkt_reltoken(&launch->token);
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
vmmfs_launch_get_mapping(struct vmmfs_launch *launch, vm_ooffset_t offset,
	vm_size_t size, struct vm_object **objectp)
{
	struct vm_object *object;
	lwkt_gettoken(&launch->token);
	object = launch->pager_object;
	if (object == NULL)
		goto closed;
	if (offset < 0 || offset >= launch->size ||
	    size > launch->size - offset) {
		lwkt_reltoken(&launch->token);
		return (EINVAL);
	}
	vm_object_reference_quick(object);
	*objectp = object;
	lwkt_reltoken(&launch->token);
	return (0);
closed:
	lwkt_reltoken(&launch->token);
	return (EBADF);
}

static int
vmmfs_launch_mmap(struct dev_mmap_single_args *ap)
{
	struct vmmfs_launch *launch = ap->a_head.a_dev->si_drv1;
	vm_ooffset_t offset = *ap->a_offset;

	if (launch == NULL || (ap->a_nprot & VM_PROT_EXECUTE) != 0)
		return (EINVAL);
	return (vmmfs_launch_get_mapping(launch, offset,
	    ap->a_size, ap->a_object));
}

static int
vmmfs_launch_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
	vm_ooffset_t offset, struct ucred *cred, u_short *color)
{
	struct vmmfs_launch *launch = handle;

	(void)cred;
	if (offset != 0 || size != launch->size ||
	    (prot & VM_PROT_EXECUTE) != 0)
		return (EINVAL);
	vmmfs_launch_hold(launch);
	*color = 0;
	return (0);
}

static void
vmmfs_launch_pager_dtor(void *handle)
{
	struct vmmfs_launch *launch = handle;

	vmmfs_launch_put(launch);
}

static int
vmmfs_launch_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
	vm_page_t *page_result)
{
	struct vmmfs_launch *launch = object->handle;
	struct vm_object *backing;
	vm_page_t page;

	if (launch == NULL || offset < 0 || offset >= launch->size ||
	    (prot & VM_PROT_EXECUTE) != 0)
		return (VM_PAGER_ERROR);
	lwkt_gettoken(&launch->token);
	if (launch->pager_object != object)
		goto failed;
	backing = launch->backing_object;
	page = vm_page_grab(backing, OFF_TO_IDX(offset),
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	/* vm_page_grab may sleep and temporarily release the token. */
	if (page == NULL)
		goto failed;
	if (launch->pager_object != object) {
		vm_page_wakeup(page);
		goto failed;
	}
	if (page->valid != VM_PAGE_BITS_ALL)
		vm_page_zero_invalid(page, TRUE);
	*page_result = page;
	lwkt_reltoken(&launch->token);
	return (VM_PAGER_OK);
failed:
	lwkt_reltoken(&launch->token);
	return (VM_PAGER_ERROR);
}
