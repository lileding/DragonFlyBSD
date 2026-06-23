/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core: the machine model.  It COMPOSES the config value objects
 * (vcpu/mem/loader) and owns the lifecycle / lease / event state.  Pure -- no
 * kernel/VFS deps -- so it builds into the kernel module AND host unit tests,
 * and is reusable by a future kvm.ko.  The caller owns storage + locking.
 *
 * Types (uint32_t/uint8_t/size_t) come from the includer.
 */
#ifndef VMM_MACHINE_H
#define VMM_MACHINE_H

#include "vmm_vcpu.h"
#include "vmm_mem.h"
#include "vmm_loader.h"

#define VMM_EVENT_CAP	32

struct vmm_machine {
	struct vmm_vcpu		vcpu;
	struct vmm_mem		mem;
	struct vmm_loader	loader;
	/* lifecycle / lease / events -- the machine's own state */
	int		stopped;
	uint32_t	lease_count;
	int		armed;
	int		deleting;
	uint8_t		ev_codes[VMM_EVENT_CAP];	/* lossy ring of event codes */
	size_t		ev_tail;
	size_t		ev_count;
};

/* Result of lease_close. */
enum vmm_close_action {
	VMM_CLOSE_NONE,
	VMM_CLOSE_DELETE,
};

/* Initialize in place (mkdir): stopped, no config, created+stopped queued. */
void	vmm_machine_init(struct vmm_machine *m);
/* All three of vcpu/mem/loader are set. */
int	vmm_machine_config_complete(const struct vmm_machine *m);

/* Lifecycle. */
int	vmm_machine_is_stopped(const struct vmm_machine *m);
void	vmm_machine_stop(struct vmm_machine *m, int force);
void	vmm_machine_start(struct vmm_machine *m);

/* Lease reference counting. */
int	vmm_machine_is_deleting(const struct vmm_machine *m);
int	vmm_machine_lease_open(struct vmm_machine *m);
enum vmm_close_action vmm_machine_lease_close(struct vmm_machine *m);
int	vmm_machine_begin_delete(struct vmm_machine *m);

/* Events: read drains queued event text lines (shared one-shot cursor). */
int	vmm_machine_events_pending(const struct vmm_machine *m);
size_t	vmm_machine_read_events(struct vmm_machine *m, char *out,
	    size_t cap);

#endif /* VMM_MACHINE_H */
