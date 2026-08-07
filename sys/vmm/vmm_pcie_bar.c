/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pathless OBJT_MGTDEVICE BAR capabilities.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysmsg.h>
#include <sys/uio.h>
#include <sys/ucred.h>
#include <sys/vnode.h>
#include <machine/atomic.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include "vmm_pcie_bar.h"

struct vmm_pcie_bar_fd {
	/*
	 * Ref map:
	 * The cdev private-data destructor owns the initial reference.  The
	 * pager ctor/dtor owns one reference for every mapped capability object.
	 * This object and its callbacks may outlive provider detach because a
	 * receiver can retain an SCM_RIGHTS BAR fd or mapping.  The global count
	 * is therefore the vmm.ko unload veto, not merely a mapped-page count.
	 */
	cdev_t		own_mut_dev;
	struct vnode	*own_mut_vnode;
	struct vm_object *own_mut_object;
	struct vm_object *own_mut_backing_object;
	vm_size_t	imm_size;
	int		mut_revoked;
	int		atomic_mut_refs;
};

static uint32_t vmm_pcie_bar_serial;
static int vmm_pcie_bar_capability_count;

static d_open_t		vmm_pcie_bar_fd_open;
static d_close_t		vmm_pcie_bar_fd_close;
static d_mmap_single_t	vmm_pcie_bar_fd_mmap_single;
static int		vmm_pcie_bar_vop_getattr(struct vop_getattr_args *);
static int		vmm_pcie_bar_fo_readwrite(struct file *, struct uio *,
			    struct ucred *, int);
static int		vmm_pcie_bar_fo_ioctl(struct file *, u_long, caddr_t,
			    struct ucred *, struct sysmsg *);
static int		vmm_pcie_bar_fo_kqfilter(struct file *, struct knote *);
static int		vmm_pcie_bar_fo_stat(struct file *, struct stat *,
			    struct ucred *);
static int		vmm_pcie_bar_fo_close(struct file *);
static int		vmm_pcie_bar_fo_seek(struct file *, off_t, int, off_t *);
static void		vmm_pcie_bar_fd_free(void *);
static int		vmm_pcie_bar_pager_fault(vm_object_t, vm_ooffset_t, int,
			    vm_page_t *);
static int		vmm_pcie_bar_pager_ctor(void *, vm_ooffset_t, vm_prot_t,
			    vm_ooffset_t, struct ucred *, u_short *);
static void		vmm_pcie_bar_pager_dtor(void *);
static int		vmm_pcie_bar_open_object_fd(struct vm_object *, vm_size_t,
			    struct file **);
static int		vmm_pcie_bar_open_fd(struct vmm_pcie_bar_fd *,
			    struct file **);
static int		vmm_pcie_bar_make_vnode(cdev_t, struct vnode **);
static void		vmm_pcie_bar_fd_ref(struct vmm_pcie_bar_fd *);
static void		vmm_pcie_bar_fd_put(struct vmm_pcie_bar_fd *);
static void		vmm_pcie_bar_cap_revoke(struct vmm_pcie_bar_fd *);
static void		vmm_pcie_bar_disarm_fp(struct file *);
static void		vmm_pcie_bar_fill_vattr(struct vattr *, vm_size_t);

static struct dev_ops vmm_pcie_bar_object_fd_ops = {
	{ "vmm_pcie_bar_object_fd", 0, D_MPSAFE },
	.d_open = vmm_pcie_bar_fd_open,
	.d_close = vmm_pcie_bar_fd_close,
	.d_mmap_single = vmm_pcie_bar_fd_mmap_single,
};

static struct cdev_pager_ops vmm_pcie_bar_pager_ops = {
	.cdev_pg_fault =	vmm_pcie_bar_pager_fault,
	.cdev_pg_ctor =	vmm_pcie_bar_pager_ctor,
	.cdev_pg_dtor =	vmm_pcie_bar_pager_dtor,
};

static struct vop_ops vmm_pcie_bar_vnode_vops = {
	.vop_default =	vop_defaultop,
	.vop_close =	vop_stdclose,
	.vop_getattr =	vmm_pcie_bar_vop_getattr,
	.vop_advlock =	(void *)vop_null,
	.vop_inactive =	(void *)vop_null,
	.vop_reclaim =	(void *)vop_null,
	.vop_pathconf =	vop_stdpathconf,
};

static struct vop_ops *vmm_pcie_bar_vnode_vops_p = &vmm_pcie_bar_vnode_vops;

static struct fileops vmm_pcie_bar_fileops = {
	.fo_read =	vmm_pcie_bar_fo_readwrite,
	.fo_write =	vmm_pcie_bar_fo_readwrite,
	.fo_ioctl =	vmm_pcie_bar_fo_ioctl,
	.fo_kqfilter =	vmm_pcie_bar_fo_kqfilter,
	.fo_stat =	vmm_pcie_bar_fo_stat,
	.fo_close =	vmm_pcie_bar_fo_close,
	.fo_shutdown =	nofo_shutdown,
	.fo_seek =	vmm_pcie_bar_fo_seek,
};

int
vmm_pcie_bar_create(struct vmm_pcie_bar *bar, uint64_t size, uint32_t flags)
{
	struct vm_object *backing;
	int error;

	if (bar == NULL || size == 0 || size > (uint64_t)VM_MAX_USER_ADDRESS ||
	    size != round_page(size))
		return EINVAL;
	bzero(bar, sizeof(*bar));
	backing = vm_object_allocate(OBJT_DEFAULT, OFF_TO_IDX(size));
	if (backing == NULL)
		return ENOMEM;
	bar->own_mut_backing_object = backing;
	error = vmm_pcie_bar_open_object_fd(backing, (vm_size_t)size,
	    &bar->own_mut_fp);
	if (error != 0) {
		vm_object_deallocate(bar->own_mut_backing_object);
		bar->own_mut_backing_object = NULL;
		return error;
	}
	bar->imm_size = size;
	bar->imm_flags = flags;
	return 0;
}

void
vmm_pcie_bar_revoke(struct vmm_pcie_bar *bar)
{
	struct vmm_pcie_bar_fd *cap;

	if (bar == NULL || bar->own_mut_fp == NULL)
		return;
	if (devfs_get_cdevpriv(bar->own_mut_fp, (void **)&cap) == 0)
		vmm_pcie_bar_cap_revoke(cap);
}

void
vmm_pcie_bar_destroy(struct vmm_pcie_bar *bar)
{

	if (bar == NULL)
		return;
	vmm_pcie_bar_revoke(bar);
	if (bar->own_mut_fp != NULL) {
		fp_close(bar->own_mut_fp);
		bar->own_mut_fp = NULL;
	}
	if (bar->own_mut_backing_object != NULL) {
		vm_object_deallocate(bar->own_mut_backing_object);
		bar->own_mut_backing_object = NULL;
	}
	bar->imm_size = 0;
	bar->imm_flags = 0;
}

int
vmm_pcie_bar_snapshot(struct vmm_pcie_bar *bar, struct vm_object **objectp,
    uint64_t *sizep)
{
	struct vm_object *object;

	if (objectp != NULL)
		*objectp = NULL;
	if (sizep != NULL)
		*sizep = 0;
	if (bar == NULL || objectp == NULL || sizep == NULL ||
	    bar->own_mut_backing_object == NULL || bar->imm_size == 0)
		return EINVAL;
	object = bar->own_mut_backing_object;
	vm_object_hold(object);
	vm_object_reference_locked(object);
	vm_object_drop(object);
	*objectp = object;
	*sizep = bar->imm_size;
	return 0;
}

int
vmm_pcie_bar_object_read32(struct vm_object *object, uint64_t size,
    uint64_t offset, uint32_t *valuep)
{
	vm_page_t page;
	uint64_t page_offset;
	uint32_t *ptr;

	if (object == NULL || valuep == NULL || (offset & 3) != 0 ||
	    offset > size || size - offset < sizeof(*valuep))
		return EINVAL;
	page_offset = offset & ~(uint64_t)PAGE_MASK;
	page = vm_page_grab(object, OFF_TO_IDX(page_offset),
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	if (page == NULL)
		return ENOMEM;
	if (page->valid != VM_PAGE_BITS_ALL)
		vm_page_zero_invalid(page, TRUE);
	ptr = (uint32_t *)(PHYS_TO_DMAP(VM_PAGE_TO_PHYS(page)) +
	    (offset & PAGE_MASK));
	*valuep = atomic_load_acq_int((volatile u_int *)ptr);
	vm_page_wakeup(page);
	return 0;
}

int
vmm_pcie_bar_mmap_active(void)
{

	return atomic_fetchadd_int(&vmm_pcie_bar_capability_count, 0) != 0;
}

static int
vmm_pcie_bar_fd_open(struct dev_open_args *ap)
{

	(void)ap;
	return 0;
}

static int
vmm_pcie_bar_fd_close(struct dev_close_args *ap)
{

	(void)ap;
	return 0;
}

static void
vmm_pcie_bar_fill_vattr(struct vattr *vap, vm_size_t size)
{

	VATTR_NULL(vap);
	vap->va_type = VCHR;
	vap->va_mode = 0600;
	vap->va_nlink = 1;
	vap->va_uid = UID_ROOT;
	vap->va_gid = GID_WHEEL;
	vap->va_fileid = 0;
	vap->va_size = (off_t)size;
	vap->va_blocksize = PAGE_SIZE;
	vap->va_bytes = (off_t)round_page(size);
	vap->va_atime.tv_sec = 0;
	vap->va_atime.tv_nsec = 0;
	vap->va_mtime = vap->va_atime;
	vap->va_ctime = vap->va_atime;
	vap->va_flags = 0;
	vap->va_gen = 1;
	vap->va_filerev = 0;
}

static int
vmm_pcie_bar_vop_getattr(struct vop_getattr_args *ap)
{
	struct vmm_pcie_bar_fd *cap;
	vm_size_t size;
	int error;

	size = 0;
	if (ap->a_fp != NULL) {
		error = devfs_get_cdevpriv(ap->a_fp, (void **)&cap);
		if (error != 0)
			return error;
		size = cap->imm_size;
	}
	vmm_pcie_bar_fill_vattr(ap->a_vap, size);
	return 0;
}

static int
vmm_pcie_bar_fo_readwrite(struct file *fp, struct uio *uio,
    struct ucred *cred, int flags)
{

	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
vmm_pcie_bar_fo_ioctl(struct file *fp, u_long com, caddr_t data,
    struct ucred *cred, struct sysmsg *msg)
{

	(void)fp;
	(void)com;
	(void)data;
	(void)cred;
	(void)msg;
	return EOPNOTSUPP;
}

static int
vmm_pcie_bar_fo_kqfilter(struct file *fp, struct knote *kn)
{

	(void)fp;
	(void)kn;
	return EOPNOTSUPP;
}

static int
vmm_pcie_bar_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{
	struct vmm_pcie_bar_fd *cap;
	int error;

	(void)cred;
	error = devfs_get_cdevpriv(fp, (void **)&cap);
	if (error != 0)
		return error;
	bzero(sb, sizeof(*sb));
	sb->st_nlink = 1;
	sb->st_mode = S_IFCHR | 0600;
	sb->st_uid = UID_ROOT;
	sb->st_gid = GID_WHEEL;
	sb->st_size = (off_t)cap->imm_size;
	sb->st_blocks = howmany(cap->imm_size, S_BLKSIZE);
	sb->st_blksize = PAGE_SIZE;
	sb->__old_st_blksize = sb->st_blksize;
	return 0;
}

static void
vmm_pcie_bar_disarm_fp(struct file *fp)
{
	struct vnode *vp;

	if (fp == NULL || fp->f_ops == &badfileops)
		return;
	vp = fp->f_data;
	fp->f_data = NULL;
	atomic_clear_int(&fp->f_flag, FHASLOCK);
	fp->f_ops = &badfileops;
	if (vp != NULL)
		(void)vn_close(vp, fp->f_flag, fp);
	devfs_clear_cdevpriv(fp);
}

static int
vmm_pcie_bar_fo_close(struct file *fp)
{

	vmm_pcie_bar_disarm_fp(fp);
	return 0;
}

static int
vmm_pcie_bar_fo_seek(struct file *fp, off_t offset, int whence, off_t *res)
{

	(void)fp;
	(void)offset;
	(void)whence;
	(void)res;
	return ESPIPE;
}

static void
vmm_pcie_bar_fd_ref(struct vmm_pcie_bar_fd *cap)
{

	atomic_add_int(&cap->atomic_mut_refs, 1);
}

static void
vmm_pcie_bar_fd_put(struct vmm_pcie_bar_fd *cap)
{

	if (atomic_fetchadd_int(&cap->atomic_mut_refs, -1) == 1) {
		atomic_add_int(&vmm_pcie_bar_capability_count, -1);
		kfree(cap, M_TEMP);
	}
}

static void
vmm_pcie_bar_cap_revoke(struct vmm_pcie_bar_fd *cap)
{
	struct vm_object *backing;
	int remove_pages;

	if (cap == NULL)
		return;
	backing = NULL;
	remove_pages = 0;
	if (cap->own_mut_object != NULL) {
		VM_OBJECT_LOCK(cap->own_mut_object);
		if (!cap->mut_revoked) {
			cap->mut_revoked = 1;
			remove_pages = 1;
			backing = cap->own_mut_backing_object;
			cap->own_mut_backing_object = NULL;
		}
		VM_OBJECT_UNLOCK(cap->own_mut_object);
	} else if (!cap->mut_revoked) {
		cap->mut_revoked = 1;
		backing = cap->own_mut_backing_object;
		cap->own_mut_backing_object = NULL;
	}
	if (remove_pages)
		vm_object_page_remove(cap->own_mut_object, 0, 0, FALSE);
	if (backing != NULL)
		vm_object_deallocate(backing);
}

static int
vmm_pcie_bar_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
	struct vmm_pcie_bar_fd *cap;

	(void)cred;
	cap = handle;
	if (cap == NULL || color == NULL || (prot & VM_PROT_EXECUTE) != 0 ||
	    foff < 0 || foff > cap->imm_size || size > cap->imm_size - foff ||
	    cap->mut_revoked || cap->own_mut_backing_object == NULL)
		return EINVAL;
	*color = 0;
	vmm_pcie_bar_fd_ref(cap);
	return 0;
}

static void
vmm_pcie_bar_pager_dtor(void *handle)
{
	struct vmm_pcie_bar_fd *cap;
	struct vm_object *backing;

	cap = handle;
	if (cap == NULL)
		return;
	backing = cap->own_mut_backing_object;
	cap->own_mut_backing_object = NULL;
	if (backing != NULL)
		vm_object_deallocate(backing);
	vmm_pcie_bar_fd_put(cap);
}

static int
vmm_pcie_bar_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
    vm_page_t *mres)
{
	struct vmm_pcie_bar_fd *cap;
	struct vm_object *backing;
	vm_page_t pg;

	cap = object->handle;
	if (cap == NULL || mres == NULL || offset < 0 || offset >= cap->imm_size ||
	    (prot & VM_PROT_EXECUTE) != 0)
		return VM_PAGER_ERROR;
	VM_OBJECT_LOCK(object);
	if (cap->mut_revoked) {
		VM_OBJECT_UNLOCK(object);
		return VM_PAGER_ERROR;
	}
	backing = cap->own_mut_backing_object;
	if (backing != NULL)
		vm_object_reference_quick(backing);
	VM_OBJECT_UNLOCK(object);
	if (backing == NULL)
		return VM_PAGER_ERROR;
	pg = vm_page_grab(backing, OFF_TO_IDX(offset),
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	vm_object_deallocate(backing);
	if (pg == NULL)
		return VM_PAGER_ERROR;
	if (pg->valid != VM_PAGE_BITS_ALL)
		vm_page_zero_invalid(pg, TRUE);
	*mres = pg;
	return VM_PAGER_OK;
}

static void
vmm_pcie_bar_fd_free(void *arg)
{
	struct vmm_pcie_bar_fd *cap;
	struct vnode *vp;

	cap = arg;
	if (cap == NULL)
		return;
	vmm_pcie_bar_cap_revoke(cap);
	vp = cap->own_mut_vnode;
	if (vp != NULL) {
		cap->own_mut_vnode = NULL;
		vx_get(vp);
		vgone_vxlocked(vp);
		vx_put(vp);
		vrele(vp);
	}
	if (cap->own_mut_dev != NULL) {
		cap->own_mut_dev->si_drv1 = NULL;
		destroy_only_dev(cap->own_mut_dev);
		cap->own_mut_dev = NULL;
	}
	if (cap->own_mut_object != NULL) {
		vm_object_deallocate(cap->own_mut_object);
		cap->own_mut_object = NULL;
	}
	vmm_pcie_bar_fd_put(cap);
}

static int
vmm_pcie_bar_fd_mmap_single(struct dev_mmap_single_args *ap)
{
	struct vmm_pcie_bar_fd *cap;
	struct vm_object *object;
	vm_ooffset_t off;

	if (ap == NULL || ap->a_head.a_dev == NULL)
		return EINVAL;
	cap = ap->a_head.a_dev->si_drv1;
	if (cap == NULL || cap->own_mut_object == NULL)
		return EINVAL;
	if ((ap->a_nprot & VM_PROT_EXECUTE) != 0)
		return EACCES;
	off = *ap->a_offset;
	if (off < 0 || off > cap->imm_size ||
	    ap->a_size > cap->imm_size - off)
		return EINVAL;
	object = cap->own_mut_object;
	VM_OBJECT_LOCK(object);
	if (cap->mut_revoked || cap->own_mut_backing_object == NULL) {
		VM_OBJECT_UNLOCK(object);
		return EINVAL;
	}
	vm_object_reference_locked(object);
	VM_OBJECT_UNLOCK(object);
	*ap->a_object = object;
	return 0;
}

static int
vmm_pcie_bar_make_vnode(cdev_t dev, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	error = getspecialvnode(VT_NON, NULL, &vmm_pcie_bar_vnode_vops_p, &vp,
	    0, 0);
	if (error != 0) {
		*vpp = NULL;
		return error;
	}
	vp->v_type = VCHR;
	error = v_associate_rdev(vp, dev);
	if (error != 0) {
		vgone_vxlocked(vp);
		vx_put(vp);
		*vpp = NULL;
		return error;
	}
	vp->v_umajor = dev->si_umajor;
	vp->v_uminor = dev->si_uminor;
	vx_unlock(vp);
	*vpp = vp;
	return 0;
}

static int
vmm_pcie_bar_open_fd(struct vmm_pcie_bar_fd *cap, struct file **fpp)
{
	struct vnode *vp;
	struct file *fp;
	int error;

	error = vmm_pcie_bar_make_vnode(cap->own_mut_dev, &vp);
	if (error != 0)
		goto fail;
	cap->own_mut_vnode = vp;
	error = falloc(NULL, &fp, NULL);
	if (error != 0)
		goto fail;
	fsetcred(fp, proc0.p_ucred);
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &vmm_pcie_bar_fileops;
	fp->f_data = vp;
	vref(vp);
	atomic_add_int(&vp->v_opencount, 1);
	atomic_add_int(&vp->v_writecount, 1);
	error = devfs_set_cdevpriv(fp, cap, vmm_pcie_bar_fd_free);
	if (error != 0) {
		fp_close(fp);
		vmm_pcie_bar_fd_free(cap);
		return error;
	}
	*fpp = fp;
	return 0;

fail:
	vmm_pcie_bar_fd_free(cap);
	return error;
}

static int
vmm_pcie_bar_open_object_fd(struct vm_object *backing, vm_size_t size,
    struct file **fpp)
{
	struct vmm_pcie_bar_fd *cap;
	uint32_t serial;

	if (backing == NULL || size == 0 || fpp == NULL)
		return EINVAL;
	cap = kmalloc(sizeof(*cap), M_TEMP, M_WAITOK | M_ZERO);
	vm_object_reference_quick(backing);
	cap->own_mut_backing_object = backing;
	cap->imm_size = round_page(size);
	cap->atomic_mut_refs = 1;
	atomic_add_int(&vmm_pcie_bar_capability_count, 1);
	cap->own_mut_object = cdev_pager_allocate(cap, OBJT_MGTDEVICE,
	    &vmm_pcie_bar_pager_ops, cap->imm_size,
	    VM_PROT_READ | VM_PROT_WRITE, 0, proc0.p_ucred);
	if (cap->own_mut_object == NULL) {
		vmm_pcie_bar_fd_free(cap);
		return ENOMEM;
	}
	serial = atomic_fetchadd_int(&vmm_pcie_bar_serial, 1);
	cap->own_mut_dev = make_only_dev(&vmm_pcie_bar_object_fd_ops, serial,
	    UID_ROOT, GID_WHEEL, 0600, "vmmb%d", serial);
	if (cap->own_mut_dev == NULL) {
		vmm_pcie_bar_fd_free(cap);
		return ENXIO;
	}
	cap->own_mut_dev->si_drv1 = cap;
	return vmm_pcie_bar_open_fd(cap, fpp);
}
