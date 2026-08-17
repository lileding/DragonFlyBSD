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
#include <sys/file2.h>
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
#define IP_CANCEL_HASH_SIZE	64

/* request states */
#define IO_REQ_QUEUED	0	/* dispatch pending */
#define IO_REQ_RUNNING	1	/* worker task running */
#define IO_REQ_PENDING	2	/* async lower layer owns it */
#define IO_REQ_RETURNING	3	/* reverse completion path */

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
	uint32_t	 req_flags;	/* completion flags (event fflags) */
	struct knote	*req_kn;	/* IO_KEVENT knote, or NULL */
	struct ioport_kevent_ctx *req_knctx;
	int		 req_state;	/* IO_REQ_* */
	int		 req_cancel_requested;
	void		(*req_cancel_hook)(struct io_req *);
	LIST_ENTRY(io_req) req_hash_link;
	struct file	*req_fp;	/* held target file, or NULL */
	struct proc	*req_proc;	/* held submitting proc */
	struct ucred	*req_cred;	/* held credentials */
	int		 req_fd;	/* target fd */
	void		*req_buf;	/* kernel buffer (copy in/out) */
	void		*req_ubuf;	/* user buffer VA */
	size_t		 req_len;	/* requested length */
	off_t		 req_offset;	/* file offset */
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
	struct kqueue	 ip_kevent_kq;	/* helper kqueue for IO_KEVENT knotes */
	LIST_HEAD(, io_req) ip_cancel_hash[IP_CANCEL_HASH_SIZE];
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

/*
 * IO_KEVENT wrapper: a knote whose f_event/f_detach delegate to the target's
 * real filterops but complete the owning io_req when the filter fires.
 */
struct ioport_kevent_ctx {
	struct io_req	*ik_req;
	struct filterops *ik_fop;	/* real filterops */
	int		 ik_done;
};

static int  ioport_kevent_attach(struct knote *kn);
static void ioport_kevent_detach(struct knote *kn);
static int  ioport_kevent_event(struct knote *kn, long hint);

static struct filterops ioport_kevent_filtops =
	{ FILTEROP_ISFD | FILTEROP_MPSAFE, ioport_kevent_attach,
	  ioport_kevent_detach, ioport_kevent_event };

static void io_frame_push(struct io_req *req,
			  void (*complete)(struct io_req *, void *), void *ctx) __unused;
static struct io_frame *io_frame_pop(struct io_req *req);
static void io_return(struct io_req *req);
static void ioport_exec(struct io_req *req, void (*fn)(struct io_req *));
static void io_task_handler(void *context, int pending);
static void ioport_post(struct io_req *req);
static void io_req_free(struct io_req *req);
static void io_req_cancel_hash_add(struct ioport *ip, struct io_req *req);
static void io_req_begin_forward(struct io_req *req,
				 void (*cancel_hook)(struct io_req *));
static int  io_req_begin_return(struct io_req *req);
static void ioport_cancel(struct ioport *ip, uint64_t tag);
static void io_cancel_kevent_hook(struct io_req *req);
static void io_cancel_worker_hook(struct io_req *req);
static int  ioevent_submit(struct ioport *ip,
			   const struct io_submit *submits, int nsubmits);
static int  ioevent_submit_one(struct ioport *ip, const struct io_submit *sub);
static int  ioevent_close(struct ioport *ip, const struct io_submit *sub);
static void io_close_worker(struct io_req *req);
static int  ioevent_rw(struct ioport *ip, const struct io_submit *sub);
static int  ioevent_kevent(struct ioport *ip, const struct io_submit *sub);
static void io_rw_worker(struct io_req *req);
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
	kqueue_terminate(&ip->ip_kevent_kq);
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

	if (!io_req_begin_return(req))
		return;		/* already returning (cancel/completion won) */

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

	if (req->req_cancel_requested) {
		req->req_error = ECANCELED;
		req->req_result = 0;
		io_return(req);
		return;
	}
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
	if (req->req_buf != NULL)
		kfree(req->req_buf, M_IOPORT);
	if (req->req_kn != NULL) {
		while (knote_acquire(req->req_kn) == 0)
			;
		knote_detach_and_drop(req->req_kn);
		kfree(req->req_knctx, M_IOPORT);
	}
	kfree(req, M_IOPORT);
}

static void
io_req_cancel_hash_add(struct ioport *ip, struct io_req *req)
{
	int slot = (int)(req->req_tag % IP_CANCEL_HASH_SIZE);

	lockmgr(&ip->ip_lock, LK_EXCLUSIVE);
	LIST_INSERT_HEAD(&ip->ip_cancel_hash[slot], req, req_hash_link);
	lockmgr(&ip->ip_lock, LK_RELEASE);
}

/* Enter the forward path: the request is now cancellable via IO_CANCEL. */
static void
io_req_begin_forward(struct io_req *req, void (*cancel_hook)(struct io_req *))
{
	req->req_state = IO_REQ_QUEUED;
	req->req_cancel_hook = cancel_hook;
	io_req_cancel_hash_add(req->req_ioport, req);
}

/* Transition to the reverse path.  Returns 1 if this caller wins the
 * transition (and must drive the completion), 0 if already returning. */
static int
io_req_begin_return(struct io_req *req)
{
	struct ioport *ip = req->req_ioport;
	int win;

	lockmgr(&ip->ip_lock, LK_EXCLUSIVE);
	if (req->req_state == IO_REQ_RETURNING) {
		win = 0;
	} else {
		req->req_state = IO_REQ_RETURNING;
		LIST_REMOVE(req, req_hash_link);
		win = 1;
	}
	lockmgr(&ip->ip_lock, LK_RELEASE);
	return (win);
}

/* IO_CANCEL: best-effort truncation of one matching forward request. */
static void
ioport_cancel(struct ioport *ip, uint64_t tag)
{
	struct io_req *req;
	struct io_req *found = NULL;
	int slot = (int)(tag % IP_CANCEL_HASH_SIZE);

	lockmgr(&ip->ip_lock, LK_EXCLUSIVE);
	LIST_FOREACH(req, &ip->ip_cancel_hash[slot], req_hash_link) {
		if (req->req_tag == tag && req->req_state != IO_REQ_RETURNING) {
			req->req_cancel_requested = 1;
			found = req;
			break;
		}
	}
	lockmgr(&ip->ip_lock, LK_RELEASE);

	if (found != NULL)
		found->req_cancel_hook(found);
}

static void
io_cancel_kevent_hook(struct io_req *req)
{
	if (req->req_kn != NULL) {
		while (knote_acquire(req->req_kn) == 0)
			;
		knote_detach_and_drop(req->req_kn);
		req->req_kn = NULL;
	}
	req->req_error = ECANCELED;
	req->req_result = 0;
	io_return(req);
}

static void
io_cancel_worker_hook(struct io_req *req)
{
	/* taskqueue_cancel() returns 0 when the task was still queued and has
	 * been removed; EBUSY when the worker already picked it up (then the
	 * trampoline sees req_cancel_requested and completes ECANCELED). */
	if (taskqueue_cancel(req->req_ioport->ip_tq, &req->req_task, NULL) == 0) {
		req->req_error = ECANCELED;
		req->req_result = 0;
		io_return(req);
	}
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
	case IO_CANCEL:
		ioport_cancel(ip, sub->tag);	/* control marker, no completion */
		return (0);
	case IO_CLOSE:
		return (ioevent_close(ip, sub));
	case IO_READ:
	case IO_WRITE:
		return (ioevent_rw(ip, sub));
	case IO_KEVENT:
		return (ioevent_kevent(ip, sub));
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

	io_req_begin_forward(req, io_cancel_worker_hook);
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
ioevent_rw(struct ioport *ip, const struct io_submit *sub)
{
	struct thread *td = curthread;
	struct file *fp;
	struct io_req *req;
	void *buf;
	size_t len;
	off_t offset;
	int error;

	len = sub->args.rw.len;
	offset = (off_t)sub->args.rw.offset;

	fp = holdfp(td, sub->fd, -1);
	if (fp == NULL)
		return (EBADF);

	buf = kmalloc((len > 0) ? len : 1, M_IOPORT, M_WAITOK);
	if (buf == NULL) {
		dropfp(td, sub->fd, fp);
		return (ENOMEM);
	}

	if (sub->opcode == IO_WRITE) {
		error = copyin(sub->args.rw.buf, buf, len);
		if (error) {
			dropfp(td, sub->fd, fp);
			kfree(buf, M_IOPORT);
			return (error);
		}
	}

	req = kmalloc(sizeof(*req), M_IOPORT, M_WAITOK | M_ZERO);
	req->req_ioport = ip;
	req->req_tag = sub->tag;
	req->req_opcode = sub->opcode;
	req->req_fp = fp;
	req->req_proc = td->td_proc;
	PHOLD(req->req_proc);
	req->req_cred = crhold(td->td_ucred);
	req->req_fd = sub->fd;
	req->req_buf = buf;
	req->req_ubuf = sub->args.rw.buf;
	req->req_len = len;
	req->req_offset = offset;

	io_req_begin_forward(req, io_cancel_worker_hook);
	ioport_exec(req, io_rw_worker);
	return (0);
}

static void
io_rw_worker(struct io_req *req)
{
	struct uio uio;
	struct iovec iov;
	int error;

	bzero(&uio, sizeof(uio));
	iov.iov_base = req->req_buf;
	iov.iov_len = req->req_len;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = req->req_offset;
	uio.uio_resid = req->req_len;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = (req->req_opcode == IO_READ) ? UIO_READ : UIO_WRITE;
	uio.uio_td = curthread;

	/* O_FOFFSET: use the explicit uio_offset from the submit record
	 * (pread/pwrite semantics) rather than the file's shared f_offset. */
	if (req->req_opcode == IO_READ)
		error = fo_read(req->req_fp, &uio, req->req_cred, O_FOFFSET);
	else
		error = fo_write(req->req_fp, &uio, req->req_cred, O_FOFFSET);

	req->req_error = error;
	req->req_result = (int64_t)(req->req_len - uio.uio_resid);

	io_return(req);
}

static int
ioport_kevent_attach(struct knote *kn)
{
	struct ioport_kevent_ctx *ctx = kn->kn_kevent.udata;
	struct file *fp = kn->kn_fp;
	int error;

	/* Delegate to the target fo_kqfilter(): sets the real filterops,
	 * kn_hook, and inserts into the target's klist. */
	error = fo_kqfilter(fp, kn);
	if (error)
		return (error);

	ctx->ik_fop = kn->kn_fop;	/* save the real filterops */
	kn->kn_fop = &ioport_kevent_filtops;
	return (0);
}

static void
ioport_kevent_detach(struct knote *kn)
{
	struct ioport_kevent_ctx *ctx = kn->kn_kevent.udata;

	if (ctx->ik_fop->f_detach != NULL)
		ctx->ik_fop->f_detach(kn);
}

static int
ioport_kevent_event(struct knote *kn, long hint)
{
	struct ioport_kevent_ctx *ctx = kn->kn_kevent.udata;
	int ready;

	if (ctx->ik_done || ctx->ik_req->req_cancel_requested)
		return (0);
	ready = ctx->ik_fop->f_event(kn, hint);
	if (ready) {
		ctx->ik_done = 1;
		ctx->ik_req->req_result = (int64_t)kn->kn_data;
		ctx->ik_req->req_flags = (uint32_t)kn->kn_fflags;
		io_return(ctx->ik_req);
	}
	return (ready);
}

static int
ioevent_kevent(struct ioport *ip, const struct io_submit *sub)
{
	struct thread *td = curthread;
	struct file *fp;
	struct knote *kn;
	struct io_req *req;
	struct ioport_kevent_ctx *ctx;
	int error;

	/* Phase 5 supports fd-based filters (EVFILT_READ/WRITE). */
	if (sub->args.kevent.filter != EVFILT_READ &&
	    sub->args.kevent.filter != EVFILT_WRITE)
		return (EOPNOTSUPP);

	fp = holdfp(td, (int)sub->args.kevent.ident, -1);
	if (fp == NULL)
		return (EBADF);

	req = kmalloc(sizeof(*req), M_IOPORT, M_WAITOK | M_ZERO);
	ctx = kmalloc(sizeof(*ctx), M_IOPORT, M_WAITOK | M_ZERO);
	kn = knote_alloc();

	req->req_ioport = ip;
	req->req_tag = sub->tag;
	req->req_opcode = sub->opcode;
	/* The file reference is held by the knote (kn_fp), dropped by
	 * knote_drop(); the io_req must not drop it a second time. */
	req->req_fp = NULL;
	req->req_proc = td->td_proc;
	PHOLD(req->req_proc);
	req->req_cred = crhold(td->td_ucred);
	req->req_fd = (int)sub->args.kevent.ident;
	req->req_kn = kn;
	req->req_knctx = ctx;

	ctx->ik_req = req;
	ctx->ik_fop = NULL;
	ctx->ik_done = 0;

	kn->kn_kq = &ip->ip_kevent_kq;
	kn->kn_fp = fp;
	kn->kn_kevent.ident = sub->args.kevent.ident;
	kn->kn_kevent.filter = sub->args.kevent.filter;
	kn->kn_kevent.flags = 0;
	kn->kn_kevent.fflags = sub->args.kevent.fflags;
	kn->kn_kevent.data = sub->args.kevent.data;
	kn->kn_kevent.udata = ctx;
	kn->kn_sfflags = sub->args.kevent.fflags;
	kn->kn_sdata = sub->args.kevent.data;
	kn->kn_status = KN_PROCESSING;
	kn->kn_fop = &ioport_kevent_filtops;

	knote_attach(kn);
	error = filter_attach(kn);
	if (error) {
		kn->kn_status |= KN_DELETING;
		knote_detach_and_drop(kn);
		kfree(ctx, M_IOPORT);
		req->req_kn = NULL;
		req->req_knctx = NULL;
		io_req_free(req);
		return (error);
	}

	io_req_begin_forward(req, io_cancel_kevent_hook);

	/* Immediate readiness check (e.g. the pipe already has data). */
	(void)ioport_kevent_event(kn, 0);
	kn->kn_status &= ~KN_PROCESSING;

	return (0);
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

			/*
			 * Deliver read data to the user buffer before the
			 * completion becomes visible.  Slice simplification:
			 * uses the reaper's vmspace (== submitter's in the
			 * single-threaded test); true pinning replaces this in
			 * the native async VOP phase.
			 */
			if (req->req_opcode == IO_READ && req->req_error == 0 &&
			    req->req_result > 0) {
				error = copyout(req->req_buf, req->req_ubuf,
						req->req_result);
				if (error) {
					req->req_error = error;
					req->req_result = 0;
				}
			}

			comp.tag = req->req_tag;
			comp.opcode = req->req_opcode;
			comp.error = req->req_error;
			comp.result = req->req_result;
			comp.flags = req->req_flags;
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
	int fd, error, i;

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
	for (i = 0; i < IP_CANCEL_HASH_SIZE; i++)
		LIST_INIT(&ip->ip_cancel_hash[i]);
	kqueue_init(&ip->ip_kevent_kq, td->td_proc->p_fd);
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
