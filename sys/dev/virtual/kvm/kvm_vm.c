/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-VM file descriptor and guest vmspace for the DragonFly KVM frontend.
 */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/sysmsg.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include <vm/vm_extern.h>

#include <sys/kvm.h>

#include "../vmm/vmm.h"
#include "kvm_internal.h"
#include "kvm_vm.h"

#define KVM_GPA_MAX	((vm_offset_t)127 * 1024 * 1024 * 1024 * 1024)
#define KVM_LINUX_IO(number)	((unsigned long)((KVMIO << 8) | (number)))

struct kvm_vm {
	/* The file descriptor owns machine and vmspace until close. */
	vmm_machine_t machine;
	struct vmspace *vmspace;
	cdev_t dev;
	struct vnode *vnode;
};

static uint32_t kvm_vm_serial;

static d_open_t kvm_vm_open;
static d_close_t kvm_vm_close;
static d_priv_dtor_t kvm_vm_destroy;
static int kvm_vm_vop_getattr(struct vop_getattr_args *);
static int kvm_vm_fo_read(struct file *, struct uio *, struct ucred *, int);
static int kvm_vm_fo_write(struct file *, struct uio *, struct ucred *, int);
static int kvm_vm_fo_ioctl(struct file *, u_long, caddr_t,
	struct ucred *, struct sysmsg *);
static int kvm_vm_fo_kqfilter(struct file *, struct knote *);
static int kvm_vm_fo_stat(struct file *, struct stat *, struct ucred *);
static int kvm_vm_fo_close(struct file *);
static int kvm_vm_fo_seek(struct file *, off_t, int, off_t *);
static int kvm_vm_make_vnode(cdev_t, struct vnode **);
static void kvm_vm_release(struct kvm_vm *);

static struct dev_ops kvm_vm_ops = {
	{ "kvm_vm", 0, D_MPSAFE },
	.d_open = kvm_vm_open,
	.d_close = kvm_vm_close,
};

static struct vop_ops kvm_vm_vnode_vops = {
	.vop_default = vop_defaultop,
	.vop_close = vop_stdclose,
	.vop_getattr = kvm_vm_vop_getattr,
	.vop_advlock = (void *)vop_null,
	.vop_inactive = (void *)vop_null,
	.vop_reclaim = (void *)vop_null,
	.vop_pathconf = vop_stdpathconf,
};

static struct vop_ops *kvm_vm_vnode_vops_p = &kvm_vm_vnode_vops;

static struct fileops kvm_vm_fileops = {
	.fo_read = kvm_vm_fo_read,
	.fo_write = kvm_vm_fo_write,
	.fo_ioctl = kvm_vm_fo_ioctl,
	.fo_kqfilter = kvm_vm_fo_kqfilter,
	.fo_stat = kvm_vm_fo_stat,
	.fo_close = kvm_vm_fo_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = kvm_vm_fo_seek,
};

int
kvm_vm_create(struct lwp *lp, int *fd)
{
	struct kvm_vm *vm;
	struct file *fp;
	struct vnode *vp;
	uint32_t serial;
	int error;

	if (lp == NULL || fd == NULL)
		return EINVAL;
	*fd = -1;
	vm = kmalloc(sizeof(*vm), M_KVM, M_WAITOK | M_ZERO);

	lwkt_gettoken(&kvm_frontend_token);
	if (kvm_draining) {
		lwkt_reltoken(&kvm_frontend_token);
		kfree(vm, M_KVM);
		return EBUSY;
	}
	++kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);

	vm->vmspace = vmspace_alloc(VM_MIN_USER_ADDRESS, KVM_GPA_MAX);
	if (vm->vmspace == NULL) {
		error = ENOMEM;
		goto fail;
	}
	error = vmm_machine_create(vm->vmspace, &vm->machine);
	if (error != 0)
		goto fail;

	serial = atomic_fetchadd_int(&kvm_vm_serial, 1);
	vm->dev = make_only_dev(&kvm_vm_ops, serial, UID_ROOT, GID_WHEEL,
	    0600, "kvmvm%d", serial);
	if (vm->dev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	vm->dev->si_drv1 = vm;
	error = kvm_vm_make_vnode(vm->dev, &vp);
	if (error != 0)
		goto fail;
	vm->vnode = vp;

	error = falloc(lp, &fp, fd);
	if (error != 0)
		goto fail;
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &kvm_vm_fileops;
	fp->f_data = vp;
	vref(vp);
	atomic_add_int(&vp->v_opencount, 1);
	atomic_add_int(&vp->v_writecount, 1);
	error = devfs_set_cdevpriv(fp, vm, kvm_vm_destroy);
	if (error != 0) {
		(void)fp_close(fp);
		fsetfd(lp->lwp_proc->p_fd, NULL, *fd);
		fdrop(fp);
		goto fail;
	}
	fsetfd(lp->lwp_proc->p_fd, fp, *fd);
	fdrop(fp);
	return 0;

fail:
	kvm_vm_release(vm);
	return error;
}

static int
kvm_vm_open(struct dev_open_args *ap)
{

	(void)ap;
	return 0;
}

static int
kvm_vm_close(struct dev_close_args *ap)
{

	(void)ap;
	return 0;
}

static int
kvm_vm_vop_getattr(struct vop_getattr_args *ap)
{

	(void)ap->a_fp;
	bzero(ap->a_vap, sizeof(*ap->a_vap));
	ap->a_vap->va_type = VCHR;
	ap->a_vap->va_mode = 0600;
	ap->a_vap->va_uid = UID_ROOT;
	ap->a_vap->va_gid = GID_WHEEL;
	ap->a_vap->va_nlink = 1;
	ap->a_vap->va_blocksize = PAGE_SIZE;
	return 0;
}

static int
kvm_vm_fo_read(struct file *fp, struct uio *uio, struct ucred *cred,
	int flags)
{

	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
kvm_vm_fo_write(struct file *fp, struct uio *uio, struct ucred *cred,
	int flags)
{

	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
kvm_vm_fo_ioctl(struct file *fp, u_long command, caddr_t data,
	struct ucred *cred, struct sysmsg *msg)
{
	struct kvm_vm *vm;
	int error;

	(void)cred;
	(void)data;
	error = devfs_get_cdevpriv(fp, (void **)&vm);
	if (error != 0)
		return error;
	switch (command) {
	case KVM_GET_VCPU_MMAP_SIZE:
	case KVM_LINUX_IO(0x04):
		msg->sysmsg_result = PAGE_SIZE;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
kvm_vm_fo_kqfilter(struct file *fp, struct knote *kn)
{

	(void)fp;
	(void)kn;
	return EOPNOTSUPP;
}

static int
kvm_vm_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{

	(void)fp;
	(void)cred;
	bzero(sb, sizeof(*sb));
	sb->st_nlink = 1;
	sb->st_mode = S_IFCHR | 0600;
	sb->st_uid = UID_ROOT;
	sb->st_gid = GID_WHEEL;
	sb->st_blksize = PAGE_SIZE;
	sb->__old_st_blksize = sb->st_blksize;
	return 0;
}

static int
kvm_vm_fo_close(struct file *fp)
{
	struct vnode *vp;

	vp = fp->f_data;
	fp->f_data = NULL;
	atomic_clear_int(&fp->f_flag, FHASLOCK);
	fp->f_ops = &badfileops;
	if (vp != NULL)
		(void)vn_close(vp, fp->f_flag, fp);
	devfs_clear_cdevpriv(fp);
	return 0;
}

static int
kvm_vm_fo_seek(struct file *fp, off_t offset, int whence, off_t *result)
{

	(void)fp;
	(void)offset;
	(void)whence;
	(void)result;
	return ESPIPE;
}

static int
kvm_vm_make_vnode(cdev_t dev, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	error = getspecialvnode(VT_NON, NULL, &kvm_vm_vnode_vops_p, &vp, 0, 0);
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

static void
kvm_vm_destroy(void *arg)
{

	kvm_vm_release(arg);
}

static void
kvm_vm_release(struct kvm_vm *vm)
{
	struct vnode *vp;
	int error;

	if (vm == NULL)
		return;
	vp = vm->vnode;
	if (vp != NULL) {
		vm->vnode = NULL;
		vx_get(vp);
		vgone_vxlocked(vp);
		vx_put(vp);
		vrele(vp);
	}
	if (vm->dev != NULL) {
		vm->dev->si_drv1 = NULL;
		destroy_only_dev(vm->dev);
		vm->dev = NULL;
	}
	if (vm->machine != NULL) {
		error = vmm_machine_destroy(vm->machine);
		KKASSERT(error == 0);
		vm->machine = NULL;
	}
	if (vm->vmspace != NULL) {
		vmspace_rel(vm->vmspace);
		vm->vmspace = NULL;
	}
	lwkt_gettoken(&kvm_frontend_token);
	KKASSERT(kvm_file_count != 0);
	--kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);
	kfree(vm, M_KVM);
}
