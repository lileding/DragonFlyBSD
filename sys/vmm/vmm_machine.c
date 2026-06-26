/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <sys/thread2.h>
#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <sys/wait.h>
#include <vm/vm_object.h>

#include "vmm_loader_x86.h"
#include "vmm_machine.h"

#define EV_CREATED	1
#define EV_STARTED	2
#define EV_STOPPED	3

struct vmm_machine_task {
	struct task task;
	vmm_machine_func fnonce_handler;
	struct vmm_machine *borrow_mut_machine;
	struct vmm_loader own_loader;
	uint32_t imm_vcpu_count;
	uint64_t imm_mem_bytes;
	char imm_loader_path[VMM_LOADER_MAX + 1];
	size_t imm_loader_len;
};

static void	vmm_machine_task_run(void *arg, int pending);
static void	vmm_machine_drain_task(void *arg, int pending);
static int	vmm_machine_task_config_complete(
		    const struct vmm_machine_task *task);
static enum vmm_machine_status vmm_machine_status(struct vmm_machine *m);
static void	vmm_machine_set_status(struct vmm_machine *m,
		    enum vmm_machine_status status);

/* --------------------------------------------------------------------- */
/* Event ring.                                                           */

static const char *
event_text(uint8_t code, size_t *len)
{
	switch (code) {
		case EV_CREATED:
			*len = 8;
			return "created\n";
		case EV_STARTED:
			*len = 8;
			return "started\n";
		case EV_STOPPED:
			*len = 8;
			return "stopped\n";
		default:
			*len = 0;
			return "";
	}
}

static void
ev_push(struct vmm_machine *m, uint8_t code)
{
	size_t slot;

	lwkt_gettoken(&m->token_events);
	slot = (m->mut_ev_tail + m->mut_ev_count) % VMM_EVENT_CAP;
	m->mut_ev_codes[slot] = code;
	if (m->mut_ev_count < VMM_EVENT_CAP)
		m->mut_ev_count++;
	else
		m->mut_ev_tail = (m->mut_ev_tail + 1) % VMM_EVENT_CAP;
	lwkt_reltoken(&m->token_events);
}

/* --------------------------------------------------------------------- */
/* Machine model.                                                        */

void
vmm_machine_init(struct vmm_machine *m)
{
	memset(m, 0, sizeof(*m));
	lwkt_token_init(&m->token_config, "vmmcfg");
	lwkt_token_init(&m->token_events, "vmmev");
	vmm_console_init(&m->own_mut_console);
	m->own_mut_taskqueue = taskqueue_create("vmm_machine", M_WAITOK,
										 taskqueue_thread_enqueue, &m->own_mut_taskqueue);
	KKASSERT(m->own_mut_taskqueue != NULL);
	(void)taskqueue_start_threads(&m->own_mut_taskqueue, 1,
							   TDPRI_KERN_DAEMON, -1, "vmm machine");
	m->mut_desired_stopped = 1;
	m->mut_status = VMM_MACHINE_STOPPED;
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
	thread_count = m->own_mut_vcpu.mut_count;
	vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
	backing = vmm_mem_detach(&m->own_mut_mem);
	vmm_vcpu_release_threads(threads, thread_count);
	vmm_mem_release_backing(backing);
	if (m->own_mut_taskqueue != NULL) {
		taskqueue_free(m->own_mut_taskqueue);
		m->own_mut_taskqueue = NULL;
	}
}

void
vmm_machine_drain(struct vmm_machine *m)
{
	struct task task;
	int error;

	TASK_INIT(&task, 0, vmm_machine_drain_task, NULL);
	error = taskqueue_enqueue(m->own_mut_taskqueue, &task);
	if (error)
		return;
	taskqueue_drain(m->own_mut_taskqueue, &task);
}

/*
 * Called in vmmfs syscall context.  This is the boundary where declarative
 * file operations become serialized machine commands.  vmmfs selects the
 * handler and updates desired-state fields before calling this function.
 * A non-NULL cred means the command needs a loader; fork the paused child
 * here while the caller's proc/lwp context is still available.
 */
int
vmm_machine_execute(struct vmm_machine *m, vmm_machine_func fnonce_handler,
					 struct ucred *cred)
{
	struct vmm_machine_task *task;
	int error = 0;

	task = kmalloc(sizeof(*task), M_TEMP, M_WAITOK | M_ZERO);
	TASK_INIT(&task->task, 0, vmm_machine_task_run, task);
	task->borrow_mut_machine = m;
	task->fnonce_handler = fnonce_handler;

	lwkt_gettoken(&m->token_config);
	task->imm_vcpu_count = m->own_mut_vcpu.mut_count;
	task->imm_mem_bytes = m->own_mut_mem.mut_bytes;
	task->imm_loader_len = m->mut_loader_len;
	memcpy(task->imm_loader_path, m->mut_loader_path,
	    task->imm_loader_len + 1);
	lwkt_reltoken(&m->token_config);

	if (cred != NULL) {
		if (!vmm_machine_task_config_complete(task) ||
		    !vmm_loader_path_is_set(task->imm_loader_len)) {
			error = EINVAL;
			goto fail;
		}
		error = vmm_loader_init(&task->own_loader, task->imm_loader_path,
		    cred);
		if (error)
			goto fail;
	}

	error = taskqueue_enqueue(m->own_mut_taskqueue, &task->task);
	if (error) {
		if (cred != NULL)
			vmm_loader_fini(&task->own_loader);
		goto fail;
	}
	return 0;

fail:
	kfree(task, M_TEMP);
	return error;
}

static void
vmm_machine_task_run(void *arg, int pending)
{
	struct vmm_machine_task *task = arg;

	(void)pending;
	task->fnonce_handler(task);

	kfree(task, M_TEMP);
}

static void
vmm_machine_drain_task(void *arg, int pending)
{
	(void)arg;
	(void)pending;
}

void
vmm_machine_start(const struct vmm_machine_task *task)
{
	struct vmm_machine *m = task->borrow_mut_machine;
	struct vmm_loader *loader = __DECONST(struct vmm_loader *,
									   &task->own_loader);
	struct vmm_mem_backing *backing = NULL;
	struct vmm_mem_backing *prepared_backing = NULL;
	struct vmm_vcpu_thread *threads = NULL;
	struct vmm_launch launch;
	struct vm_object *mem_object = NULL;
	uint64_t mem_size = 0;
	uint32_t thread_count;
	int error;

	if (vmm_machine_status(m) == VMM_MACHINE_RUNNING)
		return;

	vmm_machine_set_status(m, VMM_MACHINE_STARTING);

	error = vmm_mem_prepare(task->imm_mem_bytes, &prepared_backing);
	if (error != 0)
		goto fail;
	error = vmm_mem_publish(&m->own_mut_mem, prepared_backing);
	if (error != 0)
		goto fail;
	prepared_backing = NULL;
	error = vmm_mem_snapshot(&m->own_mut_mem, &mem_object, &mem_size);
	if (error != 0)
		goto fail;
	error = vmm_loader_install(loader, mem_object, mem_size);
	if (error != 0)
		goto fail;
	vmm_loader_resume(loader);
	error = vmm_loader_wait(loader);
	if (error != 0)
		goto fail;
	error = vmm_loader_manifest_load(loader, &launch);
	if (error != 0)
		goto fail;
	vm_object_deallocate(mem_object);
	mem_object = NULL;
	vmm_loader_fini(loader);
	error = vmm_vcpu_start(m, task->imm_vcpu_count, &launch);
	if (error != 0)
		goto fail_after_loader;

	vmm_console_reset(&m->own_mut_console);
	vmm_machine_set_status(m, VMM_MACHINE_RUNNING);
	ev_push(m, EV_STARTED);
	return;

fail:
	vmm_machine_set_status(m, VMM_MACHINE_STOPPED);
	if (prepared_backing != NULL)
		vmm_mem_release_backing(prepared_backing);
	if (mem_object != NULL)
		vm_object_deallocate(mem_object);
	vmm_loader_fini(loader);
fail_after_loader:
	vmm_vcpu_stop(m);
	thread_count = m->own_mut_vcpu.mut_count;
	vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
	vmm_machine_set_status(m, VMM_MACHINE_STOPPED);
	backing = vmm_mem_detach(&m->own_mut_mem);
	vmm_vcpu_release_threads(threads, thread_count);
	vmm_mem_release_backing(backing);
}

void
vmm_machine_stop_apic(const struct vmm_machine_task *task)
{
	(void)task;
}

void
vmm_machine_stop_force(const struct vmm_machine_task *task)
{
	struct vmm_machine *m = task->borrow_mut_machine;
	struct vmm_mem_backing *backing = NULL;
	struct vmm_vcpu_thread *threads = NULL;
	uint32_t thread_count;

	if (vmm_machine_status(m) == VMM_MACHINE_STOPPED)
		return;
	vmm_machine_set_status(m, VMM_MACHINE_STOPPING);
	vmm_vcpu_stop(m);
	thread_count = m->own_mut_vcpu.mut_count;
	vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
	vmm_machine_set_status(m, VMM_MACHINE_STOPPED);
	ev_push(m, EV_STOPPED);
	backing = vmm_mem_detach(&m->own_mut_mem);
	vmm_vcpu_release_threads(threads, thread_count);
	vmm_mem_release_backing(backing);
}

void
vmm_machine_reset_apic(const struct vmm_machine_task *task)
{
	(void)task;
}

void
vmm_machine_reset_force(const struct vmm_machine_task *task)
{
	vmm_machine_stop_force(task);
	vmm_machine_start(task);
}

static int
vmm_machine_task_config_complete(const struct vmm_machine_task *task)
{
	return task->imm_vcpu_count != 0 && task->imm_mem_bytes != 0;
}

static enum vmm_machine_status
vmm_machine_status(struct vmm_machine *m)
{
	enum vmm_machine_status status;

	lwkt_gettoken(&m->token_config);
	status = m->mut_status;
	lwkt_reltoken(&m->token_config);
	return status;
}

static void
vmm_machine_set_status(struct vmm_machine *m, enum vmm_machine_status status)
{
	lwkt_gettoken(&m->token_config);
	m->mut_status = status;
	lwkt_reltoken(&m->token_config);
}

static int
vmm_machine_config_writable(const struct vmm_machine *m)
{
	return m->mut_desired_stopped && m->mut_status == VMM_MACHINE_STOPPED;
}

size_t
vmm_machine_format_vcpu(const struct vmm_machine *m, char *out, size_t cap)
{
	size_t n;

	lwkt_gettoken(&__DECONST(struct vmm_machine *, m)->token_config);
	n = vmm_vcpu_format(&m->own_mut_vcpu, out, cap);
	lwkt_reltoken(&__DECONST(struct vmm_machine *, m)->token_config);
	return n;
}

int
vmm_machine_commit_vcpu(struct vmm_machine *m, const char *buf, size_t len)
{
	int ok;

	lwkt_gettoken(&m->token_config);
	ok = vmm_machine_config_writable(m) &&
	    vmm_vcpu_parse(&m->own_mut_vcpu, buf, len);
	lwkt_reltoken(&m->token_config);
	return ok;
}

size_t
vmm_machine_format_mem(const struct vmm_machine *m, char *out, size_t cap)
{
	size_t n;

	lwkt_gettoken(&__DECONST(struct vmm_machine *, m)->token_config);
	n = vmm_mem_format(&m->own_mut_mem, out, cap);
	lwkt_reltoken(&__DECONST(struct vmm_machine *, m)->token_config);
	return n;
}

int
vmm_machine_commit_mem(struct vmm_machine *m, const char *buf, size_t len)
{
	int ok;

	lwkt_gettoken(&m->token_config);
	ok = vmm_machine_config_writable(m) &&
	    vmm_mem_parse(&m->own_mut_mem, buf, len);
	lwkt_reltoken(&m->token_config);
	return ok;
}

size_t
vmm_machine_format_loader(const struct vmm_machine *m, char *out, size_t cap)
{
	size_t n;

	lwkt_gettoken(&__DECONST(struct vmm_machine *, m)->token_config);
	n = vmm_loader_path_format(m->mut_loader_path, m->mut_loader_len,
	    out, cap);
	lwkt_reltoken(&__DECONST(struct vmm_machine *, m)->token_config);
	return n;
}

int
vmm_machine_commit_loader(struct vmm_machine *m, const char *buf, size_t len)
{
	int ok;

	lwkt_gettoken(&m->token_config);
	ok = vmm_machine_config_writable(m) &&
	    vmm_loader_path_parse(m->mut_loader_path, &m->mut_loader_len,
		buf, len);
	lwkt_reltoken(&m->token_config);
	return ok;
}

int
vmm_machine_lease_open(struct vmm_machine *m)
{
	m->mut_lease_count++;
	m->mut_lease_armed = 1;
	return 1;
}

enum vmm_close_action
vmm_machine_lease_close(struct vmm_machine *m)
{
	enum vmm_close_action action = VMM_CLOSE_NONE;

	if (m->mut_lease_count > 0)
		m->mut_lease_count--;
	if (m->mut_lease_armed && m->mut_lease_count == 0)
		action = VMM_CLOSE_DELETE;
	if (action == VMM_CLOSE_DELETE)
		wakeup(m);
	return action;
}

int
vmm_machine_events_pending(const struct vmm_machine *m)
{
	struct vmm_machine *machine = __DECONST(struct vmm_machine *, m);
	int pending;

	lwkt_gettoken(&machine->token_events);
	pending = m->mut_ev_count > 0;
	lwkt_reltoken(&machine->token_events);
	return pending;
}

size_t
vmm_machine_read_events(struct vmm_machine *m, char *out, size_t cap)
{
	size_t n = 0;

	lwkt_gettoken(&m->token_events);
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
	lwkt_reltoken(&m->token_events);
	return n;
}
