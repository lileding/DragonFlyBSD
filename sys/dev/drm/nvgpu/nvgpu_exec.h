/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * EXEC completion boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_EXEC_H_
#define _NVGPU_EXEC_H_

#include <sys/stdint.h>

struct dma_fence;
struct nvgpu_fence;
struct nvgpu_device;
struct nvgpu_proc;

#define NVGPU_EXEC_PUSH_NO_PREFETCH	0x1u

struct nvgpu_exec_push {
	uint64_t va;
	uint32_t va_len;
	uint32_t flags;
};

struct nvgpu_exec_submit_args {
	uint32_t channel;
	const struct nvgpu_exec_push *pushes;
	uint32_t push_count;
	struct nvgpu_fence *gpu_complete_fence;
	struct dma_fence **wait_fences;
	uint32_t wait_count;
};

/* Allocate device-global pending completion state before scheduler startup. */
int nvgpu_exec_init(struct nvgpu_device *gpu);

/* Assert that all GPU completions drained and release global EXEC state. */
void nvgpu_exec_fini(struct nvgpu_device *gpu);

/*
 * Spawn one real EXEC future after the DRM shim copied userspace pushes.
 *
 * args->gpu_complete_fence is consumed on both success and error.
 * args->pushes and args->wait_fences are borrowed for the duration of the call.
 */
int nvgpu_exec_submit(struct nvgpu_proc *proc,
    struct nvgpu_exec_submit_args *args);

/* Harvest semaphore completions and signal their GPU-complete fences. */
void nvgpu_exec_harvest_completed(struct nvgpu_device *gpu);

/* Fail all pending submissions for one GSP channel after an RM fault. */
void nvgpu_exec_fail_channel(struct nvgpu_device *gpu, uint32_t chid,
    int error);

#endif /* _NVGPU_EXEC_H_ */
