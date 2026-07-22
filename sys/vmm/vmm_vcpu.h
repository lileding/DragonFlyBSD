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

struct vmm_vcpu {
	/*
	 * Lifecycle:
	 * vmm_machine's serialized command queue calls vmm_vcpu_start() and
	 * vmm_vcpu_stop().  mut_count and own_mut_threads are only accessed by
	 * that command queue.  Active vCPU threads decrement
	 * atomic_mut_active_count and wake the parent machine when they exit;
	 * the stop worker reads that count and sets atomic_mut_stop_requested.
	 * Backend teardown follows the memory backing pattern: detach
	 * own_mut_threads after all vCPUs stop, then destroy backend state and
	 * free the array outside the stop path.
	 */
	uint32_t	mut_count;		/* 0 = unset */
	struct vmm_vcpu_thread *own_mut_threads;
	u_int		atomic_mut_active_count;
	u_int		atomic_mut_stop_requested;
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
	int (*available)(void);
	int (*create)(struct vmm_machine *m, const struct vmm_launch *launch,
	    void **backendp);
	void (*destroy)(void *backend);
	void (*run)(void *backend, struct vmm_vcpu_thread *vc);
	void (*console_input)(void *backend, struct vmm_vcpu_thread *vc);
};

#define VMM_VCPU_BACKEND_SET(ops)	DATA_SET(vmm_vcpu_backend_set, ops)

/* Parse the whole buffer; update iff valid (1..256).  1 = updated, 0 = reject. */
int	vmm_vcpu_parse(struct vmm_vcpu *v, const char *buf, size_t len);
/* Serialize the value as decimal + newline; bytes written (0 if unset). */
size_t	vmm_vcpu_format(const struct vmm_vcpu *v, char *out, size_t cap);
int	vmm_vcpu_is_set(const struct vmm_vcpu *v);

int	vmm_vcpu_start(struct vmm_machine *m, uint32_t count,
	    const struct vmm_launch *launch);
void	vmm_vcpu_stop(struct vmm_machine *m);
int	vmm_vcpu_has_active(struct vmm_vcpu *v);
void	vmm_vcpu_uninit(struct vmm_vcpu *v,
	    struct vmm_vcpu_thread **threadsp);
void	vmm_vcpu_release_threads(struct vmm_vcpu_thread *threads,
	    uint32_t count);
int	vmm_vcpu_should_stop(const struct vmm_vcpu_thread *vc);
void	vmm_vcpu_console_input_locked(struct vmm_machine *m);

#endif /* VMM_VCPU_H */
