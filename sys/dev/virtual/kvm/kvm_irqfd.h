/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * KVM MSI routing and irqfd bindings.
 */
#ifndef KVM_IRQFD_H
#define KVM_IRQFD_H

#include <sys/types.h>

struct kvm_dfly_buffer;
struct kvm_irqfd;
struct kvm_vm;

int kvm_irqroute_configure(struct kvm_vm *,
	const struct kvm_dfly_buffer *);
void kvm_irqroute_clear(struct kvm_vm *);
int kvm_irqfd_configure(struct kvm_vm *, const struct kvm_irqfd *);
void kvm_irqfd_clear(struct kvm_vm *);

#endif /* KVM_IRQFD_H */
