/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The vcpu object: one vCPU owns one fixed-pCPU LWKT and one backend state.
 * A machine owns a vcpus collection for configuration and shared backend
 * context lifetime.
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

struct thread;
struct vmm_launch;
struct vmm_machine;
struct vmm_vcpu_backend_ops;

struct vmm_vcpu {
	/* This object is initialized before its LWKT is created and then immutable. */
	struct vmm_machine	*borrow_imm_machine;
	const struct vmm_vcpu_backend_ops *borrow_imm_backend_ops;
	struct thread		*borrow_mut_thread;
	void			*own_mut_backend;
	uint32_t		 imm_id;
	int			 imm_cpu;
	u_int			 atomic_mut_start_reported;
};

struct vmm_vcpus {
	/*
	 * Lock map:
	 * token_config protects mut_count, own_mut_vcpus,
	 * borrow_imm_backend_ops and own_mut_backend_context while machine
	 * lifecycle code snapshots or detaches the collection.  vCPU LWKTs use
	 * the atomic fields for startup barriers and drain accounting.
	 */
	uint32_t	mut_count;
	struct vmm_vcpu	*own_mut_vcpus;
	const struct vmm_vcpu_backend_ops *borrow_imm_backend_ops;
	void		*own_mut_backend_context;
	const struct vmm_launch *borrow_imm_launch;
	u_int		atomic_mut_active_count;
	u_int		atomic_mut_stop_requested;
	u_int		atomic_mut_exit_reason;
	u_int		atomic_mut_backend_ready_count;
	u_int		atomic_mut_start_failed;
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
	    const struct vmm_vcpu *vc, void **backendp);
	void (*vcpu_destroy)(void *backend);
	enum vmm_vcpu_exit_reason (*run)(void *backend,
	    struct vmm_vcpu *vc);
	void (*console_input)(void *backend, struct vmm_vcpu *vc);
	void (*interrupt)(void *backend, struct vmm_vcpu *vc,
	    uint8_t destination, uint8_t vector);
};

#define VMM_VCPU_BACKEND_SET(ops)	DATA_SET(vmm_vcpu_backend_set, ops)

/* Parse the whole buffer; update iff valid (1..256). */
int	vmm_vcpus_parse(struct vmm_vcpus *vcpus, const char *buf, size_t len);
size_t	vmm_vcpus_format(const struct vmm_vcpus *vcpus, char *out, size_t cap);
int	vmm_vcpus_is_set(const struct vmm_vcpus *vcpus);

int	vmm_backend_probe(void);
void	vmm_backend_uninit(void);

int	vmm_vcpus_create(struct vmm_machine *m, uint32_t count,
	    const struct vmm_launch *launch);
int	vmm_vcpu_start(struct vmm_vcpu *vc);
void	vmm_vcpus_stop(struct vmm_machine *m);
void	vmm_vcpus_request_stop(struct vmm_machine *m);
void	vmm_vcpus_uninit(struct vmm_vcpus *vcpus,
	    struct vmm_vcpu **vcpusp);
void	vmm_vcpus_release(struct vmm_vcpus *vcpus,
	    struct vmm_vcpu *vcpus_array, uint32_t count);
int	vmm_vcpu_should_stop(const struct vmm_vcpu *vc);
void	vmm_vcpu_report_started(struct vmm_vcpu *vc);
void	vmm_vcpus_console_input_locked(struct vmm_machine *m);
void	vmm_vcpus_interrupt_locked(struct vmm_machine *m,
	    uint8_t destination, uint8_t vector);

#endif /* VMM_VCPU_H */
