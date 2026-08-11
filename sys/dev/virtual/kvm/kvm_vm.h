/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-VM file descriptor for the DragonFly KVM frontend.
 */
#ifndef KVM_VM_H
#define KVM_VM_H

#include <sys/conf.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/thread.h>

#include "../vmm/vmm.h"

struct lwp;
struct vmspace;
struct vnode;
struct kvm_vcpu;
struct kvm_ioevent;
struct kvm_irqroute;
struct kvm_irqfd_binding;

#define KVM_MEMORY_SLOTS	32
#define KVM_MAX_VCPUS		256
#define KVM_MAX_IRQ_ROUTES	1024
#define KVM_GPA_MAX		((vm_offset_t)127 * 1024 * 1024 * 1024 * 1024)

struct kvm_memory_slot {
	vm_offset_t gpa;
	vm_size_t size;
	bool present;
};

struct kvm_vm {
	/* token protects slots, vcpus, ioevents, compatibility state, and references. */
	vmm_machine_t machine;
	struct vmspace *vmspace;
	struct lwkt_token token;
	struct kvm_memory_slot slots[KVM_MEMORY_SLOTS];
	struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
	TAILQ_HEAD(, kvm_ioevent) ioevents;
	TAILQ_HEAD(, kvm_irqroute) irqroutes;
	TAILQ_HEAD(, kvm_irqfd_binding) irqfds;
	uint64_t tss_address;
	uint64_t identity_map_address;
	int64_t clock_offset;
	unsigned int references;
	bool irqchip;
};

/* Creates one KVM VM fd with an independent guest vmspace. */
int kvm_vm_create(struct lwp *lp, struct vnode *vp, int *fd);

/* Keeps a VM alive while a derived vCPU fd exists. */
void kvm_vm_reference(struct kvm_vm *vm);

/* Releases one VM fd or vCPU fd ownership reference. */
void kvm_vm_release(struct kvm_vm *vm);

#endif /* KVM_VM_H */
