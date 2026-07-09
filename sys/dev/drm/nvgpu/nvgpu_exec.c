/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * EXEC completion boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_exec.h"
#include "nvdrm_nouveau_abi.h"
#include "nvdrm_sync.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_fence.h"
#include "nvgpu_future.h"
#include "nvgpu_proc.h"
#include "nvgpu_sched.h"

#include <linux/dma-fence.h>
#include <sys/errno.h>
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
static void nvgpu_exec_fake_release(struct nvgpu_future *future);

int
nvgpu_exec_submit_fake(struct nvgpu_proc *proc, struct drm_file *file,
    void *data)
{
	struct drm_nouveau_exec *req = data;
	struct nvgpu_exec_future *exec;
	struct dma_fence *done_fence;
	struct nvdrm_sync_signal_set signals;
	int error;

	if (proc == NULL || file == NULL || req == NULL)
		return (EINVAL);
	done_fence = nvgpu_fence_create(nvgpu_proc_get_gpu(proc), "exec");
	if (done_fence == NULL)
		return (ENOMEM);
	exec = kmalloc(sizeof(*exec), M_NVGPU_EXEC, M_WAITOK | M_ZERO);
	exec->seqno = ++nvgpu_exec_fake_seqno;
	exec->channel = req->channel;
	exec->push_count = req->push_count;
	nvgpu_future_init(&exec->base, proc, done_fence, nvgpu_exec_fake_poll,
	    nvgpu_exec_fake_release);
	error = nvdrm_sync_collect_waits(file, &exec->base, req->wait_count,
	    req->wait_ptr);
	if (error != 0)
		goto fail_future;
	error = nvdrm_sync_prepare_signals(file, req->sig_count, req->sig_ptr,
	    done_fence, &signals);
	if (error != 0)
		goto fail_future;
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "fake exec submit seq=%ju proc=%p channel=%u pushes=%u waits=%u sigs=%u wait_count=%u\n",
	    (uintmax_t)exec->seqno, proc, req->channel, req->push_count,
	    req->wait_count, req->sig_count, exec->base.wait_count);
	nvgpu_sched_submit_future(&exec->base);
	nvdrm_sync_publish_signals(&signals);
	nvdrm_sync_cleanup_signals(&signals);
	return (0);

fail_future:
	nvgpu_future_cancel(&exec->base, error);
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

	exec = container_of(future, struct nvgpu_exec_future, base);
	if (future->wait_error != 0) {
		nvgpu_log(NVGPU_LOG_INFO,
		    "fake exec wait error seq=%ju error=%d\n",
		    (uintmax_t)exec->seqno, future->wait_error);
		result.ready = true;
		result.result = future->wait_error;
		return (result);
	}
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "fake exec poll seq=%ju proc=%p poll=%u channel=%u pushes=%u\n",
	    (uintmax_t)exec->seqno, future->proc, exec->poll_count,
	    exec->channel, exec->push_count);
	if (exec->poll_count++ == 0) {
		nvgpu_sched_wake_future(future);
		result.ready = false;
		result.result = 0;
		return (result);
	}
	result.ready = true;
	result.result = 0;
	return (result);
}

static void
nvgpu_exec_fake_release(struct nvgpu_future *future)
{
	struct nvgpu_exec_future *exec;

	exec = container_of(future, struct nvgpu_exec_future, base);
	nvgpu_log(NVGPU_LOG_DEBUG, "fake exec release seq=%ju\n",
	    (uintmax_t)exec->seqno);
	_kfree(exec, M_NVGPU_EXEC);
}
