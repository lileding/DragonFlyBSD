/*
 * Copyright (c) 2014-2020 François Tigeot <ftigeot@wolfpond.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Deferred work, on top of taskqueue(9).
 */

#include <linux/workqueue.h>
#include <linux/slab.h>

struct workqueue_struct *system_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_power_efficient_wq;

/*
 * taskqueue counts repeated enqueues and reports the total; a work item only
 * cares that it ran.
 */
void
linux_work_fn(void *context, int pending)
{
	struct work_struct *work = context;

	work->func(work);
}

struct workqueue_struct *
_create_workqueue_common(const char *name, int flags)
{
	struct workqueue_struct *wq;
	int pri;

	wq = kmalloc(sizeof(*wq), M_DRM, M_WAITOK | M_ZERO);
	wq->tq = taskqueue_create(name, M_WAITOK, taskqueue_thread_enqueue,
	    &wq->tq);
	if (wq->tq == NULL) {
		kfree(wq);
		return NULL;
	}

	pri = (flags & WQ_HIGHPRI) ? TDPRI_INT_SUPPORT : TDPRI_KERN_DAEMON;
	if (taskqueue_start_threads(&wq->tq, 1, pri, -1, "%s", name) != 0) {
		taskqueue_free(wq->tq);
		kfree(wq);
		return NULL;
	}

	return wq;
}

void
destroy_workqueue(struct workqueue_struct *wq)
{
	if (wq == NULL)
		return;
	taskqueue_free(wq->tq);
	kfree(wq);
}

int
queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	work->wq = wq;
	return (taskqueue_enqueue(wq->tq, &work->task) == 0);
}

int
queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
    unsigned long delay)
{
	dwork->wq = wq;
	dwork->work.wq = wq;

	if (delay == 0)
		return queue_work(wq, &dwork->work);

	/*
	 * timeout_task carries its own queue, and unlike a work item it must
	 * be told which one before it is armed.
	 */
	TIMEOUT_TASK_INIT(wq->tq, &dwork->ttask, 0, linux_work_fn,
	    &dwork->work);
	return (taskqueue_enqueue_timeout(wq->tq, &dwork->ttask, delay) >= 0);
}

bool
cancel_work_sync(struct work_struct *work)
{
	struct workqueue_struct *wq = work->wq;
	int pending = 0;

	if (wq == NULL)
		return false;
	taskqueue_cancel(wq->tq, &work->task, &pending);
	taskqueue_drain(wq->tq, &work->task);

	return (pending != 0);
}

bool
cancel_delayed_work(struct delayed_work *dwork)
{
	struct workqueue_struct *wq = dwork->wq;
	int pending = 0;

	if (wq == NULL)
		return false;
	taskqueue_cancel_timeout(wq->tq, &dwork->ttask, &pending);

	return (pending != 0);
}

bool
cancel_delayed_work_sync(struct delayed_work *dwork)
{
	struct workqueue_struct *wq = dwork->wq;
	int pending = 0;

	if (wq == NULL)
		return false;
	taskqueue_cancel_timeout(wq->tq, &dwork->ttask, &pending);
	taskqueue_drain_timeout(wq->tq, &dwork->ttask);

	return (pending != 0);
}

bool
flush_work(struct work_struct *work)
{
	struct workqueue_struct *wq = work->wq;

	if (wq == NULL)
		return false;
	taskqueue_drain(wq->tq, &work->task);

	return true;
}

bool
flush_delayed_work(struct delayed_work *dwork)
{
	struct workqueue_struct *wq = dwork->wq;

	if (wq == NULL)
		return false;
	taskqueue_drain_timeout(wq->tq, &dwork->ttask);

	return true;
}

static void
linux_wq_barrier_fn(void *context, int pending)
{
	wakeup(context);
}

/*
 * Wait for everything already queued.  The queue is single-threaded, so a
 * barrier task that has run means every task ahead of it has run too.
 */
void
flush_workqueue(struct workqueue_struct *wq)
{
	struct task barrier;

	if (wq == NULL)
		return;

	TASK_INIT(&barrier, 0, linux_wq_barrier_fn, &barrier);
	taskqueue_enqueue(wq->tq, &barrier);
	taskqueue_drain(wq->tq, &barrier);
}

void
drain_workqueue(struct workqueue_struct *wq)
{
	flush_workqueue(wq);
}

bool
work_pending(struct work_struct *work)
{
	return (work->task.ta_pending != 0);
}

unsigned int
work_busy(struct work_struct *work)
{
	return (work->task.ta_pending != 0);
}

bool
delayed_work_pending(struct delayed_work *dwork)
{
	return (dwork->ttask.t.ta_pending != 0);
}

void
destroy_work_on_stack(struct work_struct *work)
{
}

void
destroy_delayed_work_on_stack(struct delayed_work *work)
{
}

static int
init_workqueues(void *arg)
{
	system_wq = alloc_workqueue("system_wq", 0, 1);
	system_highpri_wq = alloc_workqueue("system_highpri_wq", WQ_HIGHPRI, 1);
	system_long_wq = alloc_workqueue("system_long_wq", 0, 1);
	system_unbound_wq = alloc_workqueue("system_unbound_wq", WQ_UNBOUND, 1);
	system_power_efficient_wq = alloc_workqueue("system_power_efficient_wq",
	    0, 1);
	return 0;
}
SYSINIT(gpu_workqueue_init, SI_SUB_DRIVERS, SI_ORDER_MIDDLE,
    init_workqueues, NULL);

static int
destroy_workqueues(void *arg)
{
	destroy_workqueue(system_wq);
	destroy_workqueue(system_highpri_wq);
	destroy_workqueue(system_long_wq);
	destroy_workqueue(system_unbound_wq);
	destroy_workqueue(system_power_efficient_wq);
	return 0;
}
SYSUNINIT(gpu_workqueue_destroy, SI_SUB_DRIVERS, SI_ORDER_MIDDLE,
    destroy_workqueues, NULL);
