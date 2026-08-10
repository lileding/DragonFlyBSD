/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Anonymous eventfd-compatible counter for KVM ioeventfd and irqfd bindings.
 */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/fcntl.h>
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

#include <sys/kvm.h>

#include "kvm_eventfd.h"
#include "kvm_internal.h"

struct kvm_eventfd {
	/* token protects count and read_kq. */
	struct lwkt_token token;
	struct kqinfo read_kq;
	uint64_t count;
	cdev_t dev;
	struct vnode *vnode;
};

static uint32_t kvm_eventfd_serial;

static d_open_t kvm_eventfd_open;
static d_close_t kvm_eventfd_close;
static d_priv_dtor_t kvm_eventfd_destroy;
static int kvm_eventfd_vop_getattr(struct vop_getattr_args *);
static int kvm_eventfd_fo_read(struct file *, struct uio *,
	struct ucred *, int);
static int kvm_eventfd_fo_write(struct file *, struct uio *,
	struct ucred *, int);
static int kvm_eventfd_fo_ioctl(struct file *, u_long, caddr_t,
	struct ucred *, struct sysmsg *);
static int kvm_eventfd_fo_kqfilter(struct file *, struct knote *);
static int kvm_eventfd_fo_stat(struct file *, struct stat *,
	struct ucred *);
static int kvm_eventfd_fo_close(struct file *);
static int kvm_eventfd_fo_seek(struct file *, off_t, int, off_t *);
static void kvm_eventfd_filter_detach(struct knote *);
static int kvm_eventfd_filter_read(struct knote *, long);
static int kvm_eventfd_make_vnode(cdev_t, struct vnode **);
static void kvm_eventfd_release(struct kvm_eventfd *);

static struct dev_ops kvm_eventfd_ops = {
	{ "kvm_eventfd", 0, D_MPSAFE },
	.d_open = kvm_eventfd_open,
	.d_close = kvm_eventfd_close,
};

static struct vop_ops kvm_eventfd_vnode_vops = {
	.vop_default = vop_defaultop,
	.vop_close = vop_stdclose,
	.vop_getattr = kvm_eventfd_vop_getattr,
	.vop_advlock = (void *)vop_null,
	.vop_inactive = (void *)vop_null,
	.vop_reclaim = (void *)vop_null,
	.vop_pathconf = vop_stdpathconf,
};

static struct vop_ops *kvm_eventfd_vnode_vops_p = &kvm_eventfd_vnode_vops;

static struct fileops kvm_eventfd_fileops = {
	.fo_read = kvm_eventfd_fo_read,
	.fo_write = kvm_eventfd_fo_write,
	.fo_ioctl = kvm_eventfd_fo_ioctl,
	.fo_kqfilter = kvm_eventfd_fo_kqfilter,
	.fo_stat = kvm_eventfd_fo_stat,
	.fo_close = kvm_eventfd_fo_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = kvm_eventfd_fo_seek,
};

static struct filterops kvm_eventfd_read_filterops = {
	FILTEROP_ISFD | FILTEROP_MPSAFE,
	NULL,
	kvm_eventfd_filter_detach,
	kvm_eventfd_filter_read,
};

int
kvm_eventfd_create(struct lwp *lp, uint64_t initial, uint32_t flags, int *fd)
{
	struct kvm_eventfd *eventfd;
	struct file *fp;
	struct vnode *vp;
	uint32_t serial;
	int error;

	if (lp == NULL || fd == NULL || initial == UINT64_MAX ||
	    (flags & ~KVM_DFLY_EVENTFD_VALID_FLAGS) != 0)
		return EINVAL;
	*fd = -1;
	eventfd = kmalloc(sizeof(*eventfd), M_KVM, M_WAITOK | M_ZERO);
	lwkt_token_init(&eventfd->token, "kvmevent");
	SLIST_INIT(&eventfd->read_kq.ki_note);
	eventfd->count = initial;

	lwkt_gettoken(&kvm_frontend_token);
	if (kvm_draining) {
		lwkt_reltoken(&kvm_frontend_token);
		kfree(eventfd, M_KVM);
		return EBUSY;
	}
	++kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);

	serial = atomic_fetchadd_int(&kvm_eventfd_serial, 1);
	eventfd->dev = make_only_dev(&kvm_eventfd_ops, serial, UID_ROOT,
	    GID_WHEEL, 0600, "kvmevent%d", serial);
	if (eventfd->dev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	eventfd->dev->si_drv1 = eventfd;
	error = kvm_eventfd_make_vnode(eventfd->dev, &vp);
	if (error != 0)
		goto fail;
	eventfd->vnode = vp;

	error = falloc(lp, &fp, fd);
	if (error != 0)
		goto fail;
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	if ((flags & KVM_DFLY_EVENTFD_NONBLOCK) != 0)
		fp->f_flag |= FNONBLOCK;
	fp->f_ops = &kvm_eventfd_fileops;
	fp->f_data = vp;
	vref(vp);
	atomic_add_int(&vp->v_opencount, 1);
	atomic_add_int(&vp->v_writecount, 1);
	error = devfs_set_cdevpriv(fp, eventfd, kvm_eventfd_destroy);
	if (error != 0) {
		(void)fp_close(fp);
		fsetfd(lp->lwp_proc->p_fd, NULL, *fd);
		fdrop(fp);
		goto fail;
	}
	fsetfd(lp->lwp_proc->p_fd, fp, *fd);
	if ((flags & KVM_DFLY_EVENTFD_CLOEXEC) != 0) {
		error = faddfdflags(lp->lwp_proc->p_fd, *fd, UF_EXCLOSE);
		KKASSERT(error == 0);
	}
	fdrop(fp);
	return 0;

fail:
	kvm_eventfd_release(eventfd);
	return error;
}

static int
kvm_eventfd_open(struct dev_open_args *ap)
{

	(void)ap;
	return 0;
}

static int
kvm_eventfd_close(struct dev_close_args *ap)
{

	(void)ap;
	return 0;
}

static int
kvm_eventfd_vop_getattr(struct vop_getattr_args *ap)
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
kvm_eventfd_fo_read(struct file *fp, struct uio *uio, struct ucred *cred,
	int flags)
{
	struct kvm_eventfd *eventfd;
	uint64_t value;
	int error;

	(void)cred;
	(void)flags;
	if (uio->uio_resid != sizeof(value))
		return EINVAL;
	error = devfs_get_cdevpriv(fp, (void **)&eventfd);
	if (error != 0)
		return error;
	for (;;) {
		lwkt_gettoken(&eventfd->token);
		if (eventfd->count != 0) {
			value = eventfd->count;
			eventfd->count = 0;
			wakeup(eventfd);
			lwkt_reltoken(&eventfd->token);
			return uiomove((caddr_t)&value, sizeof(value), uio);
		}
		if ((fp->f_flag & FNONBLOCK) != 0) {
			lwkt_reltoken(&eventfd->token);
			return EWOULDBLOCK;
		}
		error = tsleep(eventfd, PCATCH, "kvmevent", 0);
		lwkt_reltoken(&eventfd->token);
		if (error != 0)
			return error;
	}
}

static int
kvm_eventfd_fo_write(struct file *fp, struct uio *uio, struct ucred *cred,
	int flags)
{
	struct kvm_eventfd *eventfd;
	uint64_t value;
	int error;

	(void)cred;
	(void)flags;
	if (uio->uio_resid != sizeof(value))
		return EINVAL;
	error = uiomove((caddr_t)&value, sizeof(value), uio);
	if (error != 0)
		return error;
	if (value == 0 || value == UINT64_MAX)
		return EINVAL;
	error = devfs_get_cdevpriv(fp, (void **)&eventfd);
	if (error != 0)
		return error;
	for (;;) {
		lwkt_gettoken(&eventfd->token);
		if (eventfd->count <= UINT64_MAX - 1 - value) {
			eventfd->count += value;
			KNOTE(&eventfd->read_kq.ki_note, 0);
			wakeup(eventfd);
			lwkt_reltoken(&eventfd->token);
			return 0;
		}
		if ((fp->f_flag & FNONBLOCK) != 0) {
			lwkt_reltoken(&eventfd->token);
			return EWOULDBLOCK;
		}
		error = tsleep(eventfd, PCATCH, "kvmevent", 0);
		lwkt_reltoken(&eventfd->token);
		if (error != 0)
			return error;
	}
}

static int
kvm_eventfd_fo_ioctl(struct file *fp, u_long command, caddr_t data,
	struct ucred *cred, struct sysmsg *msg)
{

	(void)fp;
	(void)command;
	(void)data;
	(void)cred;
	(void)msg;
	return EOPNOTSUPP;
}

static int
kvm_eventfd_fo_kqfilter(struct file *fp, struct knote *kn)
{
	struct kvm_eventfd *eventfd;
	int error;

	if (kn->kn_filter != EVFILT_READ)
		return EOPNOTSUPP;
	error = devfs_get_cdevpriv(fp, (void **)&eventfd);
	if (error != 0)
		return error;
	lwkt_gettoken(&eventfd->token);
	kn->kn_fop = &kvm_eventfd_read_filterops;
	kn->kn_hook = (caddr_t)eventfd;
	knote_insert(&eventfd->read_kq.ki_note, kn);
	lwkt_reltoken(&eventfd->token);
	return 0;
}

static int
kvm_eventfd_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
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
kvm_eventfd_fo_close(struct file *fp)
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
kvm_eventfd_fo_seek(struct file *fp, off_t offset, int whence, off_t *result)
{

	(void)fp;
	(void)offset;
	(void)whence;
	(void)result;
	return ESPIPE;
}

static void
kvm_eventfd_filter_detach(struct knote *kn)
{
	struct kvm_eventfd *eventfd;

	eventfd = (struct kvm_eventfd *)kn->kn_hook;
	lwkt_gettoken(&eventfd->token);
	knote_remove(&eventfd->read_kq.ki_note, kn);
	lwkt_reltoken(&eventfd->token);
}

static int
kvm_eventfd_filter_read(struct knote *kn, long hint)
{
	struct kvm_eventfd *eventfd;
	int ready;

	(void)hint;
	eventfd = (struct kvm_eventfd *)kn->kn_hook;
	lwkt_gettoken(&eventfd->token);
	kn->kn_data = eventfd->count;
	ready = eventfd->count != 0;
	lwkt_reltoken(&eventfd->token);
	return ready;
}

static int
kvm_eventfd_make_vnode(cdev_t dev, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	error = getspecialvnode(VT_NON, NULL, &kvm_eventfd_vnode_vops_p, &vp,
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

static void
kvm_eventfd_destroy(void *arg)
{

	kvm_eventfd_release(arg);
}

static void
kvm_eventfd_release(struct kvm_eventfd *eventfd)
{
	struct vnode *vp;

	if (eventfd == NULL)
		return;
	vp = eventfd->vnode;
	if (vp != NULL) {
		eventfd->vnode = NULL;
		vx_get(vp);
		vgone_vxlocked(vp);
		vx_put(vp);
		vrele(vp);
	}
	if (eventfd->dev != NULL) {
		eventfd->dev->si_drv1 = NULL;
		destroy_only_dev(eventfd->dev);
		eventfd->dev = NULL;
	}
	lwkt_gettoken(&kvm_frontend_token);
	KKASSERT(kvm_file_count != 0);
	--kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);
	kfree(eventfd, M_KVM);
}
