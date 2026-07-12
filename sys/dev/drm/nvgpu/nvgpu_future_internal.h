/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private future state shared only by future and scheduler implementations.
 */

#ifndef _NVGPU_FUTURE_INTERNAL_H_
#define _NVGPU_FUTURE_INTERNAL_H_

#include "nvgpu_future.h"

#include <sys/queue.h>
#include <sys/types.h>

struct nvgpu_future_state {
	LIST_ENTRY(nvgpu_future_state) registry_link;
	TAILQ_ENTRY(nvgpu_future_state) sched_link;
	struct nvgpu_future *future;
	struct nvgpu_future_result (*poll)(struct nvgpu_future *future);
	volatile u_int wait_count;
	volatile u_int wait_error;
	bool queued;
};

struct nvgpu_future_state *nvgpu_future_get_state(
	struct nvgpu_future *future);
/* Return the first positive producer errno observed by future's wait set. */
int nvgpu_future_get_wait_error(struct nvgpu_future *future);
void nvgpu_future_assert_empty(void);

#endif /* _NVGPU_FUTURE_INTERNAL_H_ */
