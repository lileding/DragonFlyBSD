/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-run, per-provider revocable DMA capabilities.
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
#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include "vmm_dma.h"

struct vmm_dma_cap {
	/*
	 * Ref map:
	 * The cdev private-data destructor owns the initial reference.  Every
	 * capability pager ctor/dtor owns one additional reference.  This object
	 * may outlive the machine after revoke because a provider or its descendant
	 * retains an fd or mmap.  atomic_mut_refs brackets the module unload veto.
	 *
	 * Lock map:
	 * After cdev_pager_allocate() publishes own_mut_object, that object's
	 * token protects mut_revoked and own_mut_vmspace.  Revoke detaches that
	 * vmspace after removing pmap entries; an in-flight pager fault holds its
	 * own temporary reference.  All other fields are immutable after create.
	 */
	cdev_t			own_mut_dev;
	struct vnode		*own_mut_vnode;
	struct vm_object	*own_mut_object;
	struct vmspace		*own_mut_vmspace;
	struct file		*own_mut_fp;
	uint64_t		imm_generation;
	uint64_t		imm_aperture_size;
	unsigned int		imm_range_count;
	struct vmm_mem_dma_range
				imm_ranges[VMM_MEM_DMA_MAX_RANGES];
	int			mut_revoked;
	int			atomic_mut_refs;
};

static uint32_t vmm_dma_cap_serial;
static int vmm_dma_capability_count;

static d_open_t		vmm_dma_cap_open;
static d_close_t	vmm_dma_cap_close;
static d_mmap_single_t	vmm_dma_cap_mmap_single;
static int	vmm_dma_cap_vop_getattr(struct vop_getattr_args *);
static int	vmm_dma_cap_fo_readwrite(struct file *, struct uio *,
			    struct ucred *, int);
static int	vmm_dma_cap_fo_ioctl(struct file *, u_long, caddr_t,
			    struct ucred *, struct sysmsg *);
static int	vmm_dma_cap_fo_kqfilter(struct file *, struct knote *);
static int	vmm_dma_cap_fo_stat(struct file *, struct stat *,
			    struct ucred *);
static int	vmm_dma_cap_fo_close(struct file *);
static int	vmm_dma_cap_fo_seek(struct file *, off_t, int, off_t *);
static void	vmm_dma_cap_fd_free(void *);
static int	vmm_dma_cap_pager_fault(vm_object_t, vm_ooffset_t, int,
			    vm_page_t *);
static int	vmm_dma_cap_pager_ctor(void *, vm_ooffset_t, vm_prot_t,
			    vm_ooffset_t, struct ucred *, u_short *);
static void	vmm_dma_cap_pager_dtor(void *);
static int	vmm_dma_cap_open_fd(struct vmm_dma_cap *, struct file **);
static int	vmm_dma_cap_make_vnode(cdev_t, struct vnode **);
static void	vmm_dma_cap_disarm_fp(struct file *);
static void	vmm_dma_cap_fill_vattr(struct vattr *, vm_size_t);
static int	vmm_dma_cap_range_contains(const struct vmm_dma_cap *,
			    uint64_t, uint64_t);

static struct dev_ops vmm_dma_cap_fd_ops = {
	{ "vmm_dma_cap", 0, D_MPSAFE },
	.d_open =	vmm_dma_cap_open,
	.d_close =	vmm_dma_cap_close,
	.d_mmap_single = vmm_dma_cap_mmap_single,
};

static struct cdev_pager_ops vmm_dma_cap_pager_ops = {
	.cdev_pg_fault =	vmm_dma_cap_pager_fault,
	.cdev_pg_ctor =	vmm_dma_cap_pager_ctor,
	.cdev_pg_dtor =	vmm_dma_cap_pager_dtor,
};

static struct vop_ops vmm_dma_cap_vnode_vops = {
	.vop_default =	vop_defaultop,
	.vop_close =	vop_stdclose,
	.vop_getattr =	vmm_dma_cap_vop_getattr,
	.vop_advlock =	(void *)vop_null,
	.vop_inactive =	(void *)vop_null,
	.vop_reclaim =	(void *)vop_null,
	.vop_pathconf =	vop_stdpathconf,
};

static struct vop_ops *vmm_dma_cap_vnode_vops_p = &vmm_dma_cap_vnode_vops;

static struct fileops vmm_dma_cap_fileops = {
	.fo_read =	vmm_dma_cap_fo_readwrite,
	.fo_write =	vmm_dma_cap_fo_readwrite,
	.fo_ioctl =	vmm_dma_cap_fo_ioctl,
	.fo_kqfilter = vmm_dma_cap_fo_kqfilter,
	.fo_stat =	vmm_dma_cap_fo_stat,
	.fo_close =	vmm_dma_cap_fo_close,
	.fo_shutdown =	nofo_shutdown,
	.fo_seek =	vmm_dma_cap_fo_seek,
};

void
vmm_dma_init(struct vmm_dma *dma)
{

	if (dma != NULL)
		bzero(dma, sizeof(*dma));
}

void
vmm_dma_uninit(struct vmm_dma *dma)
{

	vmm_dma_stop(dma);
}

int
vmm_dma_start(struct vmm_dma *dma, struct vmm_mem *mem)
{
	struct vmspace *vmspace;
	uint64_t aperture_size;
	unsigned int range_count;
	int error;

	if (dma == NULL || dma->own_mut_vmspace != NULL)
		return EBUSY;
	vmspace = NULL;
	aperture_size = 0;
	range_count = 0;
	error = vmm_mem_dma_snapshot(mem, &vmspace, &aperture_size,
	    dma->imm_ranges, &range_count);
	if (error != 0)
		return error;
	if (dma->mut_generation == (uint64_t)-1) {
		vmspace_rel(vmspace);
		return EOVERFLOW;
	}
	dma->mut_generation++;
	if (dma->mut_generation == 0)
		dma->mut_generation++;
	dma->own_mut_vmspace = vmspace;
	dma->imm_aperture_size = aperture_size;
	dma->imm_range_count = range_count;
	return 0;
}

void
vmm_dma_stop(struct vmm_dma *dma)
{
	struct vmspace *vmspace;

	if (dma == NULL)
		return;
	vmspace = dma->own_mut_vmspace;
	dma->own_mut_vmspace = NULL;
	dma->imm_aperture_size = 0;
	dma->imm_range_count = 0;
	bzero(dma->imm_ranges, sizeof(dma->imm_ranges));
	if (vmspace != NULL)
		vmspace_rel(vmspace);
}

int
vmm_dma_cap_create(struct vmm_dma *dma, struct vmm_dma_cap **capp)
{
	struct vmm_dma_cap *cap;
	uint32_t serial;
	int error;

	if (capp != NULL)
		*capp = NULL;
	if (dma == NULL || capp == NULL || dma->own_mut_vmspace == NULL ||
	    dma->imm_aperture_size == 0 || dma->imm_range_count == 0)
		return EINVAL;
	cap = kmalloc(sizeof(*cap), M_TEMP, M_WAITOK | M_ZERO);
	vmspace_ref(dma->own_mut_vmspace);
	cap->own_mut_vmspace = dma->own_mut_vmspace;
	cap->imm_generation = dma->mut_generation;
	cap->imm_aperture_size = dma->imm_aperture_size;
	cap->imm_range_count = dma->imm_range_count;
	bcopy(dma->imm_ranges, cap->imm_ranges, sizeof(cap->imm_ranges));
	cap->atomic_mut_refs = 1;
	atomic_add_int(&vmm_dma_capability_count, 1);
	cap->own_mut_object = cdev_pager_allocate(cap, OBJT_MGTDEVICE,
	    &vmm_dma_cap_pager_ops, cap->imm_aperture_size,
	    VM_PROT_READ | VM_PROT_WRITE, 0, proc0.p_ucred);
	if (cap->own_mut_object == NULL) {
		vmm_dma_cap_fd_free(cap);
		return ENOMEM;
	}
	serial = atomic_fetchadd_int(&vmm_dma_cap_serial, 1);
	cap->own_mut_dev = make_only_dev(&vmm_dma_cap_fd_ops, serial, UID_ROOT,
	    GID_WHEEL, 0600, "vmmdma%d", serial);
	if (cap->own_mut_dev == NULL) {
		vmm_dma_cap_fd_free(cap);
		return ENXIO;
	}
	cap->own_mut_dev->si_drv1 = cap;
	error = vmm_dma_cap_open_fd(cap, &cap->own_mut_fp);
	if (error != 0)
		return error;
	*capp = cap;
	return 0;
}

void
vmm_dma_cap_revoke(struct vmm_dma_cap *cap)
{
	struct vm_object *object;
	struct file *fp;
	struct vmspace *vmspace;

	if (cap == NULL)
		return;
	vmspace = NULL;
	object = cap->own_mut_object;
	if (object != NULL) {
		VM_OBJECT_LOCK(object);
		if (!cap->mut_revoked) {
			cap->mut_revoked = 1;
			/* Publish rejection before invalidating installed user mappings. */
			vm_object_page_remove(object, 0, 0, FALSE);
			vmspace = cap->own_mut_vmspace;
			cap->own_mut_vmspace = NULL;
		}
		VM_OBJECT_UNLOCK(object);
	} else {
		cap->mut_revoked = 1;
		vmspace = cap->own_mut_vmspace;
		cap->own_mut_vmspace = NULL;
	}
	if (vmspace != NULL)
		vmspace_rel(vmspace);
	fp = cap->own_mut_fp;
	cap->own_mut_fp = NULL;
	if (fp != NULL && fp->f_type == DTYPE_VNODE && fp->f_data != NULL)
		(void)fdrevoke(fp->f_data, DTYPE_VNODE, proc0.p_ucred);
	if (fp != NULL)
		fp_close(fp);
}

struct file *
vmm_dma_cap_file_hold(struct vmm_dma_cap *cap)
{
	struct file *fp;

	if (cap == NULL || cap->mut_revoked || cap->own_mut_fp == NULL)
		return NULL;
	fp = cap->own_mut_fp;
	fhold(fp);
	return fp;
}

uint64_t
vmm_dma_cap_generation(const struct vmm_dma_cap *cap)
{

	return cap != NULL ? cap->imm_generation : 0;
}

unsigned int
vmm_dma_cap_ranges(const struct vmm_dma_cap *cap,
    struct vmm_mem_dma_range *ranges, unsigned int range_cap)
{
	unsigned int count;

	if (cap == NULL || ranges == NULL || range_cap < cap->imm_range_count)
		return 0;
	count = cap->imm_range_count;
	bcopy(cap->imm_ranges, ranges, count * sizeof(*ranges));
	return count;
}

int
vmm_dma_mmap_active(void)
{

	return atomic_fetchadd_int(&vmm_dma_capability_count, 0) != 0;
}

static int
vmm_dma_cap_open(struct dev_open_args *ap)
{

	(void)ap;
	return 0;
}

static int
vmm_dma_cap_close(struct dev_close_args *ap)
{

	(void)ap;
	return 0;
}

static void
vmm_dma_cap_fill_vattr(struct vattr *vap, vm_size_t size)
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
vmm_dma_cap_vop_getattr(struct vop_getattr_args *ap)
{
	struct vmm_dma_cap *cap;
	vm_size_t size;
	int error;

	size = 0;
	if (ap->a_fp != NULL) {
		error = devfs_get_cdevpriv(ap->a_fp, (void **)&cap);
		if (error != 0)
			return error;
		size = cap->imm_aperture_size;
	}
	vmm_dma_cap_fill_vattr(ap->a_vap, size);
	return 0;
}

static int
vmm_dma_cap_fo_readwrite(struct file *fp, struct uio *uio,
    struct ucred *cred, int flags)
{

	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
vmm_dma_cap_fo_ioctl(struct file *fp, u_long com, caddr_t data,
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
vmm_dma_cap_fo_kqfilter(struct file *fp, struct knote *kn)
{

	(void)fp;
	(void)kn;
	return EOPNOTSUPP;
}

static int
vmm_dma_cap_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{
	struct vmm_dma_cap *cap;
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
	sb->st_size = (off_t)cap->imm_aperture_size;
	sb->st_blocks = howmany(cap->imm_aperture_size, S_BLKSIZE);
	sb->st_blksize = PAGE_SIZE;
	sb->__old_st_blksize = sb->st_blksize;
	return 0;
}

static void
vmm_dma_cap_disarm_fp(struct file *fp)
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
vmm_dma_cap_fo_close(struct file *fp)
{

	vmm_dma_cap_disarm_fp(fp);
	return 0;
}

static int
vmm_dma_cap_fo_seek(struct file *fp, off_t offset, int whence, off_t *res)
{

	(void)fp;
	(void)offset;
	(void)whence;
	(void)res;
	return ESPIPE;
}

static int
vmm_dma_cap_range_contains(const struct vmm_dma_cap *cap, uint64_t offset,
    uint64_t size)
{
	unsigned int i;

	for (i = 0; i < cap->imm_range_count; i++) {
		if (offset < cap->imm_ranges[i].raw_gpa)
			continue;
		if (offset - cap->imm_ranges[i].raw_gpa <=
		    cap->imm_ranges[i].imm_size && size <=
		    cap->imm_ranges[i].imm_size -
		    (offset - cap->imm_ranges[i].raw_gpa))
			return 1;
	}
	return 0;
}

static int
vmm_dma_cap_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
	struct vmm_dma_cap *cap;

	(void)cred;
	cap = handle;
	if (cap == NULL || color == NULL || (prot & VM_PROT_EXECUTE) != 0 ||
	    foff < 0 || foff > cap->imm_aperture_size ||
	    size > cap->imm_aperture_size - foff ||
	    cap->mut_revoked || cap->own_mut_vmspace == NULL)
		return EINVAL;
	*color = 0;
	atomic_add_int(&cap->atomic_mut_refs, 1);
	return 0;
}

static void
vmm_dma_cap_pager_dtor(void *handle)
{
	struct vmm_dma_cap *cap;
	struct vmspace *vmspace;

	cap = handle;
	if (cap == NULL)
		return;
	vmspace = cap->own_mut_vmspace;
	cap->own_mut_vmspace = NULL;
	if (vmspace != NULL)
		vmspace_rel(vmspace);
	if (atomic_fetchadd_int(&cap->atomic_mut_refs, -1) == 1) {
		atomic_add_int(&vmm_dma_capability_count, -1);
		kfree(cap, M_TEMP);
	}
}

static int
vmm_dma_cap_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
    vm_page_t *mres)
{
	struct vmm_dma_cap *cap;
	struct vmspace *vmspace;
	vm_page_t page;
	int busy;
	int error;

	cap = object->handle;
	if (cap == NULL || mres == NULL || (prot & VM_PROT_EXECUTE) != 0 ||
	    offset < 0 || !vmm_dma_cap_range_contains(cap, offset, PAGE_SIZE))
		return VM_PAGER_ERROR;
	/* The outer MGTDEVICE object token serializes this check with revoke. */
	VM_OBJECT_LOCK(object);
	if (cap->mut_revoked || cap->own_mut_vmspace == NULL) {
		VM_OBJECT_UNLOCK(object);
		return VM_PAGER_ERROR;
	}
	vmspace = cap->own_mut_vmspace;
	vmspace_ref(vmspace);
	VM_OBJECT_UNLOCK(object);
	busy = 0;
	page = vm_fault_page(&vmspace->vm_map, offset,
	    VM_PROT_READ | VM_PROT_WRITE, VM_FAULT_DIRTY, &error, &busy);
	vmspace_rel(vmspace);
	if (error != 0 || page == NULL || !busy) {
		if (page != NULL && !busy)
			vm_page_unhold(page);
		return VM_PAGER_ERROR;
	}
	*mres = page;
	return VM_PAGER_OK;
}

static void
vmm_dma_cap_fd_free(void *arg)
{
	struct vmm_dma_cap *cap;
	struct vm_object *object;
	struct vnode *vp;
	struct vmspace *vmspace;

	cap = arg;
	if (cap == NULL)
		return;
	vmm_dma_cap_revoke(cap);
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
	object = cap->own_mut_object;
	cap->own_mut_object = NULL;
	if (object != NULL) {
		vm_object_deallocate(object);
	} else if (cap->own_mut_vmspace != NULL) {
		vmspace = cap->own_mut_vmspace;
		cap->own_mut_vmspace = NULL;
		vmspace_rel(vmspace);
	}
	if (atomic_fetchadd_int(&cap->atomic_mut_refs, -1) == 1) {
		atomic_add_int(&vmm_dma_capability_count, -1);
		kfree(cap, M_TEMP);
	}
}

static int
vmm_dma_cap_mmap_single(struct dev_mmap_single_args *ap)
{
	struct vmm_dma_cap *cap;
	struct vm_object *object;
	vm_ooffset_t offset;

	if (ap == NULL || ap->a_head.a_dev == NULL)
		return EINVAL;
	cap = ap->a_head.a_dev->si_drv1;
	if (cap == NULL || cap->own_mut_object == NULL ||
	    (ap->a_nprot & VM_PROT_EXECUTE) != 0)
		return EINVAL;
	offset = *ap->a_offset;
	if (offset < 0 || !vmm_dma_cap_range_contains(cap, offset, ap->a_size))
		return EINVAL;
	object = cap->own_mut_object;
	VM_OBJECT_LOCK(object);
	if (cap->mut_revoked || cap->own_mut_vmspace == NULL) {
		VM_OBJECT_UNLOCK(object);
		return EINVAL;
	}
	vm_object_reference_locked(object);
	VM_OBJECT_UNLOCK(object);
	*ap->a_object = object;
	return 0;
}

static int
vmm_dma_cap_make_vnode(cdev_t dev, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	error = getspecialvnode(VT_NON, NULL, &vmm_dma_cap_vnode_vops_p, &vp,
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
vmm_dma_cap_open_fd(struct vmm_dma_cap *cap, struct file **fpp)
{
	struct vnode *vp;
	struct file *fp;
	int error;

	error = vmm_dma_cap_make_vnode(cap->own_mut_dev, &vp);
	if (error != 0)
		goto fail;
	cap->own_mut_vnode = vp;
	error = falloc(NULL, &fp, NULL);
	if (error != 0)
		goto fail;
	fsetcred(fp, proc0.p_ucred);
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &vmm_dma_cap_fileops;
	fp->f_data = vp;
	vref(vp);
	atomic_add_int(&vp->v_opencount, 1);
	atomic_add_int(&vp->v_writecount, 1);
	error = devfs_set_cdevpriv(fp, cap, vmm_dma_cap_fd_free);
	if (error != 0) {
		fp_close(fp);
		vmm_dma_cap_fd_free(cap);
		return error;
	}
	*fpp = fp;
	return 0;

fail:
	vmm_dma_cap_fd_free(cap);
	return error;
}
