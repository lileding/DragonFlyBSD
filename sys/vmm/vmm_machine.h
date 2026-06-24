/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core: the machine model.  It composes the config value objects
 * (vcpu/mem/loader) and owns the lifecycle / lease / event state.  It is
 * kernel code and can be reused by a future kvm.ko.
 *
 * Types (uint32_t/uint8_t/size_t) come from the includer.
 */
#ifndef VMM_MACHINE_H
#define VMM_MACHINE_H

#include <sys/thread.h>

#include "vmm_vcpu.h"
#include "vmm_mem.h"
#include "vmm_loader.h"
#include "vmm_console.h"

#define VMM_EVENT_CAP	32

struct ucred;
struct vmm_host;

struct vmm_machine_owner_ops {
	void	(*hold)(void *arg);
	void	(*release)(void *arg);
};

struct vmm_machine {
	struct vmm_vcpu		own_mut_vcpu;
	struct vmm_mem		own_mut_mem;
	struct vmm_loader	own_mut_loader;
	struct vmm_console	own_mut_console;

	/*
	 * Lock map:
	 * token_lifecycle protects all mut_ fields below and the mutable
	 * fields of own_mut_vcpu/own_mut_mem/own_mut_loader while they are
	 * reached through vmm_machine_* APIs.  It is not held across fork,
	 * exec, memory preparation/release, vCPU backend teardown, vCPU thread
	 * creation, or uiomove.
	 */
	struct lwkt_token	token_lifecycle;
	int		mut_desired_stopped;
	int		mut_running;
	int		mut_starting;
	int		mut_start_cancel;
	struct ucred	*ref_mut_start_cred;
	struct vmm_host	*borrow_mut_host;
	const struct vmm_machine_owner_ops *borrow_imm_owner_ops;
	void		*borrow_imm_owner_arg;
	uint32_t	mut_lease_count;
	int		mut_lease_armed;
	int		mut_deleting;
	uint8_t		mut_ev_codes[VMM_EVENT_CAP];	/* lossy ring */
	size_t		mut_ev_tail;
	size_t		mut_ev_count;
};

/* Result of lease_close. */
enum vmm_close_action {
	VMM_CLOSE_NONE,
	VMM_CLOSE_DELETE,
};

/* Initialize in place (mkdir): stopped, no config, created+stopped queued. */
void	vmm_machine_init(struct vmm_machine *m);
void	vmm_machine_uninit(struct vmm_machine *m);
void	vmm_machine_set_owner(struct vmm_machine *m,
	    const struct vmm_machine_owner_ops *ops, void *arg);
void	vmm_machine_lock(struct vmm_machine *m);
void	vmm_machine_unlock(struct vmm_machine *m);
int	vmm_machine_start_cancelled_locked(const struct vmm_machine *m);
/* All three of vcpu/mem/loader are set. */
int	vmm_machine_config_complete(const struct vmm_machine *m);
size_t	vmm_machine_format_vcpu(const struct vmm_machine *m, char *out,
	    size_t cap);
int	vmm_machine_commit_vcpu(struct vmm_machine *m, const char *buf,
	    size_t len);
size_t	vmm_machine_format_mem(const struct vmm_machine *m, char *out,
	    size_t cap);
int	vmm_machine_commit_mem(struct vmm_machine *m, const char *buf,
	    size_t len);
size_t	vmm_machine_format_loader(const struct vmm_machine *m, char *out,
	    size_t cap);
int	vmm_machine_commit_loader(struct vmm_machine *m, const char *buf,
	    size_t len);

/*
 * Lifecycle.  stopped is declarative: it means "desired stopped", which is
 * what vmmfs presents as the stopped control file.  starting/running are
 * current execution state, driven by an asynchronous worker.
 */
int	vmm_machine_is_stopped(const struct vmm_machine *m);
int	vmm_machine_is_running(const struct vmm_machine *m);
int	vmm_machine_starting(const struct vmm_machine *m);
int	vmm_machine_request_running(struct vmm_machine *m, struct ucred *cred,
	    struct vmm_host *host);
void	vmm_machine_request_stopped(struct vmm_machine *m, int force);
int	vmm_machine_vcpu_should_stop(struct vmm_machine *m);
void	vmm_machine_vcpu_exited(struct vmm_machine *m);

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
