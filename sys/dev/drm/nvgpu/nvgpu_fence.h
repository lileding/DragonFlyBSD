/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private fence abstraction for nvgpu futures.
 */

#ifndef _NVGPU_FENCE_H_
#define _NVGPU_FENCE_H_

#include <stdbool.h>
#include <sys/stdint.h>

struct dma_fence;
struct nvgpu_device;
struct nvgpu_fence;

typedef void (*nvgpu_fence_callback_t)(struct nvgpu_fence *fence, void *arg);

/* Create one unsignaled nvgpu fence owned by the caller. */
struct nvgpu_fence *nvgpu_fence_create(struct nvgpu_device *gpu,
    const char *timeline_name);

/* Convert a caller-owned dma_fence reference into an nvgpu fence reference. */
struct nvgpu_fence *nvgpu_fence_import_dma_ref(struct dma_fence *fence);

/* Return a new dma_fence reference for DRM syncobj publication. */
struct dma_fence *nvgpu_fence_get_dma_ref(struct nvgpu_fence *fence);

/* Add one explicit owner reference to a native nvgpu fence. */
void nvgpu_fence_addref(struct nvgpu_fence *fence);

/* Release a native dma_fence reference owned by this nvgpu fence. */
void nvgpu_fence_release(struct nvgpu_fence *fence);

/* Attach immutable EXEC producer metadata before publishing gpu_complete. */
void nvgpu_fence_set_exec_producer(struct nvgpu_fence *gpu_complete,
    uint32_t channel, struct nvgpu_fence *submitted);

/* Return an owned same-channel submit dependency or this GPU fence. */
struct nvgpu_fence *nvgpu_fence_hold_exec_wait(
    struct nvgpu_fence *gpu_complete, uint32_t channel);

bool nvgpu_fence_is_signaled(struct nvgpu_fence *fence);
int nvgpu_fence_error(struct nvgpu_fence *fence);

/* Register a callback on the fence.  The callback is one-shot. */
int nvgpu_fence_add_callback(struct nvgpu_fence *fence,
    nvgpu_fence_callback_t func, void *arg);

/* Set an optional error and signal the fence; the caller keeps its reference. */
int nvgpu_fence_signal(struct nvgpu_fence *fence, int error);

/* Wait for completion and return a positive errno from wait or producer. */
int nvgpu_fence_wait(struct nvgpu_fence *fence, bool interruptible);

#endif /* _NVGPU_FENCE_H_ */
