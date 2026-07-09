/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * EXEC completion boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_exec.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_fence.h"
#include "nvgpu_future.h"
#include "nvgpu_proc.h"
#include "nvgpu_sched.h"

#include <sys/errno.h>
#include <sys/kernel.h>
#include <machine/atomic.h>
#include <sys/malloc.h>

static MALLOC_DEFINE(M_NVGPU_EXEC, "nvgpu_exec", "nvgpu exec future");

struct nvgpu_exec_future {
	struct nvgpu_future base;
	uint64_t seqno;
	uint32_t channel;
	uint32_t push_count;
	uint32_t poll_count;
};

static uint64_t nvgpu_exec_fake_seqno;

static struct nvgpu_future_result nvgpu_exec_fake_poll(
    struct nvgpu_future *future);
static void nvgpu_exec_fake_destroy(struct nvgpu_future *future);

int
nvgpu_exec_submit_fake(struct nvgpu_proc *proc,
    struct nvgpu_exec_submit_args *args)
{
	struct nvgpu_exec_future *exec;
	struct nvgpu_fence *done_fence;
	int error;

	if (args == NULL)
		return (EINVAL);
	done_fence = args->done_fence;
	args->done_fence = NULL;
	if (proc == NULL || done_fence == NULL) {
		if (done_fence == NULL)
			return (EINVAL);
		nvgpu_fence_release(done_fence);
		return (EINVAL);
	}
	exec = kmalloc(sizeof(*exec), M_NVGPU_EXEC, M_WAITOK | M_ZERO);
	exec->seqno = atomic_fetchadd_64(&nvgpu_exec_fake_seqno, 1) + 1;
	exec->channel = args->channel;
	exec->push_count = args->push_count;
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "fake exec submit seq=%ju proc=%p channel=%u pushes=%u waits=%u\n",
	    (uintmax_t)exec->seqno, proc, args->channel, args->push_count,
	    args->wait_count);
	error = nvgpu_future_spawn(&exec->base, proc, done_fence,
	    args->wait_fences, args->wait_count, nvgpu_exec_fake_poll,
	    nvgpu_exec_fake_destroy);
	if (error != 0)
		goto fail_future;
	return (0);

fail_future:
	nvgpu_future_finish(&exec->base, error);
	args->done_fence = NULL;
	return (error);
}

void
nvgpu_exec_complete_from_intr(struct nvgpu_device *gpu __unused)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "exec intr complete\n");
}

static struct nvgpu_future_result
nvgpu_exec_fake_poll(struct nvgpu_future *future)
{
	struct nvgpu_exec_future *exec;
	struct nvgpu_future_result result;

	exec = (struct nvgpu_exec_future *)future;
	if (exec->channel == NVGPU_EXEC_FAKE_ERROR_CHANNEL) {
		nvgpu_log(NVGPU_LOG_INFO,
		    "fake exec submit error seq=%ju\n",
		    (uintmax_t)exec->seqno);
		result.ready = true;
		result.result = EIO;
		return (result);
	}
	if (exec->channel == NVGPU_EXEC_FAKE_NEVER_READY_CHANNEL) {
		nvgpu_log(NVGPU_LOG_INFO,
		    "fake exec never-ready poll seq=%ju\n",
		    (uintmax_t)exec->seqno);
		result.ready = false;
		result.result = 0;
		return (result);
	}
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "fake exec poll seq=%ju proc=%p poll=%u channel=%u pushes=%u\n",
	    (uintmax_t)exec->seqno, future->proc, exec->poll_count,
	    exec->channel, exec->push_count);
	exec->poll_count++;
	result.ready = true;
	result.result = 0;
	return (result);
}

static void
nvgpu_exec_fake_destroy(struct nvgpu_future *future)
{
	struct nvgpu_exec_future *exec;

	exec = (struct nvgpu_exec_future *)future;
	nvgpu_log(NVGPU_LOG_DEBUG, "fake exec release seq=%ju\n",
	    (uintmax_t)exec->seqno);
	_kfree(exec, M_NVGPU_EXEC);
}
