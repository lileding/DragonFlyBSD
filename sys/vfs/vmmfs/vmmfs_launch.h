/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One reference-counted startup future shared by boot and loader fd 3.
 */
#ifndef VMMFS_LAUNCH_H
#define VMMFS_LAUNCH_H

#include <sys/thread.h>
#include <dev/virtual/vmm/vmm.h>

struct file;
struct ucred;
struct vmmfs_machine;
struct sigio;

struct vmmfs_launch {
	struct vmmfs_machine *machine;
	struct lwkt_token token;
	u_int references;
	u_int claimed;
	u_int ready;
	int result;
	void (*post_launch)(struct vmmfs_launch *);
	struct vnode *vnode; /* Private cdev carrier, not a VMMFS node. */
	struct cdev *dev;
	struct vm_object *pager_object;
	struct vm_object *backing_object;
	struct sigio *loader_signal;
	uint64_t size;
	ino_t inode;
	struct vmm_cpustate cpustate;
};

extern struct vop_ops vmmfs_launch_vops;

/* Returns one caller reference. post_launch runs once, before ready and run. */
int vmmfs_launch_create(struct vmmfs_machine *,
	void (*)(struct vmmfs_launch *), struct vmmfs_launch **);
void vmmfs_launch_hold(struct vmmfs_launch *);
void vmmfs_launch_put(struct vmmfs_launch *);
/* Each file owns a launch reference. Last close competes with submit. */
int vmmfs_launch_open(struct vmmfs_launch *, struct ucred *, struct file **);
void vmmfs_launch_cancel(struct vmmfs_launch *);
/* No deadline. Signals request cancellation, then await the winning result. */
int vmmfs_launch_wait(struct vmmfs_launch *);

#endif /* VMMFS_LAUNCH_H */
