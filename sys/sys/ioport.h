/*
 * Copyright (c) 2026
 *
 * Native DragonFly asynchronous I/O control plane: ioport(2) / ioevent(2).
 *
 * ioport(2) creates a completion port represented by an ordinary file
 * descriptor (a "pfd").  ioevent(2) submits a batch of asynchronous requests
 * and/or reaps a batch of completions, mirroring the shape of kevent(2).
 *
 * Design baseline: ioport-liburing-design.md.
 *
 * ABI NOTE: the record layouts, opcode namespace, and flag values below are
 * frozen.  Field order, types, and sizes are part of the syscall ABI and must
 * not change.  The `args` union may only grow new members for new opcodes;
 * the trailing reserved words keep the overall record size stable so that
 * adding opcodes never reflows this struct.
 */

#ifndef _SYS_IOPORT_H_
#define _SYS_IOPORT_H_

#include <sys/types.h>
#include <sys/_iovec.h>		/* struct iovec */
#include <sys/_timespec.h>	/* struct timespec */

/*
 * Operation codes.  Append-only namespace; the numeric values are frozen.
 *
 * IO_CANCEL is a submit-array control marker, not a request: it allocates no
 * io_req, occupies no completion slot, and produces no completion.  Its only
 * parameter is the caller tag to cancel (io_submit.tag).
 */
#define IO_NONE		0	/* unused */
#define IO_READ		1	/* read bytes into buf at offset */
#define IO_WRITE	2	/* write bytes from buf at offset */
#define IO_READV	3	/* readv at offset */
#define IO_WRITEV	4	/* writev at offset */
#define IO_FSYNC	5	/* fsync target fd */
#define IO_FTRUNCATE	6	/* ftruncate target fd */
#define IO_KEVENT	7	/* wait on a kqueue filter (notification) */
#define IO_OPENAT	8	/* open a path (dirfd/AT_FDCWD) */
#define IO_CLOSE	9	/* close target fd */
/* ... append new opcodes here as they are implemented ... */
#define IO_MAXOPCODE	9

#define IO_CANCEL	0x80000000U	/* control marker, not a request */

/*
 * Per-request submit flags (io_submit.flags).  Unknown bits are rejected with
 * EINVAL; new flags are append-only.
 */
#define IOSUB_NONE	0x00000000U

/*
 * Completion flags (io_completion.flags).
 */
#define IOCOMPL_NONE	0x00000000U
#define IOCOMPL_MORE	0x00000001U	/* another completion follows */

/*
 * Submission record.  One native I/O or notification request.
 *
 * `fd` is the target descriptor for fd-based opcodes and -1 otherwise.
 * `args` carries the opcode-specific parameters; interpretation depends on
 * `opcode`:
 *
 *   IO_READ/IO_WRITE/IO_FSYNC/IO_FTRUNCATE: args.rw
 *   IO_READV/IO_WRITEV:                     args.rwv
 *   IO_KEVENT:                              args.kevent
 *   IO_OPENAT:                              args.openat
 *   IO_CLOSE:                               (fd only, args unused)
 */
struct io_submit {
	uint64_t	tag;		/* caller tag, echoed in completion */
	uint32_t	opcode;		/* IO_* or IO_CANCEL */
	int32_t		fd;		/* target fd, or -1 */
	uint32_t	flags;		/* IOSUB_* */
	uint32_t	reserved0;	/* must be zero */
	union {
		struct {
			void	*buf;	/* user buffer */
			uint64_t len;	/* byte count */
			uint64_t offset;/* file offset */
		} rw;
		struct {
			struct iovec *iov;	/* iovec array */
			uint64_t iovcnt;	/* iovec count */
			uint64_t offset;	/* file offset */
		} rwv;
		struct {
			uint64_t ident;	/* kqueue ident (fd/pid/signal/...) */
			int32_t	filter;	/* EVFILT_* */
			uint32_t fflags;	/* filter flags */
			int64_t	data;	/* filter data */
			void	*udata;	/* opaque user data */
		} kevent;
		struct {
			const char *path;	/* pathname */
			uint32_t flags;		/* open(2) flags */
			uint32_t mode;		/* open(2) mode */
			int32_t	dirfd;		/* dirfd or AT_FDCWD */
			uint32_t pad0;
		} openat;
	} args;
	uint64_t	reserved1[2];	/* must be zero */
};

/*
 * Completion record.  Every accepted request produces exactly one completion.
 *
 * On success `error` is 0 and `result` is the operation result: bytes
 * transferred for I/O, the new fd for fd-creating operations, or 0 for
 * operations with no return value.  On failure `error` is the errno and
 * `result` is 0.
 */
struct io_completion {
	uint64_t	tag;		/* echoed caller tag */
	int32_t		opcode;		/* IO_* */
	int32_t		error;		/* errno, 0 = success */
	int64_t		result;		/* bytes, new fd, or 0 */
	uint32_t	flags;		/* IOCOMPL_* */
	uint32_t	reserved;	/* must be zero */
};

#ifndef _KERNEL

int	ioport(unsigned nworkers);
int	ioevent(int pfd, const struct io_submit *submits, int nsubmits,
	    struct io_completion *completions, int ncompletions,
	    const struct timespec *timeout);

#endif /* !_KERNEL */

#endif /* !_SYS_IOPORT_H_ */
