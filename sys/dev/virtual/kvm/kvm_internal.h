/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private shared declarations for the DragonFly KVM frontend.
 */
#ifndef KVM_INTERNAL_H
#define KVM_INTERNAL_H

#include <sys/malloc.h>
#include <sys/thread.h>

MALLOC_DECLARE(M_KVM);

/* token protects file_count and draining. */
extern struct lwkt_token kvm_frontend_token;
extern int kvm_file_count;
extern bool kvm_draining;

#endif /* KVM_INTERNAL_H */
