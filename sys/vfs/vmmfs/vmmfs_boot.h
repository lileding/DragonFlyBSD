/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs direct boot session node.
 */
#ifndef VMMFS_BOOT_H
#define VMMFS_BOOT_H

#include <sys/types.h>
#include "vmmfs_node.h"


struct cdev;
struct vm_object;
struct vmmfs_machine;
struct vmmfs_boot_session;
struct vnode;
struct vop_ops;

struct vmmfs_boot {
	struct vmmfs_node node;
	struct vmmfs_machine *machine;
	cdev_t dev;
	ino_t inode;
	struct vmmfs_boot_session *session;
};

extern struct vop_ops vmmfs_boot_vops;

int vmmfs_boot_module_fini(void);
int vmmfs_boot_init(struct vmmfs_machine *, struct vmmfs_boot *);
int vmmfs_boot_publish(struct vmmfs_boot *);
void vmmfs_boot_fini(struct vmmfs_boot *);
int vmmfs_boot_arm_locked(struct vmmfs_boot *, struct vm_object *, uint64_t);
int vmmfs_boot_submit(struct vmmfs_boot *, const struct vmm_cpustate *);
void vmmfs_boot_revoke(struct vmmfs_boot *);
bool vmmfs_boot_is_active(struct vmmfs_boot *);

#endif /* VMMFS_BOOT_H */
