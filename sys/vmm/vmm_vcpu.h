/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The vcpu object: desired CPU count plus kernel execution threads.  Its
 * filesystem presentation lives in vmmfs_vcpu.c.
 *
 * Types (uint32_t/size_t) come from the includer.
 */
#ifndef VMM_VCPU_H
#define VMM_VCPU_H

#include <sys/linker_set.h>

enum vmm_vcpu_exit_reason {
	VMM_VCPU_EXIT_NONE,
	VMM_VCPU_EXIT_GUEST_SHUTDOWN,
	VMM_VCPU_EXIT_GUEST_RESET,
	VMM_VCPU_EXIT_GUEST_FAULT,
};

struct vmm_vcpu {
	/*
	 * Lock map:
	 * token_config protects mut_count, own_mut_threads,
	 * borrow_imm_backend_ops, and own_mut_backend_context while a control
	 * path snapshots them or the last vCPU detaches them.  Active vCPU
	 * threads publish atomic_mut_active_count, atomic_mut_stop_requested,
	 * and atomic_mut_exit_reason with atomic operations.  The first terminal
	 * vCPU moves its machine to DRAINING; every vCPU destroys only its private
	 * backend.  After active_count reaches zero, the last vCPU detaches this
	 * shared state under token_config and releases it outside the token.
	 */
	uint32_t	mut_count;		/* 0 = unset */
	struct vmm_vcpu_thread *own_mut_threads;
	const struct vmm_vcpu_backend_ops *borrow_imm_backend_ops;
	void		*own_mut_backend_context;
	u_int		atomic_mut_active_count;
	u_int		atomic_mut_stop_requested;
	u_int		atomic_mut_exit_reason;
};

struct thread;
struct vmm_launch;
struct vmm_machine;

struct vmm_vcpu_thread {
	struct vmm_machine	*borrow_imm_machine;
	const struct vmm_vcpu_backend_ops *borrow_imm_backend_ops;
	struct thread		*borrow_mut_thread;
	void			*own_mut_backend;
	uint32_t		 imm_id;
	int			 imm_cpu;
};

struct vmm_vcpu_backend_ops {
	const char *imm_name;
	/* NULL if usable; otherwise a stable module-load rejection reason. */
	const char *(*probe)(void);
	/* Module lifetime setup/teardown; no machine or vCPU is live here. */
	int (*init)(void);
	void (*uninit)(void);
	/* A context is private to the selected backend and one machine run. */
	int (*context_create)(struct vmm_machine *m, uint32_t count,
	    const struct vmm_launch *launch, void **contextp);
	void (*context_destroy)(void *context);
	int (*vcpu_create)(void *context, const struct vmm_launch *launch,
	    const struct vmm_vcpu_thread *vc, void **backendp);
	void (*vcpu_destroy)(void *backend);
	enum vmm_vcpu_exit_reason (*run)(void *backend,
	    struct vmm_vcpu_thread *vc);
	void (*console_input)(void *backend, struct vmm_vcpu_thread *vc);
	void (*interrupt)(void *backend, struct vmm_vcpu_thread *vc,
	    uint8_t vector);
};

#define VMM_VCPU_BACKEND_SET(ops)	DATA_SET(vmm_vcpu_backend_set, ops)

/* Parse the whole buffer; update iff valid (1..256).  1 = updated, 0 = reject. */
int	vmm_vcpu_parse(struct vmm_vcpu *v, const char *buf, size_t len);
/* Serialize the value as decimal + newline; bytes written (0 if unset). */
size_t	vmm_vcpu_format(const struct vmm_vcpu *v, char *out, size_t cap);
int	vmm_vcpu_is_set(const struct vmm_vcpu *v);

int	vmm_backend_probe(void);
void	vmm_backend_uninit(void);

int	vmm_vcpu_start(struct vmm_machine *m, uint32_t count,
	    const struct vmm_launch *launch);
void	vmm_vcpu_stop(struct vmm_machine *m);
int	vmm_vcpu_has_active(struct vmm_vcpu *v);
void	vmm_vcpu_request_stop(struct vmm_machine *m);
void	vmm_vcpu_uninit(struct vmm_vcpu *v,
	    struct vmm_vcpu_thread **threadsp);
void	vmm_vcpu_release_threads(struct vmm_vcpu *v,
	    struct vmm_vcpu_thread *threads, uint32_t count);
int	vmm_vcpu_should_stop(const struct vmm_vcpu_thread *vc);
void	vmm_vcpu_console_input_locked(struct vmm_machine *m);
void	vmm_vcpu_interrupt_locked(struct vmm_machine *m, uint8_t vector);

#endif /* VMM_VCPU_H */
