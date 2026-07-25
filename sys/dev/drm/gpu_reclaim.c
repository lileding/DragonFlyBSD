/*
 * Copyright (c) 2019 Matthew Dillon <dillon@backplane.com>
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
 * Deferred reclaim for objects published to lockless readers.
 *
 * A reader holds gpu_reclaim_token shared; a grace period is one exclusive
 * acquisition of that token, which by definition cannot complete until every
 * reader has let go.  Callers of call_rcu()/kfree_rcu() may be holding locks
 * or running from a callback, so they only queue the object: a worker thread
 * takes the token and runs the callbacks.
 *
 * This replaces a scheme that deferred every free by a full second and polled
 * ten times a second to notice, while its read side was a bare compiler
 * barrier that guarded nothing.
 */

#include <sys/types.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/spinlock.h>
#include <sys/spinlock2.h>

#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>

struct gpu_reclaim_elm {
	STAILQ_ENTRY(gpu_reclaim_elm) link;
	void	(*func)(struct rcu_head *arg);
	void	*ptr;
};

STAILQ_HEAD(gpu_reclaim_list, gpu_reclaim_elm);

struct lwkt_token gpu_reclaim_token =
	LWKT_TOKEN_INITIALIZER(gpu_reclaim_token);

static struct spinlock gpu_reclaim_spin =
	SPINLOCK_INITIALIZER(gpu_reclaim_spin, "gpurcl");
static struct gpu_reclaim_list gpu_reclaim_queue =
	STAILQ_HEAD_INITIALIZER(gpu_reclaim_queue);
static struct thread *gpu_reclaim_td;
static int gpu_reclaim_stop;

static void
gpu_reclaim_thread(void *arg)
{
	struct gpu_reclaim_list batch;
	struct gpu_reclaim_elm *elm;

	for (;;) {
		spin_lock(&gpu_reclaim_spin);
		while (STAILQ_EMPTY(&gpu_reclaim_queue) && !gpu_reclaim_stop)
			ssleep(&gpu_reclaim_queue, &gpu_reclaim_spin, 0,
			    "gpurcl", 0);
		if (gpu_reclaim_stop && STAILQ_EMPTY(&gpu_reclaim_queue)) {
			spin_unlock(&gpu_reclaim_spin);
			break;
		}
		STAILQ_INIT(&batch);
		STAILQ_CONCAT(&batch, &gpu_reclaim_queue);
		spin_unlock(&gpu_reclaim_spin);

		/*
		 * One exclusive acquisition is the grace period: it cannot be
		 * granted while any reader still holds the token shared.
		 */
		lwkt_gettoken(&gpu_reclaim_token);
		lwkt_reltoken(&gpu_reclaim_token);

		while ((elm = STAILQ_FIRST(&batch)) != NULL) {
			STAILQ_REMOVE_HEAD(&batch, link);
			if (elm->func != NULL)
				elm->func(elm->ptr);
			else
				kfree(elm->ptr);
			kfree(elm);
		}
	}
	gpu_reclaim_td = NULL;
	wakeup(&gpu_reclaim_td);
}

static void
gpu_reclaim_queue_one(void (*func)(struct rcu_head *), void *ptr)
{
	struct gpu_reclaim_elm *elm;

	elm = kmalloc(sizeof(*elm), M_DRM, M_INTWAIT | M_ZERO);
	elm->func = func;
	elm->ptr = ptr;

	spin_lock(&gpu_reclaim_spin);
	STAILQ_INSERT_TAIL(&gpu_reclaim_queue, elm, link);
	spin_unlock(&gpu_reclaim_spin);
	wakeup(&gpu_reclaim_queue);
}

void
__kfree_rcu(void *ptr)
{
	if (unlikely(gpu_reclaim_td == NULL)) {
		kfree(ptr);
		return;
	}
	gpu_reclaim_queue_one(NULL, ptr);
}

void
call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *))
{
	if (unlikely(gpu_reclaim_td == NULL)) {
		func(head);
		return;
	}
	gpu_reclaim_queue_one(func, head);
}

void
synchronize_rcu(void)
{
	lwkt_gettoken(&gpu_reclaim_token);
	lwkt_reltoken(&gpu_reclaim_token);
}

static int
gpu_reclaim_init(void *arg)
{
	kthread_create(gpu_reclaim_thread, NULL, &gpu_reclaim_td,
	    "gpu_reclaim");
	return 0;
}
SYSINIT(gpu_reclaim, SI_SUB_DRIVERS, SI_ORDER_ANY, gpu_reclaim_init, NULL);

static int
gpu_reclaim_uninit(void *arg)
{
	spin_lock(&gpu_reclaim_spin);
	gpu_reclaim_stop = 1;
	wakeup(&gpu_reclaim_queue);
	while (gpu_reclaim_td != NULL)
		ssleep(&gpu_reclaim_td, &gpu_reclaim_spin, 0, "gpurclx", 0);
	spin_unlock(&gpu_reclaim_spin);
	return 0;
}
SYSUNINIT(gpu_reclaim, SI_SUB_DRIVERS, SI_ORDER_ANY, gpu_reclaim_uninit, NULL);
