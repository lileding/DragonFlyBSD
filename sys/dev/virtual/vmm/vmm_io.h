/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core I/O trap registrations -- see vmm.h.
 */
#ifndef VMM_IO_H
#define VMM_IO_H

#include <sys/queue.h>

#include "vmm_vcpu.h"

enum vmm_io_space {
	VMM_IO_PIO,
	VMM_IO_MMIO,
};

struct vmm_io {
	TAILQ_ENTRY(vmm_io) entry;
	struct vmm_machine *machine;
	vmm_io_handler_t handler;
	void *argument;
	uint64_t address;
	enum vmm_io_width width;
	enum vmm_io_space space;
};

int vmm_io_handle_pio(struct vmm_vcpu *, const struct vmm_cpuexit *);
int vmm_io_handle_mmio(struct vmm_vcpu *, const struct vmm_cpuexit *);

#endif /* VMM_IO_H */
