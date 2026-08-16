/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs vCPU declaration object.
 */
#ifndef VMMFS_VCPU_H
#define VMMFS_VCPU_H

#include <sys/types.h>


struct vmmfs_machine;
struct vnode;
struct vop_ops;

struct vmmfs_vcpu_spec {
	uint32_t count;
};

struct vmmfs_vcpu {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
	struct vmm_vcpu **vcpus;
	uint32_t active_count;
};

extern struct vop_ops vmmfs_vcpu_vops;

int vmmfs_vcpu_create(struct vmmfs_machine *, struct vmmfs_vcpu *);
int vmmfs_vcpu_destroy(struct vmmfs_vcpu *);

#endif /* VMMFS_VCPU_H */
