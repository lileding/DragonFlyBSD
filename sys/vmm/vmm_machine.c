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
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/thread2.h>
#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <sys/wait.h>
#include <machine/stdarg.h>
#include <vm/vm_object.h>

#include "vmm_loader_x86.h"
#include "vmm_machine.h"

#define EV_CREATED	1
#define EV_STARTED	2
#define EV_STOPPED	3

static int vmm_debug_trace_enabled;
int vmm_debug_allow_machine_taskqueue = 1;
int vmm_debug_allow_nmkdir_vnode = 1;
int vmm_debug_allow_start_execute = 1;
int vmm_debug_allow_loader_fork = 1;
int vmm_debug_allow_loader_run = 1;
int vmm_debug_allow_vcpu_start = 1;

SYSCTL_NODE(_debug, OID_AUTO, vmm, CTLFLAG_RW, 0, "vmm debug controls");
SYSCTL_INT(_debug_vmm, OID_AUTO, trace, CTLFLAG_RW,
    &vmm_debug_trace_enabled, 0, "print vmm execution-stage trace messages");
SYSCTL_INT(_debug_vmm, OID_AUTO, allow_machine_taskqueue, CTLFLAG_RW,
    &vmm_debug_allow_machine_taskqueue, 0,
    "allow per-machine taskqueue creation during machine init");
SYSCTL_INT(_debug_vmm, OID_AUTO, allow_nmkdir_vnode, CTLFLAG_RW,
    &vmm_debug_allow_nmkdir_vnode, 0,
    "allow mkdir(machine) to bind the created machine vnode");
SYSCTL_INT(_debug_vmm, OID_AUTO, allow_start_execute, CTLFLAG_RW,
    &vmm_debug_allow_start_execute, 0,
    "allow start/reset commands that require loader context");
SYSCTL_INT(_debug_vmm, OID_AUTO, allow_loader_fork, CTLFLAG_RW,
    &vmm_debug_allow_loader_fork, 0,
    "allow vmm_machine_execute to fork the paused loader");
SYSCTL_INT(_debug_vmm, OID_AUTO, allow_loader_run, CTLFLAG_RW,
    &vmm_debug_allow_loader_run, 0,
    "allow vmm_machine_start to install, resume, and wait for loader");
SYSCTL_INT(_debug_vmm, OID_AUTO, allow_vcpu_start, CTLFLAG_RW,
    &vmm_debug_allow_vcpu_start, 0,
    "allow vmm_machine_start to enter vCPU execution");

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
vmm_debug_trace(const char *fmt, ...)
{
	__va_list ap;

	if (!vmm_debug_trace_enabled)
		return;
	kprintf("vmm: ");
	__va_start(ap, fmt);
	kvprintf(fmt, ap);
	__va_end(ap);
	kprintf("\n");
}

void
vmm_machine_init(struct vmm_machine *m)
{
	kprintf("vmm klog: core_machine_init memset begin m=%p\n", m);
	memset(m, 0, sizeof(*m));
	kprintf("vmm klog: core_machine_init token_config begin m=%p\n", m);
	lwkt_token_init(&m->token_config, "vmmcfg");
	kprintf("vmm klog: core_machine_init token_events begin m=%p\n", m);
	lwkt_token_init(&m->token_events, "vmmev");
	kprintf("vmm klog: core_machine_init console begin m=%p\n", m);
	vmm_console_init(&m->own_mut_console);
	kprintf("vmm klog: core_machine_init taskqueue gate m=%p allow=%d\n", m,
	    vmm_debug_allow_machine_taskqueue);
	vmm_debug_trace("machine_init begin m=%p allow_taskqueue=%d", m,
	    vmm_debug_allow_machine_taskqueue);
	if (vmm_debug_allow_machine_taskqueue) {
		kprintf("vmm klog: core_machine_init taskqueue_create begin m=%p\n",
		    m);
		m->own_mut_taskqueue = taskqueue_create("vmm_machine", M_WAITOK,
		    taskqueue_thread_enqueue, &m->own_mut_taskqueue);
		kprintf("vmm klog: core_machine_init taskqueue_create done m=%p tq=%p\n",
		    m, m->own_mut_taskqueue);
		KKASSERT(m->own_mut_taskqueue != NULL);
		kprintf("vmm klog: core_machine_init taskqueue_start begin m=%p tq=%p\n",
		    m, m->own_mut_taskqueue);
		(void)taskqueue_start_threads(&m->own_mut_taskqueue, 1,
		    TDPRI_KERN_DAEMON, -1, "vmm machine");
		kprintf("vmm klog: core_machine_init taskqueue_start done m=%p tq=%p\n",
		    m, m->own_mut_taskqueue);
	}
	m->mut_desired_stopped = 1;
	m->mut_status = VMM_MACHINE_STOPPED;
	kprintf("vmm klog: core_machine_init ev_created begin m=%p\n", m);
	ev_push(m, EV_CREATED);
	kprintf("vmm klog: core_machine_init ev_stopped begin m=%p\n", m);
	ev_push(m, EV_STOPPED);
	vmm_debug_trace("machine_init done m=%p tq=%p", m,
	    m->own_mut_taskqueue);
	kprintf("vmm klog: core_machine_init done m=%p tq=%p\n", m,
	    m->own_mut_taskqueue);
}

void
vmm_machine_uninit(struct vmm_machine *m)
{
	struct vmm_mem_backing *backing;
	struct vmm_vcpu_thread *threads = NULL;
	uint32_t thread_count;

	kprintf("vmm klog: core_machine_uninit begin m=%p tq=%p\n", m,
	    m->own_mut_taskqueue);
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
		kprintf("vmm klog: core_machine_uninit taskqueue_free begin m=%p tq=%p\n",
		    m, m->own_mut_taskqueue);
		taskqueue_free(m->own_mut_taskqueue);
		m->own_mut_taskqueue = NULL;
		kprintf("vmm klog: core_machine_uninit taskqueue_free done m=%p\n",
		    m);
	}
	kprintf("vmm klog: core_machine_uninit done m=%p\n", m);
}

void
vmm_machine_drain(struct vmm_machine *m)
{
	struct task task;
	int error;

	if (m->own_mut_taskqueue == NULL)
		return;
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

	vmm_debug_trace("execute begin m=%p handler=%p cred=%p tq=%p", m,
	    fnonce_handler, cred, m->own_mut_taskqueue);
	if (m->own_mut_taskqueue == NULL)
		return EBUSY;
	if (cred != NULL && !vmm_debug_allow_start_execute)
		return EBUSY;

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
		if (!vmm_debug_allow_loader_fork) {
			error = EBUSY;
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
	vmm_debug_trace("execute queued m=%p handler=%p", m, fnonce_handler);
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

	vmm_debug_trace("start begin m=%p vcpu=%u mem=%ju loader=%s", m,
	    task->imm_vcpu_count, (uintmax_t)task->imm_mem_bytes,
	    task->imm_loader_path);
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
	if (!vmm_debug_allow_loader_run) {
		error = EBUSY;
		goto fail;
	}
	error = vmm_loader_install(loader, mem_object, mem_size);
	if (error != 0)
		goto fail;
	error = vmm_loader_resume(loader);
	if (error != 0)
		goto fail;
	error = vmm_loader_wait(loader);
	if (error != 0)
		goto fail;
	error = vmm_loader_manifest_load(loader, &launch);
	if (error != 0)
		goto fail;
	vm_object_deallocate(mem_object);
	mem_object = NULL;
	vmm_loader_fini(loader);
	if (!vmm_debug_allow_vcpu_start) {
		error = EBUSY;
		goto fail_after_loader;
	}
	error = vmm_vcpu_start(m, task->imm_vcpu_count, &launch);
	if (error != 0)
		goto fail_after_loader;

	vmm_console_reset(&m->own_mut_console);
	vmm_machine_set_status(m, VMM_MACHINE_RUNNING);
	ev_push(m, EV_STARTED);
	vmm_debug_trace("start running m=%p", m);
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
