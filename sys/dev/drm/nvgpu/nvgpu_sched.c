/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Device-global future scheduler for the native NVIDIA GPU driver.
 */

#include "nvgpu_future.h"
#include "nvgpu_sched.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_proc.h"

#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/thread.h>

TAILQ_HEAD(nvgpu_future_queue, nvgpu_future);

struct nvgpu_sched {
	struct nvgpu_device *gpu;
	struct thread **threads;
	struct lwkt_token token;
	struct nvgpu_future_queue active_tasks;
	u_int num_workers;
	int idle_workers;
	bool stopping;
};

static MALLOC_DEFINE(M_NVGPU_SCHED, "nvgpu_sched", "nvgpu scheduler");

static void nvgpu_sched_run(void *arg);

int
nvgpu_sched_start(struct nvgpu_device *gpu, struct nvgpu_sched **out)
{
	struct nvgpu_sched *sched;
	int error;
	int i;

	if (gpu == NULL)
		return (EINVAL);
	sched = kmalloc(sizeof(*sched), M_NVGPU_SCHED, M_WAITOK | M_ZERO);
	sched->gpu = gpu;
	sched->num_workers = 0;
	lwkt_token_init(&sched->token, "nvgpsd");
	TAILQ_INIT(&sched->active_tasks);
	sched->threads = kmalloc(sizeof(*sched->threads) * ncpus,
	    M_NVGPU_SCHED, M_WAITOK | M_ZERO);
	for (i = 0; i < ncpus; i++) {
		error = lwkt_create(nvgpu_sched_run, sched,
		    &sched->threads[i], NULL, TDF_NOSTART, i,
		    "nvgpu_sched/%d", i);
		if (error != 0) {
			nvgpu_sched_stop(sched);
			return (error);
		}
		lwkt_setpri_initial(sched->threads[i], TDPRI_KERN_DAEMON);
		lwkt_schedule(sched->threads[i]);
		sched->num_workers++;
	}
	*out = sched;
	return (0);
}

void
nvgpu_sched_stop(struct nvgpu_sched *sched)
{
	if (sched == NULL)
		return;
	lwkt_gettoken(&sched->token);
	sched->stopping = true;
	for (int i = 0; i < sched->num_workers; i++)
		wakeup(&sched->active_tasks);
	while (sched->num_workers != 0)
		tsleep(&sched->num_workers, 0, "nvgpsx", 0);
	lwkt_reltoken(&sched->token);
	lwkt_token_uninit(&sched->token);
	_kfree(sched->threads, M_NVGPU_SCHED);
	_kfree(sched, M_NVGPU_SCHED);
}

void
nvgpu_future_wake(struct nvgpu_future *future)
{
	struct nvgpu_sched *sched;

	if (future == NULL || future->proc == NULL)
		return;
	sched = nvgpu_device_get_sched(nvgpu_proc_get_device(future->proc));
	if (sched == NULL)
		return;
	lwkt_gettoken(&sched->token);
	if (sched->stopping) {
		lwkt_reltoken(&sched->token);
		nvgpu_future_finish(future, ENODEV);
		return;
	}
	TAILQ_INSERT_TAIL(&sched->active_tasks, future, run_link);
	wakeup(&sched->active_tasks);
	lwkt_reltoken(&sched->token);
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
			sched->idle_workers++;
			tsleep(&sched->active_tasks, 0, "nvgpsd", 0);
			sched->idle_workers--;
		}
		if (TAILQ_EMPTY(&sched->active_tasks) && sched->stopping)
			break;
		future = TAILQ_FIRST(&sched->active_tasks);
		TAILQ_REMOVE(&sched->active_tasks, future, run_link);
		lwkt_reltoken(&sched->token);

		result = future->poll(future);
		if (result.ready)
			nvgpu_future_finish(future, result.result);
	}

	lwkt_gettoken(&sched->token);
	sched->num_workers--;
	if (sched->num_workers == 0)
		wakeup(&sched->num_workers);
	lwkt_reltoken(&sched->token);
	lwkt_exit();
}
