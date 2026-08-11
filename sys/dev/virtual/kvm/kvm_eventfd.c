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

#include <linux/kvm.h>

#include "kvm_eventfd.h"
#include "kvm_internal.h"

struct kvm_eventfd {
	/* token protects count, read_kq, and listeners. */
	struct lwkt_token token;
	struct kqinfo read_kq;
	TAILQ_HEAD(, kvm_eventfd_listener) listeners;
	uint64_t count;
	uint64_t signal_generation;
};

static d_priv_dtor_t kvm_eventfd_destroy;
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
static void kvm_eventfd_release(struct kvm_eventfd *);
static int kvm_eventfd_add(struct kvm_eventfd *, uint64_t, int);
static int kvm_eventfd_from_file(struct file *, struct kvm_eventfd **);

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
kvm_eventfd_create(struct lwp *lp, struct vnode *vp, uint64_t initial,
	uint32_t flags, int *fd)
{
	struct kvm_eventfd *eventfd;
	struct file *fp;
	int error;

	if (lp == NULL || vp == NULL || fd == NULL || initial == UINT64_MAX ||
	    (flags & ~KVM_DFLY_EVENTFD_VALID_FLAGS) != 0)
		return EINVAL;
	*fd = -1;
	eventfd = kmalloc(sizeof(*eventfd), M_KVM, M_WAITOK | M_ZERO);
	lwkt_token_init(&eventfd->token, "kvmevent");
	SLIST_INIT(&eventfd->read_kq.ki_note);
	TAILQ_INIT(&eventfd->listeners);
	eventfd->count = initial;

	lwkt_gettoken(&kvm_frontend_token);
	if (kvm_draining) {
		lwkt_reltoken(&kvm_frontend_token);
		kfree(eventfd, M_KVM);
		return EBUSY;
	}
	++kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);

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
	return kvm_eventfd_add(eventfd, value, (fp->f_flag & FNONBLOCK) != 0);
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
		vrele(vp);
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

static void
kvm_eventfd_destroy(void *arg)
{

	kvm_eventfd_release(arg);
}

static void
kvm_eventfd_release(struct kvm_eventfd *eventfd)
{
	if (eventfd == NULL)
		return;
	KKASSERT(TAILQ_EMPTY(&eventfd->listeners));
	lwkt_gettoken(&kvm_frontend_token);
	KKASSERT(kvm_file_count != 0);
	--kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);
	kfree(eventfd, M_KVM);
}

int
kvm_eventfd_hold(struct thread *td, int fd, struct file **fpp)
{
	struct file *fp;
	struct kvm_eventfd *eventfd;

	if (td == NULL || fpp == NULL || fd < 0)
		return EINVAL;
	*fpp = NULL;
	fp = holdfp(td, fd, -1);
	if (fp == NULL)
		return EBADF;
	if (kvm_eventfd_from_file(fp, &eventfd) != 0) {
		fdrop(fp);
		return EINVAL;
	}
	*fpp = fp;
	return 0;
}

void
kvm_eventfd_drop(struct file *fp)
{

	if (fp != NULL)
		fdrop(fp);
}

int
kvm_eventfd_signal(struct file *fp)
{
	struct kvm_eventfd *eventfd;
	int error;

	error = kvm_eventfd_from_file(fp, &eventfd);
	if (error != 0)
		return error;
	error = kvm_eventfd_add(eventfd, 1, 1);
	return error == EWOULDBLOCK ? 0 : error;
}

int
kvm_eventfd_listen(struct file *fp, struct kvm_eventfd_listener *listener)
{
	struct kvm_eventfd *eventfd;
	int error;

	if (listener == NULL || listener->callback == NULL)
		return EINVAL;
	error = kvm_eventfd_from_file(fp, &eventfd);
	if (error != 0)
		return error;
	lwkt_gettoken(&eventfd->token);
	if (listener->active) {
		lwkt_reltoken(&eventfd->token);
		return EBUSY;
	}
	listener->last_generation = eventfd->signal_generation;
	listener->references = 0;
	listener->active = 1;
	listener->eventfd = eventfd;
	TAILQ_INSERT_TAIL(&eventfd->listeners, listener, entry);
	lwkt_reltoken(&eventfd->token);
	return 0;
}

int
kvm_eventfd_unlisten(struct file *fp, struct kvm_eventfd_listener *listener)
{
	struct kvm_eventfd *eventfd;
	int error;

	if (listener == NULL)
		return EINVAL;
	error = kvm_eventfd_from_file(fp, &eventfd);
	if (error != 0)
		return error;
	lwkt_gettoken(&eventfd->token);
	if (!listener->active || listener->eventfd != eventfd) {
		lwkt_reltoken(&eventfd->token);
		return ENOENT;
	}
	TAILQ_REMOVE(&eventfd->listeners, listener, entry);
	listener->active = 0;
	listener->eventfd = NULL;
	lwkt_reltoken(&eventfd->token);
	while (atomic_load_acq_int(&listener->references) != 0)
		tsleep(listener, 0, "kvmirqfd", 0);
	return 0;
}

static int
kvm_eventfd_add(struct kvm_eventfd *eventfd, uint64_t value, int nonblock)
{
	struct kvm_eventfd_listener *listener;
	uint64_t generation;
	int error;

	for (;;) {
		lwkt_gettoken(&eventfd->token);
		if (eventfd->count <= UINT64_MAX - 1 - value) {
			eventfd->count += value;
			++eventfd->signal_generation;
			if (eventfd->signal_generation == 0)
				++eventfd->signal_generation;
			generation = eventfd->signal_generation;
			KNOTE(&eventfd->read_kq.ki_note, 0);
			wakeup(eventfd);
			lwkt_reltoken(&eventfd->token);
			break;
		}
		if (nonblock) {
			lwkt_reltoken(&eventfd->token);
			return EWOULDBLOCK;
		}
		error = tsleep(eventfd, PCATCH, "kvmevent", 0);
		lwkt_reltoken(&eventfd->token);
		if (error != 0)
			return error;
	}
	for (;;) {
		listener = NULL;
		lwkt_gettoken(&eventfd->token);
		TAILQ_FOREACH(listener, &eventfd->listeners, entry) {
			if (listener->active &&
			    listener->last_generation < generation) {
				listener->last_generation = generation;
				atomic_add_int(&listener->references, 1);
				break;
			}
		}
		lwkt_reltoken(&eventfd->token);
		if (listener == NULL)
			break;
		listener->callback(listener->argument);
		if (atomic_fetchadd_int(&listener->references, -1) == 1)
			wakeup(listener);
	}
	return 0;
}

static int
kvm_eventfd_from_file(struct file *fp, struct kvm_eventfd **eventfdp)
{
	struct kvm_eventfd *eventfd;
	int error;

	if (fp == NULL || eventfdp == NULL || fp->f_ops != &kvm_eventfd_fileops)
		return EINVAL;
	error = devfs_get_cdevpriv(fp, (void **)&eventfd);
	if (error != 0 || eventfd == NULL)
		return error != 0 ? error : ENXIO;
	*eventfdp = eventfd;
	return 0;
}
