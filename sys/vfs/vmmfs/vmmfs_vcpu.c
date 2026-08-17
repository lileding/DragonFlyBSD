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
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_vcpu_destroy(struct vmmfs_vcpu *vcpu)
{
	struct vnode *vnode;

	if (vcpu == NULL)
		return (EINVAL);
	if (atomic_fetchadd_int(&vcpu->active_count, 0) != 0 ||
	    vcpu->threads != NULL)
		return (EBUSY);
	vnode = vcpu->vnode;
	if (vnode != NULL) {
		vx_get(vnode);
		vgone_vxlocked(vnode);
		vx_put(vnode);
		vrele(vnode);
	}
	KKASSERT(vcpu->vnode == NULL);
	vcpu->machine = NULL;
	return (0);
}

int
vmmfs_vcpu_start(struct vmmfs_vcpu *vcpu, vmm_machine_t machine,
	const struct vmm_cpustate *state)
{
	struct vmmfs_vcpu_thread *thread;
	uint32_t count;
	uint32_t index;
	int error;

	if (vcpu == NULL || machine == NULL || state == NULL ||
	    vcpu->threads != NULL)
		return (EINVAL);
	count = vcpu->machine->spec.vcpu.count;
	if (count == 0)
		return (EINVAL);
	thread = kmalloc(sizeof(*thread) * count, M_VMMFS, M_WAITOK | M_ZERO);
	vcpu->threads = thread;
	vcpu->runtime_machine = machine;
	vcpu->count = count;
	atomic_store_rel_int(&vcpu->active_count, 0);
	atomic_store_rel_int(&vcpu->ready_count, 0);
	atomic_store_rel_int(&vcpu->release, 0);
	atomic_store_rel_int(&vcpu->stop_requested, 0);
	atomic_store_rel_int(&vcpu->start_error, 0);

	for (index = 0; index < count; ++index) {
		thread = &vcpu->threads[index];
		thread->group = vcpu;
		thread->index = index;
		thread->state = *state;
		atomic_add_int(&vcpu->active_count, 1);
		error = lwkt_create(vmmfs_vcpu_thread_main, thread,
		    &thread->thread, NULL, 0, -1, "vmmfs-vcpu%u", index);
		if (error != 0) {
			atomic_add_int(&vcpu->active_count, -1);
			atomic_cmpset_int(&vcpu->start_error, 0, error);
			break;
		}
	}
	while (atomic_fetchadd_int(&vcpu->ready_count, 0) !=
	    atomic_fetchadd_int(&vcpu->active_count, 0) &&
	    atomic_fetchadd_int(&vcpu->active_count, 0) != 0) {
		tsleep_interlock(vcpu, PCATCH);
		if (atomic_fetchadd_int(&vcpu->ready_count, 0) ==
		    atomic_fetchadd_int(&vcpu->active_count, 0) ||
		    atomic_fetchadd_int(&vcpu->active_count, 0) == 0)
			continue;
		error = tsleep(vcpu, PINTERLOCKED | PCATCH, "vmmvcpu", hz * 10);
		if (error != 0) {
			atomic_cmpset_int(&vcpu->start_error, 0,
			    error == EWOULDBLOCK ? ETIMEDOUT : error);
			break;
		}
	}
	error = atomic_fetchadd_int(&vcpu->start_error, 0);
	if (error != 0 || atomic_fetchadd_int(&vcpu->ready_count, 0) != count) {
		(void)vmmfs_vcpu_stop(vcpu);
		return (error != 0 ? error : EIO);
	}
	atomic_store_rel_int(&vcpu->release, 1);
	for (index = 0; index < count; ++index)
		wakeup(&vcpu->threads[index]);
	return (0);
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
	atomic_store_rel_int(&vcpu->stop_requested, 1);
	for (index = 0; index < vcpu->count; ++index) {
		if (vcpu->threads[index].vcpu != NULL) {
			(void)vmm_vcpu_kick(vcpu->threads[index].vcpu);
			wakeup(vcpu->threads[index].vcpu);
		}
		wakeup(&vcpu->threads[index]);
	}
	while (atomic_fetchadd_int(&vcpu->active_count, 0) != 0) {
		tsleep_interlock(vcpu, 0);
		if (atomic_fetchadd_int(&vcpu->active_count, 0) != 0)
			(void)tsleep(vcpu, PINTERLOCKED, "vmmvstop", 0);
	}
	for (index = 0; index < vcpu->count; ++index) {
		if (vcpu->threads[index].vcpu != NULL) {
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
	char event[96];
	int error;

	thread = argument;
	vcpu = thread->group;
	lwkt_setpri_self(TDPRI_USER_NORM);
	error = vmm_vcpu_create(vcpu->runtime_machine, &thread->state,
	    &thread->vcpu);
	if (error != 0)
		atomic_cmpset_int(&vcpu->start_error, 0, error);
	atomic_add_int(&vcpu->ready_count, 1);
	wakeup(vcpu);
	while (error == 0 && !atomic_fetchadd_int(&vcpu->release, 0) &&
	    !atomic_fetchadd_int(&vcpu->stop_requested, 0)) {
		tsleep_interlock(thread, 0);
		if (!atomic_fetchadd_int(&vcpu->release, 0) &&
		    !atomic_fetchadd_int(&vcpu->stop_requested, 0))
			(void)tsleep(thread, PINTERLOCKED, "vmmvwait", 0);
	}
	while (error == 0 && !atomic_fetchadd_int(&vcpu->stop_requested, 0)) {
		exit = NULL;
		error = vmm_vcpu_run(thread->vcpu, &exit);
		if (error == EINTR)
			continue;
		if (error != 0)
			break;
		if (exit == NULL || exit->reason == VMM_CPUEXIT_NONE)
			continue;
		if (exit->reason == VMM_CPUEXIT_HALTED) {
			if (!thread->halted_logged) {
				thread->halted_logged = true;
				ksnprintf(event, sizeof(event), "vcpu%u halted",
				    thread->index);
				vmmfs_events_log(&vcpu->machine->events, event);
			}
			(void)tsleep(thread, PCATCH, "vmmhlt", hz / 20 + 1);
			continue;
		}
		ksnprintf(event, sizeof(event), "vcpu%u exit reason=%#jx",
		    thread->index, (uintmax_t)exit->reason);
		vmmfs_events_log(&vcpu->machine->events, event);
		break;
	}
	if (error != 0 && error != EINTR &&
	    !atomic_fetchadd_int(&vcpu->stop_requested, 0)) {
		ksnprintf(event, sizeof(event), "vcpu%u failed error=%d",
		    thread->index, error);
		vmmfs_events_log(&vcpu->machine->events, event);
	}
	atomic_add_int(&vcpu->active_count, -1);
	wakeup(vcpu);
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
vmmfs_vcpu_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_vcpu *vcpu;

	vcpu = ap->a_vp->v_data;
	if (vcpu != NULL && vcpu->vnode == ap->a_vp)
		vcpu->vnode = NULL;
	ap->a_vp->v_data = NULL;
	return (0);
}
