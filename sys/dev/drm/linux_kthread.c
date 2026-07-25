/*
 * Copyright (c) 2019 François Tigeot <ftigeot@wolfpond.org>
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

#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <sys/kthread.h>

/*
   All Linux threads/processes have an associated task_struct
   a kthread is a pure kernel thread without userland context
*/

#define KTHREAD_SHOULD_STOP 1
#define KTHREAD_SHOULD_PARK 2
#define KTHREAD_EXITED	    3

static void
linux_ktfn_wrapper(void *arg)
{
	struct task_struct *task = arg;

	task->kt_exitvalue = task->kt_fn(task->kt_fndata);

	/*
	 * Release the thread's reference before it leaves, so that no thread
	 * ever exits still pointing at a task_struct.  kthread_stop() owns the
	 * free and is waiting for this; nothing may touch task afterwards.
	 */
	task->dfly_td->td_linux_task = NULL;
	set_bit(KTHREAD_EXITED, &task->kt_flags);
	wakeup(task);
}

struct task_struct *
kthread_run(int (*lfn)(void *), void *data, const char *namefmt, ...)
{
	struct task_struct *task;
	struct thread *td;
	__va_list args;
	int ret;

	task = kzalloc(sizeof(*task), GFP_KERNEL);

	__va_start(args, namefmt);
	ret = kthread_alloc(linux_ktfn_wrapper, task, &td, namefmt, args);
	__va_end(args);
	if (ret) {
		kfree(task);
		return ERR_PTR(-ENOMEM);
	}

	task->dfly_td = td;
	td->td_linux_task = task;

	task->kt_fn = lfn;
	task->kt_fndata = data;
	spin_init(&task->kt_spin, "tspin1");

	/* Start the thread here */
	lwkt_schedule(td);

	return task;
}

bool
kthread_should_stop(void)
{
	return test_bit(KTHREAD_SHOULD_STOP, &current->kt_flags);
}

int
kthread_stop(struct task_struct *ts)
{
	int exitvalue;

	set_bit(KTHREAD_SHOULD_STOP, &ts->kt_flags);

	kthread_unpark(ts);
	wake_up_process(ts);

	/*
	 * Wait for the thread to run out of its function.  Arming the
	 * interlock before each test is what keeps the wakeup from the
	 * wrapper from being missed; the timeout is only a backstop.
	 */
	for (;;) {
		tsleep_interlock(ts, 0);
		if (test_bit(KTHREAD_EXITED, &ts->kt_flags))
			break;
		tsleep(ts, PINTERLOCKED, "kstop", hz);
	}
	crit_enter();
	tsleep_remove(curthread);
	crit_exit();

	/*
	 * The thread frees itself: kthread_alloc() installs kthread_exit() as
	 * the return handler, and lwkt_exit() hands the thread back to the
	 * object cache.  Only the task_struct is ours to release.
	 */
	exitvalue = ts->kt_exitvalue;
	kfree(ts);

	return exitvalue;
}

int
kthread_park(struct task_struct *ts)
{
	set_bit(KTHREAD_SHOULD_PARK, &ts->kt_flags);
	wake_up_process(ts);

	return ts->kt_exitvalue;
}

void
kthread_unpark(struct task_struct *ts)
{
	clear_bit(KTHREAD_SHOULD_PARK, &ts->kt_flags);
	lwkt_schedule(ts->dfly_td);
	wake_up_process(ts);
}

bool
kthread_should_park(void)
{
	return test_bit(KTHREAD_SHOULD_PARK, &current->kt_flags);
}

void
kthread_parkme(void)
{
	if (test_bit(KTHREAD_SHOULD_PARK, &current->kt_flags) == 0)
		return;

	lwkt_deschedule_self(curthread);
}
