/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#ifndef _NVGPU_PROC_H_
#define _NVGPU_PROC_H_

#include "nvgpu_sched.h"

#include <sys/queue.h>
#include <sys/thread.h>

struct nvgpu_device;

/* Events are the only external control path into the proc LWKT. */
enum nvgpu_proc_event_type {
	NVGPU_PROC_EVENT_UNKNOWN = 1,
};

struct nvgpu_proc_event {
	TAILQ_ENTRY(nvgpu_proc_event) link;
	enum nvgpu_proc_event_type type;
};

TAILQ_HEAD(nvgpu_proc_event_queue, nvgpu_proc_event);

/* Per-open GPU process state.  The proc LWKT owns final lifetime after create succeeds. */
struct nvgpu_proc {
	struct nvgpu_device *gpu;
	struct thread *thread;
	struct lwkt_token token;
	struct nvgpu_proc_event_queue events;
	struct nvgpu_task_queue parked_tasks;
	struct nvgpu_task_queue active_tasks;
	bool idle;
	bool shutdown;
};

/* Create per-open GPU state and start its LWKT.  procp receives a borrowed event target. */
int nvgpu_proc_create(struct nvgpu_device *gpu, struct nvgpu_proc **procp);

/* Request async process teardown.  Final release runs on the proc LWKT. */
void nvgpu_proc_stop(struct nvgpu_proc *proc);

/* Return the borrowed physical GPU for this proc. */
struct nvgpu_device *nvgpu_proc_get_gpu(struct nvgpu_proc *proc);

#endif /* _NVGPU_PROC_H_ */
