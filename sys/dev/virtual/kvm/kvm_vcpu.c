/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-vCPU file descriptor and shared KVM_RUN state for the DragonFly KVM
 * frontend.
 */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/sysmsg.h>
#include <sys/thread.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/pmap.h>

#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <linux/kvm.h>

#include "../vmm/vmm.h"
#include "kvm_internal.h"
#include "kvm_vcpu.h"
#include "kvm_vm.h"

struct kvm_vcpu {
	/* token protects state access, running, and pending exit completion. */
	struct lwkt_token token;
	struct kvm_vm *vm;
	vmm_vcpu_t vcpu;
	struct vmm_cpustate state;
	struct vm_object *run_object;
	vm_page_t run_pages[2];
	struct kvm_run *run;
	uint8_t *pio_data;
	uint64_t apic_base;
	uint64_t vapic_address;
	uint64_t pio_gpa;
	uint64_t pio_npc;
	unsigned int id;
	uint8_t pio_address_size;
	uint8_t pio_size;
	uint8_t mmio_size;
	uint32_t mp_state;
	bool running;
	bool pio_pending;
	bool pio_in;
	bool pio_rep;
	bool pio_string;
	bool mmio_pending;
	bool mmio_write;
};

#define KVM_MSR_IA32_TSC		0x00000010U
#define KVM_MSR_KVM_WALL_CLOCK		0x00000011U
#define KVM_MSR_KVM_SYSTEM_TIME	0x00000012U
#define KVM_MSR_IA32_APICBASE		0x0000001bU
#define KVM_MSR_IA32_MTRRCAP		0x000000feU
#define KVM_MSR_IA32_MTRR_DEF_TYPE	0x000002ffU
#define KVM_MSR_IA32_SYSENTER_CS	0x00000174U
#define KVM_MSR_IA32_SYSENTER_ESP	0x00000175U
#define KVM_MSR_IA32_SYSENTER_EIP	0x00000176U
#define KVM_MSR_IA32_PAT		0x00000277U
#define KVM_MSR_EFER			0xc0000080U
#define KVM_MSR_STAR			0xc0000081U
#define KVM_MSR_LSTAR			0xc0000082U
#define KVM_MSR_CSTAR			0xc0000083U
#define KVM_MSR_SFMASK			0xc0000084U
#define KVM_MSR_KERNEL_GS_BASE		0xc0000102U
#define KVM_MSR_K7_PERFCTL0		0xc0010000U
#define KVM_MSR_K7_PERFCTR3		0xc0010007U
#define KVM_X86_EXCEPTION_GP		13U

#define KVM_VCPU_TRACE_LIMIT		64U

static int kvm_vcpu_trace;
static unsigned int kvm_vcpu_trace_count;
static unsigned int kvm_vcpu_memory_trace_count;

SYSCTL_NODE(_debug, OID_AUTO, kvm, CTLFLAG_RW, 0,
    "KVM frontend debug controls");
SYSCTL_INT(_debug_kvm, OID_AUTO, trace, CTLFLAG_RW, &kvm_vcpu_trace, 0,
    "log the first KVM vCPU exits after module load");

static d_priv_dtor_t kvm_vcpu_file_destroy;
static int kvm_vcpu_fo_read(struct file *, struct uio *, struct ucred *, int);
static int kvm_vcpu_fo_write(struct file *, struct uio *, struct ucred *, int);
static int kvm_vcpu_fo_ioctl(struct file *, u_long, caddr_t,
	struct ucred *, struct sysmsg *);
static int kvm_vcpu_fo_kqfilter(struct file *, struct knote *);
static int kvm_vcpu_fo_stat(struct file *, struct stat *, struct ucred *);
static int kvm_vcpu_fo_close(struct file *);
static int kvm_vcpu_fo_seek(struct file *, off_t, int, off_t *);
static int kvm_vcpu_alloc_run(struct kvm_vcpu *);
static void kvm_vcpu_free_run(struct kvm_vcpu *);
static int kvm_vcpu_run(struct kvm_vcpu *);
static int kvm_vcpu_get_regs(struct kvm_vcpu *, struct kvm_regs *);
static int kvm_vcpu_set_regs(struct kvm_vcpu *, const struct kvm_regs *);
static int kvm_vcpu_get_sregs(struct kvm_vcpu *, struct kvm_sregs *);
static int kvm_vcpu_set_sregs(struct kvm_vcpu *, const struct kvm_sregs *);
static int kvm_vcpu_set_cpuid(struct kvm_vcpu *,
	const struct kvm_dfly_buffer *);
static int kvm_vcpu_get_msrs(struct kvm_vcpu *,
	const struct kvm_dfly_buffer *, int *);
static int kvm_vcpu_set_msrs(struct kvm_vcpu *,
	const struct kvm_dfly_buffer *, int *);
static int kvm_vcpu_get_fpu(struct kvm_vcpu *, struct kvm_fpu *);
static int kvm_vcpu_set_fpu(struct kvm_vcpu *, const struct kvm_fpu *);
static int kvm_vcpu_get_xsave(struct kvm_vcpu *, struct kvm_xsave *);
static int kvm_vcpu_set_xsave(struct kvm_vcpu *, const struct kvm_xsave *);
static int kvm_vcpu_get_xcrs(struct kvm_vcpu *, struct kvm_xcrs *);
static int kvm_vcpu_set_xcrs(struct kvm_vcpu *, const struct kvm_xcrs *);
static int kvm_vcpu_get_events(struct kvm_vcpu *, struct kvm_vcpu_events *);
static int kvm_vcpu_set_events(struct kvm_vcpu *,
	const struct kvm_vcpu_events *);
static int kvm_vcpu_get_debugregs(struct kvm_vcpu *,
	struct kvm_debugregs *);
static int kvm_vcpu_set_debugregs(struct kvm_vcpu *,
	const struct kvm_debugregs *);
static int kvm_vcpu_get_mp_state(struct kvm_vcpu *, struct kvm_mp_state *);
static int kvm_vcpu_set_mp_state(struct kvm_vcpu *,
    const struct kvm_mp_state *);
static int kvm_vcpu_get_lapic(struct kvm_vcpu *, struct kvm_lapic_state *);
static int kvm_vcpu_set_lapic(struct kvm_vcpu *,
    const struct kvm_lapic_state *);
static void kvm_vcpu_reset_state(struct vmm_cpustate *);
static void kvm_vcpu_segment_from_vmm(struct kvm_segment *,
	const struct vmm_segment *);
static void kvm_vcpu_segment_to_vmm(struct vmm_segment *,
	const struct kvm_segment *);
static void kvm_vcpu_set_exit(struct kvm_vcpu *,
	const struct vmm_cpuexit *);
static int kvm_vcpu_handle_msr_exit(struct kvm_vcpu *,
	const struct vmm_cpuexit *);
static int kvm_vcpu_complete_pio(struct kvm_vcpu *);
static int kvm_vcpu_copy_gpa(struct kvm_vcpu *, uint64_t, void *, size_t,
	int);
static int kvm_vcpu_pio_string_gpa(struct kvm_vcpu *,
	const struct vmm_cpuexit *, uint64_t *);
static int kvm_vcpu_complete_mmio(struct kvm_vcpu *);
static int kvm_vcpu_set_mmio_exit(struct kvm_vcpu *,
	const struct vmm_cpuexit *);
static int kvm_vcpu_get_msr(struct kvm_vcpu *, uint32_t, uint64_t *);
static int kvm_vcpu_set_msr(struct kvm_vcpu *, uint32_t, uint64_t);
static void kvm_vcpu_destroy(struct kvm_vcpu *);

static struct fileops kvm_vcpu_fileops = {
	.fo_read = kvm_vcpu_fo_read,
	.fo_write = kvm_vcpu_fo_write,
	.fo_ioctl = kvm_vcpu_fo_ioctl,
	.fo_kqfilter = kvm_vcpu_fo_kqfilter,
	.fo_stat = kvm_vcpu_fo_stat,
	.fo_close = kvm_vcpu_fo_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = kvm_vcpu_fo_seek,
};

int
kvm_vcpu_create(struct kvm_vm *vm, struct lwp *lp, struct vnode *vp,
	uint32_t id, int *fd)
{
	struct kvm_vcpu *vcpu;
	struct file *fp;
	struct vmm_x64_capability capability;
	const char *stage;
	int error;

	if (vm == NULL || lp == NULL || vp == NULL || fd == NULL ||
	    id >= KVM_MAX_VCPUS) {
		if (kvm_vcpu_trace) {
			kprintf("kvm: create vcpu%u invalid vm=%p lwp=%p vnode=%p fd=%p\n",
			    id, vm, lp, vp, fd);
		}
		return EINVAL;
	}
	*fd = -1;
	stage = "allocation";
	lwkt_gettoken(&vm->token);
	if (vm->vcpus[id] != NULL) {
		lwkt_reltoken(&vm->token);
		return EEXIST;
	}
	lwkt_reltoken(&vm->token);

	vcpu = kmalloc(sizeof(*vcpu), M_KVM, M_WAITOK | M_ZERO);
	if (vcpu == NULL)
		return ENOMEM;
	vcpu->id = id;
	lwkt_token_init(&vcpu->token, "kvmvcpu");
	kvm_vcpu_reset_state(&vcpu->state);
	stage = "capability";
	error = vmm_x64_get_capability(&capability);
	if (error != 0)
		goto fail;
	vcpu->state.fpu.fx_mxcsr_mask = capability.mxcsr_mask;
	vcpu->mp_state = KVM_MP_STATE_RUNNABLE;

	stage = "run-page allocation";
	error = kvm_vcpu_alloc_run(vcpu);
	if (error != 0)
		goto fail;
	stage = "vmm vcpu allocation";
	error = vmm_vcpu_create(vm->machine, &vcpu->state, &vcpu->vcpu);
	if (error != 0)
		goto fail;

	lwkt_gettoken(&kvm_frontend_token);
	if (kvm_draining) {
		lwkt_reltoken(&kvm_frontend_token);
		error = EBUSY;
		goto fail;
	}
	++kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);

	lwkt_gettoken(&vm->token);
	if (vm->vcpus[id] != NULL) {
		lwkt_reltoken(&vm->token);
		lwkt_gettoken(&kvm_frontend_token);
		KKASSERT(kvm_file_count != 0);
		--kvm_file_count;
		lwkt_reltoken(&kvm_frontend_token);
		error = EEXIST;
		goto fail;
	}
	vcpu->vm = vm;
	vm->vcpus[id] = vcpu;
	++vm->references;
	lwkt_reltoken(&vm->token);

	stage = "file allocation";
	error = falloc(lp, &fp, fd);
	if (error != 0)
		goto fail;
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &kvm_vcpu_fileops;
	fp->f_data = vp;
	vref(vp);
	stage = "file private data";
	error = devfs_set_cdevpriv(fp, vcpu, kvm_vcpu_file_destroy);
	if (error != 0) {
		(void)fp_close(fp);
		fsetfd(lp->lwp_proc->p_fd, NULL, *fd);
		fdrop(fp);
		goto fail;
	}
	fsetfd(lp->lwp_proc->p_fd, fp, *fd);
	fdrop(fp);
	return 0;

fail:
	if (kvm_vcpu_trace)
		kprintf("kvm: create vcpu%u failed at %s: %d\n", id, stage,
		    error);
	kvm_vcpu_destroy(vcpu);
	return error;
}

int
kvm_vcpu_mmap_single(struct dev_mmap_single_args *ap)
{
	struct kvm_vcpu *vcpu;
	int error;

	if (ap == NULL || ap->a_fp == NULL ||
	    (ap->a_nprot & PROT_EXEC) != 0 || ap->a_size == 0 ||
	    (*ap->a_offset & PAGE_MASK) != 0 ||
	    (ap->a_size & PAGE_MASK) != 0 ||
	    *ap->a_offset > 2 * PAGE_SIZE - ap->a_size)
		return EINVAL;
	if (ap->a_fp->f_ops != &kvm_vcpu_fileops)
		return ENODEV;
	error = devfs_get_cdevpriv(ap->a_fp, (void **)&vcpu);
	if (error != 0 || vcpu == NULL || vcpu->run_object == NULL)
		return error != 0 ? error : ENXIO;
	vm_object_reference_quick(vcpu->run_object);
	*ap->a_object = vcpu->run_object;
	return 0;
}

static int
kvm_vcpu_fo_read(struct file *fp, struct uio *uio, struct ucred *cred,
	int flags)
{

	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
kvm_vcpu_fo_write(struct file *fp, struct uio *uio, struct ucred *cred,
	int flags)
{

	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
kvm_vcpu_fo_ioctl(struct file *fp, u_long command, caddr_t data,
	struct ucred *cred, struct sysmsg *msg)
{
	struct kvm_vcpu *vcpu;
	int error;
	int result;

	(void)cred;
	error = devfs_get_cdevpriv(fp, (void **)&vcpu);
	if (error != 0)
		return error;
	switch (command) {
	case KVM_RUN:
		return kvm_vcpu_run(vcpu);
	case KVM_GET_REGS:
		return kvm_vcpu_get_regs(vcpu, (struct kvm_regs *)data);
	case KVM_SET_REGS:
		return kvm_vcpu_set_regs(vcpu, (const struct kvm_regs *)data);
	case KVM_GET_SREGS:
		return kvm_vcpu_get_sregs(vcpu, (struct kvm_sregs *)data);
	case KVM_SET_SREGS:
		return kvm_vcpu_set_sregs(vcpu,
		    (const struct kvm_sregs *)data);
	case KVM_DFLY_SET_CPUID2:
		return kvm_vcpu_set_cpuid(vcpu,
		    (const struct kvm_dfly_buffer *)data);
	case KVM_DFLY_GET_MSRS:
		error = kvm_vcpu_get_msrs(vcpu,
		    (const struct kvm_dfly_buffer *)data, &result);
		if (error == 0)
			msg->sysmsg_result = result;
		return error;
	case KVM_DFLY_SET_MSRS:
		error = kvm_vcpu_set_msrs(vcpu,
		    (const struct kvm_dfly_buffer *)data, &result);
		if (error == 0)
			msg->sysmsg_result = result;
		return error;
	case KVM_GET_FPU:
		return kvm_vcpu_get_fpu(vcpu, (struct kvm_fpu *)data);
	case KVM_SET_FPU:
		return kvm_vcpu_set_fpu(vcpu, (const struct kvm_fpu *)data);
	case KVM_GET_XSAVE:
		return kvm_vcpu_get_xsave(vcpu, (struct kvm_xsave *)data);
	case KVM_SET_XSAVE:
		return kvm_vcpu_set_xsave(vcpu,
		    (const struct kvm_xsave *)data);
	case KVM_GET_XCRS:
		return kvm_vcpu_get_xcrs(vcpu, (struct kvm_xcrs *)data);
	case KVM_SET_XCRS:
		return kvm_vcpu_set_xcrs(vcpu,
		    (const struct kvm_xcrs *)data);
	case KVM_GET_VCPU_EVENTS:
		return kvm_vcpu_get_events(vcpu,
		    (struct kvm_vcpu_events *)data);
	case KVM_SET_VCPU_EVENTS:
		return kvm_vcpu_set_events(vcpu,
		    (const struct kvm_vcpu_events *)data);
	case KVM_GET_DEBUGREGS:
		return kvm_vcpu_get_debugregs(vcpu,
		    (struct kvm_debugregs *)data);
	case KVM_SET_DEBUGREGS:
		return kvm_vcpu_set_debugregs(vcpu,
		    (const struct kvm_debugregs *)data);
	case KVM_GET_MP_STATE:
		return kvm_vcpu_get_mp_state(vcpu,
		    (struct kvm_mp_state *)data);
	case KVM_SET_MP_STATE:
		return kvm_vcpu_set_mp_state(vcpu,
		    (const struct kvm_mp_state *)data);
	case KVM_GET_LAPIC:
		return kvm_vcpu_get_lapic(vcpu,
		    (struct kvm_lapic_state *)data);
	case KVM_SET_LAPIC:
		return kvm_vcpu_set_lapic(vcpu,
		    (const struct kvm_lapic_state *)data);
	case KVM_SET_VAPIC_ADDR:
		if ((((const struct kvm_vapic_addr *)data)->vapic_addr & PAGE_MASK) != 0 ||
		    ((const struct kvm_vapic_addr *)data)->vapic_addr >= KVM_GPA_MAX)
			return EINVAL;
		lwkt_gettoken(&vcpu->token);
		vcpu->vapic_address =
		    ((const struct kvm_vapic_addr *)data)->vapic_addr;
		lwkt_reltoken(&vcpu->token);
		return 0;
	default:
		(void)msg;
		return ENOTTY;
	}
}

int
kvm_vcpu_get_supported_msrs(uint32_t *indices, size_t *count)
{
	static const uint32_t supported[] = {
		KVM_MSR_IA32_TSC,
		KVM_MSR_IA32_APICBASE,
		KVM_MSR_IA32_MTRRCAP,
		KVM_MSR_IA32_SYSENTER_CS,
		KVM_MSR_IA32_SYSENTER_ESP,
		KVM_MSR_IA32_SYSENTER_EIP,
		KVM_MSR_IA32_PAT,
		KVM_MSR_EFER,
		KVM_MSR_STAR,
		KVM_MSR_LSTAR,
		KVM_MSR_CSTAR,
		KVM_MSR_SFMASK,
		KVM_MSR_KERNEL_GS_BASE,
	};
	size_t index;

	if (count == NULL)
		return EINVAL;
	if (indices == NULL) {
		*count = nitems(supported);
		return 0;
	}
	if (*count < nitems(supported)) {
		*count = nitems(supported);
		return E2BIG;
	}
	for (index = 0; index < nitems(supported); ++index)
		indices[index] = supported[index];
	*count = nitems(supported);
	return 0;
}

static int
kvm_vcpu_fo_kqfilter(struct file *fp, struct knote *kn)
{

	(void)fp;
	(void)kn;
	return EOPNOTSUPP;
}

static int
kvm_vcpu_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{

	(void)fp;
	(void)cred;
	bzero(sb, sizeof(*sb));
	sb->st_nlink = 1;
	sb->st_mode = S_IFCHR | 0600;
	sb->st_uid = UID_ROOT;
	sb->st_gid = GID_WHEEL;
	sb->st_blksize = PAGE_SIZE;
	sb->__old_st_blksize = sb->st_blksize;
	return 0;
}

static int
kvm_vcpu_fo_close(struct file *fp)
{
	struct vnode *vp;

	vp = fp->f_data;
	fp->f_data = NULL;
	atomic_clear_int(&fp->f_flag, FHASLOCK);
	fp->f_ops = &badfileops;
	if (vp != NULL)
		vrele(vp);
	devfs_clear_cdevpriv(fp);
	return 0;
}

static int
kvm_vcpu_fo_seek(struct file *fp, off_t offset, int whence, off_t *result)
{

	(void)fp;
	(void)offset;
	(void)whence;
	(void)result;
	return ESPIPE;
}

static int
kvm_vcpu_alloc_run(struct kvm_vcpu *vcpu)
{
	struct vm_object *object;
	vm_page_t page;
	unsigned int index;

	object = vm_object_allocate(OBJT_DEFAULT, 2);
	if (object == NULL)
		return ENOMEM;
	vm_object_set_flag(object, OBJ_NOSPLIT);
	for (index = 0; index < 2; ++index) {
		page = vm_page_grab(object, index, VM_ALLOC_NORMAL |
		    VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
		if (page == NULL)
			goto fail;
		vm_page_wire(page);
		vm_page_wakeup(page);
		vcpu->run_pages[index] = page;
	}
	vcpu->run_object = object;
	vcpu->run = (struct kvm_run *)PHYS_TO_DMAP(
	    VM_PAGE_TO_PHYS(vcpu->run_pages[0]));
	vcpu->pio_data = (uint8_t *)PHYS_TO_DMAP(
	    VM_PAGE_TO_PHYS(vcpu->run_pages[1]));
	return 0;

fail:
	while (index != 0) {
		--index;
		vm_page_busy_wait(vcpu->run_pages[index], FALSE, "kvmrun");
		vm_page_unwire(vcpu->run_pages[index], 0);
		vm_page_wakeup(vcpu->run_pages[index]);
	}
	vm_object_deallocate(object);
	return ENOMEM;
}

static void
kvm_vcpu_free_run(struct kvm_vcpu *vcpu)
{
	unsigned int index;

	if (vcpu->run_object == NULL)
		return;
	for (index = 0; index < 2; ++index) {
		vm_page_busy_wait(vcpu->run_pages[index], FALSE, "kvmrun");
		vm_page_unwire(vcpu->run_pages[index], 0);
		vm_page_wakeup(vcpu->run_pages[index]);
	}
	vm_object_deallocate(vcpu->run_object);
	vcpu->run_object = NULL;
	vcpu->run = NULL;
	vcpu->pio_data = NULL;
}

static int
kvm_vcpu_run(struct kvm_vcpu *vcpu)
{
	struct vmm_cpuexit *exit;
	int error;

	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	if (vcpu->run->immediate_exit != 0) {
		lwkt_reltoken(&vcpu->token);
		return EINTR;
	}
	if (vcpu->mp_state == KVM_MP_STATE_HALTED) {
		bzero(&vcpu->run->hw, sizeof(vcpu->run->padding));
		vcpu->run->exit_reason = KVM_EXIT_HLT;
		lwkt_reltoken(&vcpu->token);
		return 0;
	}
	if (vcpu->mp_state != KVM_MP_STATE_RUNNABLE) {
		lwkt_reltoken(&vcpu->token);
		return EOPNOTSUPP;
	}
	error = kvm_vcpu_complete_pio(vcpu);
	if (error == 0)
		error = kvm_vcpu_complete_mmio(vcpu);
	if (error == 0)
		vcpu->running = true;
	lwkt_reltoken(&vcpu->token);
	if (error != 0)
		return error;

	for (;;) {
		struct vmm_cpuevent event = {
			.type = VMM_CPUEVENT_EXCP,
			.vector = KVM_X86_EXCEPTION_GP,
		};

		exit = NULL;
		error = vmm_vcpu_run(vcpu->vcpu, &exit);
		lwkt_gettoken(&vcpu->token);
		vcpu->running = false;
		if (error == ERESTART)
			error = EINTR;
		if (error == 0 && exit != NULL &&
		    (exit->reason == VMM_CPUEXIT_RDMSR ||
		    exit->reason == VMM_CPUEXIT_WRMSR)) {
			error = kvm_vcpu_handle_msr_exit(vcpu, exit);
			if (error == 0) {
				vcpu->running = true;
				lwkt_reltoken(&vcpu->token);
				continue;
			}
			if (error == ENOENT || error == EOPNOTSUPP) {
				lwkt_reltoken(&vcpu->token);
				error = vmm_vcpu_inject(vcpu->vcpu, &event);
				lwkt_gettoken(&vcpu->token);
				if (error == 0) {
					vcpu->running = true;
					lwkt_reltoken(&vcpu->token);
					continue;
				}
			}
		}
		if (error == 0) {
			if (exit != NULL && exit->reason == VMM_CPUEXIT_NONE)
				error = EINTR;
			else
				kvm_vcpu_set_exit(vcpu, exit);
		}
		lwkt_reltoken(&vcpu->token);
		return error;
	}
}

static int
kvm_vcpu_get_regs(struct kvm_vcpu *vcpu, struct kvm_regs *regs)
{
	struct vmm_cpustate *state = &vcpu->state;

	if (regs == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	regs->rax = state->gprs[VMM_X64_GPR_RAX];
	regs->rbx = state->gprs[VMM_X64_GPR_RBX];
	regs->rcx = state->gprs[VMM_X64_GPR_RCX];
	regs->rdx = state->gprs[VMM_X64_GPR_RDX];
	regs->rsi = state->gprs[VMM_X64_GPR_RSI];
	regs->rdi = state->gprs[VMM_X64_GPR_RDI];
	regs->rsp = state->gprs[VMM_X64_GPR_RSP];
	regs->rbp = state->gprs[VMM_X64_GPR_RBP];
	regs->r8 = state->gprs[VMM_X64_GPR_R8];
	regs->r9 = state->gprs[VMM_X64_GPR_R9];
	regs->r10 = state->gprs[VMM_X64_GPR_R10];
	regs->r11 = state->gprs[VMM_X64_GPR_R11];
	regs->r12 = state->gprs[VMM_X64_GPR_R12];
	regs->r13 = state->gprs[VMM_X64_GPR_R13];
	regs->r14 = state->gprs[VMM_X64_GPR_R14];
	regs->r15 = state->gprs[VMM_X64_GPR_R15];
	regs->rip = state->gprs[VMM_X64_GPR_RIP];
	regs->rflags = state->gprs[VMM_X64_GPR_RFLAGS];
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_regs(struct kvm_vcpu *vcpu, const struct kvm_regs *regs)
{
	struct vmm_cpustate *state = &vcpu->state;

	if (regs == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	state->gprs[VMM_X64_GPR_RAX] = regs->rax;
	state->gprs[VMM_X64_GPR_RBX] = regs->rbx;
	state->gprs[VMM_X64_GPR_RCX] = regs->rcx;
	state->gprs[VMM_X64_GPR_RDX] = regs->rdx;
	state->gprs[VMM_X64_GPR_RSI] = regs->rsi;
	state->gprs[VMM_X64_GPR_RDI] = regs->rdi;
	state->gprs[VMM_X64_GPR_RSP] = regs->rsp;
	state->gprs[VMM_X64_GPR_RBP] = regs->rbp;
	state->gprs[VMM_X64_GPR_R8] = regs->r8;
	state->gprs[VMM_X64_GPR_R9] = regs->r9;
	state->gprs[VMM_X64_GPR_R10] = regs->r10;
	state->gprs[VMM_X64_GPR_R11] = regs->r11;
	state->gprs[VMM_X64_GPR_R12] = regs->r12;
	state->gprs[VMM_X64_GPR_R13] = regs->r13;
	state->gprs[VMM_X64_GPR_R14] = regs->r14;
	state->gprs[VMM_X64_GPR_R15] = regs->r15;
	state->gprs[VMM_X64_GPR_RIP] = regs->rip;
	state->gprs[VMM_X64_GPR_RFLAGS] = regs->rflags;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_get_sregs(struct kvm_vcpu *vcpu, struct kvm_sregs *sregs)
{
	struct vmm_cpustate *state = &vcpu->state;

	if (sregs == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	bzero(sregs, sizeof(*sregs));
	kvm_vcpu_segment_from_vmm(&sregs->cs, &state->segs[VMM_X64_SEG_CS]);
	kvm_vcpu_segment_from_vmm(&sregs->ds, &state->segs[VMM_X64_SEG_DS]);
	kvm_vcpu_segment_from_vmm(&sregs->es, &state->segs[VMM_X64_SEG_ES]);
	kvm_vcpu_segment_from_vmm(&sregs->fs, &state->segs[VMM_X64_SEG_FS]);
	kvm_vcpu_segment_from_vmm(&sregs->gs, &state->segs[VMM_X64_SEG_GS]);
	kvm_vcpu_segment_from_vmm(&sregs->ss, &state->segs[VMM_X64_SEG_SS]);
	kvm_vcpu_segment_from_vmm(&sregs->tr, &state->segs[VMM_X64_SEG_TR]);
	kvm_vcpu_segment_from_vmm(&sregs->ldt, &state->segs[VMM_X64_SEG_LDT]);
	sregs->gdt.base = state->segs[VMM_X64_SEG_GDT].base;
	sregs->gdt.limit = state->segs[VMM_X64_SEG_GDT].limit;
	sregs->idt.base = state->segs[VMM_X64_SEG_IDT].base;
	sregs->idt.limit = state->segs[VMM_X64_SEG_IDT].limit;
	sregs->cr0 = state->crs[VMM_X64_CR_CR0];
	sregs->cr2 = state->crs[VMM_X64_CR_CR2];
	sregs->cr3 = state->crs[VMM_X64_CR_CR3];
	sregs->cr4 = state->crs[VMM_X64_CR_CR4];
	sregs->cr8 = state->crs[VMM_X64_CR_CR8];
	sregs->efer = state->msrs[VMM_X64_MSR_EFER];
	sregs->apic_base = vcpu->apic_base;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_sregs(struct kvm_vcpu *vcpu, const struct kvm_sregs *sregs)
{
	struct vmm_cpustate *state = &vcpu->state;

	if (sregs == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	kvm_vcpu_segment_to_vmm(&state->segs[VMM_X64_SEG_CS], &sregs->cs);
	kvm_vcpu_segment_to_vmm(&state->segs[VMM_X64_SEG_DS], &sregs->ds);
	kvm_vcpu_segment_to_vmm(&state->segs[VMM_X64_SEG_ES], &sregs->es);
	kvm_vcpu_segment_to_vmm(&state->segs[VMM_X64_SEG_FS], &sregs->fs);
	kvm_vcpu_segment_to_vmm(&state->segs[VMM_X64_SEG_GS], &sregs->gs);
	kvm_vcpu_segment_to_vmm(&state->segs[VMM_X64_SEG_SS], &sregs->ss);
	kvm_vcpu_segment_to_vmm(&state->segs[VMM_X64_SEG_TR], &sregs->tr);
	kvm_vcpu_segment_to_vmm(&state->segs[VMM_X64_SEG_LDT], &sregs->ldt);
	state->segs[VMM_X64_SEG_GDT].base = sregs->gdt.base;
	state->segs[VMM_X64_SEG_GDT].limit = sregs->gdt.limit;
	state->segs[VMM_X64_SEG_IDT].base = sregs->idt.base;
	state->segs[VMM_X64_SEG_IDT].limit = sregs->idt.limit;
	state->crs[VMM_X64_CR_CR0] = sregs->cr0;
	state->crs[VMM_X64_CR_CR2] = sregs->cr2;
	state->crs[VMM_X64_CR_CR3] = sregs->cr3;
	state->crs[VMM_X64_CR_CR4] = sregs->cr4;
	state->crs[VMM_X64_CR_CR8] = sregs->cr8;
	state->msrs[VMM_X64_MSR_EFER] = sregs->efer;
	vcpu->apic_base = sregs->apic_base;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_cpuid(struct kvm_vcpu *vcpu,
	const struct kvm_dfly_buffer *buffer)
{
	struct kvm_cpuid2 header;
	struct kvm_cpuid_entry2 *kvm_entries;
	struct vmm_cpuid_entry *vmm_entries;
	const uint8_t *user_buffer;
	size_t length;
	size_t index;
	int error;

	if (buffer == NULL || buffer->data == 0 || buffer->reserved != 0 ||
	    buffer->length < sizeof(header))
		return EINVAL;
	user_buffer = (const uint8_t *)(uintptr_t)buffer->data;
	if (copyin(user_buffer, &header, sizeof(header)) != 0)
		return EFAULT;
	if (header.nent > (SIZE_MAX - sizeof(header)) /
	    sizeof(kvm_entries[0]))
		return E2BIG;
	length = sizeof(header) +
	    (size_t)header.nent * sizeof(kvm_entries[0]);
	if (length > buffer->length)
		return EINVAL;
	kvm_entries = kmalloc((size_t)header.nent * sizeof(kvm_entries[0]),
	    M_KVM, M_WAITOK | M_ZERO);
	vmm_entries = kmalloc((size_t)header.nent * sizeof(vmm_entries[0]),
	    M_KVM, M_WAITOK | M_ZERO);
	if (header.nent != 0 && copyin(user_buffer + sizeof(header),
	    kvm_entries, (size_t)header.nent * sizeof(kvm_entries[0])) != 0) {
		error = EFAULT;
		goto done;
	}
	for (index = 0; index < header.nent; ++index) {
		if ((kvm_entries[index].flags &
		    ~KVM_CPUID_FLAG_SIGNIFCANT_INDEX) != 0) {
			error = EINVAL;
			goto done;
		}
		vmm_entries[index].leaf = kvm_entries[index].function;
		vmm_entries[index].subleaf = kvm_entries[index].index;
		vmm_entries[index].flags = kvm_entries[index].flags;
		vmm_entries[index].eax = kvm_entries[index].eax;
		vmm_entries[index].ebx = kvm_entries[index].ebx;
		vmm_entries[index].ecx = kvm_entries[index].ecx;
		vmm_entries[index].edx = kvm_entries[index].edx;
	}
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		error = EBUSY;
		goto done;
	}
	lwkt_reltoken(&vcpu->token);
	error = vmm_vcpu_set_cpuid(vcpu->vcpu, vmm_entries, header.nent);

done:
	kfree(vmm_entries, M_KVM);
	kfree(kvm_entries, M_KVM);
	return error;
}

static int
kvm_vcpu_get_msrs(struct kvm_vcpu *vcpu,
	const struct kvm_dfly_buffer *buffer, int *completed)
{
	struct kvm_msrs header;
	struct kvm_msr_entry *entries;
	uint8_t *user_buffer;
	size_t length;
	uint32_t index;
	int error;

	if (completed == NULL || buffer == NULL || buffer->data == 0 ||
	    buffer->reserved != 0 || buffer->length < sizeof(header))
		return EINVAL;
	*completed = 0;
	user_buffer = (uint8_t *)(uintptr_t)buffer->data;
	if (copyin(user_buffer, &header, sizeof(header)) != 0)
		return EFAULT;
	if (header.nmsrs > (SIZE_MAX - sizeof(header)) / sizeof(entries[0]))
		return E2BIG;
	length = sizeof(header) +
	    (size_t)header.nmsrs * sizeof(entries[0]);
	if (length > buffer->length)
		return EINVAL;
	entries = kmalloc((size_t)header.nmsrs * sizeof(entries[0]), M_KVM,
	    M_WAITOK | M_ZERO);
	if (header.nmsrs != 0 && copyin(user_buffer + sizeof(header), entries,
	    (size_t)header.nmsrs * sizeof(entries[0])) != 0) {
		kfree(entries, M_KVM);
		return EFAULT;
	}
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		kfree(entries, M_KVM);
		return EBUSY;
	}
	for (index = 0; index < header.nmsrs; ++index) {
		error = kvm_vcpu_get_msr(vcpu, entries[index].index,
		    &entries[index].data);
		if (error != 0)
			break;
		++*completed;
	}
	lwkt_reltoken(&vcpu->token);
	if (copyout(entries, (void *)(user_buffer + sizeof(header)),
	    (size_t)*completed * sizeof(entries[0])) != 0)
		error = EFAULT;
	else
		error = 0;
	kfree(entries, M_KVM);
	return error;
}

static int
kvm_vcpu_set_msrs(struct kvm_vcpu *vcpu,
	const struct kvm_dfly_buffer *buffer, int *completed)
{
	struct kvm_msrs header;
	struct kvm_msr_entry *entries;
	const uint8_t *user_buffer;
	size_t length;
	uint32_t index;
	int error;

	if (completed == NULL || buffer == NULL || buffer->data == 0 ||
	    buffer->reserved != 0 || buffer->length < sizeof(header))
		return EINVAL;
	*completed = 0;
	user_buffer = (const uint8_t *)(uintptr_t)buffer->data;
	if (copyin(user_buffer, &header, sizeof(header)) != 0)
		return EFAULT;
	if (header.nmsrs > (SIZE_MAX - sizeof(header)) / sizeof(entries[0]))
		return E2BIG;
	length = sizeof(header) +
	    (size_t)header.nmsrs * sizeof(entries[0]);
	if (length > buffer->length)
		return EINVAL;
	entries = kmalloc((size_t)header.nmsrs * sizeof(entries[0]), M_KVM,
	    M_WAITOK | M_ZERO);
	if (header.nmsrs != 0 && copyin(user_buffer + sizeof(header), entries,
	    (size_t)header.nmsrs * sizeof(entries[0])) != 0) {
		kfree(entries, M_KVM);
		return EFAULT;
	}
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		kfree(entries, M_KVM);
		return EBUSY;
	}
	for (index = 0; index < header.nmsrs; ++index) {
		error = kvm_vcpu_set_msr(vcpu, entries[index].index,
		    entries[index].data);
		if (error != 0)
			break;
		++*completed;
	}
	lwkt_reltoken(&vcpu->token);
	kfree(entries, M_KVM);
	return 0;
}

static int
kvm_vcpu_get_fpu(struct kvm_vcpu *vcpu, struct kvm_fpu *fpu)
{
	const struct vmm_cpustate_fpu *state = &vcpu->state.fpu;

	if (fpu == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	bzero(fpu, sizeof(*fpu));
	bcopy(state->fx_87_ac, fpu->fpr, sizeof(fpu->fpr));
	fpu->fcw = state->fx_cw;
	fpu->fsw = state->fx_sw;
	fpu->ftwx = state->fx_tw;
	fpu->last_opcode = state->fx_opcode;
	fpu->last_ip = state->fx_ip.fa_64;
	fpu->last_dp = state->fx_dp.fa_64;
	bcopy(state->fx_xmm, fpu->xmm, sizeof(fpu->xmm));
	fpu->mxcsr = state->fx_mxcsr;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_fpu(struct kvm_vcpu *vcpu, const struct kvm_fpu *fpu)
{
	struct vmm_cpustate_fpu *state = &vcpu->state.fpu;
	struct vmm_x64_capability capability;
	int error;

	if (fpu == NULL)
		return EINVAL;
	error = vmm_x64_get_capability(&capability);
	if (error != 0)
		return error;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	bcopy(fpu->fpr, state->fx_87_ac, sizeof(fpu->fpr));
	state->fx_cw = fpu->fcw;
	state->fx_sw = fpu->fsw;
	state->fx_tw = fpu->ftwx;
	state->fx_opcode = fpu->last_opcode;
	state->fx_ip.fa_64 = fpu->last_ip;
	state->fx_dp.fa_64 = fpu->last_dp;
	bcopy(fpu->xmm, state->fx_xmm, sizeof(fpu->xmm));
	state->fx_mxcsr_mask = capability.mxcsr_mask;
	state->fx_mxcsr = fpu->mxcsr & state->fx_mxcsr_mask;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_get_xsave(struct kvm_vcpu *vcpu, struct kvm_xsave *xsave)
{
	const struct vmm_cpustate_fpu *fpu = &vcpu->state.fpu;
	uint64_t xstate_bv;

	if (xsave == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	bzero(xsave, sizeof(*xsave));
	bcopy(fpu, xsave->region, sizeof(*fpu));
	xstate_bv = vcpu->state.crs[VMM_X64_CR_XCR0];
	bcopy(&xstate_bv, (uint8_t *)xsave->region + 512,
	    sizeof(xstate_bv));
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_xsave(struct kvm_vcpu *vcpu, const struct kvm_xsave *xsave)
{
	const uint8_t *bytes;
	struct vmm_x64_capability capability;
	uint64_t xstate_bv;
	uint64_t xcomp_bv;
	size_t index;
	int error;

	if (xsave == NULL)
		return EINVAL;
	bytes = (const uint8_t *)xsave->region;
	bcopy(bytes + 512, &xstate_bv, sizeof(xstate_bv));
	bcopy(bytes + 520, &xcomp_bv, sizeof(xcomp_bv));
	if ((xstate_bv & ~0x3ULL) != 0 || xcomp_bv != 0)
		return EOPNOTSUPP;
	for (index = 576; index < sizeof(xsave->region); ++index) {
		if (bytes[index] != 0)
			return EOPNOTSUPP;
	}
	error = vmm_x64_get_capability(&capability);
	if (error != 0)
		return error;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	bcopy(bytes, &vcpu->state.fpu, sizeof(vcpu->state.fpu));
	vcpu->state.fpu.fx_mxcsr_mask = capability.mxcsr_mask;
	vcpu->state.fpu.fx_mxcsr &= capability.mxcsr_mask;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_get_events(struct kvm_vcpu *vcpu, struct kvm_vcpu_events *events)
{

	if (events == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	/* Pending injection is intentionally not exported until VMM exposes it. */
	bzero(events, sizeof(*events));
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_events(struct kvm_vcpu *vcpu,
	const struct kvm_vcpu_events *events)
{

	if (events == NULL)
		return EINVAL;
	if (events->exception.injected != 0 || events->exception.pending != 0 ||
	    events->exception.has_error_code != 0 ||
	    events->exception.error_code != 0 ||
	    events->interrupt.injected != 0 || events->interrupt.soft != 0 ||
	    events->interrupt.shadow != 0 || events->nmi.injected != 0 ||
	    events->nmi.pending != 0 || events->nmi.masked != 0 ||
	    events->sipi_vector != 0 || events->smi.smm != 0 ||
	    events->smi.pending != 0 || events->smi.smm_inside_nmi != 0 ||
	    events->smi.latched_init != 0 || events->triple_fault.pending != 0 ||
	    events->exception_has_payload != 0 ||
	    events->exception_payload != 0)
		return EOPNOTSUPP;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_get_debugregs(struct kvm_vcpu *vcpu, struct kvm_debugregs *regs)
{

	if (regs == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	bzero(regs, sizeof(*regs));
	regs->db[0] = vcpu->state.drs[VMM_X64_DR_DR0];
	regs->db[1] = vcpu->state.drs[VMM_X64_DR_DR1];
	regs->db[2] = vcpu->state.drs[VMM_X64_DR_DR2];
	regs->db[3] = vcpu->state.drs[VMM_X64_DR_DR3];
	regs->dr6 = vcpu->state.drs[VMM_X64_DR_DR6];
	regs->dr7 = vcpu->state.drs[VMM_X64_DR_DR7];
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_debugregs(struct kvm_vcpu *vcpu,
	const struct kvm_debugregs *regs)
{
	unsigned int index;

	if (regs == NULL || regs->flags != 0)
		return EINVAL;
	for (index = 0; index < nitems(regs->reserved); ++index) {
		if (regs->reserved[index] != 0)
			return EINVAL;
	}
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	vcpu->state.drs[VMM_X64_DR_DR0] = regs->db[0];
	vcpu->state.drs[VMM_X64_DR_DR1] = regs->db[1];
	vcpu->state.drs[VMM_X64_DR_DR2] = regs->db[2];
	vcpu->state.drs[VMM_X64_DR_DR3] = regs->db[3];
	vcpu->state.drs[VMM_X64_DR_DR6] = regs->dr6;
	vcpu->state.drs[VMM_X64_DR_DR7] = regs->dr7;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_get_xcrs(struct kvm_vcpu *vcpu, struct kvm_xcrs *xcrs)
{

	if (xcrs == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	bzero(xcrs, sizeof(*xcrs));
	xcrs->nr_xcrs = 1;
	xcrs->xcrs[0].xcr = 0;
	xcrs->xcrs[0].value = vcpu->state.crs[VMM_X64_CR_XCR0];
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_xcrs(struct kvm_vcpu *vcpu, const struct kvm_xcrs *xcrs)
{
	uint32_t index;

	if (xcrs == NULL || xcrs->flags != 0 || xcrs->nr_xcrs > KVM_MAX_XCRS)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	for (index = 0; index < xcrs->nr_xcrs; ++index) {
		if (xcrs->xcrs[index].xcr != 0) {
			lwkt_reltoken(&vcpu->token);
			return EINVAL;
		}
		vcpu->state.crs[VMM_X64_CR_XCR0] = xcrs->xcrs[index].value;
	}
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_get_mp_state(struct kvm_vcpu *vcpu, struct kvm_mp_state *state)
{

	if (state == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	state->mp_state = vcpu->mp_state;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_set_mp_state(struct kvm_vcpu *vcpu,
	const struct kvm_mp_state *state)
{

	if (state == NULL || (state->mp_state != KVM_MP_STATE_RUNNABLE &&
	    state->mp_state != KVM_MP_STATE_HALTED))
		return EOPNOTSUPP;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	vcpu->mp_state = state->mp_state;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

static int
kvm_vcpu_get_lapic(struct kvm_vcpu *vcpu, struct kvm_lapic_state *state)
{
	int error;

	if (state == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	error = vmm_vcpu_get_lapic(vcpu->vcpu, state->regs,
	    sizeof(state->regs));
	lwkt_reltoken(&vcpu->token);
	return error;
}

static int
kvm_vcpu_set_lapic(struct kvm_vcpu *vcpu,
    const struct kvm_lapic_state *state)
{
	int error;

	if (state == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	error = vmm_vcpu_set_lapic(vcpu->vcpu, state->regs,
	    sizeof(state->regs));
	lwkt_reltoken(&vcpu->token);
	return error;
}

static int
kvm_vcpu_get_msr(struct kvm_vcpu *vcpu, uint32_t index, uint64_t *value)
{

	if (value == NULL)
		return EINVAL;
	if (index == KVM_MSR_IA32_MTRRCAP ||
	    (index >= 0x200U && index <= 0x20fU) ||
	    (index >= 0x250U && index <= 0x26fU) ||
	    (index >= KVM_MSR_K7_PERFCTL0 &&
	    index <= KVM_MSR_K7_PERFCTR3) ||
	    index == KVM_MSR_IA32_MTRR_DEF_TYPE) {
		*value = 0;
		return 0;
	}
	switch (index) {
	case KVM_MSR_KVM_WALL_CLOCK:
	case KVM_MSR_KVM_SYSTEM_TIME:
		*value = 0;
		return 0;
	case KVM_MSR_IA32_TSC:
		*value = vcpu->state.msrs[VMM_X64_MSR_TSC];
		return 0;
	case KVM_MSR_IA32_APICBASE:
		*value = vcpu->apic_base;
		return 0;
	case KVM_MSR_IA32_SYSENTER_CS:
		*value = vcpu->state.msrs[VMM_X64_MSR_SYSENTER_CS];
		return 0;
	case KVM_MSR_IA32_SYSENTER_ESP:
		*value = vcpu->state.msrs[VMM_X64_MSR_SYSENTER_ESP];
		return 0;
	case KVM_MSR_IA32_SYSENTER_EIP:
		*value = vcpu->state.msrs[VMM_X64_MSR_SYSENTER_EIP];
		return 0;
	case KVM_MSR_IA32_PAT:
		*value = vcpu->state.msrs[VMM_X64_MSR_PAT];
		return 0;
	case KVM_MSR_EFER:
		*value = vcpu->state.msrs[VMM_X64_MSR_EFER];
		return 0;
	case KVM_MSR_STAR:
		*value = vcpu->state.msrs[VMM_X64_MSR_STAR];
		return 0;
	case KVM_MSR_LSTAR:
		*value = vcpu->state.msrs[VMM_X64_MSR_LSTAR];
		return 0;
	case KVM_MSR_CSTAR:
		*value = vcpu->state.msrs[VMM_X64_MSR_CSTAR];
		return 0;
	case KVM_MSR_SFMASK:
		*value = vcpu->state.msrs[VMM_X64_MSR_SFMASK];
		return 0;
	case KVM_MSR_KERNEL_GS_BASE:
		*value = vcpu->state.msrs[VMM_X64_MSR_KERNELGSBASE];
		return 0;
	default:
		return ENOENT;
	}
}

static int
kvm_vcpu_set_msr(struct kvm_vcpu *vcpu, uint32_t index, uint64_t value)
{

	if (index == KVM_MSR_IA32_MTRRCAP)
		return EOPNOTSUPP;
	if ((index >= 0x200U && index <= 0x20fU) ||
	    (index >= 0x250U && index <= 0x26fU) ||
	    (index >= KVM_MSR_K7_PERFCTL0 &&
	    index <= KVM_MSR_K7_PERFCTR3) ||
	    index == KVM_MSR_IA32_MTRR_DEF_TYPE)
		return value == 0 ? 0 : EOPNOTSUPP;
	switch (index) {
	case KVM_MSR_KVM_WALL_CLOCK:
	case KVM_MSR_KVM_SYSTEM_TIME:
		return value == 0 ? 0 : EOPNOTSUPP;
	case KVM_MSR_IA32_TSC:
		vcpu->state.msrs[VMM_X64_MSR_TSC] = value;
		return 0;
	case KVM_MSR_IA32_APICBASE:
		vcpu->apic_base = value;
		return 0;
	case KVM_MSR_IA32_SYSENTER_CS:
		vcpu->state.msrs[VMM_X64_MSR_SYSENTER_CS] = value;
		return 0;
	case KVM_MSR_IA32_SYSENTER_ESP:
		vcpu->state.msrs[VMM_X64_MSR_SYSENTER_ESP] = value;
		return 0;
	case KVM_MSR_IA32_SYSENTER_EIP:
		vcpu->state.msrs[VMM_X64_MSR_SYSENTER_EIP] = value;
		return 0;
	case KVM_MSR_IA32_PAT:
		vcpu->state.msrs[VMM_X64_MSR_PAT] = value;
		return 0;
	case KVM_MSR_EFER:
		vcpu->state.msrs[VMM_X64_MSR_EFER] = value;
		return 0;
	case KVM_MSR_STAR:
		vcpu->state.msrs[VMM_X64_MSR_STAR] = value;
		return 0;
	case KVM_MSR_LSTAR:
		vcpu->state.msrs[VMM_X64_MSR_LSTAR] = value;
		return 0;
	case KVM_MSR_CSTAR:
		vcpu->state.msrs[VMM_X64_MSR_CSTAR] = value;
		return 0;
	case KVM_MSR_SFMASK:
		vcpu->state.msrs[VMM_X64_MSR_SFMASK] = value;
		return 0;
	case KVM_MSR_KERNEL_GS_BASE:
		vcpu->state.msrs[VMM_X64_MSR_KERNELGSBASE] = value;
		return 0;
	default:
		return ENOENT;
	}
}

static int
kvm_vcpu_handle_msr_exit(struct kvm_vcpu *vcpu,
	const struct vmm_cpuexit *exit)
{
	uint64_t value;
	int error;

	if (exit->reason == VMM_CPUEXIT_RDMSR) {
		error = kvm_vcpu_get_msr(vcpu, exit->u.rdmsr.msr, &value);
		if (error != 0)
			return error;
		vcpu->state.gprs[VMM_X64_GPR_RAX] = (uint32_t)value;
		vcpu->state.gprs[VMM_X64_GPR_RDX] = value >> 32;
		vcpu->state.gprs[VMM_X64_GPR_RIP] = exit->u.rdmsr.npc;
		return 0;
	}
	if (exit->reason == VMM_CPUEXIT_WRMSR) {
		error = kvm_vcpu_set_msr(vcpu, exit->u.wrmsr.msr,
		    exit->u.wrmsr.val);
		if (error != 0)
			return error;
		vcpu->state.gprs[VMM_X64_GPR_RIP] = exit->u.wrmsr.npc;
		return 0;
	}
	return EINVAL;
}

static void
kvm_vcpu_reset_state(struct vmm_cpustate *state)
{
	unsigned int index;

	bzero(state, sizeof(*state));
	for (index = VMM_X64_SEG_ES; index <= VMM_X64_SEG_GS; ++index) {
		state->segs[index].limit = 0xffff;
		state->segs[index].attrib.type = 3;
		state->segs[index].attrib.s = 1;
		state->segs[index].attrib.p = 1;
	}
	state->segs[VMM_X64_SEG_CS].selector = 0xf000;
	state->segs[VMM_X64_SEG_CS].base = 0xffff0000;
	for (index = VMM_X64_SEG_GDT; index <= VMM_X64_SEG_IDT; ++index) {
		state->segs[index].limit = 0xffff;
		state->segs[index].attrib.type = 2;
		state->segs[index].attrib.s = 1;
		state->segs[index].attrib.p = 1;
	}
	state->segs[VMM_X64_SEG_LDT].limit = 0xffff;
	state->segs[VMM_X64_SEG_LDT].attrib.type = 2;
	state->segs[VMM_X64_SEG_LDT].attrib.p = 1;
	state->segs[VMM_X64_SEG_TR].limit = 0xffff;
	state->segs[VMM_X64_SEG_TR].attrib.type = 3;
	state->segs[VMM_X64_SEG_TR].attrib.p = 1;
	state->gprs[VMM_X64_GPR_RDX] = 0x600;
	state->gprs[VMM_X64_GPR_RIP] = 0xfff0;
	state->gprs[VMM_X64_GPR_RFLAGS] = 0x2;
	state->crs[VMM_X64_CR_CR0] = 0x60000010;
	state->crs[VMM_X64_CR_XCR0] = 0x1;
	state->drs[VMM_X64_DR_DR6] = 0xffff0ff0;
	state->drs[VMM_X64_DR_DR7] = 0x400;
	state->msrs[VMM_X64_MSR_PAT] = 0x0007040600070406ULL;
	state->fpu.fx_cw = 0x40;
	state->fpu.fx_tw = 0x55;
	state->fpu.fx_zero = 0x55;
	state->fpu.fx_mxcsr = 0x1f80;
}

static void
kvm_vcpu_segment_from_vmm(struct kvm_segment *dst,
	const struct vmm_segment *src)
{

	bzero(dst, sizeof(*dst));
	dst->base = src->base;
	dst->limit = src->limit;
	dst->selector = src->selector;
	dst->type = src->attrib.type;
	dst->present = src->attrib.p;
	dst->dpl = src->attrib.dpl;
	dst->db = src->attrib.def;
	dst->s = src->attrib.s;
	dst->l = src->attrib.l;
	dst->g = src->attrib.g;
	dst->avl = src->attrib.avl;
	dst->unusable = src->attrib.p == 0;
}

static void
kvm_vcpu_segment_to_vmm(struct vmm_segment *dst,
	const struct kvm_segment *src)
{

	dst->base = src->base;
	dst->limit = src->limit;
	dst->selector = src->selector;
	dst->attrib.type = src->type;
	dst->attrib.p = src->unusable == 0 && src->present != 0;
	dst->attrib.dpl = src->dpl;
	dst->attrib.def = src->db;
	dst->attrib.s = src->s;
	dst->attrib.l = src->l;
	dst->attrib.g = src->g;
	dst->attrib.avl = src->avl;
}

static void
kvm_vcpu_set_exit(struct kvm_vcpu *vcpu, const struct vmm_cpuexit *exit)
{
	struct kvm_run *run = vcpu->run;
	uint64_t rax;
	uint64_t count;
	uint32_t stack_word;
	uint32_t rax_word;
	int stack_error;
	int rax_error;
	int error;

	bzero(&run->hw, sizeof(run->padding));
	run->if_flag = (vcpu->state.gprs[VMM_X64_GPR_RFLAGS] & 0x200) != 0;
	run->cr8 = vcpu->state.crs[VMM_X64_CR_CR8];
	run->apic_base = vcpu->apic_base;
	run->ready_for_interrupt_injection = 0;
	if (kvm_vcpu_trace && exit != NULL &&
	    (exit->reason == VMM_CPUEXIT_HALTED ||
	    exit->reason == VMM_CPUEXIT_SHUTDOWN ||
	    exit->reason == VMM_CPUEXIT_INVALID)) {
		stack_error = kvm_vcpu_copy_gpa(vcpu,
		    vcpu->state.gprs[VMM_X64_GPR_RSP], &stack_word,
		    sizeof(stack_word), 0);
		rax_error = kvm_vcpu_copy_gpa(vcpu,
		    vcpu->state.gprs[VMM_X64_GPR_RAX], &rax_word,
		    sizeof(rax_word), 0);
		kprintf("kvm: vcpu%u terminal exit=%ju rip=%#jx rsp=%#jx "
		    "rflags=%#jx cr0=%#jx cr3=%#jx cr4=%#jx efer=%#jx\n",
		    vcpu->id, (uintmax_t)exit->reason,
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RIP],
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RSP],
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RFLAGS],
		    (uintmax_t)vcpu->state.crs[VMM_X64_CR_CR0],
		    (uintmax_t)vcpu->state.crs[VMM_X64_CR_CR3],
		    (uintmax_t)vcpu->state.crs[VMM_X64_CR_CR4],
		    (uintmax_t)vcpu->state.msrs[VMM_X64_MSR_EFER]);
		kprintf("kvm: vcpu%u terminal stack=%#x/%d rax=%#jx "
		    "raxmem=%#x/%d\n", vcpu->id, stack_word, stack_error,
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RAX], rax_word,
		    rax_error);
		kprintf("kvm: vcpu%u terminal cs=%#x:%#jx ss=%#x:%#jx "
		    "idt=%#jx/%#x tr=%#x:%#jx\n", vcpu->id,
		    vcpu->state.segs[VMM_X64_SEG_CS].selector,
		    (uintmax_t)vcpu->state.segs[VMM_X64_SEG_CS].base,
		    vcpu->state.segs[VMM_X64_SEG_SS].selector,
		    (uintmax_t)vcpu->state.segs[VMM_X64_SEG_SS].base,
		    (uintmax_t)vcpu->state.segs[VMM_X64_SEG_IDT].base,
		    vcpu->state.segs[VMM_X64_SEG_IDT].limit,
		    vcpu->state.segs[VMM_X64_SEG_TR].selector,
		    (uintmax_t)vcpu->state.segs[VMM_X64_SEG_TR].base);
	}
	if (kvm_vcpu_trace && kvm_vcpu_trace_count < KVM_VCPU_TRACE_LIMIT &&
	    exit != NULL && exit->reason == VMM_CPUEXIT_IO) {
		++kvm_vcpu_trace_count;
		kprintf("kvm: vcpu%u pio port=%#x %s value=%#jx size=%u str=%u rep=%u rip=%#jx npc=%#jx rsp=%#jx rcx=%#jx rsi=%#jx\n",
		    vcpu->id, exit->u.io.port, exit->u.io.in ? "in" : "out",
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RAX],
		    exit->u.io.operand_size, exit->u.io.str, exit->u.io.rep,
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RIP],
		    (uintmax_t)exit->u.io.npc,
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RSP],
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RCX],
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RSI]);
		stack_error = kvm_vcpu_copy_gpa(vcpu,
		    vcpu->state.gprs[VMM_X64_GPR_RSP], &stack_word,
		    sizeof(stack_word), 0);
		if (stack_error == 0) {
			kprintf("kvm: vcpu%u pio stack[%#jx]=%#x\n", vcpu->id,
			    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RSP],
			    stack_word);
		}
	}
	if (kvm_vcpu_trace &&
	    kvm_vcpu_memory_trace_count < KVM_VCPU_TRACE_LIMIT &&
	    exit != NULL && exit->reason == VMM_CPUEXIT_MEMORY) {
		++kvm_vcpu_memory_trace_count;
		kprintf("kvm: vcpu%u memory gpa=%#jx prot=%#x rip=%#jx len=%u bytes=%02x %02x %02x %02x\n",
		    vcpu->id, (uintmax_t)exit->u.mem.gpa, exit->u.mem.prot,
		    (uintmax_t)vcpu->state.gprs[VMM_X64_GPR_RIP],
		    exit->u.mem.inst_len, exit->u.mem.inst_bytes[0],
		    exit->u.mem.inst_bytes[1], exit->u.mem.inst_bytes[2],
		    exit->u.mem.inst_bytes[3]);
	}
	if (exit == NULL) {
		run->exit_reason = KVM_EXIT_INTR;
		return;
	}
	switch (exit->reason) {
	case VMM_CPUEXIT_MEMORY:
		if (kvm_vcpu_set_mmio_exit(vcpu, exit) != 0) {
			run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
			run->internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
			run->internal.ndata = 1;
			run->internal.data[0] = VMM_CPUEXIT_MEMORY;
		}
		return;
	case VMM_CPUEXIT_IO:
		if (exit->u.io.operand_size == 0 ||
		    exit->u.io.operand_size > sizeof(rax)) {
			run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
			run->internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
			run->internal.ndata = 1;
			run->internal.data[0] = VMM_CPUEXIT_IO;
			return;
		}
		run->exit_reason = KVM_EXIT_IO;
		run->io.direction = exit->u.io.in ? KVM_EXIT_IO_IN :
		    KVM_EXIT_IO_OUT;
		run->io.size = exit->u.io.operand_size;
		run->io.port = exit->u.io.port;
		run->io.count = 1;
		run->io.data_offset = KVM_PIO_PAGE_OFFSET * PAGE_SIZE;
		vcpu->pio_string = exit->u.io.str;
		vcpu->pio_rep = exit->u.io.rep;
		vcpu->pio_address_size = exit->u.io.address_size;
		if (exit->u.io.str) {
			count = 1;
			if (exit->u.io.rep) {
				switch (exit->u.io.address_size) {
				case 2:
					count = vcpu->state.gprs[VMM_X64_GPR_RCX] &
					    UINT16_MAX;
					break;
				case 4:
					count = vcpu->state.gprs[VMM_X64_GPR_RCX] &
					    UINT32_MAX;
					break;
				case 8:
					count = vcpu->state.gprs[VMM_X64_GPR_RCX];
					break;
				default:
					count = UINT64_MAX;
					break;
				}
			}
			if (count == 0) {
				vcpu->state.gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
				run->io.count = 0;
				return;
			}
			error = kvm_vcpu_pio_string_gpa(vcpu, exit,
			    &vcpu->pio_gpa);
			if (error != 0) {
				run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
				run->internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
				run->internal.ndata = 1;
				run->internal.data[0] = VMM_CPUEXIT_IO;
				return;
			}
			if (!exit->u.io.in) {
				error = kvm_vcpu_copy_gpa(vcpu, vcpu->pio_gpa,
				    vcpu->pio_data, exit->u.io.operand_size, 0);
				if (error != 0) {
					run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
					run->internal.suberror =
					    KVM_INTERNAL_ERROR_EMULATION;
					run->internal.ndata = 1;
					run->internal.data[0] = VMM_CPUEXIT_IO;
					return;
				}
			}
		} else if (!exit->u.io.in) {
			rax = vcpu->state.gprs[VMM_X64_GPR_RAX];
			bcopy(&rax, vcpu->pio_data, exit->u.io.operand_size);
		}
		vcpu->pio_pending = true;
		vcpu->pio_in = exit->u.io.in;
		vcpu->pio_size = exit->u.io.operand_size;
		vcpu->pio_npc = exit->u.io.npc;
		return;
	case VMM_CPUEXIT_HALTED:
		run->exit_reason = KVM_EXIT_HLT;
		return;
	case VMM_CPUEXIT_SHUTDOWN:
		run->exit_reason = KVM_EXIT_SHUTDOWN;
		return;
	case VMM_CPUEXIT_INT_READY:
		run->exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
		return;
	case VMM_CPUEXIT_INVALID:
		run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		run->fail_entry.hardware_entry_failure_reason =
		    exit->u.inv.hwcode;
		return;
	default:
		run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		run->internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
		run->internal.ndata = 1;
		run->internal.data[0] = exit->reason;
		return;
	}
}

static int
kvm_vcpu_complete_pio(struct kvm_vcpu *vcpu)
{
	uint64_t count;
	uint64_t index;
	uint64_t value;
	uint64_t mask;
	uint64_t step;
	unsigned int reg;
	int error;

	if (!vcpu->pio_pending)
		return 0;
	if (vcpu->pio_in) {
		if (vcpu->pio_string) {
			error = kvm_vcpu_copy_gpa(vcpu, vcpu->pio_gpa,
			    vcpu->pio_data, vcpu->pio_size, 1);
			if (error != 0)
				return error;
		} else {
			value = 0;
			bcopy(vcpu->pio_data, &value, vcpu->pio_size);
			mask = vcpu->pio_size == sizeof(mask) ? UINT64_MAX :
			    (1ULL << (vcpu->pio_size * NBBY)) - 1;
			vcpu->state.gprs[VMM_X64_GPR_RAX] =
			    (vcpu->state.gprs[VMM_X64_GPR_RAX] & ~mask) |
			    (value & mask);
		}
	}
	if (!vcpu->pio_string) {
		vcpu->state.gprs[VMM_X64_GPR_RIP] = vcpu->pio_npc;
		vcpu->pio_pending = false;
		return 0;
	}
	reg = vcpu->pio_in ? VMM_X64_GPR_RDI : VMM_X64_GPR_RSI;
	step = vcpu->pio_size;
	if ((vcpu->state.gprs[VMM_X64_GPR_RFLAGS] & (1ULL << 10)) != 0)
		step = (uint64_t)-step;
	switch (vcpu->pio_address_size) {
	case 2:
		index = (vcpu->state.gprs[reg] + step) & UINT16_MAX;
		vcpu->state.gprs[reg] = (vcpu->state.gprs[reg] & ~UINT16_MAX) |
		    index;
		break;
	case 4:
		index = (vcpu->state.gprs[reg] + step) & UINT32_MAX;
		vcpu->state.gprs[reg] = (uint32_t)index;
		break;
	case 8:
		vcpu->state.gprs[reg] += step;
		break;
	default:
		return EINVAL;
	}
	if (vcpu->pio_rep) {
		switch (vcpu->pio_address_size) {
		case 2:
			count = (vcpu->state.gprs[VMM_X64_GPR_RCX] - 1) &
			    UINT16_MAX;
			vcpu->state.gprs[VMM_X64_GPR_RCX] =
			    (vcpu->state.gprs[VMM_X64_GPR_RCX] & ~UINT16_MAX) |
			    count;
			break;
		case 4:
			count = (vcpu->state.gprs[VMM_X64_GPR_RCX] - 1) &
			    UINT32_MAX;
			vcpu->state.gprs[VMM_X64_GPR_RCX] = (uint32_t)count;
			break;
		case 8:
			count = --vcpu->state.gprs[VMM_X64_GPR_RCX];
			break;
		default:
			return EINVAL;
		}
		if (count != 0) {
			vcpu->pio_pending = false;
			vcpu->pio_string = false;
			return 0;
		}
	}
	vcpu->state.gprs[VMM_X64_GPR_RIP] = vcpu->pio_npc;
	vcpu->pio_pending = false;
	vcpu->pio_string = false;
	return 0;
}

static int
kvm_vcpu_copy_gpa(struct kvm_vcpu *vcpu, uint64_t gpa, void *data,
	size_t length, int write)
{
	void *pmap_handle;
	vm_paddr_t pa;
	uint64_t page_gpa;
	size_t chunk;
	size_t offset;
	int error;

	if (vcpu == NULL || vcpu->vm == NULL || data == NULL || length == 0 ||
	    gpa >= KVM_GPA_MAX || length > KVM_GPA_MAX - gpa)
		return EFAULT;
	while (length != 0) {
		page_gpa = trunc_page(gpa);
		offset = (size_t)(gpa - page_gpa);
		chunk = PAGE_SIZE - offset;
		if (chunk > length)
			chunk = length;
		error = vm_fault(&vcpu->vm->vmspace->vm_map, page_gpa,
		    write ? VM_PROT_WRITE : VM_PROT_READ,
		    write ? VM_FAULT_DIRTY : VM_FAULT_NORMAL);
		if (error != 0)
			return vm_mmap_to_errno(error);
		pmap_handle = NULL;
		pa = pmap_extract(vmspace_pmap(vcpu->vm->vmspace), page_gpa,
		    &pmap_handle);
		if (pa == 0) {
			pmap_extract_done(pmap_handle);
			return EFAULT;
		}
		if (write) {
			bcopy(data, (void *)(PHYS_TO_DMAP(pa) + offset), chunk);
		} else {
			bcopy((const void *)(PHYS_TO_DMAP(pa) + offset), data, chunk);
		}
		pmap_extract_done(pmap_handle);
		gpa += chunk;
		data = (uint8_t *)data + chunk;
		length -= chunk;
	}
	return 0;
}

static int
kvm_vcpu_pio_string_gpa(struct kvm_vcpu *vcpu,
	const struct vmm_cpuexit *exit, uint64_t *gpa)
{
	uint64_t base;
	uint64_t index;
	uint64_t mask;
	unsigned int reg;

	if (vcpu == NULL || exit == NULL || gpa == NULL ||
	    (vcpu->state.crs[VMM_X64_CR_CR0] & (1ULL << 31)) != 0 ||
	    exit->u.io.seg < VMM_X64_SEG_ES ||
	    exit->u.io.seg > VMM_X64_SEG_GS)
		return EOPNOTSUPP;
	switch (exit->u.io.address_size) {
	case 2:
		mask = UINT16_MAX;
		break;
	case 4:
		mask = UINT32_MAX;
		break;
	case 8:
		mask = UINT64_MAX;
		break;
	default:
		return EOPNOTSUPP;
	}
	reg = exit->u.io.in ? VMM_X64_GPR_RDI : VMM_X64_GPR_RSI;
	index = vcpu->state.gprs[reg] & mask;
	base = vcpu->state.segs[(unsigned int)exit->u.io.seg].base;
	if (base > UINT64_MAX - index || base + index >= KVM_GPA_MAX ||
	    exit->u.io.operand_size > KVM_GPA_MAX - (base + index))
		return EFAULT;
	*gpa = base + index;
	return 0;
}

static int
kvm_vcpu_complete_mmio(struct kvm_vcpu *vcpu)
{
	int error;

	if (!vcpu->mmio_pending)
		return 0;
	if (vcpu->mmio_write)
		error = vmm_vcpu_complete_mmio_write(vcpu->vcpu);
	else
		error = vmm_vcpu_complete_mmio_read(vcpu->vcpu,
		    vcpu->run->mmio.data, vcpu->mmio_size);
	if (error != 0)
		return error;
	vcpu->mmio_pending = false;
	return 0;
}

static int
kvm_vcpu_set_mmio_exit(struct kvm_vcpu *vcpu,
	const struct vmm_cpuexit *exit)
{
	if (exit->u.mem.width != VMM_IO_WIDTH_8 &&
	    exit->u.mem.width != VMM_IO_WIDTH_16 &&
	    exit->u.mem.width != VMM_IO_WIDTH_32 &&
	    exit->u.mem.width != VMM_IO_WIDTH_64)
		return EINVAL;
	if ((exit->u.mem.prot & (VM_PROT_READ | VM_PROT_WRITE)) == 0)
		return EINVAL;

	bzero(&vcpu->run->mmio, sizeof(vcpu->run->mmio));
	vcpu->run->exit_reason = KVM_EXIT_MMIO;
	vcpu->run->mmio.phys_addr = exit->u.mem.gpa;
	vcpu->mmio_size = exit->u.mem.width;
	vcpu->mmio_write = (exit->u.mem.prot & VM_PROT_WRITE) != 0;
	vcpu->run->mmio.len = vcpu->mmio_size;
	vcpu->run->mmio.is_write = vcpu->mmio_write;
	if (vcpu->mmio_write)
		bcopy(&exit->u.mem.value, vcpu->run->mmio.data,
		    vcpu->mmio_size);
	vcpu->mmio_pending = true;
	return 0;
}

static void
kvm_vcpu_file_destroy(void *arg)
{

	kvm_vcpu_destroy(arg);
}

static void
kvm_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	struct kvm_vm *vm;
	int error;

	if (vcpu == NULL)
		return;
	vm = vcpu->vm;
	if (vm != NULL) {
		lwkt_gettoken(&vm->token);
		if (vcpu->id < KVM_MAX_VCPUS && vm->vcpus[vcpu->id] == vcpu)
			vm->vcpus[vcpu->id] = NULL;
		lwkt_reltoken(&vm->token);
	}
	if (vcpu->vcpu != NULL) {
		error = vmm_vcpu_destroy(vcpu->vcpu);
		KKASSERT(error == 0);
		vcpu->vcpu = NULL;
	}
	kvm_vcpu_free_run(vcpu);
	if (vm != NULL) {
		kvm_vm_release(vm);
		lwkt_gettoken(&kvm_frontend_token);
		KKASSERT(kvm_file_count != 0);
		--kvm_file_count;
		lwkt_reltoken(&kvm_frontend_token);
	}
	kfree(vcpu, M_KVM);
}
