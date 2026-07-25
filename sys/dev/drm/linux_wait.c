/*
 * Copyright (c) 2019-2020 François Tigeot <ftigeot@wolfpond.org>
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

#include <linux/wait.h>
#include <linux/wait_bit.h>
#include <linux/sched.h>

int
default_wake_function(wait_queue_entry_t *q, unsigned mode, int wake_flags, void *key)
{
	return wake_up_process(q->private);
}

/*
 * Wake a waiter that sleeps on its own queue entry.
 *
 * wait_event() uses the entry address as its wait channel, so no task identity
 * is needed to wake it.  Driver-owned entries that still sleep on a task keep
 * using default_wake_function.
 */
int
wait_event_wake_function(wait_queue_entry_t *wait, unsigned mode, int wake_flags,
    void *key)
{
	wakeup(wait->private);
	return 1;
}

int
autoremove_wake_function(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
	int ret = default_wake_function(wait, mode, sync, key);

	/* Was the process woken up ? */
	if (ret)
		list_del_init(&wait->entry);

	return ret;
}

void
__wake_up_core(wait_queue_head_t *q, int num_to_wake_up)
{
	wait_queue_entry_t *curr, *next;
	int mode = TASK_NORMAL;

	list_for_each_entry_safe(curr, next, &q->head, entry) {
		if (curr->func(curr, mode, 0, NULL))
			num_to_wake_up--;

		if (num_to_wake_up == 0)
			break;
	}
}

void
__wait_event_prefix(wait_queue_head_t *wq, int flags)
{
	/*
	 * Serialize against wake_up() so that the caller observes every
	 * condition update that preceded a wakeup.  The sleep itself is
	 * interlocked on the caller's queue entry; no task state is
	 * involved.
	 */
	lockmgr(&wq->lock, LK_EXCLUSIVE);
	lockmgr(&wq->lock, LK_RELEASE);
}

/*
 * Queue the waiter and arm its wait channel.  Arming before the caller
 * re-examines its condition is what keeps a wakeup from being lost in the
 * window between the two.  Callers waiting on several queues arm the same
 * channel repeatedly, which is a no-op after the first.
 */
void
prepare_to_wait(wait_queue_head_t *q, wait_queue_entry_t *wait, int state)
{
	lockmgr(&q->lock, LK_EXCLUSIVE);
	if (list_empty(&wait->entry))
		__add_wait_queue(q, wait);
	tsleep_interlock(wait->private,
	    (state & TASK_INTERRUPTIBLE) ? PCATCH : 0);
	lockmgr(&q->lock, LK_RELEASE);
}

void
finish_wait(wait_queue_head_t *q, wait_queue_entry_t *wait)
{
	/* Drop an interlock the caller armed but never slept on. */
	crit_enter();
	tsleep_remove(curthread);
	crit_exit();

	lockmgr(&q->lock, LK_EXCLUSIVE);
	if (!list_empty(&wait->entry))
		list_del_init(&wait->entry);
	lockmgr(&q->lock, LK_RELEASE);
}

/*
 * Sleep on an armed wait entry.  Returns the jiffies remaining, at least 1 if
 * the sleep ended early, or 0 once the timeout is spent.  An indefinite
 * timeout is reported back unchanged.
 */
long
wait_chan_sleep(void *chan, long timeout, int flags)
{
	int start, error;

	if (timeout >= INT_MAX) {
		tsleep(chan, PINTERLOCKED | flags, "lwent", 0);
		return timeout;
	}

	start = ticks;
	error = tsleep(chan, PINTERLOCKED | flags, "lwent", (int)timeout);
	timeout -= (long)(ticks - start);
	if (timeout < 0)
		timeout = 0;
	if (error != EWOULDBLOCK && timeout == 0)
		timeout = 1;

	return timeout;
}

void
wait_chan_disarm(void)
{
	crit_enter();
	tsleep_remove(curthread);
	crit_exit();
}

long
wait_entry_sleep(wait_queue_entry_t *wait, long timeout, int flags)
{
	return wait_chan_sleep(wait->private, timeout, flags);
}

void
wake_up_bit(void *addr, int bit)
{
	wakeup_one(addr);
}

/* Wait for a bit to be cleared or a timeout to expire */
int
wait_on_bit_timeout(unsigned long *word, int bit, unsigned mode,
		    unsigned long timeout)
{
	int rv, awakened = 0, timeout_expired = 0;
	long start_time;

	if (!test_bit(bit, word))
		return 0;

	start_time = ticks;

	do {
		rv = tsleep(word, mode, "lwobt", timeout);
		if (rv == 0)
			awakened = 1;
		if (time_after_eq(start_time, timeout))
			timeout_expired = 1;
	} while (test_bit(bit, word) && !timeout_expired);

	if (awakened)
		return 0;

	return 1;
}

void __init_waitqueue_head(wait_queue_head_t *q,
			   const char *name, struct lock_class_key *key)
{
	lockinit(&q->lock, "lwq", 0, 0);
	INIT_LIST_HEAD(&q->head);
}

int
wait_on_bit(unsigned long *word, int bit, unsigned mode)
{
	return wait_on_bit_timeout(word, bit, mode, MAX_SCHEDULE_TIMEOUT);
}

void
init_wait_entry(struct wait_queue_entry *wq_entry, int flags)
{
	INIT_LIST_HEAD(&wq_entry->entry);
	wq_entry->flags = flags;
	wq_entry->private = wq_entry;
	wq_entry->func = autoremove_wake_function;
}
