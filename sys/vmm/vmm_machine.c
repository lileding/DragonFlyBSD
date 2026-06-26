/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/taskqueue.h>
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


struct vmm_machine_cmd {
	struct task task;
	struct vmm_machine *borrow_imm_machine;
	enum vmm_machine_event imm_event;
	enum vmm_machine_event_mode imm_mode;
	struct vmm_loader_epoch *own_loader;
};

static void	vmm_machine_cmd_task(void *arg, int pending);
static void	vmm_machine_start(struct vmm_machine *m,
		    struct vmm_loader_epoch *loader);
static void	vmm_machine_stop(struct vmm_machine *m, int apic);
static void	vmm_machine_reset(struct vmm_machine *m, int apic,
		    struct vmm_loader_epoch *loader);
static int	vmm_machine_config_complete_locked(const struct vmm_machine *m);
static void	vmm_machine_start_locked(struct vmm_machine *m);
static int	vmm_machine_wait_for_vcpu_drain(struct vmm_machine *m);
static void	vmm_machine_wait_execution_stopped(struct vmm_machine *m);
static void	vmm_machine_request_stop_locked(struct vmm_machine *m);
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
	m->own_mut_taskqueue = taskqueue_create("vmm_machine", M_WAITOK,
	    taskqueue_thread_enqueue, &m->own_mut_taskqueue);
	KKASSERT(m->own_mut_taskqueue != NULL);
	(void)taskqueue_start_threads(&m->own_mut_taskqueue, 1,
	    TDPRI_KERN_DAEMON, -1, "vmm machine");
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

	/*
	 * Callers must have removed the machine from new control-plane reach
	 * and waited for quiescence.  This routine only performs final object
	 * teardown; it must not be the path that races active vCPUs against
	 * guest RAM detach.
	 */
	vmm_machine_lock(m);
	vmm_machine_request_stop_locked(m);
	thread_count = m->own_mut_vcpu.mut_count;
	vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
	backing = vmm_machine_detach_mem_locked(m);
	vmm_machine_unlock(m);
	vmm_vcpu_release_threads(threads, thread_count);
	vmm_mem_release_backing(backing);
	if (m->own_mut_taskqueue != NULL) {
		taskqueue_free(m->own_mut_taskqueue);
		m->own_mut_taskqueue = NULL;
	}
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

int
vmm_machine_on_event(struct vmm_machine *m, enum vmm_machine_event event,
    enum vmm_machine_event_mode mode, struct ucred *cred, struct vmm_host *host)
{
	struct vmm_machine_cmd *cmd;
	struct vmm_loader_epoch *loader = NULL;
	char loader_path[VMM_LOADER_MAX + 1];
	size_t loader_path_len = 0;
	int need_loader;
	int error = 0;

	need_loader = (event == VMM_MACHINE_EVENT_START ||
	    (event == VMM_MACHINE_EVENT_RESET && mode == VMM_MACHINE_EVENT_FORCE));
	if (need_loader) {
		vmm_machine_lock(m);
		if (m->mut_deleting)
			error = ENXIO;
		else if (!vmm_machine_config_complete_locked(m))
			error = EINVAL;
		else {
			loader_path_len = vmm_loader_path(&m->own_mut_loader,
			    loader_path, VMM_LOADER_MAX);
			if (loader_path_len == 0)
				error = EINVAL;
			else
				loader_path[loader_path_len] = '\0';
		}
		vmm_machine_unlock(m);
		if (error)
			return error;

		/*
		 * The syscall that produced this event supplies the proc/lwp context
		 * needed to fork.  The child remains paused; fd3/fd4 are installed only
		 * when the serialized START/RESET command actually executes.
		 */
		error = vmm_loader_fork_paused(loader_path, cred, &loader);
		if (error)
			return error;
	}

	cmd = kmalloc(sizeof(*cmd), M_TEMP, M_WAITOK | M_ZERO);
	TASK_INIT(&cmd->task, 0, vmm_machine_cmd_task, cmd);
	cmd->borrow_imm_machine = m;
	cmd->imm_event = event;
	cmd->imm_mode = mode;
	cmd->own_loader = loader;
	loader = NULL;

	vmm_machine_lock(m);
	if (m->mut_deleting) {
		error = ENXIO;
	} else if (m->own_mut_taskqueue == NULL) {
		error = ENXIO;
	} else {
		if (event == VMM_MACHINE_EVENT_STOP) {
			m->mut_desired_stopped = 1;
			m->mut_start_cancel = 1;
		} else {
			m->mut_desired_stopped = 0;
			m->mut_start_cancel = 0;
			m->borrow_mut_host = host;
		}
		m->mut_cmd_count++;
	}
	vmm_machine_unlock(m);
	if (error) {
		if (cmd->own_loader != NULL)
			vmm_loader_free(cmd->own_loader);
		kfree(cmd, M_TEMP);
		return error;
	}

	vmm_machine_owner_hold(m);
	error = taskqueue_enqueue(m->own_mut_taskqueue, &cmd->task);
	if (error) {
		vmm_machine_lock(m);
		KKASSERT(m->mut_cmd_count > 0);
		m->mut_cmd_count--;
		vmm_machine_unlock(m);
		if (cmd->own_loader != NULL)
			vmm_loader_free(cmd->own_loader);
		kfree(cmd, M_TEMP);
		vmm_machine_owner_release(m);
	}
	return error;
}

int
vmm_machine_request_running(struct vmm_machine *m, struct ucred *cred,
    struct vmm_host *host)
{
	return vmm_machine_on_event(m, VMM_MACHINE_EVENT_START,
	    VMM_MACHINE_EVENT_FORCE, cred, host);
}

void
vmm_machine_request_stopped(struct vmm_machine *m, int force)
{
	(void)vmm_machine_on_event(m, VMM_MACHINE_EVENT_STOP,
	    force ? VMM_MACHINE_EVENT_FORCE : VMM_MACHINE_EVENT_APIC, NULL, NULL);
}

static void
vmm_machine_cmd_task(void *arg, int pending)
{
	struct vmm_machine_cmd *cmd = arg;
	struct vmm_machine *m = cmd->borrow_imm_machine;

	(void)pending;
	switch (cmd->imm_event) {
	case VMM_MACHINE_EVENT_START:
		vmm_machine_start(m, cmd->own_loader);
		cmd->own_loader = NULL;
		break;
	case VMM_MACHINE_EVENT_STOP:
		vmm_machine_stop(m, cmd->imm_mode == VMM_MACHINE_EVENT_APIC);
		break;
	case VMM_MACHINE_EVENT_RESET:
		vmm_machine_reset(m, cmd->imm_mode == VMM_MACHINE_EVENT_APIC,
		    cmd->own_loader);
		cmd->own_loader = NULL;
		break;
	}
	if (cmd->own_loader != NULL)
		vmm_loader_free(cmd->own_loader);

	vmm_machine_lock(m);
	KKASSERT(m->mut_cmd_count > 0);
	m->mut_cmd_count--;
	vmm_machine_unlock(m);
	wakeup(m);
	kfree(cmd, M_TEMP);
	vmm_machine_owner_release(m);
}

static void
vmm_machine_start(struct vmm_machine *m, struct vmm_loader_epoch *loader)
{
	struct vmm_mem_backing *backing = NULL;
	struct vmm_mem_backing *prepared_backing = NULL;
	struct vmm_launch launch;
	struct vm_object *mem_object = NULL;
	struct vmm_host *host;
	uint64_t mem_bytes;
	uint64_t mem_size = 0;
	int error;
	int release_mem = 1;
	int vcpu_owner = 0;

	if (loader == NULL)
		return;
	vmm_machine_lock(m);
	if (m->mut_running) {
		vmm_machine_unlock(m);
		vmm_loader_free(loader);
		return;
	}
	if (m->mut_deleting || m->mut_desired_stopped) {
		vmm_machine_unlock(m);
		vmm_loader_free(loader);
		return;
	}
	m->mut_starting = 1;
	mem_bytes = m->own_mut_mem.mut_bytes;
	host = m->borrow_mut_host;
	vmm_machine_unlock(m);

	error = vmm_machine_wait_for_vcpu_drain(m);
	if (error == 0)
		error = vmm_mem_prepare(mem_bytes, &prepared_backing);
	if (error == 0) {
		vmm_machine_lock(m);
		if (vmm_machine_start_cancelled_locked(m))
			error = ECANCELED;
		else {
			error = vmm_mem_publish(&m->own_mut_mem, prepared_backing);
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
		error = vmm_loader_start(loader, mem_object, mem_size, &launch);
		vm_object_deallocate(mem_object);
		mem_object = NULL;
	}
	vmm_loader_free(loader);
	loader = NULL;
	if (error == 0) {
		vmm_machine_owner_hold(m);
		vcpu_owner = 1;
		error = vmm_vcpu_start_all(m, host, &launch);
	}

	vmm_machine_lock(m);
	m->mut_starting = 0;
	if (error == 0 && !vmm_machine_start_cancelled_locked(m)) {
		vmm_machine_start_locked(m);
		release_mem = 0;
		vcpu_owner = 0;
	} else {
		vmm_vcpu_request_stop(&m->own_mut_vcpu);
		if (vmm_vcpu_has_active(&m->own_mut_vcpu)) {
			release_mem = 0;
			vcpu_owner = 0;
		}
	}
	if (release_mem)
		backing = vmm_machine_detach_mem_locked(m);
	vmm_machine_unlock(m);
	wakeup(m);

	if (release_mem)
		vmm_mem_release_backing(backing);
	if (vcpu_owner)
		vmm_machine_owner_release(m);
}

static void
vmm_machine_stop(struct vmm_machine *m, int apic)
{
	struct vmm_mem_backing *backing = NULL;
	int release_now = 0;

	vmm_machine_lock(m);
	if (!m->mut_running && !vmm_vcpu_has_active(&m->own_mut_vcpu)) {
		vmm_machine_unlock(m);
		return;
	}
	vmm_machine_request_stop_locked(m);
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
	if (!apic)
		vmm_machine_wait_execution_stopped(m);
}

static void
vmm_machine_reset(struct vmm_machine *m, int apic,
    struct vmm_loader_epoch *loader)
{
	int running;

	vmm_machine_lock(m);
	running = m->mut_running;
	vmm_machine_unlock(m);
	if (running) {
		if (apic) {
			vmm_machine_stop(m, 1);
			if (loader != NULL)
				vmm_loader_free(loader);
			return;
		}
		vmm_machine_stop(m, 0);
		vmm_machine_start(m, loader);
		return;
	}
	if (apic) {
		if (loader != NULL)
			vmm_loader_free(loader);
		return;
	}
	vmm_machine_start(m, loader);
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
	quiesced = m->mut_cmd_count == 0 && !m->mut_starting &&
	    !m->mut_running && !vmm_vcpu_has_active(&m->own_mut_vcpu);
	vmm_machine_unlock(mm);
	return quiesced;
}

void
vmm_machine_wait_quiesced(struct vmm_machine *m)
{
	while (!vmm_machine_quiesced(m)) {
		/*
		 * This is an internal teardown wait, not a vmmfs event.  Do not
		 * enqueue STOP here: delete/force-unmount/kldunload must wait for
		 * the current serialized command stream to drain, not append to it.
		 */
		vmm_machine_lock(m);
		vmm_machine_request_stop_locked(m);
		vmm_machine_unlock(m);
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

static void
vmm_machine_wait_execution_stopped(struct vmm_machine *m)
{
	for (;;) {
		vmm_machine_lock(m);
		if (!m->mut_running && !vmm_vcpu_has_active(&m->own_mut_vcpu)) {
			vmm_machine_unlock(m);
			return;
		}
		vmm_vcpu_request_stop(&m->own_mut_vcpu);
		vmm_machine_unlock(m);
		tsleep(m, 0, "vmmstp", hz / 20 + 1);
	}
}

static void
vmm_machine_request_stop_locked(struct vmm_machine *m)
{
	m->mut_desired_stopped = 1;
	m->mut_start_cancel = 1;
	vmm_vcpu_request_stop(&m->own_mut_vcpu);
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
	vmm_machine_request_stop_locked(m);
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
