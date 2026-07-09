/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Device-global future scheduler for the native NVIDIA GPU driver.
 */

#include "nvgpu_sched.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_proc.h"

#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/thread.h>

TAILQ_HEAD(nvgpu_future_queue, nvgpu_future);

struct nvgpu_sched {
	struct nvgpu_device *gpu;
	struct thread *thread;
	struct lwkt_token token;
	struct nvgpu_future_queue active_tasks;
	bool stopping;
	bool exited;
	bool idle;
};

static MALLOC_DEFINE(M_NVGPU_SCHED, "nvgpu_sched", "nvgpu scheduler");

static void nvgpu_sched_run(void *arg);
static void nvgpu_sched_enqueue_locked(struct nvgpu_sched *sched,
    struct nvgpu_future *future);
static struct nvgpu_sched *nvgpu_sched_from_future(struct nvgpu_future *future);

int
nvgpu_sched_start(struct nvgpu_device *gpu)
{
	struct nvgpu_sched *sched;
	int error;

	if (gpu == NULL)
		return (EINVAL);
	sched = kmalloc(sizeof(*sched), M_NVGPU_SCHED, M_WAITOK | M_ZERO);
	sched->gpu = gpu;
	lwkt_token_init(&sched->token, "nvgpsd");
	TAILQ_INIT(&sched->active_tasks);
	error = kthread_create(nvgpu_sched_run, sched, &sched->thread,
	    "nvgpu-sched");
	if (error != 0) {
		lwkt_token_uninit(&sched->token);
		_kfree(sched, M_NVGPU_SCHED);
		return (error);
	}
	nvgpu_device_set_sched(gpu, sched);
	return (0);
}

void
nvgpu_sched_stop(struct nvgpu_device *gpu)
{
	struct nvgpu_sched *sched;

	sched = nvgpu_device_get_sched(gpu);
	if (sched == NULL)
		return;
	lwkt_gettoken(&sched->token);
	sched->stopping = true;
	if (sched->idle)
		wakeup(&sched->active_tasks);
	while (!sched->exited)
		tsleep(&sched->active_tasks, 0, "nvgpsx", 0);
	lwkt_reltoken(&sched->token);
	nvgpu_device_set_sched(gpu, NULL);
	lwkt_token_uninit(&sched->token);
	_kfree(sched, M_NVGPU_SCHED);
}

void
nvgpu_sched_submit_future(struct nvgpu_future *future)
{
	struct nvgpu_sched *sched;

	sched = nvgpu_sched_from_future(future);
	if (sched == NULL)
		return;
	lwkt_gettoken(&sched->token);
	future->submitted = true;
	if (future->wait_count == 0)
		nvgpu_sched_enqueue_locked(sched, future);
	lwkt_reltoken(&sched->token);
}

void
nvgpu_sched_wake_future(struct nvgpu_future *future)
{
	struct nvgpu_sched *sched;

	sched = nvgpu_sched_from_future(future);
	if (sched == NULL)
		return;
	lwkt_gettoken(&sched->token);
	if (future->wait_count == 0)
		nvgpu_sched_wake_future_locked(future);
	lwkt_reltoken(&sched->token);
}

void
nvgpu_sched_lock_future(struct nvgpu_future *future)
{
	struct nvgpu_sched *sched;

	sched = nvgpu_sched_from_future(future);
	if (sched != NULL)
		lwkt_gettoken(&sched->token);
}

void
nvgpu_sched_unlock_future(struct nvgpu_future *future)
{
	struct nvgpu_sched *sched;

	sched = nvgpu_sched_from_future(future);
	if (sched != NULL)
		lwkt_reltoken(&sched->token);
}

void
nvgpu_sched_wake_future_locked(struct nvgpu_future *future)
{
	struct nvgpu_sched *sched;

	sched = nvgpu_sched_from_future(future);
	if (sched == NULL)
		return;
	if (!future->submitted || future->done)
		return;
	if (future->queued)
		return;
	if (future->polling) {
		future->wake_pending = true;
		return;
	}
	nvgpu_sched_enqueue_locked(sched, future);
}

void
nvgpu_sched_post_event(struct nvgpu_device *gpu __unused)
{
}

static void
nvgpu_sched_run(void *arg)
{
	struct nvgpu_sched *sched = arg;
	struct nvgpu_future *future;
	struct nvgpu_future_result result;
	bool finish;

	for (;;) {
		lwkt_gettoken(&sched->token);
		while (TAILQ_EMPTY(&sched->active_tasks) && !sched->stopping) {
			sched->idle = true;
			tsleep(&sched->active_tasks, 0, "nvgpsd", 0);
			sched->idle = false;
		}
		if (TAILQ_EMPTY(&sched->active_tasks) && sched->stopping)
			break;
		future = TAILQ_FIRST(&sched->active_tasks);
		TAILQ_REMOVE(&sched->active_tasks, future, run_link);
		future->queued = false;
		future->polling = true;
		lwkt_reltoken(&sched->token);

		result = future->poll(future);

		lwkt_gettoken(&sched->token);
		future->polling = false;
		finish = result.ready;
		if (!finish && future->wake_pending) {
			future->wake_pending = false;
			nvgpu_sched_enqueue_locked(sched, future);
		}
		lwkt_reltoken(&sched->token);

		if (finish)
			nvgpu_future_finish(future, result);
	}
	sched->exited = true;
	wakeup(&sched->active_tasks);
	lwkt_reltoken(&sched->token);
	kthread_exit();
}

static void
nvgpu_sched_enqueue_locked(struct nvgpu_sched *sched,
    struct nvgpu_future *future)
{
	if (future->queued || future->done)
		return;
	future->queued = true;
	TAILQ_INSERT_TAIL(&sched->active_tasks, future, run_link);
	wakeup(&sched->active_tasks);
}

static struct nvgpu_sched *
nvgpu_sched_from_future(struct nvgpu_future *future)
{
	struct nvgpu_device *gpu;

	if (future == NULL || future->proc == NULL)
		return (NULL);
	gpu = nvgpu_proc_get_gpu(future->proc);
	return (nvgpu_device_get_sched(gpu));
}
