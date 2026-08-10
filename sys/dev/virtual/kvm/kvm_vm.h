/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-VM file descriptor for the DragonFly KVM frontend.
 */
#ifndef KVM_VM_H
#define KVM_VM_H

#include <sys/types.h>

struct lwp;

/* Creates one KVM VM fd with an independent guest vmspace. */
int kvm_vm_create(struct lwp *lp, int *fd);

#endif /* KVM_VM_H */
