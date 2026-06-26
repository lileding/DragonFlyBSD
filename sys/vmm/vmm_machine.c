/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/thread2.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <sys/wait.h>
#include <vm/vm_object.h>

#include "vmm_machine.h"
#include "vmm_loader_x86.h"

#define EV_CREATED	1
#define EV_STARTED	2
#define EV_STOPPED	3
#define EV_DELETED	4

static void	vmm_machine_start_child(void *arg, struct trapframe *frame);
static void	vmm_machine_start_task(struct vmm_machine *m);
static int	vmm_machine_config_complete_locked(const struct vmm_machine *m);
static void	vmm_machine_start_locked(struct vmm_machine *m);
static int	vmm_machine_wait_for_vcpu_drain(struct vmm_machine *m);
static struct vmm_mem_backing *vmm_machine_detach_mem_locked(
		    struct vmm_machine *m);
static int	vmm_machine_begin_delete_locked(struct vmm_machine *m);

void
vmm_machine_lock(struct vmm_machine *m)
{
	lwkt_gettoken(&m->token_lifecycle);
}

void
vmm_machine_unlock(struct vmm_machine *m)
{
	lwkt_reltoken(&m->token_lifecycle);
}

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
	size_t slot = (m->mut_ev_tail + m->mut_ev_count) % VMM_EVENT_CAP;

	m->mut_ev_codes[slot] = code;
	if (m->mut_ev_count < VMM_EVENT_CAP)
		m->mut_ev_count++;
	else
		m->mut_ev_tail = (m->mut_ev_tail + 1) % VMM_EVENT_CAP;
}

/* --------------------------------------------------------------------- */
/* Machine model.                                                        */

void
vmm_machine_init(struct vmm_machine *m)
{
	memset(m, 0, sizeof(*m));
	vmm_console_init(&m->own_mut_console);
	lwkt_token_init(&m->token_lifecycle, "vmmmach");
	m->mut_desired_stopped = 1;
	ev_push(m, EV_CREATED);
	ev_push(m, EV_STOPPED);
}

void
vmm_machine_uninit(struct vmm_machine *m)
{
	struct vmm_mem_backing *backing;
	struct vmm_vcpu_thread *threads = NULL;
	uint32_t thread_count;

	vmm_machine_request_stopped(m, 1);
	vmm_machine_lock(m);
	thread_count = m->own_mut_vcpu.mut_count;
	if (m->ref_mut_start_cred != NULL) {
		crfree(m->ref_mut_start_cred);
		m->ref_mut_start_cred = NULL;
	}
	vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
	backing = vmm_machine_detach_mem_locked(m);
	vmm_machine_unlock(m);
	vmm_vcpu_release_threads(threads, thread_count);
	vmm_mem_release_backing(backing);
}

void
vmm_machine_set_owner(struct vmm_machine *m,
    const struct vmm_machine_owner_ops *ops, void *arg)
{
	m->borrow_imm_owner_ops = ops;
	m->borrow_imm_owner_arg = arg;
}

static void
vmm_machine_owner_hold(struct vmm_machine *m)
{
	if (m->borrow_imm_owner_ops != NULL &&
	    m->borrow_imm_owner_ops->hold != NULL)
		m->borrow_imm_owner_ops->hold(m->borrow_imm_owner_arg);
}

static void
vmm_machine_owner_release(struct vmm_machine *m)
{
	if (m->borrow_imm_owner_ops != NULL &&
	    m->borrow_imm_owner_ops->release != NULL)
		m->borrow_imm_owner_ops->release(m->borrow_imm_owner_arg);
}

static int
vmm_machine_start_is_cancelled(void *arg)
{
	struct vmm_machine *m = arg;
	int cancelled;

	vmm_machine_lock(m);
	cancelled = vmm_machine_start_cancelled_locked(m);
	vmm_machine_unlock(m);
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
	int vcpu_active;

	vmm_machine_lock(m);
	if (m->mut_deleting) {
		error = ENXIO;
		goto out;
	}
	if (!m->mut_desired_stopped) {
		error = ENOENT;
		goto out;
	}
	if (!vmm_machine_config_complete_locked(m)) {
		error = EINVAL;
		goto out;
	}
	m->mut_desired_stopped = 0;
	m->mut_start_cancel = 0;
	vcpu_active = vmm_vcpu_has_active(&m->own_mut_vcpu);
	if (!vcpu_active)
		vmm_vcpu_request_run(&m->own_mut_vcpu);
	if (!m->mut_starting && (!m->mut_running || vcpu_active)) {
		m->mut_starting = 1;
		m->ref_mut_start_cred = crhold(cred);
		m->borrow_mut_host = host;
		fork_worker = 1;
	}
out:
	vmm_machine_unlock(m);

	if (fork_worker) {
		vmm_machine_owner_hold(m);
		error = fork1(curthread->td_lwp, RFFDG | RFPROC | RFPGLOCK,
		    &worker);
		if (error) {
			vmm_machine_lock(m);
			m->mut_starting = 0;
			m->borrow_mut_host = NULL;
			if (m->ref_mut_start_cred != NULL) {
				crfree(m->ref_mut_start_cred);
				m->ref_mut_start_cred = NULL;
			}
			vmm_machine_unlock(m);
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
	struct vmm_mem_backing *backing = NULL;
	int release_now = 0;

	(void)force;
	vmm_machine_lock(m);
	if (!m->mut_desired_stopped) {
		m->mut_desired_stopped = 1;
		m->mut_start_cancel = 1;
	}
	vmm_vcpu_request_stop(&m->own_mut_vcpu);
	if (m->mut_running && !vmm_vcpu_has_active(&m->own_mut_vcpu)) {
		m->mut_running = 0;
		release_now = 1;
		ev_push(m, EV_STOPPED);
	}
	if (release_now)
		backing = vmm_machine_detach_mem_locked(m);
	vmm_machine_unlock(m);

	wakeup(m);
	if (release_now)
		vmm_mem_release_backing(backing);
}

static void
vmm_machine_start_task(struct vmm_machine *m)
{
	struct ucred *cred;
	struct vmm_host *host;
	struct vmm_mem_backing *backing = NULL;
	struct vmm_mem_backing *prepared_backing = NULL;
	struct vmm_launch launch;
	struct vm_object *mem_object = NULL;
	char loader_path[VMM_LOADER_MAX + 1];
	uint64_t mem_bytes;
	uint64_t mem_size = 0;
	size_t loader_path_len;
	int error;
	int vcpu_owner = 0;
	int started = 0;
	int release_mem = 1;

	vmm_machine_lock(m);
	cred = m->ref_mut_start_cred;
	m->ref_mut_start_cred = NULL;
	host = m->borrow_mut_host;
	if (cred == NULL || vmm_machine_start_cancelled_locked(m)) {
		m->mut_starting = 0;
		vmm_machine_unlock(m);
		wakeup(m);
		if (cred != NULL)
			crfree(cred);
		vmm_machine_owner_release(m);
		return;
	}
	loader_path_len = vmm_loader_path(&m->own_mut_loader, loader_path,
	    VMM_LOADER_MAX);
	mem_bytes = m->own_mut_mem.mut_bytes;
	if (loader_path_len == 0)
		error = EINVAL;
	else {
		loader_path[loader_path_len] = '\0';
		error = 0;
	}
	vmm_machine_unlock(m);

	if (error == 0)
		error = vmm_machine_wait_for_vcpu_drain(m);
	if (error == 0)
		error = vmm_mem_prepare(mem_bytes, &prepared_backing);
	if (error == 0) {
		vmm_machine_lock(m);
		if (vmm_machine_start_cancelled_locked(m))
			error = ECANCELED;
		else {
			error = vmm_mem_publish(&m->own_mut_mem,
			    prepared_backing);
			if (error == 0)
				prepared_backing = NULL;
		}
		vmm_machine_unlock(m);
	}
	if (prepared_backing != NULL) {
		vmm_mem_release_backing(prepared_backing);
		prepared_backing = NULL;
	}
	if (error == 0) {
		vmm_machine_lock(m);
		if (vmm_machine_start_cancelled_locked(m))
			error = ECANCELED;
		else
			error = vmm_mem_snapshot(&m->own_mut_mem, &mem_object,
			    &mem_size);
		vmm_machine_unlock(m);
	}
	if (error == 0) {
		error = vmm_loader_run(loader_path, mem_object, mem_size, cred,
		    &launch, vmm_machine_start_is_cancelled, m);
		vm_object_deallocate(mem_object);
		mem_object = NULL;
	}
	if (error == 0 && !vmm_machine_start_is_cancelled(m)) {
		vmm_machine_owner_hold(m);
		vcpu_owner = 1;
		error = vmm_vcpu_start_all(m, host, &launch);
	}

	vmm_machine_lock(m);
	m->mut_starting = 0;
	m->borrow_mut_host = NULL;
	if (error == 0 && !vmm_machine_start_cancelled_locked(m)) {
		vmm_machine_start_locked(m);
		started = 1;
		release_mem = 0;
		vcpu_owner = 0;
	} else {
		vmm_vcpu_request_stop(&m->own_mut_vcpu);
		if (vmm_vcpu_has_active(&m->own_mut_vcpu)) {
			release_mem = 0;
			vcpu_owner = 0;
		}
	}
	if (!started && release_mem)
		backing = vmm_machine_detach_mem_locked(m);
	vmm_machine_unlock(m);
	wakeup(m);

	if (!started && release_mem)
		vmm_mem_release_backing(backing);
	if (vcpu_owner)
		vmm_machine_owner_release(m);
	crfree(cred);
	vmm_machine_owner_release(m);
}

int
vmm_machine_vcpu_should_stop(struct vmm_machine *m)
{
	int stop;

	vmm_machine_lock(m);
	stop = m->mut_desired_stopped || m->mut_start_cancel || m->mut_deleting ||
	    m->own_mut_vcpu.mut_stop_requested;
	vmm_machine_unlock(m);
	return stop;
}

int
vmm_machine_vcpu_wait_start(struct vmm_machine *m, struct vmm_vcpu_thread *vc)
{
	int run;

	for (;;) {
		vmm_machine_lock(m);
		run = m->mut_running && !vmm_machine_start_cancelled_locked(m) &&
		    !m->own_mut_vcpu.mut_stop_requested;
		if (run || vmm_machine_start_cancelled_locked(m) ||
		    m->own_mut_vcpu.mut_stop_requested) {
			vmm_machine_unlock(m);
			return run;
		}
		vmm_machine_unlock(m);
		tsleep(vc, 0, "vmmstrt", hz / 20 + 1);
	}
}

void
vmm_machine_vcpu_exited(struct vmm_machine *m)
{
	int last;
	struct vmm_mem_backing *backing = NULL;
	struct vmm_vcpu_thread *threads = NULL;
	uint32_t thread_count;
	int release_mem = 0;
	int release_owner = 0;

	vmm_machine_lock(m);
	thread_count = m->own_mut_vcpu.mut_count;
	last = vmm_vcpu_note_exit(&m->own_mut_vcpu, &threads);
	if (last) {
		if (m->mut_running) {
			m->mut_running = 0;
			ev_push(m, EV_STOPPED);
		}
		release_mem = 1;
		release_owner = 1;
		wakeup(m);
	}
	if (release_mem)
		backing = vmm_machine_detach_mem_locked(m);
	vmm_machine_unlock(m);

	vmm_vcpu_release_threads(threads, thread_count);
	if (release_mem)
		vmm_mem_release_backing(backing);
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

static int
vmm_machine_config_complete_locked(const struct vmm_machine *m)
{
	return vmm_vcpu_is_set(&m->own_mut_vcpu) &&
	    vmm_mem_is_set(&m->own_mut_mem) &&
	    vmm_loader_is_set(&m->own_mut_loader);
}

int
vmm_machine_config_complete(const struct vmm_machine *m)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	int complete;

	vmm_machine_lock(mm);
	complete = vmm_machine_config_complete_locked(m);
	vmm_machine_unlock(mm);
	return complete;
}

static int
vmm_machine_config_writable_locked(const struct vmm_machine *m)
{
	return m->mut_desired_stopped && !m->mut_running && !m->mut_starting &&
	    !m->mut_deleting;
}

size_t
vmm_machine_format_vcpu(const struct vmm_machine *m, char *out, size_t cap)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	size_t n;

	vmm_machine_lock(mm);
	n = vmm_vcpu_format(&m->own_mut_vcpu, out, cap);
	vmm_machine_unlock(mm);
	return n;
}

int
vmm_machine_commit_vcpu(struct vmm_machine *m, const char *buf, size_t len)
{
	int ok;

	vmm_machine_lock(m);
	ok = vmm_machine_config_writable_locked(m) &&
	    vmm_vcpu_parse(&m->own_mut_vcpu, buf, len);
	vmm_machine_unlock(m);
	return ok;
}

size_t
vmm_machine_format_mem(const struct vmm_machine *m, char *out, size_t cap)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	size_t n;

	vmm_machine_lock(mm);
	n = vmm_mem_format(&m->own_mut_mem, out, cap);
	vmm_machine_unlock(mm);
	return n;
}

int
vmm_machine_commit_mem(struct vmm_machine *m, const char *buf, size_t len)
{
	int ok;

	vmm_machine_lock(m);
	ok = vmm_machine_config_writable_locked(m) &&
	    vmm_mem_parse(&m->own_mut_mem, buf, len);
	vmm_machine_unlock(m);
	return ok;
}

size_t
vmm_machine_format_loader(const struct vmm_machine *m, char *out, size_t cap)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	size_t n;

	vmm_machine_lock(mm);
	n = vmm_loader_format(&m->own_mut_loader, out, cap);
	vmm_machine_unlock(mm);
	return n;
}

int
vmm_machine_commit_loader(struct vmm_machine *m, const char *buf, size_t len)
{
	int ok;

	vmm_machine_lock(m);
	ok = vmm_machine_config_writable_locked(m) &&
	    vmm_loader_parse(&m->own_mut_loader, buf, len);
	vmm_machine_unlock(m);
	return ok;
}

int
vmm_machine_is_stopped(const struct vmm_machine *m)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	int stopped;

	vmm_machine_lock(mm);
	stopped = m->mut_desired_stopped;
	vmm_machine_unlock(mm);
	return stopped;
}

int
vmm_machine_is_running(const struct vmm_machine *m)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	int running;

	vmm_machine_lock(mm);
	running = m->mut_running;
	vmm_machine_unlock(mm);
	return running;
}

int
vmm_machine_starting(const struct vmm_machine *m)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	int starting;

	vmm_machine_lock(mm);
	starting = m->mut_starting;
	vmm_machine_unlock(mm);
	return starting;
}

int
vmm_machine_quiesced(const struct vmm_machine *m)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	int quiesced;

	vmm_machine_lock(mm);
	quiesced = !m->mut_starting && !m->mut_running &&
	    !vmm_vcpu_has_active(&m->own_mut_vcpu);
	vmm_machine_unlock(mm);
	return quiesced;
}

void
vmm_machine_wait_quiesced(struct vmm_machine *m)
{
	while (!vmm_machine_quiesced(m)) {
		vmm_machine_request_stopped(m, 1);
		tsleep(m, 0, "vmmqsc", hz / 20 + 1);
	}
}

int
vmm_machine_start_cancelled_locked(const struct vmm_machine *m)
{
	return m->mut_start_cancel || m->mut_desired_stopped || m->mut_deleting;
}

static void
vmm_machine_start_locked(struct vmm_machine *m)
{
	if (!m->mut_running && !vmm_machine_start_cancelled_locked(m)) {
		vmm_console_reset(&m->own_mut_console);
		m->mut_running = 1;
		ev_push(m, EV_STARTED);
		vmm_vcpu_wakeup_all(&m->own_mut_vcpu);
	}
}

static int
vmm_machine_wait_for_vcpu_drain(struct vmm_machine *m)
{
	int cancelled;

	for (;;) {
		vmm_machine_lock(m);
		cancelled = vmm_machine_start_cancelled_locked(m);
		if (cancelled || !vmm_vcpu_has_active(&m->own_mut_vcpu)) {
			vmm_machine_unlock(m);
			return cancelled ? ECANCELED : 0;
		}
		vmm_machine_unlock(m);
		tsleep(m, 0, "vmmdrn", hz / 20 + 1);
	}
}

static struct vmm_mem_backing *
vmm_machine_detach_mem_locked(struct vmm_machine *m)
{
	return vmm_mem_detach(&m->own_mut_mem);
}

int
vmm_machine_is_deleting(const struct vmm_machine *m)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	int deleting;

	vmm_machine_lock(mm);
	deleting = m->mut_deleting;
	vmm_machine_unlock(mm);
	return deleting;
}

int
vmm_machine_lease_open(struct vmm_machine *m)
{
	int ok = 0;

	vmm_machine_lock(m);
	if (m->mut_deleting)
		goto out;
	m->mut_lease_count++;
	m->mut_lease_armed = 1;
	ok = 1;
out:
	vmm_machine_unlock(m);
	return ok;
}

static int
vmm_machine_begin_delete_locked(struct vmm_machine *m)
{
	if (m->mut_deleting)
		return 0;
	m->mut_deleting = 1;
	m->mut_desired_stopped = 1;
	m->mut_start_cancel = 1;
	vmm_vcpu_request_stop(&m->own_mut_vcpu);
	if (m->mut_running && !vmm_vcpu_has_active(&m->own_mut_vcpu))
		m->mut_running = 0;
	ev_push(m, EV_DELETED);
	return 1;
}

enum vmm_close_action
vmm_machine_lease_close(struct vmm_machine *m)
{
	enum vmm_close_action action = VMM_CLOSE_NONE;

	vmm_machine_lock(m);
	if (m->mut_lease_count > 0)
		m->mut_lease_count--;
	if (m->mut_lease_armed && m->mut_lease_count == 0 &&
	    vmm_machine_begin_delete_locked(m))
		action = VMM_CLOSE_DELETE;
	vmm_machine_unlock(m);
	if (action == VMM_CLOSE_DELETE)
		wakeup(m);
	return action;
}

int
vmm_machine_begin_delete(struct vmm_machine *m)
{
	int first;

	vmm_machine_lock(m);
	first = vmm_machine_begin_delete_locked(m);
	vmm_machine_unlock(m);
	if (first)
		wakeup(m);
	return first;
}

int
vmm_machine_events_pending(const struct vmm_machine *m)
{
	struct vmm_machine *mm = __DECONST(struct vmm_machine *, m);
	int pending;

	vmm_machine_lock(mm);
	pending = m->mut_ev_count > 0;
	vmm_machine_unlock(mm);
	return pending;
}

size_t
vmm_machine_read_events(struct vmm_machine *m, char *out, size_t cap)
{
	size_t n = 0;

	vmm_machine_lock(m);
	while (m->mut_ev_count > 0) {
		size_t tlen;
		const char *t = event_text(m->mut_ev_codes[m->mut_ev_tail], &tlen);

		if (n + tlen > cap)
			break;
		memcpy(out + n, t, tlen);
		n += tlen;
		m->mut_ev_tail = (m->mut_ev_tail + 1) % VMM_EVENT_CAP;
		m->mut_ev_count--;
	}
	vmm_machine_unlock(m);
	return n;
}
