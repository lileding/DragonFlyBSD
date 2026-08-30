/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs memory declaration object.
 */
#ifndef VMMFS_MEMORY_H
#define VMMFS_MEMORY_H

#include "vmmfs_node.h"

#include <sys/types.h>

#include <vm/vm.h>

struct vm_object;
struct vmspace;
struct vmmfs_machine;
struct vmmfs_memory;

extern struct vop_ops vmmfs_memory_vops;
int vmmfs_memory_init(struct vmmfs_machine *, struct vmmfs_memory *,
	struct vnode **);
int vmmfs_memory_prepare(struct vmmfs_memory *, uint64_t);
int vmmfs_memory_map(struct vmmfs_memory *);
int vmmfs_memory_snapshot(struct vmmfs_memory *);
int vmmfs_memory_reset_begin(struct vmmfs_memory *, struct vmspace **);
void vmmfs_memory_reset_abort(struct vmmfs_memory *, struct vmspace *);
void vmmfs_memory_reset_commit(struct vmmfs_memory *, struct vmspace *);
void vmmfs_memory_release(struct vmmfs_memory *);
int vmmfs_memory_map_object(struct vmmfs_memory *, struct vm_object *,
	uint64_t, uint64_t, uint64_t, vm_prot_t);
void vmmfs_memory_unmap(struct vmmfs_memory *, uint64_t, uint64_t);

struct vmmfs_memory {
	struct vmmfs_node node;
	ino_t inode;
	struct vm_object *object;
	struct vmspace *boot_vmspace;
	struct vmspace *run_vmspace;
	uint64_t size;
	bool mapped;
};

#endif /* VMMFS_MEMORY_H */
