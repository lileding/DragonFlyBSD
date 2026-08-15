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

enum vmm_io_direction {
	VMM_IO_READ,
	VMM_IO_WRITE,
};

union vmm_io_handler {
	vmm_io_read_handler_t read;
	vmm_io_write_handler_t write;
};

struct vmm_io {
	TAILQ_ENTRY(vmm_io) entry;
	struct vmm_machine *machine;
	union vmm_io_handler handler;
	void *argument;
	uint64_t base;
	uint64_t size;
	enum vmm_io_space space;
	enum vmm_io_direction direction;
};

int vmm_io_handle_pio(struct vmm_vcpu *, const struct vmm_cpuexit *);
int vmm_io_dispatch_read(struct vmm_vcpu *, enum vmm_io_space,
	struct vmm_io_read *);
int vmm_io_dispatch_write(struct vmm_vcpu *, enum vmm_io_space,
    const struct vmm_io_write *);

#endif /* VMM_IO_H */
