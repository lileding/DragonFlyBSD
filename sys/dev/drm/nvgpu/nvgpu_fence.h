/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private fence abstraction for nvgpu futures.
 */

#ifndef _NVGPU_FENCE_H_
#define _NVGPU_FENCE_H_

#include <stdbool.h>

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

/* Release a native dma_fence reference owned by this nvgpu fence. */
void nvgpu_fence_release(struct nvgpu_fence *fence);

bool nvgpu_fence_is_signaled(struct nvgpu_fence *fence);
int nvgpu_fence_error(struct nvgpu_fence *fence);

/* Register a callback on the fence.  The callback is one-shot. */
int nvgpu_fence_add_callback(struct nvgpu_fence *fence,
    nvgpu_fence_callback_t func, void *arg);

/* Set an optional error and signal the fence; the caller keeps its reference. */
int nvgpu_fence_signal(struct nvgpu_fence *fence, int error);

#endif /* _NVGPU_FENCE_H_ */
