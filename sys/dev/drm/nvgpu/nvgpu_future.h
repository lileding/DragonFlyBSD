/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Stackless future primitive used by the nvgpu scheduler.
 */

#ifndef _NVGPU_FUTURE_H_
#define _NVGPU_FUTURE_H_

#include <sys/queue.h>
#include <sys/stdint.h>
#include <sys/types.h>
#include <stdbool.h>

struct nvgpu_fence;
struct nvgpu_proc;

struct nvgpu_future_result {
	bool ready;
	int result;
};

struct nvgpu_future {
	TAILQ_ENTRY(nvgpu_future) run_link;
	TAILQ_ENTRY(nvgpu_future) proc_link;
	struct nvgpu_future_result (*poll)(struct nvgpu_future *future);
	void (*destroy)(struct nvgpu_future *future);
	struct nvgpu_proc *proc;
	struct nvgpu_fence *done_fence;
	uint32_t wait_count;
	u_int error;
};

/* Spawn a future with one caller-owned done_fence reference and borrowed wait fences. */
int nvgpu_future_spawn(struct nvgpu_future *future, struct nvgpu_proc *proc,
    struct nvgpu_fence *done_fence, struct nvgpu_fence **wait_fences,
    uint32_t wait_count,
    struct nvgpu_future_result (*poll)(struct nvgpu_future *future),
    void (*destroy)(struct nvgpu_future *future));

/* Queue or requeue a future that is ready to poll. */
void nvgpu_future_wake(struct nvgpu_future *future);

/* Complete a future and release all generic resources. */
void nvgpu_future_finish(struct nvgpu_future *future, int result);

#endif /* _NVGPU_FUTURE_H_ */
