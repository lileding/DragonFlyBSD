/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs vCPU declaration object.
 */
#ifndef VMMFS_VCPU_H
#define VMMFS_VCPU_H

#include <sys/types.h>


struct vmmfs_machine;

struct vmmfs_vcpu_spec {
	uint32_t count;
};

struct vmmfs_vcpu {
	struct vmmfs_machine *machine;
	struct vmmfs_vcpu_spec spec;
	struct vmm_vcpu **vcpus;
	uint32_t running_count;
};

#endif /* VMMFS_VCPU_H */
