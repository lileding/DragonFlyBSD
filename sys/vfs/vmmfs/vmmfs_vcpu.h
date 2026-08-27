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
struct vmmfs_pcislot_config_request;
struct vnode;
struct vop_ops;

struct vmmfs_vcpu_thread {
	struct vmmfs_vcpu *group;
	vmm_vcpu_t vcpu;
	struct vmm_cpustate state;
	uint32_t index;
	bool halted_logged;
	struct vmmfs_pcislot_config_request *config_request;
	bool config_done;
};

struct vmmfs_vcpu {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
	struct vmmfs_vcpu_thread *threads;
	vmm_machine_t runtime_machine;
	/* Protects requests and the vCPU thread barriers. */
	struct lwkt_token token;
	uint32_t count;
	unsigned int active_count;
	unsigned int reset_waiting;
	bool start_ready;
	bool start_failed;
	bool stop_requested;
	bool reset_requested;
};

extern struct vop_ops vmmfs_vcpu_vops;

int vmmfs_vcpu_init(struct vmmfs_machine *, struct vmmfs_vcpu *);
int vmmfs_vcpu_fini(struct vmmfs_vcpu *);
int vmmfs_vcpu_start(struct vmmfs_vcpu *, uint32_t, vmm_machine_t,
	const struct vmm_cpustate *bsp_state);
int vmmfs_vcpu_reset(struct vmmfs_vcpu *, vmm_machine_t,
	const struct vmm_cpustate *bsp_state);
void vmmfs_vcpu_request_stop(struct vmmfs_vcpu *);
void vmmfs_vcpu_request_reset(struct vmmfs_vcpu *);

#endif /* VMMFS_VCPU_H */
