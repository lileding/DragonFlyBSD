/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-VM file descriptor and guest vmspace for the DragonFly KVM frontend.
 */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/sysmsg.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>

#include <linux/kvm.h>

#include "../vmm/vmm.h"
#include "kvm_internal.h"
#include "kvm_ioevent.h"
#include "kvm_irqfd.h"
#include "kvm_vcpu.h"
#include "kvm_vm.h"

#define KVM_LINUX_IO(number)	((unsigned long)((KVMIO << 8) | (number)))
#define KVM_X86_MSI_APIC_BASE	0xfee00000ULL

static unsigned int kvm_irq_line_trace_count;

struct kvm_memory_piece {
	STAILQ_ENTRY(kvm_memory_piece) entry;
	struct vm_object *object;
	vm_ooffset_t offset;
	vm_offset_t gpa;
	vm_size_t size;
};

STAILQ_HEAD(kvm_memory_piece_list, kvm_memory_piece);

static d_priv_dtor_t kvm_vm_file_destroy;
static int kvm_vm_fo_read(struct file *, struct uio *, struct ucred *, int);
static int kvm_vm_fo_write(struct file *, struct uio *, struct ucred *, int);
static int kvm_vm_fo_ioctl(struct file *, u_long, caddr_t,
	struct ucred *, struct sysmsg *);
static int kvm_vm_fo_kqfilter(struct file *, struct knote *);
static int kvm_vm_fo_stat(struct file *, struct stat *, struct ucred *);
static int kvm_vm_fo_close(struct file *);
static int kvm_vm_fo_seek(struct file *, off_t, int, off_t *);
static void kvm_vm_destroy(struct kvm_vm *);
static int kvm_vm_set_user_memory(struct kvm_vm *,
    const struct kvm_userspace_memory_region *);
static int kvm_vm_signal_msi(struct kvm_vm *, const struct kvm_msi *);
static int kvm_vm_set_irq_line(struct kvm_vm *, const struct kvm_irq_level *);
static int kvm_vm_get_irqchip(struct kvm_vm *, struct kvm_irqchip *);
static int kvm_vm_set_irqchip(struct kvm_vm *, const struct kvm_irqchip *);
static int kvm_vm_get_clock(struct kvm_vm *, struct kvm_clock_data *);
static int kvm_vm_set_clock(struct kvm_vm *, const struct kvm_clock_data *);
static int kvm_vm_create_pit(struct kvm_vm *, const struct kvm_pit_config *);
static int kvm_vm_get_pit(struct kvm_vm *, struct kvm_pit_state2 *);
static int kvm_vm_set_pit(struct kvm_vm *, const struct kvm_pit_state2 *);
static int kvm_vm_set_tss_address(struct kvm_vm *, uint64_t);
static int kvm_vm_set_identity_map_address(struct kvm_vm *, uint64_t);
static int kvm_vm_collect_memory(struct vmspace *, vm_offset_t, vm_size_t,
	vm_offset_t, struct kvm_memory_piece_list *, int *);
static void kvm_vm_drop_memory(struct kvm_memory_piece_list *);

static struct fileops kvm_vm_fileops = {
	.fo_read = kvm_vm_fo_read,
	.fo_write = kvm_vm_fo_write,
	.fo_ioctl = kvm_vm_fo_ioctl,
	.fo_kqfilter = kvm_vm_fo_kqfilter,
	.fo_stat = kvm_vm_fo_stat,
	.fo_close = kvm_vm_fo_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = kvm_vm_fo_seek,
};

int
kvm_vm_create(struct lwp *lp, struct vnode *vp, int *fd)
{
	struct kvm_vm *vm;
	struct file *fp;
	int error;

	if (lp == NULL || vp == NULL || fd == NULL)
		return EINVAL;
	*fd = -1;
	vm = kmalloc(sizeof(*vm), M_KVM, M_WAITOK | M_ZERO);
	lwkt_token_init(&vm->token, "kvmvm");
	TAILQ_INIT(&vm->ioevents);
	TAILQ_INIT(&vm->irqroutes);
	TAILQ_INIT(&vm->irqfds);
	vm->references = 1;

	lwkt_gettoken(&kvm_frontend_token);
	if (kvm_draining) {
		lwkt_reltoken(&kvm_frontend_token);
		kfree(vm, M_KVM);
		return EBUSY;
	}
	++kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);

	vm->vmspace = vmspace_alloc(VM_MIN_USER_ADDRESS, KVM_GPA_MAX);
	if (vm->vmspace == NULL) {
		error = ENOMEM;
		goto fail;
	}
	error = vmm_machine_create(vm->vmspace, &vm->machine);
	if (error != 0)
		goto fail;

	error = falloc(lp, &fp, fd);
	if (error != 0)
		goto fail;
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &kvm_vm_fileops;
	fp->f_data = vp;
	vref(vp);
	error = devfs_set_cdevpriv(fp, vm, kvm_vm_file_destroy);
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
	kvm_vm_release(vm);
	return error;
}

static int
kvm_vm_fo_read(struct file *fp, struct uio *uio, struct ucred *cred,
	int flags)
{

	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
kvm_vm_fo_write(struct file *fp, struct uio *uio, struct ucred *cred,
	int flags)
{

	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
kvm_vm_fo_ioctl(struct file *fp, u_long command, caddr_t data,
	struct ucred *cred, struct sysmsg *msg)
{
	struct kvm_vm *vm;
	int error;

	(void)cred;
	error = devfs_get_cdevpriv(fp, (void **)&vm);
	if (error != 0)
		return error;
	switch (command) {
	case KVM_CHECK_EXTENSION:
	case KVM_LINUX_IO(0x03):
		msg->sysmsg_result = kvm_capability((int)(intptr_t)*(caddr_t *)data);
		return 0;
	case KVM_GET_VCPU_MMAP_SIZE:
	case KVM_LINUX_IO(0x04):
		msg->sysmsg_result = 2 * PAGE_SIZE;
		return 0;
	case KVM_CREATE_VCPU:
	case KVM_LINUX_IO(0x41): {
		uint32_t id;
		int fd;

		id = (uint32_t)(uintptr_t)*(caddr_t *)data;
		error = kvm_vcpu_create(vm, curthread->td_lwp, fp->f_data, id,
		    &fd);
		if (error == 0)
			msg->sysmsg_result = fd;
		return error;
	}
	case KVM_SET_USER_MEMORY_REGION:
		return kvm_vm_set_user_memory(vm,
		    (const struct kvm_userspace_memory_region *)data);
	case KVM_SET_TSS_ADDR:
	case KVM_LINUX_IO(0x47):
		return kvm_vm_set_tss_address(vm,
		    (uint64_t)(uintptr_t)*(caddr_t *)data);
	case KVM_SET_IDENTITY_MAP_ADDR:
		return kvm_vm_set_identity_map_address(vm,
		    *(const uint64_t *)data);
	case KVM_CREATE_IRQCHIP:
	case KVM_LINUX_IO(0x60):
		error = vmm_machine_create_irqchip(vm->machine);
		if (error == 0) {
			lwkt_gettoken(&vm->token);
			vm->irqchip = true;
			lwkt_reltoken(&vm->token);
		}
		return error;
	case KVM_CREATE_PIT2:
		return kvm_vm_create_pit(vm,
		    (const struct kvm_pit_config *)data);
	case KVM_GET_PIT2:
		return kvm_vm_get_pit(vm, (struct kvm_pit_state2 *)data);
	case KVM_SET_PIT2:
		return kvm_vm_set_pit(vm,
		    (const struct kvm_pit_state2 *)data);
	case KVM_IRQ_LINE:
		return kvm_vm_set_irq_line(vm,
		    (const struct kvm_irq_level *)data);
	case KVM_GET_IRQCHIP:
		return kvm_vm_get_irqchip(vm, (struct kvm_irqchip *)data);
	case KVM_SET_IRQCHIP:
		return kvm_vm_set_irqchip(vm,
		    (const struct kvm_irqchip *)data);
	case KVM_SET_CLOCK:
		return kvm_vm_set_clock(vm,
		    (const struct kvm_clock_data *)data);
	case KVM_GET_CLOCK:
		return kvm_vm_get_clock(vm, (struct kvm_clock_data *)data);
	case KVM_IOEVENTFD:
		return kvm_ioevent_configure(vm,
		    (const struct kvm_ioeventfd *)data);
	case KVM_DFLY_SET_GSI_ROUTING:
		return kvm_irqroute_configure(vm,
		    (const struct kvm_dfly_buffer *)data);
	case KVM_IRQFD:
		return kvm_irqfd_configure(vm, (const struct kvm_irqfd *)data);
	case KVM_SIGNAL_MSI:
	case KVM_LINUX_IO(0xa5):
		return kvm_vm_signal_msi(vm, (const struct kvm_msi *)data);
	default:
		return ENOTTY;
	}
}

static int
kvm_vm_create_pit(struct kvm_vm *vm, const struct kvm_pit_config *config)
{
	unsigned int index;

	if (vm == NULL || config == NULL ||
	    (config->flags & ~KVM_PIT_SPEAKER_DUMMY) != 0)
		return EINVAL;
	for (index = 0; index < nitems(config->pad); ++index) {
		if (config->pad[index] != 0)
			return EINVAL;
	}
	return vmm_machine_create_pit(vm->machine);
}

static int
kvm_vm_get_pit(struct kvm_vm *vm, struct kvm_pit_state2 *state)
{
	struct vmm_pit_state pit_state;
	unsigned int index;
	int error;

	if (vm == NULL || state == NULL)
		return EINVAL;
	error = vmm_machine_get_pit(vm->machine, &pit_state);
	if (error != 0)
		return error;
	bzero(state, sizeof(*state));
	for (index = 0; index < nitems(state->channels); ++index) {
		state->channels[index].count = pit_state.channels[index].count;
		state->channels[index].latched_count =
		    pit_state.channels[index].latched_count;
		state->channels[index].count_latched =
		    pit_state.channels[index].count_latched;
		state->channels[index].status_latched =
		    pit_state.channels[index].status_latched;
		state->channels[index].status = pit_state.channels[index].status;
		state->channels[index].read_state =
		    pit_state.channels[index].read_state;
		state->channels[index].write_state =
		    pit_state.channels[index].write_state;
		state->channels[index].write_latch =
		    pit_state.channels[index].write_latch;
		state->channels[index].rw_mode = pit_state.channels[index].rw_mode;
		state->channels[index].mode = pit_state.channels[index].mode;
		state->channels[index].bcd = pit_state.channels[index].bcd;
		state->channels[index].gate = pit_state.channels[index].gate;
		state->channels[index].count_load_time =
		    pit_state.channels[index].count_load_time;
	}
	state->flags = pit_state.flags;
	return 0;
}

static int
kvm_vm_set_pit(struct kvm_vm *vm, const struct kvm_pit_state2 *state)
{
	struct vmm_pit_state pit_state;
	unsigned int index;

	if (vm == NULL || state == NULL)
		return EINVAL;
	for (index = 0; index < nitems(state->reserved); ++index) {
		if (state->reserved[index] != 0)
			return EINVAL;
	}
	bzero(&pit_state, sizeof(pit_state));
	for (index = 0; index < nitems(state->channels); ++index) {
		pit_state.channels[index].count = state->channels[index].count;
		pit_state.channels[index].latched_count =
		    state->channels[index].latched_count;
		pit_state.channels[index].count_latched =
		    state->channels[index].count_latched;
		pit_state.channels[index].status_latched =
		    state->channels[index].status_latched;
		pit_state.channels[index].status = state->channels[index].status;
		pit_state.channels[index].read_state = state->channels[index].read_state;
		pit_state.channels[index].write_state =
		    state->channels[index].write_state;
		pit_state.channels[index].write_latch =
		    state->channels[index].write_latch;
		pit_state.channels[index].rw_mode = state->channels[index].rw_mode;
		pit_state.channels[index].mode = state->channels[index].mode;
		pit_state.channels[index].bcd = state->channels[index].bcd;
		pit_state.channels[index].gate = state->channels[index].gate;
		pit_state.channels[index].count_load_time =
		    state->channels[index].count_load_time;
	}
	pit_state.flags = state->flags;
	return vmm_machine_set_pit(vm->machine, &pit_state);
}

/*
 * These legacy KVM addresses reserve guest pages for a shadow-MMU irqchip.
 * VMM uses NPT, so they are retained as KVM VM compatibility state only.
 */
static int
kvm_vm_set_tss_address(struct kvm_vm *vm, uint64_t address)
{

	if ((address & PAGE_MASK) != 0 || address >= KVM_GPA_MAX)
		return EINVAL;
	lwkt_gettoken(&vm->token);
	vm->tss_address = address;
	lwkt_reltoken(&vm->token);
	return 0;
}

static int
kvm_vm_set_identity_map_address(struct kvm_vm *vm, uint64_t address)
{

	if ((address & PAGE_MASK) != 0 || address >= KVM_GPA_MAX)
		return EINVAL;
	lwkt_gettoken(&vm->token);
	vm->identity_map_address = address;
	lwkt_reltoken(&vm->token);
	return 0;
}

static int
kvm_vm_signal_msi(struct kvm_vm *vm, const struct kvm_msi *msi)
{
	uint64_t address;

	if (msi == NULL || (msi->flags & ~KVM_MSI_VALID_DEVID) != 0)
		return EINVAL;
	address = ((uint64_t)msi->address_hi << 32) | msi->address_lo;
	/*
	 * Linux KVM accepts an all-zero APIC-style message from QEMU's
	 * in-kernel APIC memory region.  Its zero destination fields mean
	 * physical APIC ID 0; vmm's core MSI ABI requires the architectural
	 * x86 APIC base bits, so normalize only this frontend representation.
	 */
	if (address == 0)
		address = KVM_X86_MSI_APIC_BASE;
	return vmm_machine_raise_msi(vm->machine, address, msi->data);
}

static int
kvm_vm_set_irq_line(struct kvm_vm *vm, const struct kvm_irq_level *line)
{
	if (line == NULL || line->level > 1)
		return EINVAL;
	if (kvm_debug_trace &&
	    kvm_irq_line_trace_count < KVM_DEBUG_TRACE_LIMIT) {
		++kvm_irq_line_trace_count;
		kprintf("kvm: irq line gsi=%u level=%u\n", line->irq,
		    line->level);
	}
	return vmm_machine_set_irq(vm->machine, line->irq, line->level != 0);
}

static int
kvm_vm_get_irqchip(struct kvm_vm *vm, struct kvm_irqchip *irqchip)
{
	struct vmm_ioapic_state state;
	struct vmm_pic_state pic_state;
	uint32_t chip_id;
	uint32_t pin;
	int error;

	if (irqchip == NULL)
		return EINVAL;
	chip_id = irqchip->chip_id;
	bzero(irqchip, sizeof(*irqchip));
	irqchip->chip_id = chip_id;
	switch (irqchip->chip_id) {
	case KVM_IRQCHIP_PIC_MASTER:
	case KVM_IRQCHIP_PIC_SLAVE:
		error = vmm_machine_get_pic(vm->machine, &pic_state);
		if (error != 0)
			return error;
		if (irqchip->chip_id == KVM_IRQCHIP_PIC_MASTER)
			bcopy(&pic_state.master, &irqchip->chip.pic,
			    sizeof(irqchip->chip.pic));
		else
			bcopy(&pic_state.slave, &irqchip->chip.pic,
			    sizeof(irqchip->chip.pic));
		return 0;
	case KVM_IRQCHIP_IOAPIC:
		bzero(&state, sizeof(state));
		error = vmm_machine_get_ioapic(vm->machine, &state);
		if (error != 0)
			return error;
		irqchip->chip.ioapic.base_address = state.base;
		irqchip->chip.ioapic.ioregsel = state.select;
		irqchip->chip.ioapic.id = state.id;
		irqchip->chip.ioapic.irr = state.irr;
		for (pin = 0; pin < KVM_IOAPIC_NUM_PINS; ++pin)
			irqchip->chip.ioapic.redirtbl[pin].bits = state.redir[pin];
		return 0;
	default:
		return EINVAL;
	}
}

static int
kvm_vm_set_irqchip(struct kvm_vm *vm, const struct kvm_irqchip *irqchip)
{
	struct vmm_ioapic_state state;
	struct vmm_pic_state pic_state;
	uint32_t pin;
	int error;

	if (irqchip == NULL)
		return EINVAL;
	switch (irqchip->chip_id) {
	case KVM_IRQCHIP_PIC_MASTER:
	case KVM_IRQCHIP_PIC_SLAVE:
		error = vmm_machine_get_pic(vm->machine, &pic_state);
		if (error != 0)
			return error;
		if (irqchip->chip_id == KVM_IRQCHIP_PIC_MASTER)
			bcopy(&irqchip->chip.pic, &pic_state.master,
			    sizeof(pic_state.master));
		else
			bcopy(&irqchip->chip.pic, &pic_state.slave,
			    sizeof(pic_state.slave));
		return vmm_machine_set_pic(vm->machine, &pic_state);
	case KVM_IRQCHIP_IOAPIC:
		bzero(&state, sizeof(state));
		state.base = irqchip->chip.ioapic.base_address;
		state.select = irqchip->chip.ioapic.ioregsel;
		state.id = irqchip->chip.ioapic.id;
		state.irr = irqchip->chip.ioapic.irr;
		for (pin = 0; pin < KVM_IOAPIC_NUM_PINS; ++pin)
			state.redir[pin] = irqchip->chip.ioapic.redirtbl[pin].bits;
		return vmm_machine_set_ioapic(vm->machine, &state);
	default:
		return EINVAL;
	}
}

static int
kvm_vm_set_clock(struct kvm_vm *vm, const struct kvm_clock_data *clock)
{
	struct timespec monotonic;
	struct timespec realtime;
	uint64_t clock_value;
	uint64_t now_monotonic;
	uint64_t now_realtime;

	if (vm == NULL || clock == NULL ||
	    (clock->flags & ~(KVM_CLOCK_TSC_STABLE | KVM_CLOCK_REALTIME |
	    KVM_CLOCK_HOST_TSC)) != 0)
		return EINVAL;
	nanouptime(&monotonic);
	getnanotime(&realtime);
	now_monotonic = (uint64_t)monotonic.tv_sec * 1000000000ULL +
	    monotonic.tv_nsec;
	now_realtime = (uint64_t)realtime.tv_sec * 1000000000ULL +
	    realtime.tv_nsec;
	clock_value = clock->clock;
	if ((clock->flags & KVM_CLOCK_REALTIME) != 0 &&
	    now_realtime > clock->realtime) {
		if (clock_value > UINT64_MAX - (now_realtime - clock->realtime))
			return EINVAL;
		clock_value += now_realtime - clock->realtime;
	}
	lwkt_gettoken(&vm->token);
	vm->clock_offset = (int64_t)(clock_value - now_monotonic);
	lwkt_reltoken(&vm->token);
	return 0;
}

static int
kvm_vm_get_clock(struct kvm_vm *vm, struct kvm_clock_data *clock)
{
	struct timespec monotonic;
	struct timespec realtime;
	int64_t offset;

	if (vm == NULL || clock == NULL)
		return EINVAL;
	nanouptime(&monotonic);
	getnanotime(&realtime);
	lwkt_gettoken(&vm->token);
	offset = vm->clock_offset;
	lwkt_reltoken(&vm->token);
	bzero(clock, sizeof(*clock));
	clock->clock = (uint64_t)monotonic.tv_sec * 1000000000ULL +
		monotonic.tv_nsec + offset;
	clock->realtime = (uint64_t)realtime.tv_sec * 1000000000ULL +
		realtime.tv_nsec;
	clock->flags = KVM_CLOCK_REALTIME;
	return 0;
}

static int
kvm_vm_set_user_memory(struct kvm_vm *vm,
	const struct kvm_userspace_memory_region *region)
{
	struct kvm_memory_piece_list pieces;
	struct kvm_memory_piece *piece;
	struct kvm_memory_slot *slot;
	vm_offset_t gpa;
	vm_offset_t gpa_end;
	vm_offset_t hva;
	vm_size_t size;
	vm_prot_t protection;
	int count;
	int error;
	int piece_count;
	int slot_index;

	if (region == NULL || region->slot >= KVM_MEMORY_SLOTS)
		return EINVAL;
	/* Dirty logging is optional migration state, not guest mapping state. */
	if ((region->flags &
	    ~(KVM_MEM_READONLY | KVM_MEM_LOG_DIRTY_PAGES)) != 0)
		return EOPNOTSUPP;

	gpa = region->guest_phys_addr;
	size = region->memory_size;
	hva = region->userspace_addr;
	if (size == 0) {
		if (region->flags != 0)
			return EINVAL;
		lwkt_gettoken(&vm->token);
		slot = &vm->slots[region->slot];
		if (slot->present) {
			vm_map_remove(&vm->vmspace->vm_map, slot->gpa,
			    slot->gpa + slot->size);
			slot->present = false;
		}
		lwkt_reltoken(&vm->token);
		return 0;
	}
	if ((gpa & PAGE_MASK) != 0 || (hva & PAGE_MASK) != 0 ||
	    (size & PAGE_MASK) != 0 || hva == 0 || size > KVM_GPA_MAX ||
	    gpa > KVM_GPA_MAX - size || hva > (vm_offset_t)-1 - size)
		return EINVAL;
	gpa_end = gpa + size;

	STAILQ_INIT(&pieces);
	piece_count = 0;
	if (curthread->td_lwp == NULL)
		return EINVAL;
	error = kvm_vm_collect_memory(curthread->td_lwp->lwp_proc->p_vmspace,
	    hva, size, gpa, &pieces, &piece_count);
	if (error != 0)
		return error;

	protection = VM_PROT_READ | VM_PROT_EXECUTE;
	if ((region->flags & KVM_MEM_READONLY) == 0)
		protection |= VM_PROT_WRITE;

	count = vm_map_entry_reserve(piece_count + MAP_RESERVE_COUNT);
	lwkt_gettoken(&vm->token);
	for (slot_index = 0; slot_index < KVM_MEMORY_SLOTS; ++slot_index) {
		slot = &vm->slots[slot_index];
		if (slot_index == region->slot || !slot->present)
			continue;
		if (gpa < slot->gpa + slot->size && slot->gpa < gpa_end) {
			error = EEXIST;
			goto out;
		}
	}

	slot = &vm->slots[region->slot];
	if (slot->present) {
		vm_map_remove(&vm->vmspace->vm_map, slot->gpa,
		    slot->gpa + slot->size);
		slot->present = false;
	}

	error = 0;
	vm_map_lock(&vm->vmspace->vm_map);
	STAILQ_FOREACH(piece, &pieces, entry) {
		vm_object_hold(piece->object);
		error = vm_map_insert(&vm->vmspace->vm_map, &count,
		    piece->object, NULL, piece->offset, NULL, piece->gpa,
		    piece->gpa + piece->size, VM_MAPTYPE_NORMAL, VM_SUBSYS_NVMM,
		    protection, VM_PROT_ALL, 0);
		vm_object_drop(piece->object);
		if (error != 0)
			break;
		piece->object = NULL;
	}
	vm_map_unlock(&vm->vmspace->vm_map);
	if (error == 0)
		error = vm_map_inherit(&vm->vmspace->vm_map, gpa, gpa_end,
		    VM_INHERIT_SHARE);
	if (error != 0) {
		vm_map_remove(&vm->vmspace->vm_map, gpa, gpa_end);
		error = vm_mmap_to_errno(error);
		goto out;
	}
	slot->gpa = gpa;
	slot->size = size;
	slot->present = true;

out:
	lwkt_reltoken(&vm->token);
	vm_map_entry_release(count);
	kvm_vm_drop_memory(&pieces);
	return error;
}

static int
kvm_vm_collect_memory(struct vmspace *vmspace, vm_offset_t hva,
	vm_size_t size, vm_offset_t gpa, struct kvm_memory_piece_list *pieces,
	int *piece_count)
{
	struct vm_map *map;
	struct vm_map_entry *entry;
	struct kvm_memory_piece *piece;
	vm_offset_t end;
	vm_offset_t piece_end;
	int error;

	if (vmspace == NULL || pieces == NULL || piece_count == NULL)
		return EINVAL;
	map = &vmspace->vm_map;
	end = hva + size;
	while (hva < end) {
		piece = kmalloc(sizeof(*piece), M_KVM, M_WAITOK | M_ZERO);
		lwkt_gettoken(&map->token);
		vm_map_lock(map);
		if (!vm_map_lookup_entry(map, hva, &entry) ||
		    entry->maptype != VM_MAPTYPE_NORMAL) {
			vm_map_unlock(map);
			lwkt_reltoken(&map->token);
			kfree(piece, M_KVM);
			error = EFAULT;
			goto fail;
		}
		/* Anonymous QEMU RAM gets an object on first durable reference. */
		if (entry->ba.object == NULL)
			vm_map_entry_allocate_object(entry);
		piece_end = entry->ba.end < end ? entry->ba.end : end;
		piece->object = entry->ba.object;
		vm_object_hold(piece->object);
		vm_object_reference_locked(piece->object);
		vm_object_drop(piece->object);
		piece->offset = entry->ba.offset + (hva - entry->ba.start);
		piece->gpa = gpa;
		piece->size = piece_end - hva;
		vm_map_unlock(map);
		lwkt_reltoken(&map->token);
		STAILQ_INSERT_TAIL(pieces, piece, entry);
		++*piece_count;
		gpa += piece->size;
		hva = piece_end;
	}
	return 0;

fail:
	kvm_vm_drop_memory(pieces);
	return error;
}

static void
kvm_vm_drop_memory(struct kvm_memory_piece_list *pieces)
{
	struct kvm_memory_piece *piece;

	while ((piece = STAILQ_FIRST(pieces)) != NULL) {
		STAILQ_REMOVE_HEAD(pieces, entry);
		if (piece->object != NULL)
			vm_object_deallocate(piece->object);
		kfree(piece, M_KVM);
	}
}

static int
kvm_vm_fo_kqfilter(struct file *fp, struct knote *kn)
{

	(void)fp;
	(void)kn;
	return EOPNOTSUPP;
}

static int
kvm_vm_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
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
kvm_vm_fo_close(struct file *fp)
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
kvm_vm_fo_seek(struct file *fp, off_t offset, int whence, off_t *result)
{

	(void)fp;
	(void)offset;
	(void)whence;
	(void)result;
	return ESPIPE;
}

static void
kvm_vm_file_destroy(void *arg)
{

	kvm_vm_release(arg);
}

void
kvm_vm_release(struct kvm_vm *vm)
{
	int destroy;

	if (vm == NULL)
		return;
	lwkt_gettoken(&vm->token);
	KKASSERT(vm->references != 0);
	destroy = --vm->references == 0;
	lwkt_reltoken(&vm->token);
	if (destroy)
		kvm_vm_destroy(vm);
}

void
kvm_vm_reference(struct kvm_vm *vm)
{

	KKASSERT(vm != NULL);
	lwkt_gettoken(&vm->token);
	KKASSERT(vm->references != 0);
	++vm->references;
	lwkt_reltoken(&vm->token);
}

static void
kvm_vm_destroy(struct kvm_vm *vm)
{
	unsigned int id;
	int error;

	for (id = 0; id < KVM_MAX_VCPUS; ++id)
		KKASSERT(vm->vcpus[id] == NULL);
	kvm_ioevent_clear(vm);
	kvm_irqfd_clear(vm);
	kvm_irqroute_clear(vm);
	if (vm->machine != NULL) {
		error = vmm_machine_destroy(vm->machine);
		KKASSERT(error == 0);
		vm->machine = NULL;
	}
	if (vm->vmspace != NULL) {
		pmap_del_all_cpus(vm->vmspace);
		vmspace_rel(vm->vmspace);
		vm->vmspace = NULL;
	}
	lwkt_gettoken(&kvm_frontend_token);
	KKASSERT(kvm_file_count != 0);
	--kvm_file_count;
	lwkt_reltoken(&kvm_frontend_token);
	kfree(vm, M_KVM);
}
