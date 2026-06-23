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

#ifdef _KERNEL
#include <sys/lock.h>
#endif

#include "vmm_vcpu.h"
#include "vmm_mem.h"
#include "vmm_loader.h"
#include "vmm_console.h"

#define VMM_EVENT_CAP	32

#ifdef _KERNEL
struct ucred;
struct vmm_host;

struct vmm_machine_owner_ops {
	void	(*hold)(void *arg);
	void	(*release)(void *arg);
};
#endif

struct vmm_machine {
	struct vmm_vcpu		vcpu;
	struct vmm_mem		mem;
	struct vmm_loader	loader;
	struct vmm_console	console;
	/* lifecycle / lease / events -- the machine's own state */
	int		desired_stopped;
	int		running;
	int		starting;
	int		start_cancel;
#ifdef _KERNEL
	struct lock	lifecycle_lock;
	struct ucred	*start_cred;
	struct vmm_host	*host;
	const struct vmm_machine_owner_ops *owner_ops;
	void		*owner_arg;
#endif
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
#ifdef _KERNEL
void	vmm_machine_uninit(struct vmm_machine *m);
void	vmm_machine_set_owner(struct vmm_machine *m,
	    const struct vmm_machine_owner_ops *ops, void *arg);
#endif
/* All three of vcpu/mem/loader are set. */
int	vmm_machine_config_complete(const struct vmm_machine *m);

/*
 * Lifecycle.  stopped is declarative: it means "desired stopped", which is
 * what vmmfs presents as the stopped control file.  starting/running are
 * current execution state, driven by an asynchronous worker.
 */
int	vmm_machine_is_stopped(const struct vmm_machine *m);
int	vmm_machine_is_running(const struct vmm_machine *m);
int	vmm_machine_starting(const struct vmm_machine *m);
int	vmm_machine_start_cancelled(const struct vmm_machine *m);
int	vmm_machine_request_start(struct vmm_machine *m);
int	vmm_machine_start_worker_begin(struct vmm_machine *m);
void	vmm_machine_start_worker_done(struct vmm_machine *m, int started);
void	vmm_machine_stop(struct vmm_machine *m, int force);
void	vmm_machine_start(struct vmm_machine *m);
#ifdef _KERNEL
int	vmm_machine_request_running(struct vmm_machine *m, struct ucred *cred,
	    struct vmm_host *host);
void	vmm_machine_request_stopped(struct vmm_machine *m, int force);
int	vmm_machine_vcpu_should_stop(struct vmm_machine *m);
void	vmm_machine_vcpu_exited(struct vmm_machine *m);
#endif

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
