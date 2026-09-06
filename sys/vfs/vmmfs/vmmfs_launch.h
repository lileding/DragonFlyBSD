/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One guest-memory handle shared by boot and loader fd 3.
 */
#ifndef VMMFS_LAUNCH_H
#define VMMFS_LAUNCH_H

#include "vmmfs_node.h"
#include <dev/virtual/vmm/vmm.h>

struct file;
struct vmmfs_mount;
struct vm_object;
struct ucred;

struct vmmfs_launch {
	struct vmmfs_node node;
	struct cdev *dev;
	struct vm_object *pager_object;
	struct vm_object *backing_object;
	struct vmm_cpustate cpustate;
	/* EINPROGRESS until the single submission or abort has completed. */
	int result;
};

extern struct vop_ops vmmfs_launch_vops;

/* Returns one vnode reference; the caller either publishes it or releases it. */
int vmmfs_launch_create(struct vmmfs_node *,
	uint64_t, struct vnode **);
int vmmfs_launch_map(struct vmmfs_launch *, struct vm_object *);
/* Each file owns its vnode reference.  Final close aborts only its launch. */
int vmmfs_launch_open(struct vnode *, struct ucred *, struct file **);
void vmmfs_launch_revoke(struct vmmfs_launch *);
void vmmfs_launch_complete(struct vmmfs_launch *, int);
/* No deadline.  An interrupt aborts this launch before returning the signal error. */
int vmmfs_launch_wait(struct vmmfs_launch *);

#endif /* VMMFS_LAUNCH_H */
