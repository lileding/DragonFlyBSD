/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Anonymous event counter fd used by the DragonFly KVM frontend.
 */
#ifndef KVM_EVENTFD_H
#define KVM_EVENTFD_H

#include <sys/queue.h>
#include <sys/types.h>

struct file;
struct lwp;
struct thread;
struct vnode;
struct kvm_eventfd;

/*
 * A listener is notified after an eventfd write has released the eventfd
 * token.  The owner must unregister and wait for references before freeing
 * the listener.
 */
struct kvm_eventfd_listener {
	TAILQ_ENTRY(kvm_eventfd_listener) entry;
	void (*callback)(void *);
	void *argument;
	struct kvm_eventfd *eventfd;
	uint64_t last_generation;
	volatile int references;
	int active;
};

int kvm_eventfd_create(struct lwp *lp, struct vnode *vp, uint64_t initial,
	uint32_t flags, int *fd);
int kvm_eventfd_hold(struct thread *td, int fd, struct file **fpp);
void kvm_eventfd_drop(struct file *fp);
int kvm_eventfd_signal(struct file *fp);
int kvm_eventfd_listen(struct file *fp, struct kvm_eventfd_listener *listener);
int kvm_eventfd_unlisten(struct file *fp,
	struct kvm_eventfd_listener *listener);

#endif /* KVM_EVENTFD_H */
