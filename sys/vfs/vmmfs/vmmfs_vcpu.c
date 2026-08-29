/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs machine vCPU declaration node.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/globaldata.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/usched.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/cpu.h>

#include "vmmfs.h"

#define VMMFS_VCPU_MODE 0644

static int vmmfs_vcpu_access(struct vop_access_args *);
static int vmmfs_vcpu_getattr(struct vop_getattr_args *);
static int vmmfs_vcpu_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_vcpu_open(struct vop_open_args *);
static int vmmfs_vcpu_read(struct vop_read_args *);
static int vmmfs_vcpu_setattr(struct vop_setattr_args *);
static int vmmfs_vcpu_write(struct vop_write_args *);
static int vmmfs_vcpu_inactive(struct vop_inactive_args *);
static int vmmfs_vcpu_reclaim(struct vop_reclaim_args *);
static bool vmmfs_vcpu_is_stop_requested(struct vmmfs_vcpu *);
static bool vmmfs_vcpu_is_reset_requested(struct vmmfs_vcpu *);
static void vmmfs_vcpu_thread_destroy(struct vmmfs_vcpu_thread *);
static int vmmfs_vcpu_thread_start(struct vmmfs_vcpu_thread *);
static void vmmfs_vcpu_thread_wait_start(struct vmmfs_vcpu_thread *);
static void vmmfs_vcpu_thread_stop(struct vmmfs_vcpu_thread *);
static void vmmfs_vcpu_thread_reset(struct vmmfs_vcpu_thread *);
static void vmmfs_vcpu_thread_main(void *, struct trapframe *);
static void vmmfs_vcpu_drop(struct vmmfs_node *);

struct vop_ops vmmfs_vcpu_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_vcpu_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_vcpu_getattr,
	.vop_getattr_lite = vmmfs_vcpu_getattr_lite,
	.vop_open = vmmfs_vcpu_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_vcpu_read,
	.vop_inactive = vmmfs_vcpu_inactive,
	.vop_reclaim = vmmfs_vcpu_reclaim,
	.vop_setattr = vmmfs_vcpu_setattr,
	.vop_write = vmmfs_vcpu_write,
};

static int
vmmfs_vcpu_load(struct vmmfs_vcpu *vcpu, char *buffer, size_t capacity,
	size_t *length)
{
	uint32_t count;
	int result;

	if (vcpu == NULL || vmmfs_machine_is_dead(vmmfs_vcpu_machine(vcpu)))
		return (ENOENT);
	lwkt_gettoken(&vmmfs_vcpu_machine(vcpu)->token);
	count = vcpu->count;
	lwkt_reltoken(&vmmfs_vcpu_machine(vcpu)->token);
	result = ksnprintf(buffer, capacity, "%u\n", count);
	if (result < 0 || (size_t)result >= capacity)
		return (EOVERFLOW);
	*length = (size_t)result;
	return (0);
}

static int
vmmfs_vcpu_store(struct vmmfs_vcpu *vcpu, const char *buffer, size_t length)
{
	uint64_t value;
	size_t index;
	unsigned int digit;

	if (length == 0)
		return (EINVAL);
	if (buffer[length - 1] == '\n')
		--length;
	if (length == 0)
		return (EINVAL);

	value = 0;
	for (index = 0; index < length; ++index) {
		if (buffer[index] < '0' || buffer[index] > '9')
			return (EINVAL);
		digit = (unsigned int)(buffer[index] - '0');
		if (value > (UINT32_MAX - digit) / 10)
			return (ERANGE);
		value = value * 10 + digit;
	}

	lwkt_gettoken(&vmmfs_vcpu_machine(vcpu)->token);
	if (vmmfs_vcpu_machine(vcpu)->dead) {
		lwkt_reltoken(&vmmfs_vcpu_machine(vcpu)->token);
		return (ENOENT);
	}
	if (vmmfs_vcpu_machine(vcpu)->machine != NULL) {
		lwkt_reltoken(&vmmfs_vcpu_machine(vcpu)->token);
		return (EBUSY);
	}
	vcpu->count = (uint32_t)value;
	lwkt_reltoken(&vmmfs_vcpu_machine(vcpu)->token);
	return (0);
}

int
vmmfs_vcpu_init(struct vmmfs_machine *machine, struct vmmfs_vcpu *vcpu)
{
	struct vmmfs_mount *state;
	int error = 0;

	if (machine == NULL || vcpu == NULL)
		return (EINVAL);
	bzero(vcpu, sizeof(*vcpu));
	lwkt_token_init(&vcpu->token, "vmmfsvcpu");
	vmmfs_node_setup(&vcpu->node, &machine->branch.node, vmmfs_vcpu_drop, NULL);
	state = (struct vmmfs_mount *)vmmfs_machine_root(machine)->mount->mnt_data;
	vcpu->inode = atomic_fetchadd_int(&state->next_inode, 1);
	if (state->vcpu_vops == NULL) {
		error = ENXIO;
		goto fail;
	}

	return (0);

fail:
	vmmfs_node_drop(&vcpu->node);
	return (error);
}

static void
vmmfs_vcpu_drop(struct vmmfs_node *node)
{
	struct vmmfs_vcpu *vcpu;

	vcpu = (struct vmmfs_vcpu *)node;
	KKASSERT(vcpu != NULL);
	lwkt_gettoken(&vcpu->token);
	if (vcpu->active_count != 0 || vcpu->threads != NULL) {
		lwkt_reltoken(&vcpu->token);
		panic("vmmfs_vcpu_drop: vCPU threads are still active");
	}
	if (vcpu->node.vnode != NULL) {
		lwkt_reltoken(&vcpu->token);
		panic("vmmfs_vcpu_drop: vnode is still published");
	}
	lwkt_reltoken(&vcpu->token);
	lwkt_token_uninit(&vcpu->token);
	vcpu->inode = 0;
}

int
vmmfs_vcpu_publish(struct vmmfs_vcpu *vcpu)
{
	struct vmmfs_mount *state;

	if (vcpu == NULL || vmmfs_vcpu_machine(vcpu) == NULL)
		return (EINVAL);
	state = (struct vmmfs_mount *)vmmfs_machine_root(vmmfs_vcpu_machine(vcpu))->mount->mnt_data;
	if (state == NULL || state->vcpu_vops == NULL)
		return (ENXIO);
	return (vmmfs_node_publish_regular(&vcpu->node,
	    vmmfs_machine_root(vmmfs_vcpu_machine(vcpu))->mount, &state->vcpu_vops, VREG, vcpu));
}


int
vmmfs_vcpu_start(struct vmmfs_vcpu *vcpu, uint32_t count,
	vmm_machine_t machine,
	const struct vmm_cpustate *bsp_state)
{
	struct vmmfs_vcpu_thread *thread;
	struct vmmfs_vcpu_thread *threads;
	uint32_t index;
	bool workers_started;
	int error;

	if (vcpu == NULL || machine == NULL || bsp_state == NULL ||
	    vcpu->threads != NULL)
		return (EINVAL);
	if (count == 0 || count > VMMFS_MACHINE_INDEX_MAX + 1)
		return (ERANGE);
	thread = kmalloc(sizeof(*thread) * count, M_VMMFS, M_WAITOK | M_ZERO);
	lwkt_gettoken(&vcpu->token);
	vcpu->threads = thread;
	vcpu->runtime_machine = machine;
	vcpu->start_ready = false;
	vcpu->start_failed = false;
	lwkt_reltoken(&vcpu->token);

	/* Construct every vCPU before any worker process is allowed to run. */
	vcpu->threads[0].state = *bsp_state;
	for (index = 0; index < count; ++index) {
		thread = &vcpu->threads[index];
		thread->group = vcpu;
		thread->index = index;
		{
			vmm_vcpu_t created_vcpu;

			created_vcpu = NULL;
			error = vmm_vcpu_create(machine, &thread->state,
			    &created_vcpu);
			if (error == 0) {
				lwkt_gettoken(&vcpu->token);
				thread->vcpu = created_vcpu;
				lwkt_reltoken(&vcpu->token);
			}
		}
		if (error != 0)
			goto failed;
		error = vmm_vcpu_set_memory_exit_mode(thread->vcpu,
		    VMM_MEMORY_EXIT_EMULATE);
		if (error != 0)
			goto failed;
	}

	/*
	 * fork1() has no paused-process counterpart.  Each worker therefore
	 * waits on the group channel until this loop has created the complete
	 * vCPU set.  A launch failure wakes the partial group only to tear it
	 * down; the caller retains ownership of the array in that case.
	 */
	for (index = 0; index < count; ++index) {
		error = vmmfs_vcpu_thread_start(&vcpu->threads[index]);
		if (error != 0)
			goto failed;
	}
	lwkt_gettoken(&vcpu->token);
	vcpu->start_ready = true;
	lwkt_reltoken(&vcpu->token);
	wakeup(vcpu);
	return (0);

failed:
	lwkt_gettoken(&vcpu->token);
	workers_started = vcpu->active_count != 0;
	if (workers_started) {
		vcpu->start_failed = true;
		vcpu->stop_requested = true;
		vcpu->reset_requested = false;
		vcpu->start_ready = true;
	}
	lwkt_reltoken(&vcpu->token);
	if (workers_started) {
		vmmfs_vcpu_request_stop(vcpu);
		wakeup(vcpu);
		for (;;) {
			tsleep_interlock(vcpu, 0);
			lwkt_gettoken(&vcpu->token);
			if (vcpu->active_count == 0) {
				crit_enter();
				tsleep_remove(curthread);
				crit_exit();
				lwkt_reltoken(&vcpu->token);
				break;
			}
			lwkt_reltoken(&vcpu->token);
			(void)tsleep(vcpu, PINTERLOCKED, "vmmfsstart", 0);
		}
	}
	for (index = 0; index < count; ++index) {
		thread = &vcpu->threads[index];
		if (thread->vcpu != NULL) {
			int destroy_error;

			destroy_error = vmm_vcpu_destroy(thread->vcpu);
			KKASSERT(destroy_error == 0);
			thread->vcpu = NULL;
		}
	}
	lwkt_gettoken(&vcpu->token);
	threads = vcpu->threads;
	vcpu->threads = NULL;
	vcpu->runtime_machine = NULL;
	vcpu->active_count = 0;
	vcpu->reset_waiting = 0;
	vcpu->start_ready = false;
	vcpu->start_failed = false;
	vcpu->stop_requested = false;
	vcpu->reset_requested = false;
	lwkt_reltoken(&vcpu->token);
	kfree(threads, M_VMMFS);
	return (error);
}

void
vmmfs_vcpu_request_stop(struct vmmfs_vcpu *vcpu)
{
	uint32_t index;

	if (vcpu == NULL)
		return;
	lwkt_gettoken(&vcpu->token);
	vcpu->stop_requested = true;
	vcpu->reset_requested = false;
	for (index = 0; index < vcpu->count; ++index) {
		if (vcpu->threads == NULL)
			break;
		if (vcpu->threads[index].vcpu != NULL)
			(void)vmm_vcpu_kick(vcpu->threads[index].vcpu);
	}
	lwkt_reltoken(&vcpu->token);
	wakeup(vcpu);
}

void
vmmfs_vcpu_request_reset(struct vmmfs_vcpu *vcpu)
{
	uint32_t index;

	if (vcpu == NULL)
		return;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->stop_requested || vcpu->reset_requested) {
		lwkt_reltoken(&vcpu->token);
		return;
	}
	vcpu->reset_requested = true;
	if (vcpu->threads != NULL) {
		for (index = 0; index < vcpu->count; ++index) {
			if (vcpu->threads[index].vcpu != NULL)
				(void)vmm_vcpu_kick(vcpu->threads[index].vcpu);
		}
	}
	lwkt_reltoken(&vcpu->token);
	wakeup(vcpu);
}

int
vmmfs_vcpu_reset(struct vmmfs_vcpu *vcpu, vmm_machine_t machine,
	const struct vmm_cpustate *bsp_state)
{
	struct vmmfs_vcpu_thread *thread;
	uint32_t index;
	int error;

	if (vcpu == NULL || machine == NULL || bsp_state == NULL ||
	    vcpu->threads == NULL)
		return (EINVAL);
	for (index = 0; index < vcpu->count; ++index) {
		thread = &vcpu->threads[index];
		if (thread->vcpu != NULL)
			return (EBUSY);
		bzero(&thread->state, sizeof(thread->state));
		if (index == 0)
			thread->state = *bsp_state;
		error = vmm_vcpu_create(machine, &thread->state, &thread->vcpu);
		if (error != 0)
			goto failed;
		error = vmm_vcpu_set_memory_exit_mode(thread->vcpu,
		    VMM_MEMORY_EXIT_EMULATE);
		if (error != 0)
			goto failed;
	}
	lwkt_gettoken(&vcpu->token);
	if (vcpu->stop_requested) {
		lwkt_reltoken(&vcpu->token);
		error = EINTR;
		goto failed;
	}
	vcpu->runtime_machine = machine;
	vcpu->reset_requested = false;
	vcpu->reset_waiting = 0;
	lwkt_reltoken(&vcpu->token);
	wakeup(vcpu);
	return (0);

failed:
	for (index = 0; index < vcpu->count; ++index) {
		thread = &vcpu->threads[index];
		if (thread->vcpu == NULL)
			continue;
		(void)vmm_vcpu_destroy(thread->vcpu);
		thread->vcpu = NULL;
	}
	return (error);
}

static bool
vmmfs_vcpu_is_stop_requested(struct vmmfs_vcpu *vcpu)
{
	bool requested;

	lwkt_gettoken(&vcpu->token);
	requested = vcpu->stop_requested;
	lwkt_reltoken(&vcpu->token);
	return (requested);
}

static bool
vmmfs_vcpu_is_reset_requested(struct vmmfs_vcpu *vcpu)
{
	bool requested;

	lwkt_gettoken(&vcpu->token);
	requested = vcpu->reset_requested;
	lwkt_reltoken(&vcpu->token);
	return (requested);
}

static void
vmmfs_vcpu_thread_destroy(struct vmmfs_vcpu_thread *thread)
{
	vmm_vcpu_t vcpu;
	int error;

	lwkt_gettoken(&thread->group->token);
	vcpu = thread->vcpu;
	lwkt_reltoken(&thread->group->token);
	if (vcpu == NULL)
		return;
	error = vmm_vcpu_destroy(vcpu);
	KKASSERT(error == 0);
	lwkt_gettoken(&thread->group->token);
	KKASSERT(thread->vcpu == vcpu);
	thread->vcpu = NULL;
	lwkt_reltoken(&thread->group->token);
}

static int
vmmfs_vcpu_thread_start(struct vmmfs_vcpu_thread *thread)
{
	struct proc *process;
	struct lwp *lwp;
	struct vmmfs_vcpu *vcpu;
	int error;

	vcpu = thread->group;
	/*
	 * RFNOWAIT makes the reaper own this kernel worker.  vmmfs retains
	 * lifecycle ownership through vcpu->active_count, not through wait(2).
	 */
	error = fork1(&lwp0, RFMEM | RFFDG | RFPROC | RFNOWAIT, &process);
	if (error != 0)
		return (error);
	process->p_flags |= P_SYSTEM;
	(void)ksnprintf(process->p_comm, sizeof(process->p_comm),
	    "vmm%u-vcpu%u", vmmfs_vcpu_machine(vcpu)->id, thread->index);
	lwp = ONLY_LWP_IN_PROC(process);
	lwp->lwp_thread->td_ucred = crhold(proc0.p_ucred);
	cpu_set_fork_handler(lwp, vmmfs_vcpu_thread_main, thread);

	/* Make the worker visible to the stop barrier before it can run. */
	lwkt_gettoken(&vcpu->token);
	++vcpu->active_count;
	lwkt_reltoken(&vcpu->token);
	start_forked_proc(&lwp0, process);
	return (0);
}

static void
vmmfs_vcpu_thread_wait_start(struct vmmfs_vcpu_thread *thread)
{
	struct vmmfs_vcpu *vcpu;

	vcpu = thread->group;
	for (;;) {
		tsleep_interlock(vcpu, 0);
		lwkt_gettoken(&vcpu->token);
		if (vcpu->start_ready) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			lwkt_reltoken(&vcpu->token);
			return;
		}
		lwkt_reltoken(&vcpu->token);
		(void)tsleep(vcpu, PINTERLOCKED, "vmmfsstart", 0);
	}
}

static void
vmmfs_vcpu_thread_stop(struct vmmfs_vcpu_thread *thread)
{
	struct vmmfs_vcpu *vcpu;
	struct vmmfs_machine *machine;
	struct vmmfs_vcpu_thread *threads;
	bool start_failed;

	vcpu = thread->group;
	machine = vmmfs_vcpu_machine(vcpu);
	if (thread->index != 0) {
		vmmfs_vcpu_thread_destroy(thread);
		lwkt_gettoken(&vcpu->token);
		KKASSERT(vcpu->active_count > 1);
		--vcpu->active_count;
		lwkt_reltoken(&vcpu->token);
		wakeup(vcpu);
		exit1(0);
	}

	vmmfs_vcpu_request_stop(vcpu);
	for (;;) {
		tsleep_interlock(vcpu, 0);
		lwkt_gettoken(&vcpu->token);
		if (vcpu->active_count == 1) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			lwkt_reltoken(&vcpu->token);
			break;
		}
		lwkt_reltoken(&vcpu->token);
		(void)tsleep(vcpu, PINTERLOCKED, "vmmfsstop", 0);
	}
	vmmfs_vcpu_thread_destroy(thread);
	lwkt_gettoken(&vcpu->token);
	KKASSERT(vcpu->active_count == 1);
	start_failed = vcpu->start_failed;
	if (start_failed) {
		vcpu->active_count = 0;
		vcpu->reset_waiting = 0;
		lwkt_reltoken(&vcpu->token);
		wakeup(vcpu);
		exit1(0);
	}
	threads = vcpu->threads;
	vcpu->threads = NULL;
	vcpu->runtime_machine = NULL;
	vcpu->active_count = 0;
	vcpu->reset_waiting = 0;
	vcpu->start_ready = false;
	vcpu->start_failed = false;
	vcpu->stop_requested = false;
	vcpu->reset_requested = false;
	lwkt_reltoken(&vcpu->token);
	kfree(threads, M_VMMFS);
	vmmfs_machine_vcpu_stopped(machine);
	exit1(0);
}

static void
vmmfs_vcpu_thread_reset(struct vmmfs_vcpu_thread *thread)
{
	struct vmmfs_vcpu *vcpu;
	int error;

	vcpu = thread->group;
	if (thread->index != 0) {
		vmmfs_vcpu_thread_destroy(thread);
		lwkt_gettoken(&vcpu->token);
		++vcpu->reset_waiting;
		lwkt_reltoken(&vcpu->token);
		wakeup(vcpu);
		for (;;) {
			tsleep_interlock(vcpu, 0);
			lwkt_gettoken(&vcpu->token);
			if (!vcpu->reset_requested || vcpu->stop_requested) {
				crit_enter();
				tsleep_remove(curthread);
				crit_exit();
				lwkt_reltoken(&vcpu->token);
				break;
			}
			lwkt_reltoken(&vcpu->token);
			(void)tsleep(vcpu, PINTERLOCKED, "vmmfsreset", 0);
		}
		return;
	}

	for (;;) {
		tsleep_interlock(vcpu, 0);
		lwkt_gettoken(&vcpu->token);
		if (vcpu->stop_requested ||
		    vcpu->reset_waiting == vcpu->count - 1) {
			crit_enter();
			tsleep_remove(curthread);
			crit_exit();
			lwkt_reltoken(&vcpu->token);
			break;
		}
		lwkt_reltoken(&vcpu->token);
		(void)tsleep(vcpu, PINTERLOCKED, "vmmfsreset", 0);
	}
	if (vmmfs_vcpu_is_stop_requested(vcpu))
		return;
	vmmfs_vcpu_thread_destroy(thread);
	error = vmmfs_machine_vcpu_reset(vmmfs_vcpu_machine(vcpu));
	if (error != 0) {
		vmmfs_events_log(&vmmfs_vcpu_machine(vcpu)->events,
		    VMMFS_MACHINE_EVENT_RESET_FAILED, "error=%d", error);
		vmmfs_vcpu_request_stop(vcpu);
		return;
	}
	vmmfs_events_log(&vmmfs_vcpu_machine(vcpu)->events,
	    VMMFS_MACHINE_EVENT_RESET_COMPLETED, NULL);
}

static void
vmmfs_vcpu_thread_main(void *argument, struct trapframe *frame)
{
	struct vmmfs_vcpu_thread *thread;
	struct vmmfs_vcpu *vcpu;
	struct vmm_cpuexit *exit;
	struct vmm_cpuevent exception = {
		.type = VMM_CPUEVENT_EXCP,
		.vector = VMM_X64_EXCEPTION_GP,
	};
	int error = 0;

	thread = argument;
	vcpu = thread->group;
	(void)frame;
	vmmfs_vcpu_thread_wait_start(thread);
	for (;;) {
		lwpkthreaddeferred();
		if (curthread->td_lwp->lwp_mpflags & LWP_MP_WEXIT)
			break;
		if (vmmfs_vcpu_is_stop_requested(vcpu))
			break;
		if (vmmfs_vcpu_is_reset_requested(vcpu)) {
			vmmfs_vcpu_thread_reset(thread);
			continue;
		}
		KKASSERT(thread->vcpu != NULL);
		/*
		 * Guest execution is kernel work on behalf of this LWP.  Join the
		 * existing user scheduler before VMRUN so a reschedule AST can
		 * release this CPU to a normal user process.
		 */
		lwkt_passive_recover(curthread);
		curthread->td_lwp->lwp_proc->p_usched->acquire_curproc(
		    curthread->td_lwp);
		curthread->td_release = lwkt_passive_release;
		exit = NULL;
		error = vmm_vcpu_run(thread->vcpu, &exit);
		if (error == ERESTART) {
			lwpkthreaddeferred();
			if (curthread->td_lwp->lwp_mpflags & LWP_MP_WEXIT)
				break;
			lwkt_user_yield();
			continue;
		}
		if (error == EINTR)
			continue;
		if (error != 0 || exit == NULL) {
			if (error == 0)
				error = EIO;
			goto out;
		}

		switch (exit->reason) {
		case VMM_CPUEXIT_NONE:
			continue;
		case VMM_CPUEXIT_RDMSR:
		case VMM_CPUEXIT_WRMSR:
			error = vmm_vcpu_inject(thread->vcpu, &exception);
			if (error == 0)
				continue;
			vmmfs_events_log(&vmmfs_vcpu_machine(vcpu)->events,
			    VMMFS_MACHINE_EVENT_VCPU_INJECT_GP_FAILED,
			    "index=%u access=%s error=%d", thread->index,
			    exit->reason == VMM_CPUEXIT_RDMSR ? "rdmsr" : "wrmsr",
			    error);
			goto out;
		case VMM_CPUEXIT_HALTED:
			if (!thread->halted_logged) {
				thread->halted_logged = true;
				vmmfs_events_log(&vmmfs_vcpu_machine(vcpu)->events,
				    VMMFS_MACHINE_EVENT_VCPU_HALTED,
				    "index=%u rip=%#jx", thread->index,
				    (uintmax_t)thread->state.gprs[VMM_X64_GPR_RIP]);
			}
			error = vmm_vcpu_wait(thread->vcpu);
			if (error == 0 || error == EINTR)
				continue;
			goto out;
		case VMM_CPUEXIT_MONITOR:
		case VMM_CPUEXIT_MWAIT:
			if (exit->u.insn.npc == 0) {
				error = EIO;
				goto out;
			}
			thread->state.gprs[VMM_X64_GPR_RIP] = exit->u.insn.npc;
			if (exit->reason == VMM_CPUEXIT_MONITOR)
				continue;
			error = vmm_vcpu_wait(thread->vcpu);
			if (error == 0 || error == EINTR)
				continue;
			goto out;
		case VMM_CPUEXIT_IO:
			error = vmmfs_pciroot_io(&vmmfs_vcpu_machine(vcpu)->pciroot,
			    thread, &thread->state, exit);
			if (error == 0)
				continue;
			goto out;
		case VMM_CPUEXIT_MEMORY:
			error = vmmfs_pciroot_memory(&vmmfs_vcpu_machine(vcpu)->pciroot,
			    thread, exit);
			if (error == 0)
				continue;
			goto out;
		case VMM_CPUEXIT_SHUTDOWN:
			vmmfs_events_log(&vmmfs_vcpu_machine(vcpu)->events,
			    VMMFS_MACHINE_EVENT_VCPU_SHUTDOWN, "index=%u", thread->index);
			vmmfs_vcpu_request_reset(vcpu);
			continue;
		default:
			vmmfs_events_log(&vmmfs_vcpu_machine(vcpu)->events,
			    VMMFS_MACHINE_EVENT_VCPU_UNSUPPORTED_EXIT,
			    "index=%u reason=%#jx", thread->index,
			    (uintmax_t)exit->reason);
			error = EIO;
			goto out;
		}
	}

out:
	if (error != 0 && !vmmfs_vcpu_is_stop_requested(vcpu)) {
		vmmfs_events_log(&vmmfs_vcpu_machine(vcpu)->events,
		    VMMFS_MACHINE_EVENT_VCPU_FAILED, "index=%u error=%d",
		    thread->index, error);
	}
	if (!vmmfs_vcpu_is_stop_requested(vcpu))
		(void)vmmfs_machine_stop_request(vmmfs_vcpu_machine(vcpu), "guest-exit");
	vmmfs_vcpu_thread_stop(thread);
}

static int
vmmfs_vcpu_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_VCPU_MODE, 0));
}

static int
vmmfs_vcpu_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_vcpu *vcpu;
	struct vattr *vattr;
	char buffer[32];
	size_t length;
	int error;

	vcpu = ap->a_vp->v_data;
	if (vcpu == NULL)
		return (ENOENT);
	error = vmmfs_vcpu_load(vcpu, buffer, sizeof(buffer), &length);
	if (error != 0)
		return (error);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_VCPU_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = vcpu->inode;
	vattr->va_size = length;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = length;
	return (0);
}

static int
vmmfs_vcpu_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_VCPU_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_vcpu_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_vcpu_read(struct vop_read_args *ap)
{
	struct vmmfs_vcpu *vcpu;
	struct uio *uio;
	char buffer[32];
	size_t length;
	off_t offset;
	int error;

	vcpu = ap->a_vp->v_data;
	if (vcpu == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	error = vmmfs_vcpu_load(vcpu, buffer, sizeof(buffer), &length);
	if (error != 0)
		return (error);
	offset = uio->uio_offset;
	if ((size_t)offset >= length)
		return (0);
	return (uiomove(buffer + offset, length - (size_t)offset, uio));
}

static int
vmmfs_vcpu_setattr(struct vop_setattr_args *ap)
{
	/* Accept the O_TRUNC size update performed before a control write. */
	(void)ap;
	return (0);
}

static int
vmmfs_vcpu_write(struct vop_write_args *ap)
{
	struct vmmfs_vcpu *vcpu;
	struct uio *uio;
	char buffer[32];
	size_t length;
	int error;

	vcpu = ap->a_vp->v_data;
	if (vcpu == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset != 0 || uio->uio_resid == 0 ||
	    (size_t)uio->uio_resid >= sizeof(buffer))
		return (EINVAL);
	length = (size_t)uio->uio_resid;
	error = uiomove(buffer, length, uio);
	if (error != 0)
		return (error);
	return (vmmfs_vcpu_store(vcpu, buffer, length));
}

static int
vmmfs_vcpu_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_vcpu *vcpu;
	struct vmmfs_machine *machine;

	vcpu = ap->a_vp->v_data;
	if (vcpu == NULL)
		return (0);
	machine = vmmfs_vcpu_machine(vcpu);
	if (!vmmfs_machine_is_dead(machine))
		return (0);
	vmmfs_node_inactive(&vcpu->node, ap->a_vp);
	return (0);
}

static int
vmmfs_vcpu_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_vcpu *vcpu;
	struct vmmfs_machine *machine;
	bool reclaim;

	vcpu = ap->a_vp->v_data;
	if (vcpu == NULL || vmmfs_vcpu_machine(vcpu) == NULL)
		return (0);
	machine = vmmfs_vcpu_machine(vcpu);
	lwkt_gettoken(&machine->token);
	reclaim = vmmfs_node_reclaim(&vcpu->node, ap->a_vp);
	lwkt_reltoken(&machine->token);
	if (reclaim)
		vmmfs_node_drop(&vcpu->node);
	return (0);
}
