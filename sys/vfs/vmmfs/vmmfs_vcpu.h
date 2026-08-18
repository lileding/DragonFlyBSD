/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs vCPU declaration object.
 */
#ifndef VMMFS_VCPU_H
#define VMMFS_VCPU_H

#include <sys/thread.h>
#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct thread;
struct vnode;
struct vop_ops;

struct vmmfs_vcpu_spec {
	uint32_t count;
};

struct vmmfs_vcpu_thread {
	struct vmmfs_vcpu *group;
	struct thread *thread;
	vmm_vcpu_t vcpu;
	struct vmm_cpustate state;
	uint32_t index;
	bool halted_logged;
};

struct vmmfs_vcpu {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
	struct vmmfs_vcpu_thread *threads;
	vmm_machine_t runtime_machine;
	/* Protects stop_requested and active_count. */
	struct lwkt_token token;
	uint32_t count;
	unsigned int active_count;
	bool stop_requested;
};

extern struct vop_ops vmmfs_vcpu_vops;

int vmmfs_vcpu_create(struct vmmfs_machine *, struct vmmfs_vcpu *);
int vmmfs_vcpu_destroy(struct vmmfs_vcpu *);
int vmmfs_vcpu_start(struct vmmfs_vcpu *, vmm_machine_t,
	const struct vmm_cpustate *bsp_state);
int vmmfs_vcpu_stop(struct vmmfs_vcpu *);

#endif /* VMMFS_VCPU_H */
