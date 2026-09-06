/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs direct boot session node.
 */
#ifndef VMMFS_BOOT_H
#define VMMFS_BOOT_H

#include <sys/types.h>
#include "vmmfs_node.h"


struct vmmfs_mount;
struct cdev;
struct vm_object;
struct vmm_cpustate;
struct vmmfs_machine;
struct vnode;
struct vop_ops;

struct vmmfs_boot {
	struct vmmfs_node node;
	cdev_t dev;
};

int vmmfs_boot_init(struct vmmfs_mount *, struct vmmfs_node *,
	struct vmmfs_boot *, struct vnode **);
#endif /* VMMFS_BOOT_H */
