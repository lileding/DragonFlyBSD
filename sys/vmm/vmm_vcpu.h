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

struct vmm_vcpu {
	/*
	 * Lock map:
	 * All mut_ fields are protected by the parent vmm_machine's
	 * token_lifecycle.  vmm_vcpu helpers that inspect or mutate them are
	 * called with that token held, except vmm_vcpu_start_all(), which
	 * acquires the parent token around each short state transition.
	 * Backend teardown follows the memory backing pattern: detach
	 * own_mut_threads with the token held, then destroy backend state and
	 * free the array after dropping the token.
	 */
	uint32_t	mut_count;		/* 0 = unset */
	struct vmm_vcpu_thread *own_mut_threads;
	uint32_t	mut_active_count;
	int		mut_stop_requested;
};

struct thread;
struct vmm_host;
struct vmm_launch;
struct vmm_machine;

struct vmm_vcpu_thread {
	struct vmm_machine	*borrow_imm_machine;
	struct thread		*borrow_mut_thread;
	void			*own_mut_backend;
	uint32_t		 imm_id;
	int			 imm_cpu;
};

/* Parse the whole buffer; update iff valid (1..256).  1 = updated, 0 = reject. */
int	vmm_vcpu_parse(struct vmm_vcpu *v, const char *buf, size_t len);
/* Serialize the value as decimal + newline; bytes written (0 if unset). */
size_t	vmm_vcpu_format(const struct vmm_vcpu *v, char *out, size_t cap);
int	vmm_vcpu_is_set(const struct vmm_vcpu *v);

int	vmm_vcpu_start_all(struct vmm_machine *m, struct vmm_host *host,
	    const struct vmm_launch *launch);
void	vmm_vcpu_request_stop(struct vmm_vcpu *v);
void	vmm_vcpu_request_run(struct vmm_vcpu *v);
int	vmm_vcpu_has_active(const struct vmm_vcpu *v);
int	vmm_vcpu_note_exit(struct vmm_vcpu *v,
	    struct vmm_vcpu_thread **threadsp);
void	vmm_vcpu_uninit(struct vmm_vcpu *v,
	    struct vmm_vcpu_thread **threadsp);
void	vmm_vcpu_release_threads(struct vmm_vcpu_thread *threads,
	    uint32_t count);

#endif /* VMM_VCPU_H */
