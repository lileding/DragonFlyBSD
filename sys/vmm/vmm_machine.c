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
#include <machine/atomic.h>
#include <machine/stdarg.h>
#include <vm/vm_object.h>

#include "vmm_loader_x86.h"
#include "vmm_machine.h"
#include "vmm_pcie.h"

static int vmm_debug_trace_enabled;
int vmm_debug_allow_machine_taskqueue = 1;
int vmm_debug_allow_nmkdir_vnode = 1;
int vmm_debug_allow_start_execute = 1;
int vmm_debug_allow_machine_task_run = 1;
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
SYSCTL_INT(_debug_vmm, OID_AUTO, allow_machine_task_run, CTLFLAG_RW,
    &vmm_debug_allow_machine_task_run, 0,
    "allow queued per-machine tasks to run their command handler");
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
};

static void	vmm_machine_task_run(void *arg, int pending);
static void	vmm_machine_drain_task(void *arg, int pending);
static void	vmm_machine_guest_exit(
		    const struct vmm_machine_task *task);
static enum vmm_machine_status vmm_machine_status(struct vmm_machine *m);
static void	vmm_machine_set_status(struct vmm_machine *m,
			    enum vmm_machine_status status);

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
vmm_machine_logf(struct vmm_machine *m, const char *fmt, ...)
{
	char msg[384];
	char line[512];
	__va_list ap;
	size_t len;
	size_t i;
	int n;

	if (m == NULL || m->own_mut_events_buf == NULL ||
	    m->imm_events_cap == 0)
		return;
	__va_start(ap, fmt);
	n = kvsnprintf(msg, sizeof(msg), fmt, ap);
	__va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(msg))
		n = (int)sizeof(msg) - 1;
	for (i = 0; i < (size_t)n; i++) {
		if (msg[i] == '\n' || msg[i] == '\r')
			msg[i] = ' ';
	}
	lwkt_gettoken(&m->token_events);
	m->mut_events_seq++;
	n = ksnprintf(line, sizeof(line), "%010ju %s\n",
	    (uintmax_t)m->mut_events_seq, msg);
	if (n < 0) {
		lwkt_reltoken(&m->token_events);
		return;
	}
	len = (size_t)n;
	if (len >= sizeof(line))
		len = sizeof(line) - 1;
	for (i = 0; i < len; i++) {
		size_t pos;

		if (m->mut_events_len < m->imm_events_cap) {
			pos = (m->mut_events_start + m->mut_events_len) %
			    m->imm_events_cap;
			m->mut_events_len++;
		} else {
			pos = m->mut_events_start;
			m->mut_events_start = (m->mut_events_start + 1) %
			    m->imm_events_cap;
			m->mut_events_drop_bytes++;
		}
		m->own_mut_events_buf[pos] = line[i];
	}
	lwkt_reltoken(&m->token_events);
	wakeup(m);
}

void
vmm_machine_init(struct vmm_machine *m, struct vmm_pcie *pcie)
{
	kprintf("vmm klog: core_machine_init memset begin m=%p\n", m);
	memset(m, 0, sizeof(*m));
	kprintf("vmm klog: core_machine_init token_config begin m=%p\n", m);
	lwkt_token_init(&m->token_config, "vmmcfg");
	kprintf("vmm klog: core_machine_init token_events begin m=%p\n", m);
	lwkt_token_init(&m->token_events, "vmmev");
	vmm_pcie_root_init(&m->own_mut_pcie_root, pcie, m);
	m->imm_events_cap = VMM_EVENT_LOG_SIZE;
	m->own_mut_events_buf = kmalloc(m->imm_events_cap, M_TEMP,
	    M_WAITOK | M_ZERO);
	vmm_machine_logf(m, "machine created");
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
	vmm_machine_logf(m, "state stopped reason=create");
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
	if (m->own_mut_boot_launch != NULL) {
		kfree(m->own_mut_boot_launch, M_TEMP);
		m->own_mut_boot_launch = NULL;
	}
	vmm_console_detach(&m->own_mut_console);
	vmm_pcie_root_uninit(&m->own_mut_pcie_root);
	if (m->own_mut_taskqueue != NULL) {
		kprintf("vmm klog: core_machine_uninit taskqueue_free begin m=%p tq=%p\n",
		    m, m->own_mut_taskqueue);
		taskqueue_free(m->own_mut_taskqueue);
		m->own_mut_taskqueue = NULL;
		kprintf("vmm klog: core_machine_uninit taskqueue_free done m=%p\n",
		    m);
	}
	if (m->own_mut_events_buf != NULL) {
		kfree(m->own_mut_events_buf, M_TEMP);
		m->own_mut_events_buf = NULL;
		m->imm_events_cap = 0;
	}
	kprintf("vmm klog: core_machine_uninit done m=%p\n", m);
}

void
vmm_machine_drain(struct vmm_machine *m)
{
	struct task task;
	int error;

	vmm_debug_trace("machine_drain begin m=%p tq=%p", m,
	    m->own_mut_taskqueue);
	if (m->own_mut_taskqueue == NULL) {
		vmm_debug_trace("machine_drain skipped m=%p reason=no_taskqueue", m);
		return;
	}
	TASK_INIT(&task, 0, vmm_machine_drain_task, NULL);
	error = taskqueue_enqueue(m->own_mut_taskqueue, &task);
	if (error) {
		vmm_debug_trace("machine_drain enqueue failed m=%p error=%d", m,
		    error);
		return;
	}
	vmm_debug_trace("machine_drain queued m=%p task=%p", m, &task);
	taskqueue_drain(m->own_mut_taskqueue, &task);
	vmm_debug_trace("machine_drain done m=%p task=%p", m, &task);
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
	uint64_t mem_bytes;
	uint32_t vcpu_count;
	size_t loader_len;
	int config_complete;
	int loader_initialized = 0;
	int error = 0;

	vmm_debug_trace("execute begin m=%p handler=%p cred=%p tq=%p", m,
	    fnonce_handler, cred, m->own_mut_taskqueue);
	if (m->own_mut_taskqueue == NULL) {
		vmm_machine_logf(m, "command enqueue failed error=%d reason=no_taskqueue",
		    EBUSY);
		return EBUSY;
	}
	if (cred != NULL && !vmm_debug_allow_start_execute) {
		vmm_machine_logf(m, "command enqueue failed error=%d reason=start_gated",
		    EBUSY);
		return EBUSY;
	}

	task = kmalloc(sizeof(*task), M_TEMP, M_WAITOK | M_ZERO);
	TASK_INIT(&task->task, 0, vmm_machine_task_run, task);
	task->borrow_mut_machine = m;
	task->fnonce_handler = fnonce_handler;

	if (cred != NULL) {
		/* vmmfs froze config by clearing desired_stopped before this call. */
		lwkt_gettoken(&m->token_config);
		vcpu_count = m->own_mut_vcpu.mut_count;
		mem_bytes = m->own_mut_mem.mut_bytes;
		loader_len = m->mut_loader_len;
		lwkt_reltoken(&m->token_config);
		config_complete = vcpu_count != 0 && mem_bytes != 0 &&
		    vmm_loader_path_is_set(loader_len);
		if (!config_complete) {
			bzero(&task->own_loader, sizeof(task->own_loader));
			task->own_loader.atomic_mut_state = VMM_LOADER_FAILED;
			task->own_loader.mut_exit_code = EINVAL;
			vmm_machine_logf(m,
			    "loader prepare failed error=%d reason=incomplete_config vcpu=%u mem=%ju loader_len=%ju command=queued",
			    EINVAL, vcpu_count, (uintmax_t)mem_bytes,
			    (uintmax_t)loader_len);
		} else if (!vmm_debug_allow_loader_fork) {
			error = EBUSY;
			vmm_machine_logf(m,
			    "command enqueue failed error=%d reason=loader_fork_gated",
			    error);
			goto fail;
		} else {
			error = vmm_loader_init(&task->own_loader,
			    m->mut_loader_path, cred);
			if (error != 0) {
				if (atomic_fetchadd_int(&task->own_loader.atomic_mut_state,
				    0) != VMM_LOADER_FAILED) {
					vmm_machine_logf(m,
					    "loader prepare failed error=%d reason=init_failed",
					    error);
					goto fail;
				}
				vmm_machine_logf(m,
				    "loader prepare failed error=%d reason=early_exit exit_status=%d command=queued",
				    error, task->own_loader.mut_exit_code);
			} else {
				loader_initialized = 1;
				vmm_machine_logf(m, "loader forked path=%s",
				    task->own_loader.imm_path);
			}
		}
	}

	error = taskqueue_enqueue(m->own_mut_taskqueue, &task->task);
	if (error) {
		vmm_machine_logf(m, "command enqueue failed error=%d reason=taskqueue",
		    error);
		goto fail;
	}
	vmm_machine_logf(m, "command queued handler=%p loader=%s",
	    fnonce_handler, task->own_loader.imm_path != NULL ?
	    task->own_loader.imm_path : "-");
	vmm_debug_trace("execute queued m=%p handler=%p", m, fnonce_handler);
	return 0;

fail:
	if (loader_initialized)
		vmm_loader_fini(&task->own_loader);
	kfree(task, M_TEMP);
	return error;
}

static void
vmm_machine_task_run(void *arg, int pending)
{
	struct vmm_machine_task *task = arg;

	(void)pending;
	vmm_debug_trace("task_run begin task=%p m=%p handler=%p pending=%d",
	    task, task->borrow_mut_machine, task->fnonce_handler, pending);
	if (!vmm_debug_allow_machine_task_run) {
		vmm_debug_trace("task_run gated task=%p m=%p handler=%p",
		    task, task->borrow_mut_machine, task->fnonce_handler);
		vmm_machine_logf(task->borrow_mut_machine,
		    "command gated handler=%p", task->fnonce_handler);
		vmm_loader_fini(&task->own_loader);
		kfree(task, M_TEMP);
		return;
	}
	vmm_machine_logf(task->borrow_mut_machine, "command begin handler=%p",
	    task->fnonce_handler);
	task->fnonce_handler(task);
	vmm_machine_logf(task->borrow_mut_machine, "command done handler=%p",
	    task->fnonce_handler);
	vmm_debug_trace("task_run done task=%p m=%p handler=%p",
	    task, task->borrow_mut_machine, task->fnonce_handler);

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
	uint64_t mem_bytes;
	uint32_t vcpu_count;
	uint32_t thread_count;
	int error;
	int loader_exit_code;

	lwkt_gettoken(&m->token_config);
	vcpu_count = m->own_mut_vcpu.mut_count;
	mem_bytes = m->own_mut_mem.mut_bytes;
	lwkt_reltoken(&m->token_config);
	vmm_debug_trace("start begin m=%p vcpu=%u mem=%ju loader=%s", m,
	    vcpu_count, (uintmax_t)mem_bytes,
	    loader->imm_path != NULL ? loader->imm_path : "-");
	vmm_machine_logf(m, "start begin vcpu=%u mem=%ju loader=%s",
	    vcpu_count, (uintmax_t)mem_bytes,
	    loader->imm_path != NULL ? loader->imm_path : "-");
	if (vmm_machine_status(m) == VMM_MACHINE_RUNNING) {
		vmm_machine_logf(m, "start ignored reason=already_running");
		vmm_loader_fini(loader);
		return;
	}

	vmm_machine_set_status(m, VMM_MACHINE_STARTING);
	vmm_machine_logf(m, "state starting");
	if (atomic_fetchadd_int(&loader->atomic_mut_state, 0) !=
	    VMM_LOADER_PAUSED) {
		error = loader->mut_exit_code;
		if (error == 0)
			error = ECANCELED;
		vmm_machine_logf(m,
		    "loader unavailable error=%d state=%d",
		    error, atomic_fetchadd_int(&loader->atomic_mut_state, 0));
		goto fail;
	}

	error = vmm_mem_prepare(mem_bytes, &prepared_backing);
	if (error != 0) {
		vmm_machine_logf(m, "mem prepare failed error=%d bytes=%ju",
		    error, (uintmax_t)mem_bytes);
		goto fail;
	}
	vmm_machine_logf(m, "mem prepared bytes=%ju",
	    (uintmax_t)mem_bytes);
	error = vmm_mem_publish(&m->own_mut_mem, prepared_backing);
	if (error != 0) {
		vmm_machine_logf(m, "mem publish failed error=%d", error);
		goto fail;
	}
	prepared_backing = NULL;
	vmm_machine_logf(m, "mem published");
	error = vmm_mem_snapshot(&m->own_mut_mem, &mem_object, &mem_size);
	if (error != 0) {
		vmm_machine_logf(m, "mem snapshot failed error=%d", error);
		goto fail;
	}
	vmm_machine_logf(m, "mem snapshot object=%p bytes=%ju", mem_object,
	    (uintmax_t)mem_size);
	if (!vmm_debug_allow_loader_run) {
		error = EBUSY;
		vmm_machine_logf(m, "loader run gated error=%d", error);
		goto fail;
	}
	error = vmm_loader_install(loader, mem_object, mem_size);
	if (error != 0) {
		vmm_machine_logf(m, "loader install failed error=%d", error);
		goto fail;
	}
	vmm_machine_logf(m, "loader installed mem_bytes=%ju",
	    (uintmax_t)mem_size);
	error = vmm_loader_resume(loader);
	if (error != 0) {
		vmm_machine_logf(m, "loader resume failed error=%d", error);
		goto fail;
	}
	vmm_machine_logf(m, "loader resumed");
	error = vmm_loader_wait(loader);
	if (error != 0) {
		loader_exit_code = loader->mut_exit_code;
		vmm_machine_logf(m,
		    "loader wait failed error=%d reason=%s exit_status=%d",
		    error, loader_exit_code == -1 ? "timeout" : "exit",
		    loader_exit_code);
		goto fail;
	}
	vmm_machine_logf(m, "loader exited ok");
	error = vmm_loader_manifest_load(loader, &launch);
	if (error != 0) {
		vmm_machine_logf(m, "manifest rejected error=%d", error);
		goto fail;
	}
	vmm_machine_logf(m,
	    "manifest accepted ranges=%u rip=0x%jx rsp=0x%jx cr3=0x%jx",
	    launch.imm_range_count,
	    (uintmax_t)launch.imm_vcpu0.gpr[VMM_X64_GPR_RIP],
	    (uintmax_t)launch.imm_vcpu0.gpr[VMM_X64_GPR_RSP],
	    (uintmax_t)launch.imm_vcpu0.cr[VMM_X64_CR_CR3]);
	vm_object_deallocate(mem_object);
	mem_object = NULL;
	vmm_loader_fini(loader);
	m->own_mut_boot_launch = kmalloc(sizeof(*m->own_mut_boot_launch),
	    M_TEMP, M_WAITOK);
	*m->own_mut_boot_launch = launch;
	error = vmm_mem_start_run(&m->own_mut_mem);
	if (error != 0) {
		vmm_machine_logf(m, "mem runtime start failed error=%d", error);
		goto fail_after_loader;
	}
	vmm_machine_logf(m, "mem runtime started source=boot_snapshot");
	if (!vmm_debug_allow_vcpu_start) {
		error = EBUSY;
		vmm_machine_logf(m, "vcpu start gated error=%d", error);
		goto fail_after_loader;
	}
	vmm_console_reset(&m->own_mut_console);
	error = vmm_vcpu_start(m, vcpu_count,
	    m->own_mut_boot_launch);
	if (error != 0) {
		vmm_machine_logf(m, "vcpu start failed error=%d count=%u",
		    error, vcpu_count);
		goto fail_after_loader;
	}

	vmm_machine_set_status(m, VMM_MACHINE_RUNNING);
	vmm_machine_logf(m, "state running");
	vmm_debug_trace("start running m=%p", m);
	return;

fail:
	vmm_machine_logf(m, "start failed error=%d", error);
	if (prepared_backing != NULL)
		vmm_mem_release_backing(prepared_backing);
	if (mem_object != NULL)
		vm_object_deallocate(mem_object);
	vmm_loader_fini(loader);
fail_after_loader:
	vmm_machine_logf(m, "start cleanup begin error=%d", error);
	vmm_vcpu_stop(m);
	thread_count = m->own_mut_vcpu.mut_count;
	vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
	vmm_machine_set_status(m, VMM_MACHINE_STOPPED);
	vmm_machine_logf(m, "state stopped reason=start_failed error=%d",
	    error);
	backing = vmm_mem_detach(&m->own_mut_mem);
	vmm_vcpu_release_threads(threads, thread_count);
	vmm_mem_release_backing(backing);
	if (m->own_mut_boot_launch != NULL) {
		kfree(m->own_mut_boot_launch, M_TEMP);
		m->own_mut_boot_launch = NULL;
	}
	vmm_machine_logf(m, "start cleanup done error=%d", error);
}

void
vmm_machine_stop_apic(const struct vmm_machine_task *task)
{
	vmm_machine_logf(task->borrow_mut_machine,
	    "stop apic requested result=noop");
}

void
vmm_machine_vcpu_exited(struct vmm_machine *m)
{
	u_int exit_reason;
	int error;

	exit_reason = atomic_load_acq_int(
	    &m->own_mut_vcpu.atomic_mut_exit_reason);
	if (exit_reason == VMM_VCPU_EXIT_NONE)
		return;
	error = vmm_machine_execute(m, vmm_machine_guest_exit, NULL);
	if (error != 0) {
		vmm_machine_logf(m,
		    "guest exit cleanup enqueue failed reason=%u error=%d",
		    exit_reason, error);
	}
}

static void
vmm_machine_guest_exit(const struct vmm_machine_task *task)
{
	struct vmm_machine *m = task->borrow_mut_machine;
	struct vmm_mem_backing *backing;
	struct vmm_vcpu_thread *threads;
	uint32_t thread_count;
	u_int exit_reason;
	int error;

	exit_reason = atomic_load_acq_int(
	    &m->own_mut_vcpu.atomic_mut_exit_reason);
	if (vmm_machine_status(m) != VMM_MACHINE_RUNNING)
		return;
	KKASSERT(atomic_load_acq_int(
	    &m->own_mut_vcpu.atomic_mut_active_count) == 0);
	if (exit_reason == VMM_VCPU_EXIT_GUEST_RESET) {
		vmm_machine_set_status(m, VMM_MACHINE_STOPPING);
		vmm_machine_logf(m, "state stopping reason=guest_reset");
		thread_count = m->own_mut_vcpu.mut_count;
		vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
		vmm_vcpu_release_threads(threads, thread_count);
		error = vmm_mem_reset_run(&m->own_mut_mem);
		if (error == 0 && m->own_mut_boot_launch == NULL)
			error = EINVAL;
		if (error == 0) {
			vmm_console_reset(&m->own_mut_console);
			vmm_machine_set_status(m, VMM_MACHINE_STARTING);
			vmm_machine_logf(m, "state starting reason=guest_reset");
			error = vmm_vcpu_start(m, thread_count,
			    m->own_mut_boot_launch);
		}
		if (error == 0) {
			vmm_machine_set_status(m, VMM_MACHINE_RUNNING);
			vmm_machine_logf(m, "state running reason=guest_reset");
			return;
		}
		vmm_machine_logf(m, "guest reset failed error=%d", error);
		vmm_vcpu_stop(m);
		vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
		vmm_machine_set_status(m, VMM_MACHINE_STOPPED);
		vmm_machine_logf(m,
		    "state stopped reason=guest_reset_failed error=%d", error);
		backing = vmm_mem_detach(&m->own_mut_mem);
		vmm_vcpu_release_threads(threads, thread_count);
		vmm_mem_release_backing(backing);
		if (m->own_mut_boot_launch != NULL) {
			kfree(m->own_mut_boot_launch, M_TEMP);
			m->own_mut_boot_launch = NULL;
		}
		return;
	}
	if (exit_reason != VMM_VCPU_EXIT_GUEST_SHUTDOWN &&
	    exit_reason != VMM_VCPU_EXIT_GUEST_FAULT)
		return;
	vmm_machine_set_status(m, VMM_MACHINE_STOPPING);
	vmm_machine_logf(m, "state stopping reason=%s",
	    exit_reason == VMM_VCPU_EXIT_GUEST_SHUTDOWN ?
	    "guest_shutdown" : "guest_fault");
	thread_count = m->own_mut_vcpu.mut_count;
	vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
	vmm_machine_set_status(m, VMM_MACHINE_STOPPED);
	vmm_machine_logf(m, "state stopped reason=%s",
	    exit_reason == VMM_VCPU_EXIT_GUEST_SHUTDOWN ?
	    "guest_shutdown" : "guest_fault");
	backing = vmm_mem_detach(&m->own_mut_mem);
	vmm_vcpu_release_threads(threads, thread_count);
	vmm_mem_release_backing(backing);
	if (m->own_mut_boot_launch != NULL) {
		kfree(m->own_mut_boot_launch, M_TEMP);
		m->own_mut_boot_launch = NULL;
	}
}

void
vmm_machine_stop_force(const struct vmm_machine_task *task)
{
	struct vmm_machine *m = task->borrow_mut_machine;
	struct vmm_mem_backing *backing = NULL;
	struct vmm_vcpu_thread *threads = NULL;
	uint32_t thread_count;

	vmm_machine_logf(m, "stop force begin");
	if (vmm_machine_status(m) == VMM_MACHINE_STOPPED) {
		vmm_machine_logf(m, "stop force ignored reason=already_stopped");
		return;
	}
	vmm_machine_set_status(m, VMM_MACHINE_STOPPING);
	vmm_machine_logf(m, "state stopping reason=force");
	vmm_vcpu_stop(m);
	thread_count = m->own_mut_vcpu.mut_count;
	vmm_vcpu_uninit(&m->own_mut_vcpu, &threads);
	vmm_machine_set_status(m, VMM_MACHINE_STOPPED);
	vmm_machine_logf(m, "state stopped reason=force");
	backing = vmm_mem_detach(&m->own_mut_mem);
	vmm_vcpu_release_threads(threads, thread_count);
	vmm_mem_release_backing(backing);
	if (m->own_mut_boot_launch != NULL) {
		kfree(m->own_mut_boot_launch, M_TEMP);
		m->own_mut_boot_launch = NULL;
	}
	vmm_machine_logf(m, "stop force done");
}

void
vmm_machine_reset_apic(const struct vmm_machine_task *task)
{
	vmm_machine_logf(task->borrow_mut_machine,
	    "reset apic requested result=noop");
}

void
vmm_machine_reset_force(const struct vmm_machine_task *task)
{
	vmm_machine_logf(task->borrow_mut_machine, "reset force begin");
	vmm_machine_stop_force(task);
	vmm_machine_start(task);
	vmm_machine_logf(task->borrow_mut_machine, "reset force done");
}

void
vmm_machine_console_input(struct vmm_machine *m)
{
	lwkt_gettoken(&m->token_config);
	if (m->mut_status == VMM_MACHINE_RUNNING)
		vmm_vcpu_console_input_locked(m);
	lwkt_reltoken(&m->token_config);
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
	vmm_machine_logf(m, "config vcpu %s count=%u", ok ? "accepted" :
	    "rejected", m->own_mut_vcpu.mut_count);
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
	vmm_machine_logf(m, "config mem %s bytes=%ju", ok ? "accepted" :
	    "rejected", (uintmax_t)m->own_mut_mem.mut_bytes);
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
	vmm_machine_logf(m, "config loader %s path=%s", ok ? "accepted" :
	    "rejected", ok ? m->mut_loader_path : "-");
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
	pending = m->mut_events_len > 0;
	lwkt_reltoken(&machine->token_events);
	return pending;
}

size_t
vmm_machine_read_events(struct vmm_machine *m, off_t off, char *out, size_t cap)
{
	size_t n = 0;
	size_t pos;
	size_t first;

	if (cap == 0 || off < 0)
		return 0;
	lwkt_gettoken(&m->token_events);
	if ((uint64_t)off >= m->mut_events_len || m->own_mut_events_buf == NULL)
		goto out;
	n = cap;
	if (n > m->mut_events_len - (size_t)off)
		n = m->mut_events_len - (size_t)off;
	pos = (m->mut_events_start + (size_t)off) % m->imm_events_cap;
	first = n;
	if (first > m->imm_events_cap - pos)
		first = m->imm_events_cap - pos;
	memcpy(out, m->own_mut_events_buf + pos, first);
	if (first < n) {
		memcpy(out + first, m->own_mut_events_buf, n - first);
	}
out:
	lwkt_reltoken(&m->token_events);
	return n;
}
