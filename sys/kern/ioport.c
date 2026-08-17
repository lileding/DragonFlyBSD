/*
 * Copyright (c) 2026
 *
 * Native asynchronous I/O control plane: ioport(2) / ioevent(2).
 *
 * Phase 2: ioport(2) creates a completion port (a "pfd"): a per-port
 * taskqueue with nworkers worker LWKTs for executing legacy synchronous
 * fileops and deferred continuations, plus the kqueue EVFILT_READ hook that
 * will report a non-empty completion queue.  ioevent(2) submit/reap and the
 * completion queue itself arrive in Phase 3.
 *
 * See ioport-liburing-design.md for the design baseline.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/malloc.h>
#include <sys/file.h>
#include <sys/fcntl.h>
#include <sys/filedesc.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/event.h>
#include <sys/eventvar.h>
#include <sys/sysmsg.h>
#include <sys/sysproto.h>
#include <sys/taskqueue.h>
#include <sys/thread.h>
#include <sys/ioport.h>

static MALLOC_DEFINE(M_IOPORT, "ioport", "memory for ioport system");

/* Kernel-internal completion port object. */
struct ioport {
	struct taskqueue *ip_tq;	/* worker taskqueue */
	struct kqinfo	 ip_kq;		/* EVFILT_READ knotes */
	int		 ip_nworkers;	/* number of worker LWKTs */
};

static int ioport_read(struct file *fp, struct uio *uio,
		       struct ucred *cred, int flags);
static int ioport_write(struct file *fp, struct uio *uio,
			struct ucred *cred, int flags);
static int ioport_ioctl(struct file *fp, u_long com, caddr_t data,
			struct ucred *cred, struct sysmsg *msg);
static int ioport_kqfilter(struct file *fp, struct knote *kn);
static int ioport_stat(struct file *fp, struct stat *sb, struct ucred *cred);
static int ioport_close(struct file *fp);

static struct fileops ioportops = {
	.fo_read = ioport_read,
	.fo_write = ioport_write,
	.fo_ioctl = ioport_ioctl,
	.fo_kqfilter = ioport_kqfilter,
	.fo_stat = ioport_stat,
	.fo_close = ioport_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = badfo_seek
};

/* EVFILT_READ filter: fires when the completion queue is non-empty. */
static void filt_ioportdetach(struct knote *kn);
static int  filt_ioport(struct knote *kn, long hint);

static struct filterops ioport_read_filtops =
	{ FILTEROP_ISFD | FILTEROP_MPSAFE, NULL, filt_ioportdetach,
	  filt_ioport };

static int
ioport_read(struct file *fp, struct uio *uio, struct ucred *cred, int flags)
{
	return (ENXIO);
}

static int
ioport_write(struct file *fp, struct uio *uio, struct ucred *cred, int flags)
{
	return (ENXIO);
}

static int
ioport_ioctl(struct file *fp, u_long com, caddr_t data,
	     struct ucred *cred, struct sysmsg *msg)
{
	return (ENOTTY);
}

static int
ioport_kqfilter(struct file *fp, struct knote *kn)
{
	struct ioport *ip = (struct ioport *)fp->f_data;

	if (kn->kn_filter != EVFILT_READ)
		return (EOPNOTSUPP);

	kn->kn_fop = &ioport_read_filtops;
	kn->kn_hook = (caddr_t)ip;
	knote_insert(&ip->ip_kq.ki_note, kn);
	return (0);
}

static void
filt_ioportdetach(struct knote *kn)
{
	struct ioport *ip = (struct ioport *)kn->kn_hook;

	knote_remove(&ip->ip_kq.ki_note, kn);
}

static int
filt_ioport(struct knote *kn, long hint)
{
	/*
	 * Phase 3 will fill this from the completion queue count.  For now
	 * there are never any completions, so the filter never fires.
	 */
	kn->kn_data = 0;
	return (0);
}

static int
ioport_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{
	bzero((void *)sb, sizeof(*sb));
	sb->st_blksize = sizeof(struct io_completion);
	sb->st_mode = S_IFIFO;
	return (0);
}

static int
ioport_close(struct file *fp)
{
	struct ioport *ip = (struct ioport *)fp->f_data;

	fp->f_data = NULL;
	if (ip->ip_tq != NULL)
		taskqueue_free(ip->ip_tq);
	kfree(ip, M_IOPORT);
	return (0);
}

/*
 * ioport(2) -- create a completion port (pfd) with nworkers worker LWKTs.
 */
int
sys_ioport(struct sysmsg *sysmsg, const struct ioport_args *uap)
{
	struct thread *td = curthread;
	struct ioport *ip;
	struct file *fp;
	int fd, error;

	if (uap->nworkers == 0)
		return (EINVAL);

	error = falloc(td->td_lwp, &fp, &fd);
	if (error)
		return (error);

	ip = kmalloc(sizeof(*ip), M_IOPORT, M_WAITOK | M_ZERO);
	ip->ip_nworkers = (int)uap->nworkers;
	ip->ip_tq = taskqueue_create("ioport", M_WAITOK,
				     taskqueue_thread_enqueue, &ip->ip_tq);
	if (ip->ip_tq == NULL) {
		kfree(ip, M_IOPORT);
		fdrop(fp);
		return (ENOMEM);
	}
	taskqueue_start_threads(&ip->ip_tq, ip->ip_nworkers,
				TDPRI_KERN_DAEMON, -1, "ioport");

	fp->f_flag = FREAD | FWRITE;
	fp->f_type = DTYPE_IOPORT;
	fp->f_ops = &ioportops;
	fp->f_data = ip;

	fsetfd(td->td_proc->p_fd, fp, fd);
	sysmsg->sysmsg_result = fd;
	fdrop(fp);
	return (0);
}

/*
 * ioevent(2) -- submit a batch of requests and/or reap completions.
 * Phase 3.
 */
int
sys_ioevent(struct sysmsg *sysmsg, const struct ioevent_args *uap)
{
	return (ENOSYS);
}
