/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs memory declaration object.
 */
#ifndef VMMFS_MEMORY_H
#define VMMFS_MEMORY_H

#include <sys/types.h>

struct vm_object;
struct vmspace;
struct vmmfs_machine;

struct vmmfs_memory_spec {
	uint64_t size;
};

struct vmmfs_memory {
	struct vmmfs_machine *machine;
	struct vmmfs_memory_spec spec;
	struct vm_object *object;
	struct vmspace *boot_vmspace;
	struct vmspace *run_vmspace;
};

#endif /* VMMFS_MEMORY_H */
