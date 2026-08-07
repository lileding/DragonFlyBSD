/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core process-exit observation domain.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/spinlock.h>
#include <sys/spinlock2.h>
#include <sys/thread.h>

#include "vmm_domain.h"

struct vmm_domain {
	struct spinlock spin;
	RB_HEAD(vmm_domain_proc_tree, vmm_domain_proc_handler) proc_handlers;
};

static struct vmm_domain vmm_global_domain;

static int	vmm_domain_proc_cmp(struct vmm_domain_proc_handler *a,
		    struct vmm_domain_proc_handler *b);
static void	vmm_domain_proc_exit(struct thread *td);

RB_PROTOTYPE_STATIC(vmm_domain_proc_tree, vmm_domain_proc_handler, rb_entry,
    vmm_domain_proc_cmp);
RB_GENERATE_STATIC(vmm_domain_proc_tree, vmm_domain_proc_handler, rb_entry,
    vmm_domain_proc_cmp);

int
vmm_domain_init(void)
{
	int error;

	spin_init(&vmm_global_domain.spin, "vmmdom");
	RB_INIT(&vmm_global_domain.proc_handlers);
	error = at_exit(vmm_domain_proc_exit);
	if (error)
		spin_uninit(&vmm_global_domain.spin);
	return error;
}

void
vmm_domain_uninit(void)
{
	rm_at_exit(vmm_domain_proc_exit);
	KKASSERT(RB_EMPTY(&vmm_global_domain.proc_handlers));
	spin_uninit(&vmm_global_domain.spin);
}

int
vmm_domain_proc_register(struct vmm_domain_proc_handler *handler)
{
	struct vmm_domain_proc_handler *old;

	if (handler == NULL || handler->imm_pid <= 0 ||
	    handler->fnonce_exit_cb == NULL)
		return EINVAL;

	spin_lock(&vmm_global_domain.spin);
	old = RB_INSERT(vmm_domain_proc_tree, &vmm_global_domain.proc_handlers,
	    handler);
	spin_unlock(&vmm_global_domain.spin);
	return old == NULL ? 0 : EEXIST;
}

void
vmm_domain_proc_unregister(struct vmm_domain_proc_handler *handler)
{
	struct vmm_domain_proc_handler *found;
	struct vmm_domain_proc_handler key;

	if (handler == NULL || handler->imm_pid <= 0)
		return;

	key.imm_pid = handler->imm_pid;
	spin_lock(&vmm_global_domain.spin);
	found = RB_FIND(vmm_domain_proc_tree, &vmm_global_domain.proc_handlers,
	    &key);
	if (found == handler)
		RB_REMOVE(vmm_domain_proc_tree, &vmm_global_domain.proc_handlers,
		    handler);
	spin_unlock(&vmm_global_domain.spin);
}

static int
vmm_domain_proc_cmp(struct vmm_domain_proc_handler *a,
    struct vmm_domain_proc_handler *b)
{
	if (a->imm_pid < b->imm_pid)
		return -1;
	if (a->imm_pid > b->imm_pid)
		return 1;
	return 0;
}

static void
vmm_domain_proc_exit(struct thread *td)
{
	struct vmm_domain_proc_handler key;
	struct vmm_domain_proc_handler *handler;
	void *optional_wait_chan = NULL;
	struct proc *p;
	pid_t pid;
	int exit_code;

	if (td == NULL || td->td_proc == NULL)
		return;
	p = td->td_proc;
	pid = p->p_pid;
	exit_code = p->p_xstat;
	key.imm_pid = pid;

	spin_lock(&vmm_global_domain.spin);
	handler = RB_FIND(vmm_domain_proc_tree,
	    &vmm_global_domain.proc_handlers, &key);
	if (handler != NULL) {
		handler->fnonce_exit_cb(handler->borrow_mut_arg, exit_code);
		optional_wait_chan = handler->optional_borrow_wait_chan;
	}
	spin_unlock(&vmm_global_domain.spin);

	if (optional_wait_chan != NULL)
		wakeup(optional_wait_chan);
}
