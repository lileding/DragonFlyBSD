/* SPDX-License-Identifier: MIT */
/*
 * DragonFly liburing compatibility backend: local SQ/CQ in user memory, backed
 * by ioport()/ioevent().  Linux-only operations fail honestly.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioport.h>
#include "liburing.h"

#define LIBURING_VERSION_MAJOR	2
#define LIBURING_VERSION_MINOR	14

static unsigned
roundup_pow2(unsigned n)
{
	unsigned e = 1;

	while (e < n)
		e <<= 1;
	return e;
}

/* Push a completion into the local CQ. */
static void
post_cqe(struct io_uring *ring, __u64 user_data, __s32 res, __u32 flags)
{
	unsigned tail = *ring->cq.ktail;
	struct io_uring_cqe *cqe;

	cqe = &ring->cq.cqes[tail & ring->cq.ring_mask];
	cqe->user_data = user_data;
	cqe->res = res;
	cqe->flags = flags;
	*ring->cq.ktail = tail + 1;
}

int
io_uring_queue_init_params(unsigned entries, struct io_uring *ring,
			   struct io_uring_params *p)
{
	long ncpus;
	int nworkers, pfd;

	memset(ring, 0, sizeof(*ring));
	if (entries == 0)
		return -EINVAL;

	entries = roundup_pow2(entries);
	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 1)
		ncpus = 1;
	nworkers = (int)entries;
	if (nworkers > (int)(4 * ncpus))
		nworkers = (int)(4 * ncpus);
	if (nworkers < 1)
		nworkers = 1;

	pfd = (int)syscall(SYS_ioport, nworkers);
	if (pfd < 0)
		return -errno;

	ring->ring_fd = pfd;
	ring->sq.ring_entries = entries;
	ring->sq.ring_mask = entries - 1;
	ring->sq.sqe_head = 0;
	ring->sq.sqe_tail = 0;
	ring->sq.sqes = calloc(entries, sizeof(struct io_uring_sqe));
	if (ring->sq.sqes == NULL)
		goto fail_sq;

	ring->cq.ring_entries = entries;
	ring->cq.ring_mask = entries - 1;
	ring->cq.cqes = calloc(entries, sizeof(struct io_uring_cqe));
	ring->cq.khead = malloc(sizeof(unsigned));
	ring->cq.ktail = malloc(sizeof(unsigned));
	if (ring->cq.cqes == NULL || ring->cq.khead == NULL ||
	    ring->cq.ktail == NULL)
		goto fail_cq;
	*ring->cq.khead = 0;
	*ring->cq.ktail = 0;

	if (p != NULL) {
		memset(p, 0, sizeof(*p));
		p->sq_entries = entries;
		p->cq_entries = entries;
		p->features = 0;	/* no mmap ring, no sqpoll */
	}
	return 0;

fail_cq:
	free(ring->cq.cqes);
	free(ring->cq.khead);
	free(ring->cq.ktail);
	free(ring->sq.sqes);
fail_sq:
	close(pfd);
	return -ENOMEM;
}

int
io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags)
{
	struct io_uring_params p;

	/* IORING_SETUP_* that change ring layout/mmap semantics are not
	 * expressible on DragonFly; reject them. */
	if (flags != 0)
		return -EINVAL;
	memset(&p, 0, sizeof(p));
	p.flags = flags;
	return io_uring_queue_init_params(entries, ring, &p);
}

void
io_uring_queue_exit(struct io_uring *ring)
{
	if (ring->ring_fd >= 0)
		close(ring->ring_fd);
	free(ring->sq.sqes);
	free(ring->cq.cqes);
	free(ring->cq.khead);
	free(ring->cq.ktail);
	memset(ring, 0, sizeof(*ring));
	ring->ring_fd = -1;
}

int
io_uring_queue_mmap(int fd, struct io_uring_params *p, struct io_uring *ring)
{
	(void)fd; (void)p; (void)ring;
	errno = ENOSYS;
	return -1;
}

struct io_uring_sqe *
io_uring_get_sqe(struct io_uring *ring)
{
	unsigned next = ring->sq.sqe_tail + 1;
	struct io_uring_sqe *sqe;

	if (next - ring->sq.sqe_head > ring->sq.ring_entries)
		return NULL;
	sqe = &ring->sq.sqes[ring->sq.sqe_tail & ring->sq.ring_mask];
	ring->sq.sqe_tail = next;
	return sqe;
}

/* Reap native completions into the local CQ (non-blocking). */
static void
reap_native(struct io_uring *ring)
{
	struct io_completion comp;
	struct timespec nb = { 0, 0 };
	int ret;

	for (;;) {
		ret = (int)syscall(SYS_ioevent, ring->ring_fd, NULL, 0,
				   &comp, 1, &nb);
		if (ret != 1)
			break;
		post_cqe(ring, comp.tag,
			 comp.error ? (__s32)-comp.error : (__s32)comp.result,
			 (__u32)comp.flags);
	}
}

int
io_uring_submit(struct io_uring *ring)
{
	unsigned head = ring->sq.sqe_head;
	unsigned tail = ring->sq.sqe_tail;
	unsigned nr = tail - head;
	unsigned i, nsub;
	struct io_submit *subs;
	int ret = 0;

	if (nr == 0)
		return 0;

	subs = calloc(nr, sizeof(*subs));
	if (subs == NULL)
		return -ENOMEM;

	nsub = 0;
	for (i = 0; i < nr; i++) {
		struct io_uring_sqe *sqe =
			&ring->sq.sqes[head & ring->sq.ring_mask];
		head++;

		/* Unsupported per-SQE flags (IOSQE_IO_LINK/DRAIN/...) are not
		 * implemented: fail honestly instead of silently dropping the
		 * requested semantics. */
		if (sqe->flags != 0) {
			post_cqe(ring, sqe->user_data, -EOPNOTSUPP, 0);
			continue;
		}

		switch (sqe->opcode) {
		case IORING_OP_NOP:
			post_cqe(ring, sqe->user_data, 0, 0);
			break;
		case IORING_OP_READ:
		case IORING_OP_WRITE:
			if (sqe->rw_flags != 0) {
				post_cqe(ring, sqe->user_data, -EOPNOTSUPP, 0);
				break;
			}
			subs[nsub].tag = sqe->user_data;
			subs[nsub].opcode = (sqe->opcode == IORING_OP_READ) ?
					     IO_READ : IO_WRITE;
			subs[nsub].fd = sqe->fd;
			subs[nsub].args.rw.buf =
				(void *)(unsigned long)sqe->addr;
			subs[nsub].args.rw.len = sqe->len;
			subs[nsub].args.rw.offset = sqe->off;
			nsub++;
			break;
		default:
			/* Honest failure for unsupported Linux ops. */
			post_cqe(ring, sqe->user_data, -EOPNOTSUPP, 0);
			break;
		}
	}

	if (nsub > 0) {
		struct timespec nb = { 0, 0 };

		ret = (int)syscall(SYS_ioevent, ring->ring_fd, subs,
				   (int)nsub, NULL, 0, &nb);
		if (ret != 0)
			ret = -errno;
	}

	free(subs);
	ring->sq.sqe_head = tail;
	/* Surface a native submit error instead of reporting every SQE as
	 * submitted and stranding the application on completions that will
	 * never arrive. */
	if (ret != 0)
		return ret;
	return (int)nr;
}

int
io_uring_submit_and_get_events(struct io_uring *ring)
{
	int nr = io_uring_submit(ring);

	reap_native(ring);
	return nr;
}

int
io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr)
{
	(void)io_uring_submit(ring);
	return io_uring_wait_cqe_nr(ring, NULL, wait_nr);
}

int
io_uring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr)
{
	return io_uring_wait_cqe_nr(ring, cqe_ptr, 1);
}

int
io_uring_wait_cqe_nr(struct io_uring *ring, struct io_uring_cqe **cqe_ptr,
		     unsigned wait_nr)
{
	struct io_completion comp;
	unsigned ready;
	int ret;

	/* Reap anything already pending first. */
	reap_native(ring);
	for (;;) {
		ready = *ring->cq.ktail - *ring->cq.khead;
		if (ready >= wait_nr) {
			if (cqe_ptr != NULL)
				*cqe_ptr = &ring->cq.cqes[
					*ring->cq.khead & ring->cq.ring_mask];
			return 0;
		}
		/* Block indefinitely (NULL timeout): wait_cqe() must not give up
		 * on its own after a fixed interval. */
		ret = (int)syscall(SYS_ioevent, ring->ring_fd, NULL, 0,
				   &comp, 1, NULL);
		if (ret != 1)
			return -errno;
		post_cqe(ring, comp.tag,
			 comp.error ? (__s32)-comp.error : (__s32)comp.result,
			 (__u32)comp.flags);
	}
}

int
io_uring_peek_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr)
{
	reap_native(ring);
	if (*ring->cq.ktail == *ring->cq.khead)
		return -EAGAIN;
	*cqe_ptr = &ring->cq.cqes[*ring->cq.khead & ring->cq.ring_mask];
	return 0;
}

int
io_uring_peek_batch_cqe(struct io_uring *ring, struct io_uring_cqe **cqes,
			unsigned count)
{
	unsigned i, ready;

	reap_native(ring);
	ready = *ring->cq.ktail - *ring->cq.khead;
	if (ready > count)
		ready = count;
	for (i = 0; i < ready; i++)
		cqes[i] = &ring->cq.cqes[(*ring->cq.khead + i) &
					 ring->cq.ring_mask];
	return (int)ready;
}

int
io_uring_cqe_seen(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	(void)cqe;
	++*ring->cq.khead;
	return 0;
}

void
io_uring_cq_advance(struct io_uring *ring, unsigned nr)
{
	*ring->cq.khead += nr;
}

unsigned
io_uring_sq_ready(const struct io_uring *ring)
{
	return ring->sq.sqe_tail - ring->sq.sqe_head;
}

unsigned
io_uring_sq_space_left(const struct io_uring *ring)
{
	return ring->sq.ring_entries - (ring->sq.sqe_tail - ring->sq.sqe_head);
}

unsigned
io_uring_cq_ready(const struct io_uring *ring)
{
	return *ring->cq.ktail - *ring->cq.khead;
}

int
io_uring_setup(unsigned entries, struct io_uring_params *p)
{
	(void)entries; (void)p;
	errno = ENOSYS;
	return -1;
}

int
io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
	       unsigned flags, sigset_t *sig)
{
	(void)fd; (void)to_submit; (void)min_complete; (void)flags; (void)sig;
	errno = ENOSYS;
	return -1;
}

int
io_uring_enter2(int fd, unsigned to_submit, unsigned min_complete,
		unsigned flags, sigset_t *sig, size_t sz)
{
	(void)fd; (void)to_submit; (void)min_complete; (void)flags; (void)sig;
	(void)sz;
	errno = ENOSYS;
	return -1;
}

int
io_uring_register(int fd, unsigned opcode, const void *arg, unsigned nr_args)
{
	(void)fd; (void)opcode; (void)arg; (void)nr_args;
	errno = ENOSYS;
	return -1;
}

int
io_uring_major_version(void)
{
	return LIBURING_VERSION_MAJOR;
}

int
io_uring_minor_version(void)
{
	return LIBURING_VERSION_MINOR;
}

bool
io_uring_check_version(int major, int minor)
{
	return major > LIBURING_VERSION_MAJOR ||
	       (major == LIBURING_VERSION_MAJOR &&
		minor > LIBURING_VERSION_MINOR);
}
