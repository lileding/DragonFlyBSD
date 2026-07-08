/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 *
 * This file owns the process LWKT that will later receive all per-open GPU
 * work as events.  This first stage intentionally creates no VMM or channel.
 */

#include "nvgpu_proc.h"
#include "nvgpu_channel.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_sched.h"
#include "nvgpu_unload.h"
#include "nvgpu_vm.h"

#include <sys/param.h>
#include <sys/globaldata.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/systm.h>

static MALLOC_DEFINE(M_NVGPU_PROC, "nvgpu_proc", "nvgpu process state");

static void nvgpu_proc_run(void *arg);
static void nvgpu_proc_handle_event(struct nvgpu_proc *proc,
    struct nvgpu_proc_event *event);

/* Return the borrowed physical GPU for this proc. */
struct nvgpu_device *
nvgpu_proc_get_gpu(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return (NULL);
	return (proc->gpu);
}

/* Create per-open GPU state and start its LWKT.  procp receives a borrowed event target. */
int
nvgpu_proc_create(struct nvgpu_device *gpu, struct nvgpu_proc **procp)
{
	struct nvgpu_proc *proc;
	int error;

	if (gpu == NULL || procp == NULL)
		return (EINVAL);

	proc = kmalloc(sizeof(*proc), M_NVGPU_PROC, M_WAITOK | M_ZERO);

	proc->gpu = gpu;
	lwkt_token_init(&proc->token, "nvgprc");
	TAILQ_INIT(&proc->events);
	TAILQ_INIT(&proc->parked_tasks);
	TAILQ_INIT(&proc->active_tasks);
	TAILQ_INIT(&proc->channels);
	proc->vm = NULL;
	proc->idle = false;
	proc->shutdown = false;

	error = lwkt_create(nvgpu_proc_run, proc, &proc->thread, NULL,
	    TDF_NOSTART, mycpu->gd_cpuid, "nvgpu_proc");
	if (error != 0) {
		lwkt_token_uninit(&proc->token);
		_kfree(proc, M_NVGPU_PROC);
		return (error);
	}
	lwkt_setpri_initial(proc->thread, TDPRI_KERN_USER);
	lwkt_schedule(proc->thread);

	*procp = proc;
	nvgpu_log(NVGPU_LOG_DEBUG, "proc created proc=%p\n", proc);
	return (0);
}

/* Request async process teardown.  Final release runs on the proc LWKT. */
void
nvgpu_proc_stop(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return;

	lwkt_gettoken(&proc->token);
	proc->shutdown = true;
	if (proc->idle)
		wakeup(proc);
	lwkt_reltoken(&proc->token);
	nvgpu_log(NVGPU_LOG_DEBUG, "proc stop posted proc=%p\n", proc);
}

/* Handle one event after it has been removed from the proc queue. */
static void
nvgpu_proc_handle_event(struct nvgpu_proc *proc,
    struct nvgpu_proc_event *event)
{
	switch (event->type) {
	default:
		nvgpu_log(NVGPU_LOG_INFO,
		    "unknown proc event type=%d proc=%p\n", event->type, proc);
		break;
	}
	_kfree(event, M_NVGPU_PROC);
}

/* Run the cooperative per-open proc loop.  The LWKT owns final proc lifetime. */
static void
nvgpu_proc_run(void *arg)
{
	struct nvgpu_proc *proc = arg;
	struct nvgpu_proc_event *event;

	for (;;) {
		nvgpu_sched_run(proc);
		// no active task now

		lwkt_gettoken(&proc->token);
		event = TAILQ_FIRST(&proc->events);
		while (event == NULL) {
			if (proc->shutdown && TAILQ_EMPTY(&proc->parked_tasks))
				goto shutdown;

			// idle now, wait for next interrupt
			proc->idle = true;
			tsleep_interlock(proc, 0);
			(void)tsleep(proc, PINTERLOCKED, "nvgpup", hz / 10);
			proc->idle = false;
		    event = TAILQ_FIRST(&proc->events);
		}
		TAILQ_REMOVE(&proc->events, event, link);
		lwkt_reltoken(&proc->token);
		nvgpu_proc_handle_event(proc, event);
	}

shutdown:
	lwkt_reltoken(&proc->token);
	nvgpu_channel_destroy_all(proc);
	nvgpu_vm_destroy(proc);
	nvgpu_unload_release_by_drm(proc->gpu);
	nvgpu_log(NVGPU_LOG_DEBUG, "proc destroy proc=%p\n", proc);
	lwkt_token_uninit(&proc->token);
	_kfree(proc, M_NVGPU_PROC);
	lwkt_exit();
}
