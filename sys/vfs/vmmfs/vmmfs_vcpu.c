/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs machine vCPU declaration node.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

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
static void vmmfs_vcpu_thread_main(void *);

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

	if (vcpu == NULL || vmmfs_machine_is_dead(vcpu->machine))
		return (ENOENT);
	count = vcpu->machine->spec.vcpu.count;
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

	lwkt_gettoken(&vcpu->machine->token);
	if (vcpu->machine->dead) {
		lwkt_reltoken(&vcpu->machine->token);
		return (ENOENT);
	}
	if (!vcpu->machine->stopped.expect_stopped ||
	    vcpu->machine->machine != NULL) {
		lwkt_reltoken(&vcpu->machine->token);
		return (EBUSY);
	}
	vcpu->machine->spec.vcpu.count = (uint32_t)value;
	lwkt_reltoken(&vcpu->machine->token);
	return (0);
}

int
vmmfs_vcpu_create(struct vmmfs_machine *machine, struct vmmfs_vcpu *vcpu)
{
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	bzero(vcpu, sizeof(*vcpu));
	vcpu->machine = machine;
	lwkt_token_init(&vcpu->token, "vmmfsvcpu");
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
	vcpu->inode = atomic_fetchadd_int(&state->next_inode, 1);
	if (state->vcpu_vops == NULL)
		return (ENXIO);

	error = getnewvnode(VT_SYNTH, machine->root->mount, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = vcpu;
	vnode->v_ops = &state->vcpu_vops;
	vnode->v_type = VREG;
	vcpu->vnode = vnode;
	vmmfs_machine_hold(machine);
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_vcpu_destroy(struct vmmfs_vcpu *vcpu)
{
	if (vcpu == NULL)
		return (EINVAL);
	lwkt_gettoken(&vcpu->token);
	if (vcpu->active_count != 0 || vcpu->threads != NULL) {
		lwkt_reltoken(&vcpu->token);
		return (EBUSY);
	}
	lwkt_reltoken(&vcpu->token);
	if (vcpu->vnode != NULL)
		return (EBUSY);
	vcpu->machine = NULL;
	return (0);
}

int
vmmfs_vcpu_start(struct vmmfs_vcpu *vcpu, vmm_machine_t machine,
	const struct vmm_cpustate *bsp_state)
{
	struct vmmfs_vcpu_thread *thread;
	uint32_t count;
	uint32_t index;
	int error;

	if (vcpu == NULL || machine == NULL || bsp_state == NULL ||
	    vcpu->threads != NULL)
		return (EINVAL);
	count = vcpu->machine->spec.vcpu.count;
	if (count == 0)
		return (EINVAL);
	thread = kmalloc(sizeof(*thread) * count, M_VMMFS, M_WAITOK | M_ZERO);
	vcpu->threads = thread;
	vcpu->runtime_machine = machine;
	vcpu->count = count;
	lwkt_gettoken(&vcpu->token);
	vcpu->stop_requested = false;
	lwkt_reltoken(&vcpu->token);

	/* Create every LWKT paused so no vCPU can observe partial setup. */
	for (index = 0; index < count; ++index) {
		thread = &vcpu->threads[index];
		thread->group = vcpu;
		thread->index = index;
		error = lwkt_create(vmmfs_vcpu_thread_main, thread,
		    &thread->thread, NULL, TDF_NOSTART, -1, "vmmfs-vcpu%u",
		    index);
		if (error != 0)
			goto failed;
	}

	/* Construct every vCPU before any LWKT is allowed to run. */
	vcpu->threads[0].state = *bsp_state;
	for (index = 0; index < count; ++index) {
		thread = &vcpu->threads[index];
		error = vmm_vcpu_create(machine, &thread->state, &thread->vcpu);
		if (error != 0)
			goto failed;
		error = vmm_vcpu_set_memory_exit_mode(thread->vcpu,
		    VMM_MEMORY_EXIT_EMULATE);
		if (error != 0)
			goto failed;
	}

	lwkt_gettoken(&vcpu->token);
	vcpu->active_count = count;
	lwkt_reltoken(&vcpu->token);
	for (index = 0; index < count; ++index)
		lwkt_schedule(vcpu->threads[index].thread);
	return (0);

failed:
	for (index = 0; index < count; ++index) {
		thread = &vcpu->threads[index];
		if (thread->vcpu != NULL) {
			int destroy_error;

			destroy_error = vmm_vcpu_destroy(thread->vcpu);
			KKASSERT(destroy_error == 0);
			thread->vcpu = NULL;
		}
		if (thread->thread != NULL) {
			lwkt_free_thread(thread->thread);
			thread->thread = NULL;
		}
	}
	kfree(vcpu->threads, M_VMMFS);
	vcpu->threads = NULL;
	vcpu->runtime_machine = NULL;
	vcpu->count = 0;
	return (error);
}

int
vmmfs_vcpu_stop(struct vmmfs_vcpu *vcpu)
{
	uint32_t index;
	int error;

	if (vcpu == NULL)
		return (EINVAL);
	if (vcpu->threads == NULL)
		return (0);
	lwkt_gettoken(&vcpu->token);
	vcpu->stop_requested = true;
	lwkt_reltoken(&vcpu->token);
	for (index = 0; index < vcpu->count; ++index) {
		if (vcpu->threads[index].vcpu != NULL) {
			(void)vmm_vcpu_kick(vcpu->threads[index].vcpu);
		}
	}
	lwkt_gettoken(&vcpu->token);
	while (vcpu->active_count != 0) {
		tsleep_interlock(vcpu, 0);
		if (vcpu->active_count != 0)
			(void)tsleep(vcpu, PINTERLOCKED, "vmmvstop", 0);
	}
	lwkt_reltoken(&vcpu->token);
	for (index = 0; index < vcpu->count; ++index) {
		if (vcpu->threads[index].vcpu != NULL) {
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u stopped rip=%#jx rcx=%#jx rsi=%#jx rdi=%#jx",
			    index,
			    (uintmax_t)vcpu->threads[index].state.gprs[VMM_X64_GPR_RIP],
			    (uintmax_t)vcpu->threads[index].state.gprs[VMM_X64_GPR_RCX],
			    (uintmax_t)vcpu->threads[index].state.gprs[VMM_X64_GPR_RSI],
			    (uintmax_t)vcpu->threads[index].state.gprs[VMM_X64_GPR_RDI]);
			error = vmm_vcpu_destroy(vcpu->threads[index].vcpu);
			if (error != 0)
				return (error);
			vcpu->threads[index].vcpu = NULL;
		}
	}
	kfree(vcpu->threads, M_VMMFS);
	vcpu->threads = NULL;
	vcpu->runtime_machine = NULL;
	vcpu->count = 0;
	return (0);
}

static void
vmmfs_vcpu_thread_main(void *argument)
{
	struct vmmfs_vcpu_thread *thread;
	struct vmmfs_vcpu *vcpu;
	struct vmm_cpuexit *exit;
	struct vmm_cpuevent exception = {
		.type = VMM_CPUEVENT_EXCP,
		.vector = VMM_X64_EXCEPTION_GP,
	};
	bool stop_requested;
	int error = 0;

	thread = argument;
	vcpu = thread->group;
	lwkt_setpri_self(TDPRI_USER_NORM);
	KKASSERT(thread->vcpu != NULL);
	for (;;) {
		lwkt_gettoken(&vcpu->token);
		stop_requested = vcpu->stop_requested;
		lwkt_reltoken(&vcpu->token);
		if (stop_requested)
			break;

		exit = NULL;
		error = vmm_vcpu_run(thread->vcpu, &exit);

		lwkt_gettoken(&vcpu->token);
		/*
		 * SVM uses ERESTART when DragonFly has pending root work.
		 * Unlike the NVMM ioctl path, this is a pure kernel LWKT.  Use
		 * the kernel yield path so a stale LWKT reschedule request is
		 * consumed before attempting VMRUN again.
		 */
		if (error == ERESTART) {
			lwkt_reltoken(&vcpu->token);
			lwkt_yield_quick();
			continue;
		}
		if (error == EINTR) {
			stop_requested = vcpu->stop_requested;
			lwkt_reltoken(&vcpu->token);
			if (stop_requested)
				goto out;
			continue;
		}
		if (error != 0) {
			lwkt_reltoken(&vcpu->token);
			goto out;
		}
		if (exit == NULL) {
			error = EIO;
			lwkt_reltoken(&vcpu->token);
			goto out;
		}

		switch (exit->reason) {
		case VMM_CPUEXIT_NONE:
			lwkt_reltoken(&vcpu->token);
			continue;
		case VMM_CPUEXIT_NMI_READY:
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u unexpected nmi-ready exit", thread->index);
			lwkt_reltoken(&vcpu->token);
			goto out;
		case VMM_CPUEXIT_RDMSR:
		case VMM_CPUEXIT_WRMSR:
			lwkt_reltoken(&vcpu->token);
			error = vmm_vcpu_inject(thread->vcpu, &exception);
			if (error == 0) {
				continue;
			}
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u %s inject-gp failed error=%d", thread->index,
			    exit->reason == VMM_CPUEXIT_RDMSR ? "rdmsr" : "wrmsr",
			    error);
			goto out;
		case VMM_CPUEXIT_HALTED:
			if (!thread->halted_logged) {
				thread->halted_logged = true;
				vmmfs_events_log(&vcpu->machine->events,
				    "vcpu%u halted rip=%#jx", thread->index,
				    (uintmax_t)thread->state.gprs[VMM_X64_GPR_RIP]);
			}
			lwkt_reltoken(&vcpu->token);
			error = vmm_vcpu_wait(thread->vcpu);
			if (error == 0)
				continue;
			goto out;
		case VMM_CPUEXIT_MONITOR:
			if (exit->u.insn.npc == 0) {
				vmmfs_events_log(&vcpu->machine->events,
				    "vcpu%u monitor missing-npc", thread->index);
				lwkt_reltoken(&vcpu->token);
				goto out;
			}
			thread->state.gprs[VMM_X64_GPR_RIP] = exit->u.insn.npc;
			lwkt_reltoken(&vcpu->token);
			continue;
		case VMM_CPUEXIT_MWAIT:
			if (exit->u.insn.npc == 0) {
				vmmfs_events_log(&vcpu->machine->events,
				    "vcpu%u mwait missing-npc", thread->index);
				lwkt_reltoken(&vcpu->token);
				goto out;
			}
			thread->state.gprs[VMM_X64_GPR_RIP] = exit->u.insn.npc;
			lwkt_reltoken(&vcpu->token);
			error = vmm_vcpu_wait(thread->vcpu);
			if (error == 0)
				continue;
			goto out;
		case VMM_CPUEXIT_IO:
			lwkt_reltoken(&vcpu->token);
			error = vmmfs_pciroot_io(&vcpu->machine->pciroot,
			    thread, &thread->state, exit);
			if (error == 0)
				continue;
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u unhandled pio %s port=%#x width=%u str=%d rep=%d rip=%#jx",
			    thread->index, exit->u.io.in ? "read" : "write",
			    exit->u.io.port, exit->u.io.operand_size, exit->u.io.str,
			    exit->u.io.rep,
			    (uintmax_t)thread->state.gprs[VMM_X64_GPR_RIP]);
			goto out;
		case VMM_CPUEXIT_MEMORY:
			lwkt_reltoken(&vcpu->token);
			error = vmmfs_pciroot_memory(&vcpu->machine->pciroot,
			    thread, exit);
			if (error == 0)
				continue;
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u unhandled memory gpa=%#jx prot=%#x width=%u value=%#jx rip=%#jx error=%d",
			    thread->index, (uintmax_t)exit->u.mem.gpa,
			    exit->u.mem.prot, exit->u.mem.width,
			    (uintmax_t)exit->u.mem.value,
			    (uintmax_t)thread->state.gprs[VMM_X64_GPR_RIP], error);
			goto out;
		case VMM_CPUEXIT_SHUTDOWN:
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u guest shutdown", thread->index);
			lwkt_reltoken(&vcpu->token);
			goto out;
		case VMM_CPUEXIT_INT_READY:
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u unexpected interrupt-ready exit", thread->index);
			lwkt_reltoken(&vcpu->token);
			goto out;
		case VMM_CPUEXIT_TPR_CHANGED:
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u unexpected tpr-changed exit", thread->index);
			lwkt_reltoken(&vcpu->token);
			goto out;
		case VMM_CPUEXIT_CPUID:
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u unhandled cpuid exit", thread->index);
			lwkt_reltoken(&vcpu->token);
			goto out;
		case VMM_CPUEXIT_INVALID:
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u invalid exit hwcode=%#jx", thread->index,
			    (uintmax_t)exit->u.inv.hwcode);
			lwkt_reltoken(&vcpu->token);
			goto out;
		default:
			vmmfs_events_log(&vcpu->machine->events,
			    "vcpu%u unsupported exit reason=%#jx", thread->index,
			    (uintmax_t)exit->reason);
			lwkt_reltoken(&vcpu->token);
			goto out;
		}
	}

out:
	lwkt_gettoken(&vcpu->token);
	if (error != 0 && error != EINTR && error != ERESTART &&
	    !vcpu->stop_requested) {
		vmmfs_events_log(&vcpu->machine->events,
		    "vcpu%u failed error=%d", thread->index, error);
	}
	KKASSERT(vcpu->active_count != 0);
	--vcpu->active_count;
	wakeup(vcpu);
	lwkt_reltoken(&vcpu->token);
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
	machine = vcpu->machine;
	if (!vmmfs_machine_vnode_detach(machine, &vcpu->vnode, ap->a_vp))
		return (0);
	ap->a_vp->v_data = NULL;
	vmmfs_machine_put(machine);
	return (0);
}

static int
vmmfs_vcpu_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_vcpu *vcpu;
	struct vmmfs_machine *machine;

	vcpu = ap->a_vp->v_data;
	if (vcpu != NULL) {
		machine = vcpu->machine;
		if (vcpu->vnode == ap->a_vp)
			vcpu->vnode = NULL;
	} else {
		machine = NULL;
	}
	ap->a_vp->v_data = NULL;
	if (machine != NULL)
		vmmfs_machine_put(machine);
	return (0);
}
