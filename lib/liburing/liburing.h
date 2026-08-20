/* SPDX-License-Identifier: MIT */
/*
 * DragonFly liburing compatibility layer.
 *
 * Provides the liburing queue API backed by the native ioport()/ioevent()
 * completion-port syscalls.  The SQ/CQ live in ordinary user memory (no Linux
 * ring mmap).  Linux-only operations and raw syscall wrappers fail honestly
 * with -ENOSYS/-EOPNOTSUPP.
 */
#ifndef LIB_URING_H
#define LIB_URING_H

#include <sys/types.h>
#include <sys/uio.h>
#include <stddef.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include "io_uring.h"

#ifdef __cplusplus
extern "C" {
#endif

struct io_uring_sq {
	unsigned *khead;
	unsigned *ktail;
	unsigned *kring_mask;
	unsigned *kring_entries;
	unsigned *kflags;
	unsigned *kdropped;
	unsigned *array;
	struct io_uring_sqe *sqes;

	unsigned sqe_head;
	unsigned sqe_tail;

	size_t ring_sz;
	void *ring_ptr;

	unsigned ring_mask;
	unsigned ring_entries;

	unsigned sqes_sz;
	unsigned pad;
};

struct io_uring_cq {
	unsigned *khead;
	unsigned *ktail;
	unsigned *kring_mask;
	unsigned *kring_entries;
	unsigned *kflags;
	unsigned *koverflow;
	struct io_uring_cqe *cqes;

	size_t ring_sz;
	void *ring_ptr;

	unsigned ring_mask;
	unsigned ring_entries;

	unsigned pad[2];
};

struct io_uring {
	struct io_uring_sq sq;
	struct io_uring_cq cq;
	unsigned flags;
	int ring_fd;

	unsigned features;
	int enter_ring_fd;
	__u8 int_flags;
	__u8 pad[3];
	unsigned pad2;
};

struct io_sqring_offsets {
	__u32 head;
	__u32 tail;
	__u32 ring_mask;
	__u32 ring_entries;
	__u32 flags;
	__u32 dropped;
	__u32 array;
	__u32 resv1;
	__u64 user_addr;
};

struct io_cqring_offsets {
	__u32 head;
	__u32 tail;
	__u32 ring_mask;
	__u32 ring_entries;
	__u32 overflow;
	__u32 cqes;
	__u32 flags;
	__u32 resv1;
	__u64 user_addr;
};

struct io_uring_params {
	__u32 sq_entries;
	__u32 cq_entries;
	__u32 flags;
	__u32 sq_thread_cpu;
	__u32 sq_thread_idle;
	__u32 features;
	__u32 wq_fd;
	__u32 resv[3];
	struct io_sqring_offsets sq_off;
	struct io_cqring_offsets cq_off;
};

/* ---- queue API ---- */
int io_uring_queue_init(unsigned entries, struct io_uring *ring,
			unsigned flags);
int io_uring_queue_init_params(unsigned entries, struct io_uring *ring,
			       struct io_uring_params *p);
void io_uring_queue_exit(struct io_uring *ring);
int io_uring_queue_mmap(int fd, struct io_uring_params *p,
			struct io_uring *ring);

struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring);

int io_uring_submit(struct io_uring *ring);
int io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr);
int io_uring_submit_and_get_events(struct io_uring *ring);

int io_uring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr);
int io_uring_wait_cqe_nr(struct io_uring *ring, struct io_uring_cqe **cqe_ptr,
			 unsigned wait_nr);
int io_uring_peek_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr);
int io_uring_peek_batch_cqe(struct io_uring *ring,
			    struct io_uring_cqe **cqes, unsigned count);

int io_uring_cqe_seen(struct io_uring *ring, struct io_uring_cqe *cqe);
void io_uring_cq_advance(struct io_uring *ring, unsigned nr);

unsigned io_uring_sq_ready(const struct io_uring *ring);
unsigned io_uring_sq_space_left(const struct io_uring *ring);
unsigned io_uring_cq_ready(const struct io_uring *ring);

/* ---- prep helpers ---- */
static inline void io_uring_sqe_set_data(struct io_uring_sqe *sqe, void *data)
{
	sqe->user_data = (__u64)(unsigned long)data;
}

static inline void *io_uring_cqe_get_data(const struct io_uring_cqe *cqe)
{
	return (void *)(unsigned long)cqe->user_data;
}

static inline void io_uring_sqe_set_flags(struct io_uring_sqe *sqe,
					  unsigned flags)
{
	sqe->flags = (__u8)flags;
}

static inline void io_uring_prep_rw(int op, struct io_uring_sqe *sqe, int fd,
				    const void *addr, unsigned len, __u64 offset)
{
	sqe->opcode = (__u8)op;
	sqe->flags = 0;
	sqe->ioprio = 0;
	sqe->fd = fd;
	sqe->off = offset;
	sqe->addr = (__u64)(unsigned long)addr;
	sqe->len = len;
	sqe->rw_flags = 0;
	sqe->user_data = 0;
	sqe->__pad2[0] = 0;
	sqe->__pad2[1] = 0;
}

static inline void io_uring_prep_read(struct io_uring_sqe *sqe, int fd,
				      void *buf, unsigned nbytes, __u64 offset)
{
	io_uring_prep_rw(IORING_OP_READ, sqe, fd, buf, nbytes, offset);
}

static inline void io_uring_prep_write(struct io_uring_sqe *sqe, int fd,
				       const void *buf, unsigned nbytes,
				       __u64 offset)
{
	io_uring_prep_rw(IORING_OP_WRITE, sqe, fd, buf, nbytes, offset);
}

static inline void io_uring_prep_nop(struct io_uring_sqe *sqe)
{
	sqe->opcode = IORING_OP_NOP;
	sqe->flags = 0;
	sqe->ioprio = 0;
	sqe->fd = -1;
	sqe->off = 0;
	sqe->addr = 0;
	sqe->len = 0;
	sqe->rw_flags = 0;
	sqe->user_data = 0;
	sqe->__pad2[0] = 0;
	sqe->__pad2[1] = 0;
}

/* ---- raw syscall wrappers: always fail, never issue Linux syscalls ---- */
int io_uring_setup(unsigned entries, struct io_uring_params *p);
int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
		   unsigned flags, sigset_t *sig);
int io_uring_enter2(int fd, unsigned to_submit, unsigned min_complete,
		    unsigned flags, sigset_t *sig, size_t sz);
int io_uring_register(int fd, unsigned opcode, const void *arg,
		      unsigned nr_args);

int io_uring_major_version(void);
int io_uring_minor_version(void);
bool io_uring_check_version(int major, int minor);

#ifdef __cplusplus
}
#endif

#endif
