/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private dma_fence implementation for nvgpu futures.
 */

#ifndef _NVGPU_FENCE_H_
#define _NVGPU_FENCE_H_

struct dma_fence;
struct nvgpu_device;

/* Create one unsignaled dma_fence owned by the caller. */
struct dma_fence *nvgpu_fence_create(struct nvgpu_device *gpu,
    const char *timeline_name);

/* Set an optional error and signal the fence; the caller keeps its reference. */
int nvgpu_fence_signal(struct dma_fence *fence, int error);

#endif /* _NVGPU_FENCE_H_ */
