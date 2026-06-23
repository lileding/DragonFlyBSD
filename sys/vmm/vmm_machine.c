/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.  Pure C, no kernel calls beyond
 * memcpy/memset (which libkern and the host C library both provide), so it
 * builds for the kernel module and for host unit tests.
 */
#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/lock.h>
#include <sys/proc.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <sys/wait.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#include "vmm_machine.h"

#define EV_CREATED	1
#define EV_STARTED	2
#define EV_STOPPED	3
#define EV_DELETED	4

#ifdef _KERNEL
static void	vmm_machine_start_child(void *arg, struct trapframe *frame);
static void	vmm_machine_start_task(struct vmm_machine *m);
#endif

/* --------------------------------------------------------------------- */
/* Event ring.                                                           */

static const char *
event_text(uint8_t code, size_t *len)
{
	switch (code) {
	case EV_CREATED: *len = 8; return "created\n";
	case EV_STARTED: *len = 8; return "started\n";
	case EV_STOPPED: *len = 8; return "stopped\n";
	case EV_DELETED: *len = 8; return "deleted\n";
	default:	 *len = 0; return "";
	}
}

static void
ev_push(struct vmm_machine *m, uint8_t code)
{
	size_t slot = (m->ev_tail + m->ev_count) % VMM_EVENT_CAP;

	m->ev_codes[slot] = code;
	if (m->ev_count < VMM_EVENT_CAP)
		m->ev_count++;
	else
		m->ev_tail = (m->ev_tail + 1) % VMM_EVENT_CAP;
}

/* --------------------------------------------------------------------- */
/* Machine model.                                                        */

void
vmm_machine_init(struct vmm_machine *m)
{
	memset(m, 0, sizeof(*m));
	vmm_console_init(&m->console);
#ifdef _KERNEL
	lockinit(&m->lifecycle_lock, "vmmmach", 0, 0);
#endif
	m->desired_stopped = 1;
	ev_push(m, EV_CREATED);
	ev_push(m, EV_STOPPED);
}

#ifdef _KERNEL
void
vmm_machine_uninit(struct vmm_machine *m)
{
	vmm_machine_request_stopped(m, 1);
	if (m->start_cred != NULL) {
		crfree(m->start_cred);
		m->start_cred = NULL;
	}
	vmm_vcpu_uninit(&m->vcpu);
	vmm_mem_release(&m->mem);
	lockuninit(&m->lifecycle_lock);
}

void
vmm_machine_set_owner(struct vmm_machine *m,
    const struct vmm_machine_owner_ops *ops, void *arg)
{
	m->owner_ops = ops;
	m->owner_arg = arg;
}

static void
vmm_machine_owner_hold(struct vmm_machine *m)
{
	if (m->owner_ops != NULL && m->owner_ops->hold != NULL)
		m->owner_ops->hold(m->owner_arg);
}

static void
vmm_machine_owner_release(struct vmm_machine *m)
{
	if (m->owner_ops != NULL && m->owner_ops->release != NULL)
		m->owner_ops->release(m->owner_arg);
}

static int
vmm_machine_start_is_cancelled(void *arg)
{
	struct vmm_machine *m = arg;
	int cancelled;

	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
	cancelled = vmm_machine_start_cancelled(m);
	lockmgr(&m->lifecycle_lock, LK_RELEASE);
	return cancelled;
}

int
vmm_machine_request_running(struct vmm_machine *m, struct ucred *cred,
    struct vmm_host *host)
{
	struct proc *worker;
	struct lwp *worker_lwp;
	int error = 0;
	int fork_worker = 0;

	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
	if (m->deleting) {
		error = ENXIO;
		goto out;
	}
	if (!m->desired_stopped) {
		error = ENOENT;
		goto out;
	}
	if (!vmm_machine_config_complete(m)) {
		error = EINVAL;
		goto out;
	}
	m->desired_stopped = 0;
	m->start_cancel = 0;
	vmm_vcpu_request_run(&m->vcpu);
	if (!m->running && !m->starting) {
		m->starting = 1;
		m->start_cred = crhold(cred);
		m->host = host;
		vmm_machine_owner_hold(m);
		fork_worker = 1;
	}
out:
	lockmgr(&m->lifecycle_lock, LK_RELEASE);

	if (fork_worker) {
		error = fork1(curthread->td_lwp, RFFDG | RFPROC | RFPGLOCK,
		    &worker);
		if (error) {
			lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
			m->starting = 0;
			m->host = NULL;
			if (m->start_cred != NULL) {
				crfree(m->start_cred);
				m->start_cred = NULL;
			}
			lockmgr(&m->lifecycle_lock, LK_RELEASE);
			vmm_machine_owner_release(m);
		} else {
			PHOLD(worker);
			worker_lwp = ONLY_LWP_IN_PROC(worker);
			cpu_set_fork_handler(worker_lwp,
			    vmm_machine_start_child, m);
			start_forked_proc(curthread->td_lwp, worker);
			PRELE(worker);
		}
	}
	return error;
}

void
vmm_machine_request_stopped(struct vmm_machine *m, int force)
{
	int release_now = 0;

	(void)force;
	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
	if (!m->desired_stopped) {
		m->desired_stopped = 1;
		m->start_cancel = 1;
	}
	vmm_vcpu_request_stop(&m->vcpu);
	if (m->running && !vmm_vcpu_has_active(&m->vcpu)) {
		m->running = 0;
		release_now = 1;
		ev_push(m, EV_STOPPED);
	}
	lockmgr(&m->lifecycle_lock, LK_RELEASE);

	wakeup(m);
	if (release_now)
		vmm_mem_release(&m->mem);
}

static void
vmm_machine_start_task(struct vmm_machine *m)
{
	struct ucred *cred;
	int error;
	int vcpu_owner = 0;
	int started = 0;
	int release_mem = 1;

	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
	cred = m->start_cred;
	m->start_cred = NULL;
	if (cred == NULL || vmm_machine_start_cancelled(m)) {
		m->starting = 0;
		lockmgr(&m->lifecycle_lock, LK_RELEASE);
		if (cred != NULL)
			crfree(cred);
		vmm_machine_owner_release(m);
		return;
	}
	lockmgr(&m->lifecycle_lock, LK_RELEASE);

	error = vmm_mem_prepare(&m->mem);
	if (error == 0 && !vmm_machine_start_is_cancelled(m)) {
		error = vmm_loader_run(&m->loader, &m->mem, cred,
		    vmm_machine_start_is_cancelled, m);
	}
	if (error == 0 && !vmm_machine_start_is_cancelled(m)) {
		vmm_machine_owner_hold(m);
		vcpu_owner = 1;
		error = vmm_vcpu_start_all(m, m->host);
	}

	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
	m->starting = 0;
	m->host = NULL;
	if (error == 0 && !vmm_machine_start_cancelled(m)) {
		vmm_machine_start(m);
		started = 1;
		release_mem = 0;
		vcpu_owner = 0;
	} else {
		vmm_vcpu_request_stop(&m->vcpu);
		if (vmm_vcpu_has_active(&m->vcpu)) {
			release_mem = 0;
			vcpu_owner = 0;
		}
	}
	lockmgr(&m->lifecycle_lock, LK_RELEASE);

	if (!started && release_mem)
		vmm_mem_release(&m->mem);
	if (vcpu_owner)
		vmm_machine_owner_release(m);
	crfree(cred);
	vmm_machine_owner_release(m);
}

int
vmm_machine_vcpu_should_stop(struct vmm_machine *m)
{
	int stop;

	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
	stop = m->desired_stopped || m->start_cancel || m->deleting ||
	    m->vcpu.stop_requested;
	lockmgr(&m->lifecycle_lock, LK_RELEASE);
	return stop;
}

void
vmm_machine_vcpu_exited(struct vmm_machine *m)
{
	int last;
	int release_mem = 0;
	int release_owner = 0;

	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
	last = vmm_vcpu_note_exit(&m->vcpu);
	if (last) {
		if (m->running) {
			m->running = 0;
			ev_push(m, EV_STOPPED);
		}
		release_mem = 1;
		release_owner = 1;
		wakeup(m);
	}
	lockmgr(&m->lifecycle_lock, LK_RELEASE);

	if (release_mem)
		vmm_mem_release(&m->mem);
	if (release_owner)
		vmm_machine_owner_release(m);
}

static void
vmm_machine_start_child(void *arg, struct trapframe *frame)
{
	(void)frame;
	vmm_machine_start_task(arg);
	exit1(W_EXITCODE(0, 0));
}
#endif

int
vmm_machine_config_complete(const struct vmm_machine *m)
{
	return vmm_vcpu_is_set(&m->vcpu) && vmm_mem_is_set(&m->mem) &&
	    vmm_loader_is_set(&m->loader);
}

int
vmm_machine_is_stopped(const struct vmm_machine *m)
{
	return m->desired_stopped;
}

int
vmm_machine_is_running(const struct vmm_machine *m)
{
	return m->running;
}

int
vmm_machine_starting(const struct vmm_machine *m)
{
	return m->starting;
}

int
vmm_machine_start_cancelled(const struct vmm_machine *m)
{
	return m->start_cancel || m->desired_stopped || m->deleting;
}

int
vmm_machine_request_start(struct vmm_machine *m)
{
	if (!m->desired_stopped)
		return 0;
	m->desired_stopped = 0;
	m->start_cancel = 0;
	return 1;
}

int
vmm_machine_start_worker_begin(struct vmm_machine *m)
{
	if (vmm_machine_start_cancelled(m) || m->running || m->starting)
		return 0;
	m->starting = 1;
	return 1;
}

void
vmm_machine_start_worker_done(struct vmm_machine *m, int started)
{
	m->starting = 0;
	if (started && !vmm_machine_start_cancelled(m))
		vmm_machine_start(m);
}

void
vmm_machine_stop(struct vmm_machine *m, int force)
{
	(void)force;
	if (!m->desired_stopped) {
		m->desired_stopped = 1;
		m->start_cancel = 1;
	}
	if (m->running) {
		m->running = 0;
		ev_push(m, EV_STOPPED);
	}
}

void
vmm_machine_start(struct vmm_machine *m)
{
	if (!m->running && !vmm_machine_start_cancelled(m)) {
		m->running = 1;
		ev_push(m, EV_STARTED);
	}
}

int
vmm_machine_is_deleting(const struct vmm_machine *m)
{
	return m->deleting;
}

int
vmm_machine_lease_open(struct vmm_machine *m)
{
	if (m->deleting)
		return 0;
	m->lease_count++;
	m->armed = 1;
	return 1;
}

enum vmm_close_action
vmm_machine_lease_close(struct vmm_machine *m)
{
	if (m->lease_count > 0)
		m->lease_count--;
	if (m->armed && m->lease_count == 0 && !m->deleting) {
		m->deleting = 1;
		ev_push(m, EV_DELETED);
		return VMM_CLOSE_DELETE;
	}
	return VMM_CLOSE_NONE;
}

int
vmm_machine_begin_delete(struct vmm_machine *m)
{
#ifdef _KERNEL
	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
#endif
	if (m->deleting)
#ifdef _KERNEL
	{
		lockmgr(&m->lifecycle_lock, LK_RELEASE);
		return 0;
	}
#else
		return 0;
#endif
	m->deleting = 1;
	m->desired_stopped = 1;
	m->start_cancel = 1;
#ifdef _KERNEL
	vmm_vcpu_request_stop(&m->vcpu);
	if (m->running && !vmm_vcpu_has_active(&m->vcpu))
		m->running = 0;
#else
	m->running = 0;
#endif
	ev_push(m, EV_DELETED);
#ifdef _KERNEL
	lockmgr(&m->lifecycle_lock, LK_RELEASE);
	wakeup(m);
#endif
	return 1;
}

int
vmm_machine_events_pending(const struct vmm_machine *m)
{
	return m->ev_count > 0;
}

size_t
vmm_machine_read_events(struct vmm_machine *m, char *out, size_t cap)
{
	size_t n = 0;

	while (m->ev_count > 0) {
		size_t tlen;
		const char *t = event_text(m->ev_codes[m->ev_tail], &tlen);

		if (n + tlen > cap)
			break;
		memcpy(out + n, t, tlen);
		n += tlen;
		m->ev_tail = (m->ev_tail + 1) % VMM_EVENT_CAP;
		m->ev_count--;
	}
	return n;
}
