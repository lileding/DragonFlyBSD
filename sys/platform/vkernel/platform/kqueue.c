/*
 * Copyright (c) 2006 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * $DragonFly: src/sys/platform/vkernel/platform/kqueue.c,v 1.4 2007/06/17 16:46:17 dillon Exp $
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/systimer.h>
#include <sys/sysctl.h>
#include <sys/signal.h>
#include <sys/interrupt.h>
#include <sys/bus.h>
#include <sys/time.h>
#include <sys/event.h>
#include <sys/time.h>
#include <machine/cpu.h>
#include <machine/globaldata.h>
#include <machine/md_var.h>

#include <sys/thread2.h>

#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>

struct kqueue_info {
	void (*func)(void *, struct intrframe *);
	void *data;
	int fd;
};

int KQueueFd = -1;

/*
 * Initialize kqueue based I/O
 *
 * Use SIGIO to get an immediate event when the kqueue has something waiting
 * for us.  Setup the SIGIO signal as a mailbox signal for efficiency.
 *
 * Currently only read events are supported.
 */
void
init_kqueue(void)
{
	struct sigaction sa;

	bzero(&sa, sizeof(sa));
	sa.sa_mailbox = &mdcpu->gd_mailbox;
	sa.sa_flags = SA_MAILBOX | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGIO, &sa, NULL);
	KQueueFd = kqueue();
	if (fcntl(KQueueFd, F_SETOWN, getpid()) < 0)
		panic("Cannot configure kqueue for SIGIO, update your kernel");
	if (fcntl(KQueueFd, F_SETFL, O_ASYNC) < 0)
		panic("Cannot configure kqueue for SIGIO, update your kernel");
}

/*
 * A SIGIO mailbox event cause a system call interruption and a timely
 * poll.  If the mailbox is active we clean out all pending kqueue events.
 * It is really that simple.
 *
 * We specify EV_CLEAR for all events to ensure no requeues before their
 * time.  A new SIGIO is not generated unless all events are cleared from
 * the kqueue.
 */
void
signalmailbox(struct intrframe *frame)
{
	struct mdglobaldata *gd = mdcpu;
	struct timespec ts;
	struct kevent kevary[8];
	int n;
	int i;

	/*
	 * we only need to wake up our shutdown thread once.  
	 * Keep it non-zero so the shutdown thread can detect it.
	 */

	if (mdcpu->gd_shutdown > 0) {
		mdcpu->gd_shutdown = -1;
		wakeup(&mdcpu->gd_shutdown);
	}

	if (gd->gd_mailbox == 0)
		return;
	gd->gd_mailbox = 0;
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	crit_enter();
	do {
		n = kevent(KQueueFd, NULL, 0, kevary, 8, &ts);
		for (i = 0; i < n; ++i) {
			struct kevent *kev = &kevary[i];
			struct kqueue_info *info = (void *)kev->udata;

			info->func(info->data, frame);
		}
	} while (n == 8);
	crit_exit();
}

/*
 * Generic I/O event support
 */
struct kqueue_info *
kqueue_add(int fd, void (*func)(void *, struct intrframe *), void *data)
{
	struct timespec ts = { 0, 0 };
	struct kqueue_info *info;
	struct kevent kev;

	info = kmalloc(sizeof(*info), M_DEVBUF, M_ZERO|M_INTWAIT);
	info->func = func;
	info->data = data;
	info->fd = fd;
	EV_SET(&kev, fd, EVFILT_READ, EV_ADD|EV_ENABLE|EV_CLEAR, 0, 0, info);
	if (kevent(KQueueFd, &kev, 1, NULL, 0, &ts) < 0)
		panic("kqueue: kevent() call could not add descriptor");
	return(info);
}

/*
 * Medium resolution timer support
 */
struct kqueue_info *
kqueue_add_timer(void (*func)(void *, struct intrframe *), void *data)
{
	struct kqueue_info *info;

	info = kmalloc(sizeof(*info), M_DEVBUF, M_ZERO|M_INTWAIT);
	info->func = func;
	info->data = data;
	info->fd = (uintptr_t)info;
	return(info);
}

void
kqueue_reload_timer(struct kqueue_info *info, int ms)
{
	struct timespec ts = { 0, 0 };
	struct kevent kev;

	KKASSERT(ms > 0);

	EV_SET(&kev, info->fd, EVFILT_TIMER,
		EV_ADD|EV_ENABLE|EV_ONESHOT|EV_CLEAR, 0, (uintptr_t)ms, info);
	if (kevent(KQueueFd, &kev, 1, NULL, 0, &ts) < 0)
		panic("kqueue_reload_timer: Failed");
}

/*
 * Destroy a previously added kqueue event
 */
void
kqueue_del(struct kqueue_info *info)
{
	struct timespec ts = { 0, 0 };
	struct kevent kev;

	KKASSERT(info->fd >= 0);
	EV_SET(&kev, info->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	if (kevent(KQueueFd, &kev, 1, NULL, 0, &ts) < 0)
		panic("kevent: failed to delete descriptor %d", info->fd);
	info->fd = -1;
	kfree(info, M_DEVBUF);
}

