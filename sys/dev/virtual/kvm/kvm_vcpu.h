/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-vCPU file descriptor for the DragonFly KVM frontend.
 */
#ifndef KVM_VCPU_H
#define KVM_VCPU_H

#include <sys/types.h>

struct kvm_vm;
struct lwp;
struct vnode;
struct dev_mmap_single_args;

/* Creates one vCPU fd attached to an open KVM VM. */
int kvm_vcpu_create(struct kvm_vm *vm, struct lwp *lp, struct vnode *vp,
	uint32_t id, int *fd);

/* Maps the KVM_RUN shared pages for a vCPU descriptor. */
int kvm_vcpu_mmap_single(struct dev_mmap_single_args *ap);

/* Copies the MSR indices implemented by KVM vCPU state handling. */
int kvm_vcpu_get_supported_msrs(uint32_t *indices, size_t *count);

#endif /* KVM_VCPU_H */
