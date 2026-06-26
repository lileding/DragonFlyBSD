/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The loader object -- see vmm_loader.h.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/fcntl.h>
#include <sys/imgact.h>
#include <sys/kern_syscall.h>
#include <sys/nlookup.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
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

#include "vmm_parse.h"
#include "vmm_loader.h"
#include "vmm_loader_x86.h"

int
vmm_loader_path_parse(char *path, size_t *lenp, const char *buf, size_t len)
{
	size_t pl;
	const char *p = vmm_trim(buf, len, &pl);

	if (path == NULL || lenp == NULL || pl == 0 || pl > VMM_LOADER_MAX)
		return 0;
	memcpy(path, p, pl);
	path[pl] = '\0';
	*lenp = pl;
	return 1;
}

size_t
vmm_loader_path_format(const char *path, size_t len, char *out, size_t cap)
{
	size_t need = len + 1;

	if (len == 0 || need > cap)
		return 0;
	memcpy(out, path, len);
	out[len] = '\n';
	return need;
}

int
vmm_loader_path_is_set(size_t len)
{
	return len != 0;
}

#define VMM_MANIFEST_SIZE	PAGE_SIZE

struct vmm_loader_fd {
	/*
	 * Ref map:
	 * atomic_mut_refs keeps this heap object alive while either the devfs
	 * file private data or the cdev pager object can call back into vmm.  The
	 * initial ref belongs to devfs cdevpriv; the pager ctor/dtor pair owns a
	 * second ref.  vmm_loader_fd_active mirrors this heap-object lifetime so
	 * module unload refuses while any loader fd or mmap can still reach these
	 * callbacks.
	 *
	 * Lock map:
	 * after cdev_pager_allocate() publishes own_mut_object, its token protects
	 * mut_revoked and own_mut_backing_object.  The pager ctor runs before
	 * own_mut_object is assigned, so it only observes the construction-time
	 * state; mmap and fault paths recheck under the object token.
	 */
	cdev_t		own_mut_dev;
	struct vm_object *own_mut_object;		/* mmap capability */
	struct vm_object *own_mut_backing_object;	/* guest RAM/manifest */
	vm_size_t	imm_size;
	int		atomic_mut_refs;
	int		mut_revoked;		/* own_mut_object token */
};

static uint32_t vmm_loader_fd_serial;
static int vmm_loader_fd_active;

int
vmm_loader_busy(void)
{
	return atomic_fetchadd_int(&vmm_loader_fd_active, 0) != 0;
}


static d_open_t		vmm_loader_fd_open;
static d_close_t	vmm_loader_fd_close;
static d_mmap_single_t	vmm_loader_fd_mmap_single;
static int		vmm_loader_vop_getattr(struct vop_getattr_args *ap);
static int		vmm_loader_fo_readwrite(struct file *fp,
			    struct uio *uio, struct ucred *cred, int flags);
static int		vmm_loader_fo_ioctl(struct file *fp, u_long com,
			    caddr_t data, struct ucred *cred,
			    struct sysmsg *msg);
static int		vmm_loader_fo_kqfilter(struct file *fp,
			    struct knote *kn);
static int		vmm_loader_fo_stat(struct file *fp, struct stat *sb,
			    struct ucred *cred);
static int		vmm_loader_fo_close(struct file *fp);
static int		vmm_loader_fo_seek(struct file *fp, off_t offset,
			    int whence, off_t *res);
static void		vmm_loader_disarm_fp(struct file *fp);
static void		vmm_loader_fd_revoke(struct vmm_loader_fd *lfd);
static int		vmm_loader_pager_fault(vm_object_t object,
			    vm_ooffset_t offset, int prot, vm_page_t *mres);
static int		vmm_loader_pager_ctor(void *handle,
			    vm_ooffset_t size, vm_prot_t prot,
			    vm_ooffset_t foff, struct ucred *cred,
			    u_short *color);
static void		vmm_loader_pager_dtor(void *handle);

static struct dev_ops vmm_loader_object_fd_ops = {
	{ "vmm_loader_object_fd", 0, D_MPSAFE },
	.d_open = vmm_loader_fd_open,
	.d_close = vmm_loader_fd_close,
	.d_mmap_single = vmm_loader_fd_mmap_single,
};

static struct cdev_pager_ops vmm_loader_pager_ops = {
	.cdev_pg_fault =	vmm_loader_pager_fault,
	.cdev_pg_ctor =		vmm_loader_pager_ctor,
	.cdev_pg_dtor =		vmm_loader_pager_dtor,
};

static struct vop_ops vmm_loader_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_close =		vop_stdclose,
	.vop_getattr =		vmm_loader_vop_getattr,
	.vop_pathconf =		vop_stdpathconf,
};

static struct vop_ops *vmm_loader_vnode_vops_p = &vmm_loader_vnode_vops;

static struct fileops vmm_loader_fileops = {
	.fo_read =		vmm_loader_fo_readwrite,
	.fo_write =		vmm_loader_fo_readwrite,
	.fo_ioctl =		vmm_loader_fo_ioctl,
	.fo_kqfilter =		vmm_loader_fo_kqfilter,
	.fo_stat =		vmm_loader_fo_stat,
	.fo_close =		vmm_loader_fo_close,
	.fo_shutdown =		nofo_shutdown,
	.fo_seek =		vmm_loader_fo_seek,
};

static int
vmm_loader_fd_open(struct dev_open_args *ap)
{
	(void)ap;
	return 0;
}

static int
vmm_loader_fd_close(struct dev_close_args *ap)
{
	(void)ap;
	return 0;
}

static void
vmm_loader_fill_vattr(struct vattr *vap, vm_size_t size)
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
vmm_loader_vop_getattr(struct vop_getattr_args *ap)
{
	struct vmm_loader_fd *lfd;
	vm_size_t size = 0;
	int error;

	if (ap->a_fp != NULL) {
		error = devfs_get_cdevpriv(ap->a_fp, (void **)&lfd);
		if (error)
			return error;
		size = lfd->imm_size;
	}
	vmm_loader_fill_vattr(ap->a_vap, size);
	return 0;
}

static int
vmm_loader_fo_readwrite(struct file *fp, struct uio *uio,
    struct ucred *cred, int flags)
{
	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
vmm_loader_fo_ioctl(struct file *fp, u_long com, caddr_t data,
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
vmm_loader_fo_kqfilter(struct file *fp, struct knote *kn)
{
	(void)fp;
	(void)kn;
	return EOPNOTSUPP;
}

static int
vmm_loader_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{
	struct vmm_loader_fd *lfd;
	int error;

	(void)cred;
	error = devfs_get_cdevpriv(fp, (void **)&lfd);
	if (error)
		return error;
	bzero(sb, sizeof(*sb));
	sb->st_nlink = 1;
	sb->st_mode = S_IFCHR | 0600;
	sb->st_uid = UID_ROOT;
	sb->st_gid = GID_WHEEL;
	sb->st_size = (off_t)lfd->imm_size;
	sb->st_blocks = howmany(lfd->imm_size, S_BLKSIZE);
	sb->st_blksize = PAGE_SIZE;
	sb->__old_st_blksize = sb->st_blksize;
	return 0;
}

static void
vmm_loader_disarm_fp(struct file *fp)
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
vmm_loader_fo_close(struct file *fp)
{
	vmm_loader_disarm_fp(fp);
	return 0;
}

static int
vmm_loader_fo_seek(struct file *fp, off_t offset, int whence, off_t *res)
{
	(void)fp;
	(void)offset;
	(void)whence;
	(void)res;
	return ESPIPE;
}

static void
vmm_loader_fd_ref(struct vmm_loader_fd *lfd)
{
	atomic_add_int(&lfd->atomic_mut_refs, 1);
}

static void
vmm_loader_fd_put(struct vmm_loader_fd *lfd)
{
	if (atomic_fetchadd_int(&lfd->atomic_mut_refs, -1) == 1) {
		atomic_add_int(&vmm_loader_fd_active, -1);
		wakeup(&vmm_loader_fd_active);
		kfree(lfd, M_TEMP);
	}
}

static int
vmm_loader_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
	struct vmm_loader_fd *lfd = handle;

	(void)cred;
	if (lfd == NULL || color == NULL)
		return EINVAL;
	if (prot & VM_PROT_EXECUTE)
		return EACCES;
	if (foff < 0 || foff > lfd->imm_size || size > lfd->imm_size - foff)
		return EINVAL;
	if (lfd->mut_revoked || lfd->own_mut_backing_object == NULL)
		return EINVAL;

	*color = 0;
	vmm_loader_fd_ref(lfd);
	return 0;
}

static void
vmm_loader_pager_dtor(void *handle)
{
	struct vmm_loader_fd *lfd = handle;

	if (lfd != NULL)
		vmm_loader_fd_put(lfd);
}

static int
vmm_loader_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
    vm_page_t *mres)
{
	struct vmm_loader_fd *lfd;
	struct vm_object *backing;
	vm_page_t pg;

	lfd = object->handle;
	if (lfd == NULL || mres == NULL)
		return VM_PAGER_ERROR;
	if (offset < 0 || offset >= lfd->imm_size)
		return VM_PAGER_ERROR;
	if (prot & VM_PROT_EXECUTE)
		return VM_PAGER_ERROR;
	/*
	 * The fd object is only a revocable mmap capability.  It is not the
	 * storage object.  The returned page belongs to the real backing
	 * object: fd3's guest RAM or fd4's manifest page.
	 *
	 * DragonFly's OBJT_MGTDEVICE fault path allows this direct page return
	 * without inserting the page into the fd object.  The fd object still
	 * owns the map backing list for user mappings, so revoke can remove
	 * those pmap entries by calling vm_object_page_remove() on the fd
	 * object itself.  This keeps loader-to-vCPU handoff zero-copy and lets
	 * revoke cut off userland without releasing fd3 guest RAM.
	 *
	 * vm_fault_object() calls this pager with the fd object token held.
	 * VM_OBJECT_LOCK() is a recursive token hold here; the matching
	 * VM_OBJECT_UNLOCK() below drops only the inner hold, so the outer
	 * fault path still serializes through its later pmap_enter().
	 */
	VM_OBJECT_LOCK(object);
	if (lfd->mut_revoked || lfd->own_mut_backing_object == NULL) {
		VM_OBJECT_UNLOCK(object);
		return VM_PAGER_ERROR;
	}
	backing = lfd->own_mut_backing_object;
	vm_object_reference_quick(backing);
	VM_OBJECT_UNLOCK(object);
	pg = vm_page_grab(backing, OFF_TO_IDX(offset),
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO |
	    VM_ALLOC_RETRY);
	vm_object_deallocate(backing);
	if (pg == NULL)
		return VM_PAGER_ERROR;
	if (pg->valid != VM_PAGE_BITS_ALL)
		vm_page_zero_invalid(pg, TRUE);
	*mres = pg;
	return VM_PAGER_OK;
}

static void
vmm_loader_fd_revoke(struct vmm_loader_fd *lfd)
{
	struct vm_object *backing = NULL;
	struct vm_object *object;

	object = lfd->own_mut_object;
	if (object != NULL) {
		VM_OBJECT_LOCK(object);
		if (!lfd->mut_revoked) {
			lfd->mut_revoked = 1;
			backing = lfd->own_mut_backing_object;
			lfd->own_mut_backing_object = NULL;
			/*
			 * vm_fault() enters OBJT_MGTDEVICE pages while holding
			 * this fd object token through pmap_enter().  Keep
			 * revoke under the same token so no in-flight fault can
			 * pass the revoked check and install a fresh pmap entry
			 * after this removal pass.
			 */
			vm_object_page_remove(object, 0, 0, FALSE);
		}
		VM_OBJECT_UNLOCK(object);
	} else if (!lfd->mut_revoked) {
		lfd->mut_revoked = 1;
		backing = lfd->own_mut_backing_object;
		lfd->own_mut_backing_object = NULL;
	}
	if (backing != NULL)
		vm_object_deallocate(backing);
}

static void
vmm_loader_fd_free(void *arg)
{
	struct vmm_loader_fd *lfd = arg;

	vmm_loader_fd_revoke(lfd);
	if (lfd->own_mut_dev != NULL) {
		lfd->own_mut_dev->si_drv1 = NULL;
		destroy_only_dev(lfd->own_mut_dev);
		lfd->own_mut_dev = NULL;
	}
	if (lfd->own_mut_object != NULL) {
		vm_object_deallocate(lfd->own_mut_object);
		lfd->own_mut_object = NULL;
	}
	vmm_loader_fd_put(lfd);
}

static int
vmm_loader_fd_mmap_single(struct dev_mmap_single_args *ap)
{
	struct vmm_loader_fd *lfd;
	struct vm_object *object;
	vm_ooffset_t off;
	int error;

	error = devfs_get_cdevpriv(ap->a_fp, (void **)&lfd);
	if (error)
		return error;
	object = lfd->own_mut_object;
	if ((ap->a_fp->f_flag & FREVOKED) || object == NULL)
		return EINVAL;
	/*
	 * DragonFly rejects MAP_PRIVATE/MAP_COPY for non-/dev/zero VCHR
	 * mappings in fp_mmap() before this d_mmap_single hook is called.
	 * This layer only needs to enforce the vmm-specific executable and
	 * revoke checks.
	 */
	if (ap->a_nprot & VM_PROT_EXECUTE)
		return EACCES;
	off = *ap->a_offset;
	if (off < 0 || off > lfd->imm_size || ap->a_size > lfd->imm_size - off)
		return EINVAL;

	VM_OBJECT_LOCK(object);
	if (lfd->mut_revoked || lfd->own_mut_backing_object == NULL) {
		VM_OBJECT_UNLOCK(object);
		return EINVAL;
	}
	vm_object_reference_locked(object);
	VM_OBJECT_UNLOCK(object);
	if (ap->a_maxprotp != NULL)
		*ap->a_maxprotp &= ~VM_PROT_EXECUTE;
	*ap->a_object = object;
	return 0;
}

static int
vmm_loader_make_vnode(cdev_t dev, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	error = getspecialvnode(VT_NON, NULL, &vmm_loader_vnode_vops_p, &vp,
	    0, 0);
	if (error) {
		*vpp = NULL;
		return error;
	}
	vp->v_type = VCHR;
	error = v_associate_rdev(vp, dev);
	if (error) {
		vx_unlock(vp);
		vrele(vp);
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
vmm_loader_open_fd(struct vmm_loader_fd *lfd, struct file **fpp)
{
	struct vnode *vp;
	struct file *fp;
	int error;

	error = vmm_loader_make_vnode(lfd->own_mut_dev, &vp);
	if (error)
		goto fail;
	error = falloc(NULL, &fp, NULL);
	if (error) {
		vrele(vp);
		goto fail;
	}
	/*
	 * This is an internal mmap capability, not a normal devfs path.  Build
	 * the file directly so the worker does not need to resolve a devfs
	 * path, and so pathless devfs vnodes do not go through VOP_ACCESS.
	 */
	fsetcred(fp, proc0.p_ucred);
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &vmm_loader_fileops;
	fp->f_data = vp;
	vref(vp);
	atomic_add_int(&vp->v_opencount, 1);
	atomic_add_int(&vp->v_writecount, 1);
	vrele(vp);
	error = devfs_set_cdevpriv(fp, lfd, vmm_loader_fd_free);
	if (error) {
		fp_close(fp);
		vmm_loader_fd_free(lfd);
		return error;
	}
	*fpp = fp;
	return 0;

fail:
	vmm_loader_fd_free(lfd);
	return error;
}

static int
vmm_loader_open_object_fd(struct vm_object *object, vm_size_t size,
    struct file **fpp)
{
	struct vmm_loader_fd *lfd;
	uint32_t serial;

	if (object == NULL || size == 0)
		return EINVAL;

	lfd = kmalloc(sizeof(*lfd), M_TEMP, M_WAITOK | M_ZERO);
	vm_object_reference_quick(object);
	lfd->own_mut_backing_object = object;
	lfd->imm_size = round_page(size);
	lfd->atomic_mut_refs = 1;
	atomic_add_int(&vmm_loader_fd_active, 1);
	lfd->own_mut_object = cdev_pager_allocate(lfd, OBJT_MGTDEVICE,
	    &vmm_loader_pager_ops, lfd->imm_size,
	    VM_PROT_READ | VM_PROT_WRITE, 0, proc0.p_ucred);
	if (lfd->own_mut_object == NULL) {
		vmm_loader_fd_free(lfd);
		return EINVAL;
	}
	serial = atomic_fetchadd_int(&vmm_loader_fd_serial, 1);
	lfd->own_mut_dev = make_only_dev(&vmm_loader_object_fd_ops, serial, UID_ROOT,
	    GID_WHEEL, 0600, "vmmld%d", serial);
	if (lfd->own_mut_dev == NULL) {
		vmm_loader_fd_free(lfd);
		return ENXIO;
	}
	lfd->own_mut_dev->si_drv1 = lfd;
	return vmm_loader_open_fd(lfd, fpp);
}

static int
vmm_loader_manifest_object(struct vm_object **objectp)
{
	struct vm_object *object;
	vm_page_t pg;

	object = vm_object_allocate(OBJT_DEFAULT, OFF_TO_IDX(VMM_MANIFEST_SIZE));
	if (object == NULL)
		return ENOMEM;
	vm_object_set_flag(object, OBJ_NOSPLIT);
	vm_object_hold(object);
	pg = vm_page_grab(object, 0, VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM |
	    VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	if (pg == NULL) {
		vm_object_drop(object);
		vm_object_deallocate(object);
		return ENOMEM;
	}
	vm_page_wakeup(pg);
	vm_object_drop(object);
	*objectp = object;
	return 0;
}

static int
vmm_loader_make_args(const char *path, struct image_args *args)
{
	char *buf;
	size_t len;
	int error;

	bzero(args, sizeof(*args));
	buf = kmalloc(ARG_MAX + PATH_MAX, M_TEMP, M_WAITOK | M_ZERO);
	args->buf = buf;
	args->begin_argv = buf;
	args->endp = buf;
	args->space = ARG_MAX;
	args->fname = buf + ARG_MAX;

	error = copystr(path, args->fname, PATH_MAX, &len);
	if (error)
		goto fail;
	if (len > (size_t)args->space) {
		error = E2BIG;
		goto fail;
	}
	bcopy(args->fname, args->endp, len);
	args->endp += len;
	args->space -= (int)len;
	args->argc = 1;
	args->begin_envv = args->endp;
	args->envc = 0;
	return 0;

fail:
	kfree(args->buf, M_TEMP);
	args->buf = NULL;
	return error;
}

static void
vmm_loader_free_args(struct image_args *args)
{
	if (args->buf != NULL) {
		kfree(args->buf, M_TEMP);
		args->buf = NULL;
	}
}

static int
vmm_loader_install_fd(struct file *fp, int target_fd)
{
	int fd, error;

	error = kern_close(target_fd);
	if (error != 0 && error != EBADF)
		return error;
	error = fdalloc(curproc, target_fd, &fd);
	if (error)
		return error;
	if (fd != target_fd) {
		fsetfd(curproc->p_fd, NULL, fd);
		return EBUSY;
	}
	/*
	 * fsetfd() takes a descriptor-table reference with fhold().  The
	 * loader object keeps its original reference so fdrevoke() can close
	 * loader descriptors without freeing fp before vmm_loader_close_fds().
	 */
	fsetfd(curproc->p_fd, fp, target_fd);
	return 0;
}

static void
vmm_loader_revoke_fp(struct file *fp)
{
	struct vmm_loader_fd *lfd;

	if (fp != NULL && fp->f_type == DTYPE_VNODE && fp->f_data != NULL) {
		if (devfs_get_cdevpriv(fp, (void **)&lfd) == 0)
			vmm_loader_fd_revoke(lfd);
		(void)fdrevoke(fp->f_data, DTYPE_VNODE, proc0.p_ucred);
	}
}

static void
vmm_loader_revoke(struct vmm_loader *loader)
{
	vmm_loader_revoke_fp(loader->own_mut_mem_fp);
	vmm_loader_revoke_fp(loader->own_mut_manifest_fp);
}

static void
vmm_loader_close_fds(struct vmm_loader *loader)
{
	/*
	 * The loader process may have forked or dup'd these file pointers.
	 * Revoke already invalidated the capability; here we only drop
	 * the worker's references so cdevpriv remains valid until final close.
	 */
	if (loader->own_mut_manifest_fp != NULL) {
		fp_close(loader->own_mut_manifest_fp);
		loader->own_mut_manifest_fp = NULL;
	}
	if (loader->own_mut_mem_fp != NULL) {
		fp_close(loader->own_mut_mem_fp);
		loader->own_mut_mem_fp = NULL;
	}
}

int
vmm_loader_manifest_load(struct vmm_loader *loader, struct vmm_launch *launch)
{
	vm_page_t pg;
	void *data;
	int error;

	if (loader == NULL || launch == NULL ||
	    loader->own_mut_manifest_object == NULL)
		return EINVAL;
	vm_object_hold(loader->own_mut_manifest_object);
	pg = vm_page_lookup_busy_wait(loader->own_mut_manifest_object, 0, FALSE,
	    "vmmmf");
	if (pg == NULL) {
		vm_object_drop(loader->own_mut_manifest_object);
		return ENOEXEC;
	}
	data = (void *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(pg));
	error = vmm_loader_x86_manifest_load(loader->imm_mem_size, data,
	    VMM_MANIFEST_SIZE, launch);
	vm_page_wakeup(pg);
	vm_object_drop(loader->own_mut_manifest_object);
	return error;
}

static void
vmm_loader_child(void *arg, struct trapframe *frame)
{
	struct vmm_loader *loader = arg;
	struct nlookupdata nd;
	struct image_args args;
	int error;
	int state;

	(void)frame;
	/*
	 * State is the condition, wakeup is only the notification.  Publish
	 * PAUSED before sleeping so vmm_loader_init() can return only after
	 * the child is ready for a later resume.
	 */
	if (!atomic_cmpset_int(&loader->atomic_mut_state, VMM_LOADER_INITING,
	    VMM_LOADER_PAUSED))
		exit1(W_EXITCODE(127, SIGKILL));
	wakeup(&loader->own_handler);
	for (;;) {
		state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (state == VMM_LOADER_RUNNING)
			break;
		if (state == VMM_LOADER_OK || state == VMM_LOADER_FAILED)
			exit1(W_EXITCODE(127, SIGKILL));
		tsleep_interlock(loader, PCATCH);
		state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (state == VMM_LOADER_RUNNING ||
		    state == VMM_LOADER_OK || state == VMM_LOADER_FAILED)
			continue;
		error = tsleep(loader, PINTERLOCKED | PCATCH, "vmmldp", 0);
		if (error)
			exit1(W_EXITCODE(127, SIGKILL));
	}

	/*
	 * wakeup(loader) is the resume notification for this paused child.  The
	 * machine task only calls it after vmm_loader_install() has prepared
	 * fd3/fd4, so no separate "installed" state is needed here.
	 */
	error = vmm_loader_install_fd(loader->own_mut_mem_fp, 3);
	if (error == 0)
		error = vmm_loader_install_fd(loader->own_mut_manifest_fp, 4);
	if (error == 0)
		error = vmm_loader_make_args(loader->imm_path, &args);
	if (error == 0) {
		error = nlookup_init(&nd, loader->imm_path, UIO_SYSSPACE,
		    NLC_FOLLOW);
		if (error == 0) {
			error = kern_execve(&nd, NULL, 0, &args);
			nlookup_done(&nd);
		}
		vmm_loader_free_args(&args);
	}

	if (error < 0)
		exit1(W_EXITCODE(127, SIGABRT));
	if (error != 0)
		exit1(W_EXITCODE(127, 0));
	/* kern_execve() succeeded; return through fork_trampoline to userland. */
}

static void
vmm_loader_set_proc_cred(struct proc *p, struct ucred *cred)
{
	struct lwp *lwp;
	struct ucred *old;

	old = p->p_ucred;
	p->p_ucred = crhold(cred);
	crfree(old);

	lwp = ONLY_LWP_IN_PROC(p);
	old = lwp->lwp_thread->td_ucred;
	lwp->lwp_thread->td_ucred = crhold(cred);
	crfree(old);
}

static void
vmm_loader_exit_cb(void *arg, int exit_code)
{
	struct vmm_loader *loader = arg;
	int old, state;

	state = (WIFEXITED(exit_code) && WEXITSTATUS(exit_code) == 0) ?
	    VMM_LOADER_OK : VMM_LOADER_FAILED;
	for (;;) {
		old = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (old == VMM_LOADER_OK || old == VMM_LOADER_FAILED)
			return;
		if (atomic_cmpset_int(&loader->atomic_mut_state, old, state))
			return;
	}
}

static void
vmm_loader_kill(struct vmm_loader *loader)
{
	struct proc *p;

	if (loader == NULL)
		return;
	if (loader->imm_pid > 0) {
		p = pfind(loader->imm_pid);
		if (p != NULL) {
			ksignal(p, SIGKILL);
			PRELE(p);
		}
	}
	/* wakeup(loader) is the paused-child resume/kill channel. */
	wakeup(loader);
	vmm_loader_revoke(loader);
}

int
vmm_loader_init(struct vmm_loader *loader, const char *path,
    struct ucred *cred)
{
	struct proc *child;
	struct lwp *child_lwp;
	int error;
	int state;

	if (loader == NULL)
		return EINVAL;
	bzero(loader, sizeof(*loader));
	if (path == NULL || path[0] == '\0' || cred == NULL)
		return EINVAL;
	loader->imm_path = path;
	loader->atomic_mut_state = VMM_LOADER_INITING;
	error = fork1(curthread->td_lwp,
	    RFFDG | RFPROC | RFPGLOCK | RFNOWAIT, &child);
	if (error)
		return error;

	loader->imm_pid = child->p_pid;
	loader->own_handler.imm_pid = loader->imm_pid;
	loader->own_handler.fnonce_exit_cb = vmm_loader_exit_cb;
	loader->own_handler.borrow_mut_arg = loader;
	loader->own_handler.optional_borrow_wait_chan = &loader->own_handler;

	error = vmm_domain_proc_register(&loader->own_handler);
	if (error) {
		ksignal(child, SIGKILL);
		wakeup(loader);
		loader->imm_pid = 0;
		return error;
	}

	child_lwp = ONLY_LWP_IN_PROC(child);
	vmm_loader_set_proc_cred(child, cred);
	cpu_set_fork_handler(child_lwp, vmm_loader_child, loader);
	PHOLD(child);
	start_forked_proc(curthread->td_lwp, child);
	PRELE(child);
	for (;;) {
		state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (state == VMM_LOADER_PAUSED)
			return 0;
		if (state == VMM_LOADER_OK || state == VMM_LOADER_FAILED) {
			error = ENOEXEC;
			break;
		}
		tsleep_interlock(&loader->own_handler, 0);
		state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (state == VMM_LOADER_PAUSED ||
		    state == VMM_LOADER_OK || state == VMM_LOADER_FAILED)
			continue;
		error = tsleep(&loader->own_handler, PINTERLOCKED, "vmmldi",
		    hz * 10);
		if (error == EWOULDBLOCK) {
			vmm_loader_kill(loader);
			for (;;) {
				state = atomic_fetchadd_int(
				    &loader->atomic_mut_state, 0);
				if (state == VMM_LOADER_OK ||
				    state == VMM_LOADER_FAILED)
					break;
				tsleep_interlock(&loader->own_handler, 0);
				state = atomic_fetchadd_int(
				    &loader->atomic_mut_state, 0);
				if (state == VMM_LOADER_OK ||
				    state == VMM_LOADER_FAILED)
					continue;
				(void)tsleep(&loader->own_handler, PINTERLOCKED,
				    "vmmldix", hz * 10);
				vmm_loader_kill(loader);
			}
			error = ENOEXEC;
			break;
		}
	}
	vmm_domain_proc_unregister(&loader->own_handler);
	loader->imm_pid = 0;
	return error;
}

int
vmm_loader_install(struct vmm_loader *loader, struct vm_object *mem_object,
    uint64_t mem_size)
{
	int error;

	if (loader == NULL || mem_object == NULL || mem_size == 0)
		return EINVAL;
	if (atomic_fetchadd_int(&loader->atomic_mut_state, 0) !=
	    VMM_LOADER_PAUSED)
		return ECANCELED;

	loader->imm_mem_size = mem_size;
	error = vmm_loader_open_object_fd(mem_object,
	    (vm_size_t)loader->imm_mem_size, &loader->own_mut_mem_fp);
	if (error)
		return error;
	error = vmm_loader_manifest_object(&loader->own_mut_manifest_object);
	if (error)
		return error;
	error = vmm_loader_open_object_fd(loader->own_mut_manifest_object,
	    VMM_MANIFEST_SIZE, &loader->own_mut_manifest_fp);
	return error;
}

int
vmm_loader_resume(struct vmm_loader *loader)
{
	if (loader == NULL)
		return EINVAL;
	if (!atomic_cmpset_int(&loader->atomic_mut_state, VMM_LOADER_PAUSED,
	    VMM_LOADER_RUNNING))
		return ECANCELED;
	wakeup(loader);
	return 0;
}

int
vmm_loader_wait(struct vmm_loader *loader)
{
	int error;
	int state;

	if (loader == NULL)
		return EINVAL;
	for (;;) {
		state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (state == VMM_LOADER_OK)
			return 0;
		if (state != VMM_LOADER_RUNNING)
			return ENOEXEC;
		tsleep_interlock(&loader->own_handler, 0);
		state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (state != VMM_LOADER_RUNNING)
			continue;
		error = tsleep(&loader->own_handler, PINTERLOCKED, "vmmld",
		    hz * 10);
		if (error == EWOULDBLOCK) {
			vmm_loader_kill(loader);
			return ENOEXEC;
		}
	}
}

void
vmm_loader_fini(struct vmm_loader *loader)
{
	int error;
	int state;

	if (loader == NULL)
		return;

	state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
	if (state != VMM_LOADER_OK && state != VMM_LOADER_FAILED)
		vmm_loader_kill(loader);
	for (;;) {
		state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (state == VMM_LOADER_OK || state == VMM_LOADER_FAILED)
			break;
		tsleep_interlock(&loader->own_handler, 0);
		state = atomic_fetchadd_int(&loader->atomic_mut_state, 0);
		if (state == VMM_LOADER_OK || state == VMM_LOADER_FAILED)
			continue;
		error = tsleep(&loader->own_handler, PINTERLOCKED, "vmmldx",
		    hz * 10);
		if (error == EWOULDBLOCK)
			vmm_loader_kill(loader);
	}
	vmm_domain_proc_unregister(&loader->own_handler);
	vmm_loader_revoke(loader);
	vmm_loader_close_fds(loader);
	if (loader->own_mut_manifest_object != NULL) {
		vm_object_deallocate(loader->own_mut_manifest_object);
		loader->own_mut_manifest_object = NULL;
	}
	loader->imm_pid = 0;
}
