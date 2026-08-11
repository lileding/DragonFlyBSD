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
struct vmm_io;
struct vmm_x64_pic;
struct vmm_x64_pit;

struct vmm_machine {
	/* token protects lifecycle state, vCPU counters, io_list, pic, and pit. */
	struct lwkt_token token;
	const struct vmm_backend_ops *backend;
	/* Borrowed from the caller and fixed for this machine's lifetime. */
	struct vmspace *vmspace;
	/* Private storage owned by the selected backend. */
	void *backend_state;
	struct vmm_x64_pic *pic;
	struct vmm_x64_pit *pit;
	bool irqchip;
	bool destroying;
	unsigned int vcpu_count;
	unsigned int next_vcpu_id;
	unsigned int run_count;
	TAILQ_HEAD(, vmm_io) io_list;
};

MALLOC_DECLARE(M_VMM);

#endif /* VMM_MACHINE_H */
