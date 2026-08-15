/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * KVM ioeventfd bindings -- see linux/kvm.h.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/globaldata.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/thread.h>

#include "kvm_eventfd.h"
#include "kvm_internal.h"
#include "kvm_ioevent.h"

static int kvm_ioevent_bind(struct kvm_vm *,
    const struct kvm_ioeventfd *);
static int kvm_ioevent_unbind(struct kvm_vm *,
    const struct kvm_ioeventfd *);
static int kvm_ioevent_trap(struct kvm_ioevent *, enum vmm_io_width,
    vmm_io_t *);
static void kvm_ioevent_untrap(struct kvm_ioevent *);
static int kvm_ioevent_handler(vmm_vcpu_t, void *,
    const struct vmm_io_write *);
static int kvm_ioevent_matches(const struct kvm_ioevent *,
    const struct kvm_ioeventfd *, const struct file *);

int
kvm_ioevent_configure(struct kvm_vm *vm, const struct kvm_ioeventfd *request)
{

	if (vm == NULL || request == NULL || request->fd < 0 ||
	    (request->flags & ~KVM_IOEVENTFD_VALID_FLAG_MASK) != 0 ||
	    (request->len != 1 && request->len != 2 && request->len != 4 &&
	    request->len != 8 && request->len != 0))
		return EINVAL;
	if ((request->flags & KVM_IOEVENTFD_FLAG_DEASSIGN) != 0)
		return kvm_ioevent_unbind(vm, request);
	return kvm_ioevent_bind(vm, request);
}

void
kvm_ioevent_clear(struct kvm_vm *vm)
{
	struct kvm_ioevent *event;

	for (;;) {
		lwkt_gettoken(&vm->token);
		event = TAILQ_FIRST(&vm->ioevents);
		if (event != NULL)
			TAILQ_REMOVE(&vm->ioevents, event, entry);
		lwkt_reltoken(&vm->token);
		if (event == NULL)
			break;
		kvm_ioevent_untrap(event);
		kvm_eventfd_drop(event->eventfp);
		kfree(event, M_KVM);
	}
}

static int
kvm_ioevent_bind(struct kvm_vm *vm, const struct kvm_ioeventfd *request)
{
	struct kvm_ioevent *event;
	struct file *eventfp;
	int error;

	error = kvm_eventfd_hold(curthread, request->fd, &eventfp);
	if (error != 0)
		return error;
	event = kmalloc(sizeof(*event), M_KVM, M_WAITOK | M_ZERO);
	if (event == NULL) {
		kvm_eventfd_drop(eventfp);
		return ENOMEM;
	}
	event->vm = vm;
	event->eventfp = eventfp;
	event->address = request->addr;
	event->datamatch = request->datamatch;
	event->length = request->len;
	event->flags = request->flags;
	if ((request->flags & KVM_IOEVENTFD_FLAG_PIO) != 0 &&
	    (request->addr > UINT16_MAX || (request->len != 0 &&
	    request->addr + request->len > (uint64_t)UINT16_MAX + 1))) {
		kvm_eventfd_drop(eventfp);
		kfree(event, M_KVM);
		return EINVAL;
	}
	if (request->len != 0)
		error = kvm_ioevent_trap(event, (enum vmm_io_width)request->len,
		    &event->io[0]);
	else {
		static const enum vmm_io_width widths[] = {
			VMM_IO_WIDTH_8,
			VMM_IO_WIDTH_16,
			VMM_IO_WIDTH_32,
			VMM_IO_WIDTH_64,
		};
		size_t index;

		for (index = 0; index < nitems(widths); ++index) {
			if ((request->flags & KVM_IOEVENTFD_FLAG_PIO) != 0 &&
			    widths[index] == VMM_IO_WIDTH_64)
				continue;
			if ((request->flags & KVM_IOEVENTFD_FLAG_PIO) != 0 &&
			    request->addr + widths[index] > (uint64_t)UINT16_MAX + 1)
				continue;
			error = kvm_ioevent_trap(event, widths[index],
			    &event->io[index]);
			if (error != 0)
				break;
		}
	}
	if (error != 0) {
		kvm_ioevent_untrap(event);
		kvm_eventfd_drop(eventfp);
		kfree(event, M_KVM);
		return error;
	}
	lwkt_gettoken(&vm->token);
	TAILQ_INSERT_TAIL(&vm->ioevents, event, entry);
	lwkt_reltoken(&vm->token);
	return 0;
}

static int
kvm_ioevent_unbind(struct kvm_vm *vm, const struct kvm_ioeventfd *request)
{
	struct kvm_ioevent *event;
	struct file *eventfp;
	int error;

	error = kvm_eventfd_hold(curthread, request->fd, &eventfp);
	if (error != 0)
		return error;
	lwkt_gettoken(&vm->token);
	TAILQ_FOREACH(event, &vm->ioevents, entry) {
		if (kvm_ioevent_matches(event, request, eventfp))
			break;
	}
	if (event != NULL)
		TAILQ_REMOVE(&vm->ioevents, event, entry);
	lwkt_reltoken(&vm->token);
	kvm_eventfd_drop(eventfp);
	if (event == NULL)
		return ENOENT;
	kvm_ioevent_untrap(event);
	kvm_eventfd_drop(event->eventfp);
	kfree(event, M_KVM);
	return 0;
}

static int
kvm_ioevent_handler(vmm_vcpu_t vcpu, void *argument,
    const struct vmm_io_write *write)
{
	struct kvm_ioevent *event = argument;

	(void)vcpu;
	if (event == NULL || write == NULL ||
	    write->address != event->address ||
	    (event->length != 0 && write->width != event->length))
		return ENOENT;
	if ((event->flags & KVM_IOEVENTFD_FLAG_DATAMATCH) != 0 &&
	    write->value != event->datamatch)
		return ENOENT;
	return kvm_eventfd_signal(event->eventfp);
}

static int
kvm_ioevent_trap(struct kvm_ioevent *event, enum vmm_io_width width,
    vmm_io_t *io)
{

	if ((event->flags & KVM_IOEVENTFD_FLAG_PIO) != 0) {
		return vmm_machine_trap_pio_write(event->vm->machine,
		    event->address, width, kvm_ioevent_handler, event, io);
	}
	return vmm_machine_trap_mmio_write(event->vm->machine,
	    event->address, width, kvm_ioevent_handler, event, io);
}

static void
kvm_ioevent_untrap(struct kvm_ioevent *event)
{
	size_t index;

	for (index = 0; index < nitems(event->io); ++index) {
		if (event->io[index] == NULL)
			continue;
		(void)vmm_machine_untrap(event->vm->machine, event->io[index]);
		event->io[index] = NULL;
	}
}

static int
kvm_ioevent_matches(const struct kvm_ioevent *event,
    const struct kvm_ioeventfd *request, const struct file *eventfp)
{

	return event->eventfp == eventfp && event->address == request->addr &&
	    event->datamatch == request->datamatch &&
	    event->length == request->len && event->flags ==
	    (request->flags & ~KVM_IOEVENTFD_FLAG_DEASSIGN);
}
