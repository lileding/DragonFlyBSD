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
extern int kvm_debug_trace;

#define KVM_DEBUG_TRACE_LIMIT 64U

/* Returns the supported value for one Linux KVM capability. */
int kvm_capability(int capability);

#endif /* KVM_INTERNAL_H */
