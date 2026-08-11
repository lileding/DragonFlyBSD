/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * KVM MSI routing and irqfd bindings.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/globaldata.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/uio.h>

#include <machine/atomic.h>

#include <linux/kvm.h>

#include "kvm_eventfd.h"
#include "kvm_internal.h"
#include "kvm_irqfd.h"
#include "kvm_vm.h"

struct kvm_irqroute {
	TAILQ_ENTRY(kvm_irqroute) entry;
	uint32_t gsi;
	uint32_t type;
	uint32_t pin;
	uint64_t address;
	uint32_t data;
};

struct kvm_irqfd_binding {
	TAILQ_ENTRY(kvm_irqfd_binding) entry;
	struct kvm_vm *vm;
	struct file *eventfp;
	struct kvm_eventfd_listener listener;
	uint32_t gsi;
};

TAILQ_HEAD(kvm_irqroute_list, kvm_irqroute);

static int kvm_irqroute_copyin(const struct kvm_dfly_buffer *,
	struct kvm_irq_routing_entry **, uint32_t *);
static int kvm_irqroute_validate(const struct kvm_irq_routing_entry *,
	uint32_t);
static struct kvm_irqroute *kvm_irqroute_find(struct kvm_vm *, uint32_t);
static int kvm_irqfd_bind(struct kvm_vm *, const struct kvm_irqfd *);
static int kvm_irqfd_unbind(struct kvm_vm *, const struct kvm_irqfd *);
static void kvm_irqfd_signal(void *);
static int kvm_irqfd_matches(const struct kvm_irqfd_binding *,
	const struct kvm_irqfd *, const struct file *);

int
kvm_irqroute_configure(struct kvm_vm *vm,
	const struct kvm_dfly_buffer *buffer)
{
	struct kvm_irqroute_list routes;
	struct kvm_irqroute_list old_routes;
	struct kvm_irq_routing_entry *entries;
	struct kvm_irqroute *route;
	uint32_t count;
	uint32_t index;
	int error;

	if (vm == NULL || buffer == NULL)
		return EINVAL;
	error = kvm_irqroute_copyin(buffer, &entries, &count);
	if (error != 0)
		return error;
	TAILQ_INIT(&routes);
	for (index = 0; index < count; ++index) {
		error = kvm_irqroute_validate(entries, index);
		if (error != 0)
			goto fail;
		if (entries[index].type == KVM_IRQ_ROUTING_IRQCHIP &&
		    entries[index].u.irqchip.irqchip != KVM_IRQCHIP_IOAPIC)
			continue;
		route = kmalloc(sizeof(*route), M_KVM, M_WAITOK | M_ZERO);
		route->gsi = entries[index].gsi;
		route->type = entries[index].type;
		if (route->type == KVM_IRQ_ROUTING_MSI) {
			route->address = entries[index].u.msi.address_lo |
			    ((uint64_t)entries[index].u.msi.address_hi << 32);
			route->data = entries[index].u.msi.data;
		} else {
			route->pin = entries[index].u.irqchip.pin;
		}
		TAILQ_INSERT_TAIL(&routes, route, entry);
	}
	TAILQ_INIT(&old_routes);
	lwkt_gettoken(&vm->token);
	while ((route = TAILQ_FIRST(&vm->irqroutes)) != NULL) {
		TAILQ_REMOVE(&vm->irqroutes, route, entry);
		TAILQ_INSERT_TAIL(&old_routes, route, entry);
	}
	while ((route = TAILQ_FIRST(&routes)) != NULL) {
		TAILQ_REMOVE(&routes, route, entry);
		TAILQ_INSERT_TAIL(&vm->irqroutes, route, entry);
	}
	lwkt_reltoken(&vm->token);
	while ((route = TAILQ_FIRST(&old_routes)) != NULL) {
		TAILQ_REMOVE(&old_routes, route, entry);
		kfree(route, M_KVM);
	}
	kfree(entries, M_KVM);
	return 0;

fail:
	while ((route = TAILQ_FIRST(&routes)) != NULL) {
		TAILQ_REMOVE(&routes, route, entry);
		kfree(route, M_KVM);
	}
	kfree(entries, M_KVM);
	return error;
}

void
kvm_irqroute_clear(struct kvm_vm *vm)
{
	struct kvm_irqroute *route;

	for (;;) {
		lwkt_gettoken(&vm->token);
		route = TAILQ_FIRST(&vm->irqroutes);
		if (route != NULL)
			TAILQ_REMOVE(&vm->irqroutes, route, entry);
		lwkt_reltoken(&vm->token);
		if (route == NULL)
			break;
		kfree(route, M_KVM);
	}
}

int
kvm_irqfd_configure(struct kvm_vm *vm, const struct kvm_irqfd *request)
{

	if (vm == NULL || request == NULL || request->fd < 0 ||
	    (request->flags & ~(KVM_IRQFD_FLAG_DEASSIGN |
	    KVM_IRQFD_FLAG_RESAMPLE)) != 0)
		return EINVAL;
	if ((request->flags & KVM_IRQFD_FLAG_RESAMPLE) != 0)
		return EOPNOTSUPP;
	if ((request->flags & KVM_IRQFD_FLAG_DEASSIGN) != 0)
		return kvm_irqfd_unbind(vm, request);
	return kvm_irqfd_bind(vm, request);
}

void
kvm_irqfd_clear(struct kvm_vm *vm)
{
	struct kvm_irqfd_binding *binding;
	int error;

	for (;;) {
		lwkt_gettoken(&vm->token);
		binding = TAILQ_FIRST(&vm->irqfds);
		if (binding != NULL)
			TAILQ_REMOVE(&vm->irqfds, binding, entry);
		lwkt_reltoken(&vm->token);
		if (binding == NULL)
			break;
		error = kvm_eventfd_unlisten(binding->eventfp, &binding->listener);
		KKASSERT(error == 0);
		kvm_eventfd_drop(binding->eventfp);
		kfree(binding, M_KVM);
	}
}

static int
kvm_irqroute_copyin(const struct kvm_dfly_buffer *buffer,
	struct kvm_irq_routing_entry **entriesp, uint32_t *countp)
{
	struct kvm_irq_routing header;
	struct kvm_irq_routing_entry *entries;
	size_t length;

	if (buffer->data == 0 || buffer->reserved != 0 ||
	    buffer->length < sizeof(header))
		return EINVAL;
	if (copyin((const void *)(uintptr_t)buffer->data, &header,
	    sizeof(header)) != 0)
		return EFAULT;
	if (header.flags != 0 || header.nr > KVM_MAX_IRQ_ROUTES ||
	    header.nr > (SIZE_MAX - sizeof(header)) / sizeof(*entries))
		return EINVAL;
	length = sizeof(header) +
	    (size_t)header.nr * sizeof(*entries);
	if (buffer->length != length)
		return EINVAL;
	if (header.nr == 0) {
		*entriesp = NULL;
		*countp = 0;
		return 0;
	}
	entries = kmalloc((size_t)header.nr * sizeof(*entries), M_KVM,
	    M_WAITOK | M_ZERO);
	if (copyin((const char *)(uintptr_t)buffer->data +
	    sizeof(header), entries, (size_t)header.nr * sizeof(*entries)) != 0) {
		kfree(entries, M_KVM);
		return EFAULT;
	}
	*entriesp = entries;
	*countp = header.nr;
	return 0;
}

static int
kvm_irqroute_validate(const struct kvm_irq_routing_entry *entries,
	uint32_t index)
{
	if (entries[index].type == KVM_IRQ_ROUTING_MSI) {
		if ((entries[index].flags & ~KVM_MSI_VALID_DEVID) != 0)
			return EINVAL;
		return 0;
	}
	if (entries[index].type != KVM_IRQ_ROUTING_IRQCHIP ||
	    entries[index].flags != 0)
		return EOPNOTSUPP;
	switch (entries[index].u.irqchip.irqchip) {
	case KVM_IRQCHIP_PIC_MASTER:
	case KVM_IRQCHIP_PIC_SLAVE:
	case KVM_IRQCHIP_IOAPIC:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static struct kvm_irqroute *
kvm_irqroute_find(struct kvm_vm *vm, uint32_t gsi)
{
	struct kvm_irqroute *route;

	TAILQ_FOREACH(route, &vm->irqroutes, entry) {
		if (route->gsi == gsi)
			return route;
	}
	return NULL;
}

static int
kvm_irqfd_bind(struct kvm_vm *vm, const struct kvm_irqfd *request)
{
	struct kvm_irqfd_binding *binding;
	struct kvm_irqfd_binding *other;
	struct file *eventfp;
	int error;

	error = kvm_eventfd_hold(curthread, request->fd, &eventfp);
	if (error != 0)
		return error;
	lwkt_gettoken(&vm->token);
	if (kvm_irqroute_find(vm, request->gsi) == NULL) {
		lwkt_reltoken(&vm->token);
		kvm_eventfd_drop(eventfp);
		return ENOENT;
	}
	TAILQ_FOREACH(other, &vm->irqfds, entry) {
		if (other->gsi == request->gsi && other->eventfp == eventfp) {
			lwkt_reltoken(&vm->token);
			kvm_eventfd_drop(eventfp);
			return EEXIST;
		}
	}
	lwkt_reltoken(&vm->token);
	binding = kmalloc(sizeof(*binding), M_KVM, M_WAITOK | M_ZERO);
	binding->vm = vm;
	binding->eventfp = eventfp;
	binding->gsi = request->gsi;
	binding->listener.callback = kvm_irqfd_signal;
	binding->listener.argument = binding;
	error = kvm_eventfd_listen(eventfp, &binding->listener);
	if (error != 0) {
		kvm_eventfd_drop(eventfp);
		kfree(binding, M_KVM);
		return error;
	}
	lwkt_gettoken(&vm->token);
	if (kvm_irqroute_find(vm, request->gsi) == NULL) {
		lwkt_reltoken(&vm->token);
		error = kvm_eventfd_unlisten(eventfp, &binding->listener);
		KKASSERT(error == 0);
		kvm_eventfd_drop(eventfp);
		kfree(binding, M_KVM);
		return ENOENT;
	}
	TAILQ_FOREACH(other, &vm->irqfds, entry) {
		if (other->gsi == request->gsi && other->eventfp == eventfp)
			break;
	}
	if (other != NULL) {
		lwkt_reltoken(&vm->token);
		error = kvm_eventfd_unlisten(eventfp, &binding->listener);
		KKASSERT(error == 0);
		kvm_eventfd_drop(eventfp);
		kfree(binding, M_KVM);
		return EEXIST;
	}
	TAILQ_INSERT_TAIL(&vm->irqfds, binding, entry);
	lwkt_reltoken(&vm->token);
	return 0;
}

static int
kvm_irqfd_unbind(struct kvm_vm *vm, const struct kvm_irqfd *request)
{
	struct kvm_irqfd_binding *binding;
	struct file *eventfp;
	int error;

	error = kvm_eventfd_hold(curthread, request->fd, &eventfp);
	if (error != 0)
		return error;
	lwkt_gettoken(&vm->token);
	TAILQ_FOREACH(binding, &vm->irqfds, entry) {
		if (kvm_irqfd_matches(binding, request, eventfp))
			break;
	}
	if (binding != NULL)
		TAILQ_REMOVE(&vm->irqfds, binding, entry);
	lwkt_reltoken(&vm->token);
	kvm_eventfd_drop(eventfp);
	if (binding == NULL)
		return ENOENT;
	error = kvm_eventfd_unlisten(binding->eventfp, &binding->listener);
	KKASSERT(error == 0);
	kvm_eventfd_drop(binding->eventfp);
	kfree(binding, M_KVM);
	return 0;
}

static void
kvm_irqfd_signal(void *argument)
{
	struct kvm_irqfd_binding *binding = argument;
	struct kvm_irqroute *route;
	uint64_t address;
	uint32_t data;
	uint32_t pin;
	uint32_t type;
	int found;

	found = 0;
	lwkt_gettoken(&binding->vm->token);
	route = kvm_irqroute_find(binding->vm, binding->gsi);
	if (route != NULL) {
		type = route->type;
		address = route->address;
		data = route->data;
		pin = route->pin;
		found = 1;
	}
	lwkt_reltoken(&binding->vm->token);
	if (found) {
		if (type == KVM_IRQ_ROUTING_MSI)
			(void)vmm_machine_raise_msi(binding->vm->machine, address, data);
		else
			(void)vmm_machine_raise_irq(binding->vm->machine, pin);
	}
}

static int
kvm_irqfd_matches(const struct kvm_irqfd_binding *binding,
	const struct kvm_irqfd *request, const struct file *eventfp)
{

	return binding->eventfp == eventfp && binding->gsi == request->gsi;
}
