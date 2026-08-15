/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * KVM ioeventfd bindings -- see sys/kvm.h.
 */
#ifndef KVM_IOEVENT_H
#define KVM_IOEVENT_H

#include <sys/queue.h>

#include <sys/kvm.h>

#include "kvm_vm.h"

struct file;

struct kvm_ioevent {
	TAILQ_ENTRY(kvm_ioevent) entry;
	struct kvm_vm *vm;
	struct file *eventfp;
	vmm_io_t io;
	uint64_t address;
	uint64_t datamatch;
	uint32_t length;
	uint32_t flags;
};

int kvm_ioevent_configure(struct kvm_vm *, const struct kvm_ioeventfd *);
void kvm_ioevent_clear(struct kvm_vm *);

#endif /* KVM_IOEVENT_H */
