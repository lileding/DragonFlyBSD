/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs filesystem interfaces.
 */
#ifndef VMMFS_H
#define VMMFS_H

#include <sys/malloc.h>
#include <sys/types.h>

#include "vmmfs_machine.h"
#include "vmmfs_root.h"

struct mount;
struct vop_ops;

struct vmmfs_mount {
	struct mount *mount;
	volatile u_int next_inode;
	struct vmmfs_root *root;
	struct vop_ops *root_vops;
	struct vop_ops *machine_vops;
	struct vop_ops *vcpu_vops;
	struct vop_ops *memory_vops;
	struct vop_ops *loader_vops;
	struct vop_ops *stopped_vops;
	struct vop_ops *events_vops;
	struct vop_ops *pciroot_vops;
	struct vop_ops *pcislot_vops;
};

MALLOC_DECLARE(M_VMMFS);

#endif /* VMMFS_H */
