/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Stackless future interface for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_FUTURE_H_
#define _NVGPU_FUTURE_H_

#include <stdbool.h>
#include <sys/types.h>

struct nvgpu_fence;
struct nvgpu_future;

/* Result returned by one concrete future poll. */
struct nvgpu_future_result {
	int result;
	bool ready;
};

#define NVGPU_FUTURE_PENDING \
	((struct nvgpu_future_result){ .result = 0, .ready = false })

#define NVGPU_FUTURE_READY(value) \
	((struct nvgpu_future_result){ .result = (value), .ready = true })

/*
 * A future exposes only its consuming poll operation.
 *
 * Before returning pending, poll transfers ownership to exactly one waker.
 * Before returning ready, poll publishes its result, releases every concrete
 * resource, and frees itself.  The caller must not access future after poll.
 */
struct nvgpu_future {
	struct nvgpu_future_result (*poll)(struct nvgpu_future *future);
};

/*
 * Spawn a future after all borrowed wait fences have completed.
 *
 * On success this function consumes future.  The wait array and fence pointers
 * are borrowed for the call; private callback state retains required fence
 * references.  A zero-length wait set queues the future immediately.
 */
int nvgpu_future_spawn(struct nvgpu_future *future,
	struct nvgpu_fence **waits, size_t wait_count);

#endif /* _NVGPU_FUTURE_H_ */
