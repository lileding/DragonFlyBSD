/*
 * Kernel-internal ioport/ioevent data structures and entry points shared
 * between the completion-port core (kern/ioport.c) and the VFS asynchronous
 * layer (fo_begin_io -> vop_begin_io).  This is NOT part of the frozen user
 * ABI in <sys/ioport.h>.
 */

#ifndef _SYS_IOPORT_VAR_H_
#define _SYS_IOPORT_VAR_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/malloc.h>

MALLOC_DECLARE(M_IOPORT);

struct file;
struct knote;
struct proc;
struct ucred;
struct ioport;
struct ioport_kevent_ctx;
struct io_req;

/* request states */
#define IO_REQ_QUEUED	0	/* dispatch pending */
#define IO_REQ_RUNNING	1	/* worker task running */
#define IO_REQ_PENDING	2	/* async lower layer owns it */
#define IO_REQ_RETURNING	3	/* reverse completion path */

#define IO_FRAME_MAX	8

/* One continuation frame in the io_req reverse-completion chain. */
struct io_frame {
	void	(*fr_complete)(struct io_req *, void *);
	void	*fr_context;
};

/*
 * A single accepted request.  One request yields exactly one completion.
 * The VFS async layer may read the request parameters and write req_error /
 * req_result / req_buf / req_data_off, and drives the reverse path through
 * io_return().  Everything else is owned by the completion-port core.
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
	volatile int	 req_refs;	/* active references (main + transient) */
	LIST_ENTRY(io_req) req_hash_link;
	struct file	*req_fp;	/* held target file, or NULL */
	struct proc	*req_proc;	/* held submitting proc */
	struct ucred	*req_cred;	/* held credentials */
	int		 req_fd;	/* target fd */
	void		*req_buf;	/* kernel buffer (copy in/out) */
	void		*req_ubuf;	/* user buffer VA */
	size_t		 req_len;	/* requested length */
	off_t		 req_offset;	/* file offset */
	off_t		 req_data_off;	/* user data offset within req_buf */
	int		 req_copied;	/* data already copied to user (skip reap) */
	struct io_frame	 req_frames[IO_FRAME_MAX];
	int		 req_nframes;
	struct task	 req_task;	/* worker task for ioport_exec */
	void		(*req_fn)(struct io_req *);
	STAILQ_ENTRY(io_req) req_cq_link;
};

/*
 * ioport core entry points used by the VFS async layer (fo_begin_io /
 * vop_begin_io).
 */
void	io_return(struct io_req *req);		/* gated reverse entry */
void	io_return_next(struct io_req *req);	/* re-entrant frame unwind */
void	io_frame_push(struct io_req *req,
		      void (*complete)(struct io_req *, void *), void *ctx);
void	ioport_exec(struct io_req *req, void (*fn)(struct io_req *));
void	io_rw_worker(struct io_req *req);	/* fo_read/fo_write on a worker */
void	io_req_begin_forward(struct io_req *req,
			     void (*cancel_hook)(struct io_req *));
void	io_req_set_cancel(struct io_req *req,
			  void (*cancel_hook)(struct io_req *));

#endif /* !_SYS_IOPORT_VAR_H_ */
