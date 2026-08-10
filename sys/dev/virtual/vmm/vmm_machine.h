/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine private declarations.  Public callers include vmm.h.
 */
#ifndef VMM_MACHINE_H
#define VMM_MACHINE_H

#include <sys/malloc.h>
#include <sys/thread.h>

#include "vmm.h"

struct vmm_backend_ops;
struct vmspace;

struct vmm_machine {
	/* token protects irqchip, vcpu_count, next_vcpu_id, and run_count. */
	struct lwkt_token token;
	const struct vmm_backend_ops *backend;
	/* Borrowed from the caller and fixed for this machine's lifetime. */
	struct vmspace *vmspace;
	/* Private storage owned by the selected backend. */
	void *backend_state;
	bool irqchip;
	unsigned int vcpu_count;
	unsigned int next_vcpu_id;
	unsigned int run_count;
};

MALLOC_DECLARE(M_VMM);

#endif /* VMM_MACHINE_H */
