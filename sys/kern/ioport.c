/*
 * Copyright (c) 2026
 *
 * Native asynchronous I/O control plane: ioport(2) / ioevent(2).
 *
 * Phase 3: the io_req continuation model, the completion queue, and the
 * ioevent(2) submit/reap path.  IO_CLOSE is the first implemented opcode; it
 * runs on the port worker taskqueue via ioport_exec() and posts exactly one
 * completion.
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
#include <sys/lock.h>
#include <sys/queue.h>
#include <sys/sysmsg.h>
#include <sys/sysproto.h>
#include <sys/taskqueue.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sys/kern_syscall.h>
#include <sys/ioport.h>

static MALLOC_DEFINE(M_IOPORT, "ioport", "memory for ioport system");

#define IO_FRAME_MAX	8

struct io_req;
struct ioport;

/* One continuation frame in the io_req reverse-completion chain. */
struct io_frame {
	void	(*fr_complete)(struct io_req *, void *);
	void	*fr_context;
};

/*
 * A single accepted request.  One request yields exactly one completion.
 */
struct io_req {
	struct ioport	*req_ioport;	/* owning port */
	uint64_t	 req_tag;	/* caller tag */
	int		 req_opcode;	/* IO_* */
	int		 req_error;	/* final errno */
	int64_t		 req_result;	/* final result */
	struct file	*req_fp;	/* held target file, or NULL */
	struct proc	*req_proc;	/* held submitting proc */
	struct ucred	*req_cred;	/* held credentials */
	int		 req_fd;	/* target fd */
	struct io_frame	 req_frames[IO_FRAME_MAX];
	int		 req_nframes;
	struct task	 req_task;	/* worker task for ioport_exec */
	void		(*req_fn)(struct io_req *);
	STAILQ_ENTRY(io_req) req_cq_link;
};

/* Kernel-internal completion port. */
struct ioport {
	struct taskqueue *ip_tq;	/* worker taskqueue */
	struct kqinfo	 ip_kq;		/* EVFILT_READ knotes */
	struct lock	 ip_lock;	/* protects ip_cq / ip_cq_count */
	STAILQ_HEAD(, io_req) ip_cq;	/* completion queue */
	int		 ip_cq_count;	/* pending completions */
	int		 ip_nworkers;
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

static void filt_ioportdetach(struct knote *kn);
static int  filt_ioport(struct knote *kn, long hint);

static struct filterops ioport_read_filtops =
	{ FILTEROP_ISFD | FILTEROP_MPSAFE, NULL, filt_ioportdetach,
	  filt_ioport };

static void io_frame_push(struct io_req *req,
			  void (*complete)(struct io_req *, void *), void *ctx) __unused;
static struct io_frame *io_frame_pop(struct io_req *req);
static void io_return(struct io_req *req);
static void ioport_exec(struct io_req *req, void (*fn)(struct io_req *));
static void io_task_handler(void *context, int pending);
static void ioport_post(struct io_req *req);
static void io_req_free(struct io_req *req);
static int  ioevent_submit(struct ioport *ip,
			   const struct io_submit *submits, int nsubmits);
static int  ioevent_submit_one(struct ioport *ip, const struct io_submit *sub);
static int  ioevent_close(struct ioport *ip, const struct io_submit *sub);
static void io_close_worker(struct io_req *req);
static int  ioevent_reap(struct ioport *ip, struct io_completion *completions,
			 int ncompletions, struct timespec *tsp, int *res);

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
	struct ioport *ip = (struct ioport *)kn->kn_hook;

	lockmgr(&ip->ip_lock, LK_EXCLUSIVE);
	kn->kn_data = ip->ip_cq_count;
	lockmgr(&ip->ip_lock, LK_RELEASE);
	return (kn->kn_data > 0);
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
	struct io_req *req;

	fp->f_data = NULL;

	/* Drain any un-reaped completions. */
	for (;;) {
		lockmgr(&ip->ip_lock, LK_EXCLUSIVE);
		req = STAILQ_FIRST(&ip->ip_cq);
		if (req != NULL) {
			STAILQ_REMOVE_HEAD(&ip->ip_cq, req_cq_link);
			ip->ip_cq_count--;
		}
		lockmgr(&ip->ip_lock, LK_RELEASE);
		if (req == NULL)
			break;
		io_req_free(req);
	}

	if (ip->ip_tq != NULL)
		taskqueue_free(ip->ip_tq);
	lockuninit(&ip->ip_lock);
	kfree(ip, M_IOPORT);
	return (0);
}

static void
io_frame_push(struct io_req *req, void (*complete)(struct io_req *, void *),
	      void *ctx)
{
	KASSERT(req->req_nframes < IO_FRAME_MAX, ("io_frame stack overflow"));
	req->req_frames[req->req_nframes].fr_complete = complete;
	req->req_frames[req->req_nframes].fr_context = ctx;
	req->req_nframes++;
}

static struct io_frame *
io_frame_pop(struct io_req *req)
{
	if (req->req_nframes == 0)
		return (NULL);
	req->req_nframes--;
	return (&req->req_frames[req->req_nframes]);
}

static void
io_return(struct io_req *req)
{
	struct io_frame *fr;

	fr = io_frame_pop(req);
	if (fr != NULL)
		fr->fr_complete(req, fr->fr_context);
	else
		ioport_post(req);
}

static void
ioport_exec(struct io_req *req, void (*fn)(struct io_req *))
{
	req->req_fn = fn;
	TASK_INIT(&req->req_task, 0, io_task_handler, req);
	taskqueue_enqueue(req->req_ioport->ip_tq, &req->req_task);
}

static void
io_task_handler(void *context, int pending)
{
	struct io_req *req = (struct io_req *)context;

	req->req_fn(req);
}

static void
ioport_post(struct io_req *req)
{
	struct ioport *ip = req->req_ioport;

	lockmgr(&ip->ip_lock, LK_EXCLUSIVE);
	STAILQ_INSERT_TAIL(&ip->ip_cq, req, req_cq_link);
	ip->ip_cq_count++;
	wakeup(ip);
	lockmgr(&ip->ip_lock, LK_RELEASE);

	KNOTE(&ip->ip_kq.ki_note, 0);
}

static void
io_req_free(struct io_req *req)
{
	if (req->req_fp != NULL)
		fdrop(req->req_fp);
	if (req->req_cred != NULL)
		crfree(req->req_cred);
	if (req->req_proc != NULL)
		PRELE(req->req_proc);
	kfree(req, M_IOPORT);
}

static int
ioevent_submit(struct ioport *ip, const struct io_submit *submits, int nsubmits)
{
	struct io_submit sub;
	int i, error;

	for (i = 0; i < nsubmits; i++) {
		error = copyin(&submits[i], &sub, sizeof(sub));
		if (error)
			return (error);
		error = ioevent_submit_one(ip, &sub);
		if (error)
			return (error);
	}
	return (0);
}

static int
ioevent_submit_one(struct ioport *ip, const struct io_submit *sub)
{
	switch (sub->opcode) {
	case IO_CLOSE:
		return (ioevent_close(ip, sub));
	default:
		return (EOPNOTSUPP);
	}
}

static int
ioevent_close(struct ioport *ip, const struct io_submit *sub)
{
	struct thread *td = curthread;
	struct file *fp;
	struct io_req *req;

	/* Synchronous validation: the fd must exist and be valid. */
	fp = holdfp(td, sub->fd, -1);
	if (fp == NULL)
		return (EBADF);
	dropfp(td, sub->fd, fp);

	req = kmalloc(sizeof(*req), M_IOPORT, M_WAITOK | M_ZERO);
	req->req_ioport = ip;
	req->req_tag = sub->tag;
	req->req_opcode = sub->opcode;
	req->req_fd = sub->fd;
	req->req_proc = td->td_proc;
	PHOLD(req->req_proc);
	req->req_cred = crhold(td->td_ucred);

	ioport_exec(req, io_close_worker);
	return (0);
}

static void
io_close_worker(struct io_req *req)
{
	struct filedesc *fdp = req->req_proc->p_fd;
	int error;

	error = kern_close_fdp(fdp, req->req_fd, req->req_proc);
	req->req_error = error;
	req->req_result = 0;

	io_return(req);
}

static int
ioevent_reap(struct ioport *ip, struct io_completion *completions,
	     int ncompletions, struct timespec *tsp, int *res)
{
	struct io_completion comp;
	struct io_req *req;
	int total, error, timo;

	total = 0;
	error = 0;
	*res = 0;

	for (;;) {
		while (total < ncompletions) {
			lockmgr(&ip->ip_lock, LK_EXCLUSIVE);
			req = STAILQ_FIRST(&ip->ip_cq);
			if (req != NULL) {
				STAILQ_REMOVE_HEAD(&ip->ip_cq, req_cq_link);
				ip->ip_cq_count--;
			}
			lockmgr(&ip->ip_lock, LK_RELEASE);
			if (req == NULL)
				break;

			comp.tag = req->req_tag;
			comp.opcode = req->req_opcode;
			comp.error = req->req_error;
			comp.result = req->req_result;
			comp.flags = 0;
			comp.reserved = 0;
			io_req_free(req);

			error = copyout(&comp, &completions[total],
					sizeof(comp));
			if (error) {
				*res = -1;
				return (error);
			}
			total++;
		}

		if (total > 0)
			break;

		/* No completions yet.  Decide whether to wait. */
		if (tsp == NULL) {
			timo = 0;
		} else if (tsp->tv_sec == 0 && tsp->tv_nsec == 0) {
			break;		/* non-blocking poll */
		} else {
			timo = tstohz_high(tsp);
			if (timo < 1)
				timo = 1;
		}

		lockmgr(&ip->ip_lock, LK_EXCLUSIVE);
		if (STAILQ_EMPTY(&ip->ip_cq)) {
			error = lksleep(ip, &ip->ip_lock, PCATCH, "iocq",
					timo);
		}
		lockmgr(&ip->ip_lock, LK_RELEASE);
		if (error != 0)
			break;
	}

	*res = total;
	return (error);
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
	lockinit(&ip->ip_lock, "ioport", 0, 0);
	STAILQ_INIT(&ip->ip_cq);
	ip->ip_cq_count = 0;

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
 */
int
sys_ioevent(struct sysmsg *sysmsg, const struct ioevent_args *uap)
{
	struct thread *td = curthread;
	struct timespec ts, *tsp;
	struct file *fp;
	struct ioport *ip;
	int error;

	if (uap->timeout != NULL) {
		error = copyin(uap->timeout, &ts, sizeof(ts));
		if (error)
			return (error);
		tsp = &ts;
	} else {
		tsp = NULL;
	}

	fp = holdfp(td, uap->pfd, -1);
	if (fp == NULL)
		return (EBADF);
	if (fp->f_type != DTYPE_IOPORT) {
		dropfp(td, uap->pfd, fp);
		return (EBADF);
	}
	ip = (struct ioport *)fp->f_data;

	error = ioevent_submit(ip, uap->submits, uap->nsubmits);
	if (error) {
		dropfp(td, uap->pfd, fp);
		return (error);
	}

	error = ioevent_reap(ip, uap->completions, uap->ncompletions, tsp,
			     &sysmsg->sysmsg_result);

	dropfp(td, uap->pfd, fp);
	return (error);
}
