/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proc-owned GPU channel interface.
 */

#ifndef _NVGPU_CHANNEL_H_
#define _NVGPU_CHANNEL_H_

#include <sys/stdint.h>
#include <sys/types.h>

#define NVGPU_CHANNEL_GPFIFO_ENTRIES	512u
#define NVGPU_CHANNEL_PUSH_NO_PREFETCH	0x1u

struct nvgpu_channel;
struct nvgpu_fence;
struct nvgpu_future;
struct nvgpu_proc;
struct nvgpu_sema;

/* One native GPFIFO push adapted from the nouveau EXEC payload. */
struct nvgpu_channel_push {
	uint64_t va;
	uint32_t va_len;
	uint32_t flags;
};

/*
 * Native channel creation request and result.
 *
 * proc and the structure are borrowed for the call.  The channel module fills
 * the result fields only after the proc owns the newly created channel.
 */
struct nvgpu_channel_create_args {
	struct nvgpu_proc *proc;
	uint32_t fb_ctxdma_handle;
	uint32_t tt_ctxdma_handle;
	int32_t channel_id;
	uint32_t pushbuf_domains;
	uint32_t notifier_handle;
	uint32_t nr_subchan;
};

/* Native data consumed by one atomic channel submission transaction. */
struct nvgpu_channel_submit_args {
	const struct nvgpu_channel_push *pushes;
	size_t push_count;
	struct nvgpu_fence *submitted;
	struct nvgpu_sema *sema;
};

/*
 * Create a user channel and transfer its ownership to args->proc.
 *
 * On success result receives a borrowed pointer valid until proc finalization.
 * This function may sleep while creating the GSP channel and backing VM.
 */
int nvgpu_channel_create(struct nvgpu_channel_create_args *args,
	struct nvgpu_channel **result);

/*
 * Destroy one proc-owned channel after all futures borrowing it are gone.
 * The call consumes the channel and may sleep while releasing GSP objects.
 */
void nvgpu_channel_destroy(struct nvgpu_channel *channel);

/*
 * Atomically write one FIFO submission, park future, and ring the doorbell.
 *
 * This function is MPSAFE.  On zero interrupt state owns future.  EAGAIN means
 * the FIFO was full and caller still owns future; no GPU work was published.
 * Every other error also leaves future with caller and no submission in flight.
 */
int nvgpu_channel_submit(struct nvgpu_channel *channel,
	struct nvgpu_channel_submit_args *args,
	struct nvgpu_future *future);

#endif /* _NVGPU_CHANNEL_H_ */
