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

#include "vmm_console.h"
#include "vmm_loader.h"
#include "vmm_mem.h"
#include "vmm_vcpu.h"

#define VMM_EVENT_LOG_SIZE (64 * 1024)

struct ucred;
struct taskqueue;
struct vmm_launch;
struct vmm_machine_task;

typedef void (*vmm_machine_func)(const struct vmm_machine_task *task);

enum vmm_machine_status {
	VMM_MACHINE_STARTING,
	VMM_MACHINE_RUNNING,
	VMM_MACHINE_STOPPING,
	VMM_MACHINE_STOPPED,
};

struct vmm_machine {
	/* token_config protects control-plane config and mut_status. */
	/* token_events protects the retained textual event ring. */
	struct lwkt_token token_config;
	struct lwkt_token token_events;
	struct vmm_vcpu own_mut_vcpu;
	struct vmm_mem own_mut_mem;
	struct vmm_console own_mut_console;
	struct taskqueue *own_mut_taskqueue;
	/* Owned only by the serialized taskqueue after loader acceptance. */
	struct vmm_launch *own_mut_boot_launch;
	enum vmm_machine_status mut_status;

	int mut_desired_stopped;
	char mut_loader_path[VMM_LOADER_MAX + 1];
	size_t mut_loader_len;
	uint32_t mut_lease_count;
	int mut_lease_armed;
	char *own_mut_events_buf;
	size_t imm_events_cap;
	size_t mut_events_start;
	size_t mut_events_len;
	uint64_t mut_events_seq;
	uint64_t mut_events_drop_bytes;
};

/* Result of lease_close. */
enum vmm_close_action {
	VMM_CLOSE_NONE,
	VMM_CLOSE_DELETE,
};

/* Initialize in place (mkdir): stopped, no config, created+stopped queued. */
void vmm_machine_init(struct vmm_machine *m);
void vmm_machine_uninit(struct vmm_machine *m);
void vmm_machine_drain(struct vmm_machine *m);
void vmm_debug_trace(const char *fmt, ...);
extern int vmm_debug_allow_machine_taskqueue;
extern int vmm_debug_allow_nmkdir_vnode;
extern int vmm_debug_allow_start_execute;
extern int vmm_debug_allow_machine_task_run;
extern int vmm_debug_allow_loader_fork;
extern int vmm_debug_allow_loader_run;
extern int vmm_debug_allow_vcpu_start;
size_t vmm_machine_format_vcpu(const struct vmm_machine *m, char *out,
    size_t cap);
int vmm_machine_commit_vcpu(struct vmm_machine *m, const char *buf, size_t len);
size_t vmm_machine_format_mem(const struct vmm_machine *m, char *out,
    size_t cap);
int vmm_machine_commit_mem(struct vmm_machine *m, const char *buf, size_t len);
size_t vmm_machine_format_loader(const struct vmm_machine *m, char *out,
    size_t cap);
int vmm_machine_commit_loader(struct vmm_machine *m, const char *buf,
    size_t len);

/*
 * Lifecycle.  stopped is declarative: it means "desired stopped", which is
 * what vmmfs presents as the stopped control file.  vmmfs translates file
 * operations into ordered command handlers; vmm_machine_execute() snapshots
 * config and any syscall-context-only state (currently the paused loader
 * process) and queues a serialized command.  starting/running are current
 * execution state, not proof that a just-returned vmmfs operation already
 * completed.
 */
int vmm_machine_execute(struct vmm_machine *m, vmm_machine_func fnonce_handler,
    struct ucred *cred);
void vmm_machine_console_input(struct vmm_machine *m);
/* Called by the last vCPU thread after an explicit terminal guest exit. */
void vmm_machine_vcpu_exited(struct vmm_machine *m);
void vmm_machine_start(const struct vmm_machine_task *task);
void vmm_machine_stop_apic(const struct vmm_machine_task *task);
void vmm_machine_stop_force(const struct vmm_machine_task *task);
void vmm_machine_reset_apic(const struct vmm_machine_task *task);
void vmm_machine_reset_force(const struct vmm_machine_task *task);

/* Lease reference counting. */
int vmm_machine_lease_open(struct vmm_machine *m);
enum vmm_close_action vmm_machine_lease_close(struct vmm_machine *m);

/* Events: retained per-machine textual log, one line per record. */
int vmm_machine_events_pending(const struct vmm_machine *m);
size_t vmm_machine_read_events(struct vmm_machine *m, off_t off, char *out,
    size_t cap);
void vmm_machine_logf(struct vmm_machine *m, const char *fmt, ...);

#endif /* VMM_MACHINE_H */
