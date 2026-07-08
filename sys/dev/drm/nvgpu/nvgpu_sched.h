/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Scheduler event boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_SCHED_H_
#define _NVGPU_SCHED_H_

#include <sys/queue.h>
#include <sys/types.h>

struct nvgpu_device;
struct nvgpu_proc;
struct nvgpu_sched;
struct nvgpu_future;

struct nvgpu_future_result {
	bool ready;
	int result;
};

struct nvgpu_future {
	TAILQ_ENTRY(nvgpu_future) link;
	struct nvgpu_future_result (*poll)(struct nvgpu_proc *proc,
	    struct nvgpu_future *future);
};

TAILQ_HEAD(nvgpu_future_queue, nvgpu_future);

struct nvgpu_sched {
	struct nvgpu_future_queue active_futures;
};

/* Initialize scheduler state embedded in one nvgpu_proc. */
void nvgpu_sched_init(struct nvgpu_sched *sched);

/* Poll every active future once from the owning proc LWKT. */
void nvgpu_sched_run(struct nvgpu_proc *proc);

/* Post an event to scheduler state.  Must be MPSAFE. */
void nvgpu_sched_post_event(struct nvgpu_device *gpu);

#endif /* _NVGPU_SCHED_H_ */
