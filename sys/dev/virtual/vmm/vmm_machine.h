/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine private declarations.  Public callers include vmm.h.
 */
#ifndef VMM_MACHINE_H
#define VMM_MACHINE_H

#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/thread.h>

#include "vmm.h"

struct vmm_backend_ops;
struct vmspace;

struct vmm_memory_mapping {
	TAILQ_ENTRY(vmm_memory_mapping) entry;
	/* The mapping owns one vm_object reference. */
	struct vm_object *object;
	uint64_t offset;
	uint64_t gpa_base;
	uint64_t gpa_size;
};

TAILQ_HEAD(vmm_memory_mapping_list, vmm_memory_mapping);

struct vmm_machine {
	/* token protects vmspace, memory, memory_generation, vcpu_count, and run_count. */
	struct lwkt_token token;
	const struct vmm_backend_ops *backend;
	/* backend_data is fixed between backend machine create and destroy. */
	void *backend_data;
	struct vmspace *vmspace;
	struct vmm_memory_mapping_list memory;
	uint64_t memory_generation;
	unsigned int vcpu_count;
	unsigned int run_count;
};

MALLOC_DECLARE(M_VMM);

#endif /* VMM_MACHINE_H */
