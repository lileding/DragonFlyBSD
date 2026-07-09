/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * EXEC completion boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_EXEC_H_
#define _NVGPU_EXEC_H_

#include <sys/stdint.h>

struct nvgpu_fence;
struct nvgpu_device;
struct nvgpu_proc;

struct nvgpu_exec_submit_args {
	uint32_t channel;
	uint32_t push_count;
	struct nvgpu_fence *done_fence;
	struct nvgpu_fence **wait_fences;
	uint32_t wait_count;
};

#define NVGPU_EXEC_FAKE_ERROR_CHANNEL		0xfffffff0U
#define NVGPU_EXEC_FAKE_NEVER_READY_CHANNEL	0xfffffff1U

/*
 * Submit a synchronization-only fake EXEC future.
 *
 * args->done_fence is consumed by this function on both success and error.
 * args->wait_fences are borrowed for the duration of the call.
 */
int nvgpu_exec_submit_fake(struct nvgpu_proc *proc,
    struct nvgpu_exec_submit_args *args);

/* Handle one EXEC completion event.  gpu is borrowed; wake scheduler state, do not free futures inline. */
void nvgpu_exec_complete_from_intr(struct nvgpu_device *gpu);

#endif /* _NVGPU_EXEC_H_ */
