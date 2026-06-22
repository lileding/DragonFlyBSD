/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core: the pure machine model -- config parsing, the desired-state
 * registers, the lifecycle state machine, lease reference counting, and the
 * event ring.  No kernel dependencies, so the same code compiles into the
 * kernel module AND into host unit tests (vmm_machine_test.c).  The caller (the
 * vmmfs control plane) owns the storage and the locking; these functions only
 * touch the state they are given.
 */
#ifndef VMM_MACHINE_H
#define VMM_MACHINE_H

/* Types (uint32_t/uint64_t/uint8_t/size_t) come from the includer:
 * <sys/types.h> in the kernel, <stdint.h>/<stddef.h> on the host. */

#define VMM_LOADER_MAX	256
#define VMM_EVENT_CAP	32

struct vmm_machine {
	int		stopped;
	uint32_t	vcpu;			/* 0 = unset */
	uint64_t	mem;			/* 0 = unset */
	char		loader[VMM_LOADER_MAX];
	size_t		loader_len;		/* 0 = unset */
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

/* Desired-config registers.  commit_* parse the whole buffer and update the
 * desired value iff valid, returning 1 if updated, 0 if rejected (old value
 * kept).  *_text serialize the current desired value, returning bytes written
 * (0 if unset). */
int	vmm_machine_commit_vcpu(struct vmm_machine *m, const char *buf,
	    size_t len);
int	vmm_machine_commit_mem(struct vmm_machine *m, const char *buf,
	    size_t len);
int	vmm_machine_commit_loader(struct vmm_machine *m, const char *buf,
	    size_t len);
size_t	vmm_machine_vcpu_text(const struct vmm_machine *m, char *out,
	    size_t cap);
size_t	vmm_machine_mem_text(const struct vmm_machine *m, char *out,
	    size_t cap);
size_t	vmm_machine_loader_text(const struct vmm_machine *m, char *out,
	    size_t cap);
/* Loader path without trailing newline (for start-time resolution). */
size_t	vmm_machine_loader_path(const struct vmm_machine *m, char *out,
	    size_t cap);
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
