/*
 * Copyright (c) 2015-2020 François Tigeot <ftigeot@wolfpond.org>
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

#include <linux/interrupt.h>
#include <linux/slab.h>

#include <sys/kthread.h>

/*
 * Each tasklet gets its own thread.  The single shared runner this replaces
 * serialized every engine's submission behind one thread; nothing in the
 * callers wanted that, and i915 raises one tasklet per engine.
 */
static void
tasklet_thread(void *arg)
{
	struct tasklet_struct *t = arg;

	lockmgr(&t->lock, LK_EXCLUSIVE);
	for (;;) {
		if (test_bit(TASKLET_IS_DYING, &t->state))
			break;

		if (atomic_read(&t->count) != 0 ||
		    !test_and_clear_bit(TASKLET_STATE_SCHED, &t->state)) {
			lksleep(t, &t->lock, 0, "tkidle", 0);
			continue;
		}

		set_bit(TASKLET_STATE_RUN, &t->state);
		lockmgr(&t->lock, LK_RELEASE);
		if (t->func)
			t->func(t->data);
		lockmgr(&t->lock, LK_EXCLUSIVE);
		clear_bit(TASKLET_STATE_RUN, &t->state);
		wakeup(&t->state);
	}
	t->td = NULL;
	wakeup(&t->td);
	lockmgr(&t->lock, LK_RELEASE);
}

void
tasklet_init(struct tasklet_struct *t,
	     void (*func)(unsigned long), unsigned long data)
{
	t->state = 0;
	t->func = func;
	t->data = data;
	atomic_set(&t->count, 0);
	lockinit(&t->lock, "ltskl", 0, 0);
	kthread_create(tasklet_thread, t, &t->td, "tasklet");
}

static void
tasklet_wake(struct tasklet_struct *t)
{
	lockmgr(&t->lock, LK_EXCLUSIVE);
	set_bit(TASKLET_STATE_SCHED, &t->state);
	wakeup(t);
	lockmgr(&t->lock, LK_RELEASE);
}

void
tasklet_schedule(struct tasklet_struct *t)
{
	tasklet_wake(t);
}

/*
 * Linux runs high-priority tasklets ahead of the rest.  With a thread per
 * tasklet there is no shared queue to jump, so this is the same call.
 */
void
tasklet_hi_schedule(struct tasklet_struct *t)
{
	tasklet_wake(t);
}

void
tasklet_kill(struct tasklet_struct *t)
{
	lockmgr(&t->lock, LK_EXCLUSIVE);
	set_bit(TASKLET_IS_DYING, &t->state);
	wakeup(t);
	while (t->td != NULL)
		lksleep(&t->td, &t->lock, 0, "tkkill", 0);
	lockmgr(&t->lock, LK_RELEASE);
}

int
tasklet_trylock(struct tasklet_struct *t)
{
	return !test_and_set_bit(TASKLET_STATE_RUN, &t->state);
}

void
tasklet_unlock(struct tasklet_struct *t)
{
	clear_bit(TASKLET_STATE_RUN, &t->state);
}

void
tasklet_unlock_wait(struct tasklet_struct *t)
{
	lockmgr(&t->lock, LK_EXCLUSIVE);
	while (test_bit(TASKLET_STATE_RUN, &t->state))
		lksleep(&t->state, &t->lock, 0, "tkwait", 0);
	lockmgr(&t->lock, LK_RELEASE);
}
