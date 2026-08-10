/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Anonymous event counter fd used by the DragonFly KVM frontend.
 */
#ifndef KVM_EVENTFD_H
#define KVM_EVENTFD_H

#include <sys/types.h>

struct lwp;

int kvm_eventfd_create(struct lwp *lp, uint64_t initial, uint32_t flags,
	int *fd);

#endif /* KVM_EVENTFD_H */
