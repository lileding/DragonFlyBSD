/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Stackless future primitive used by the nvgpu scheduler.
 */

#ifndef _NVGPU_FUTURE_H_
#define _NVGPU_FUTURE_H_

#include <sys/queue.h>
#include <sys/stdint.h>
#include <stdbool.h>

#include <linux/dma-fence.h>

struct nvgpu_proc;

struct nvgpu_future_result {
	bool ready;
	int result;
};

struct nvgpu_future_wait;
TAILQ_HEAD(nvgpu_future_wait_list, nvgpu_future_wait);

struct nvgpu_future {
	TAILQ_ENTRY(nvgpu_future) run_link;
	TAILQ_ENTRY(nvgpu_future) proc_link;
	struct nvgpu_future_result (*poll)(struct nvgpu_future *future);
	void (*release)(struct nvgpu_future *future);
	struct nvgpu_proc *proc;
	struct dma_fence *done_fence;
	struct nvgpu_future_wait_list waits;
	uint32_t wait_count;
	int wait_error;
	bool submitted;
	bool queued;
	bool polling;
	bool wake_pending;
	bool done;
};

/* Initialize a future with one caller-owned done_fence reference. */
void nvgpu_future_init(struct nvgpu_future *future, struct nvgpu_proc *proc,
    struct dma_fence *done_fence,
    struct nvgpu_future_result (*poll)(struct nvgpu_future *future),
    void (*release)(struct nvgpu_future *future));

/* Add one wait fence callback to the future, or consume an already-signaled fence. */
int nvgpu_future_add_wait(struct nvgpu_future *future,
    struct dma_fence *fence);

/* Complete a future and release all generic resources. */
void nvgpu_future_finish(struct nvgpu_future *future,
    struct nvgpu_future_result result);

/* Cancel all pending waits and drop the future-owned done_fence reference. */
void nvgpu_future_cancel(struct nvgpu_future *future, int error);

#endif /* _NVGPU_FUTURE_H_ */
