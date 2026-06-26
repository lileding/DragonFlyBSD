/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core: global process-exit observation domain.
 */
#ifndef VMM_DOMAIN_H
#define VMM_DOMAIN_H

#include <sys/types.h>
#include <sys/tree.h>

struct thread;

struct vmm_domain_proc_handler {
	RB_ENTRY(vmm_domain_proc_handler) rb_entry;
	pid_t		imm_pid;

	/*
	 * Called from the global at_exit hook while the domain spinlock is
	 * held.  It must only perform atomic/simple state publication.  It
	 * must not sleep, allocate/free memory, or take blocking locks.
	 */
	void		(*fnonce_exit_cb)(void *arg, int exit_code);
	void		*borrow_mut_arg;

	/*
	 * Optional wait channel used after fnonce_exit_cb publishes state.
	 * vmm_domain only passes it to wakeup(); it never dereferences or owns
	 * this pointer.
	 */
	void		*optional_borrow_wait_chan;
};

int	vmm_domain_init(void);
void	vmm_domain_uninit(void);
int	vmm_domain_proc_register(struct vmm_domain_proc_handler *handler);
void	vmm_domain_proc_unregister(struct vmm_domain_proc_handler *handler);

#endif /* VMM_DOMAIN_H */
