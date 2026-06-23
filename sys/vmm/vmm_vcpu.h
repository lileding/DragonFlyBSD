/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The vcpu config object: the desired CPU count.  Pure value + logic, no
 * kernel/VFS deps -- part of the reusable vmm_ core (vmm.ko and a future
 * kvm.ko).  Its filesystem presentation lives in vmmfs_vcpu.c.
 *
 * Types (uint32_t/size_t) come from the includer.
 */
#ifndef VMM_VCPU_H
#define VMM_VCPU_H

struct vmm_vcpu {
	uint32_t	count;		/* 0 = unset */
#ifdef _KERNEL
	struct vmm_vcpu_thread *threads;
	uint32_t	active_count;
	int		stop_requested;
#endif
};

#ifdef _KERNEL
struct thread;
struct vmm_host;
struct vmm_machine;

struct vmm_vcpu_thread {
	struct vmm_machine	*machine;
	struct thread		*thread;
	uint32_t		 id;
	int			 cpu;
};
#endif

/* Parse the whole buffer; update iff valid (1..256).  1 = updated, 0 = reject. */
int	vmm_vcpu_parse(struct vmm_vcpu *v, const char *buf, size_t len);
/* Serialize the value as decimal + newline; bytes written (0 if unset). */
size_t	vmm_vcpu_format(const struct vmm_vcpu *v, char *out, size_t cap);
int	vmm_vcpu_is_set(const struct vmm_vcpu *v);

#ifdef _KERNEL
int	vmm_vcpu_start_all(struct vmm_machine *m, struct vmm_host *host);
void	vmm_vcpu_request_stop(struct vmm_vcpu *v);
void	vmm_vcpu_request_run(struct vmm_vcpu *v);
int	vmm_vcpu_has_active(const struct vmm_vcpu *v);
int	vmm_vcpu_note_exit(struct vmm_vcpu *v);
void	vmm_vcpu_uninit(struct vmm_vcpu *v);
#endif

#endif /* VMM_VCPU_H */
