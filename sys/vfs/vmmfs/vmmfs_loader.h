/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs loader declaration object.
 */
#ifndef VMMFS_LOADER_H
#define VMMFS_LOADER_H

#include "vmmfs_node.h"

#include <sys/param.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_mount;
struct vmmfs_machine;
struct vmmfs_loader;
struct vnode;
struct ucred;

int vmmfs_loader_init(struct vmmfs_mount *, struct vmmfs_node *,
	struct vmmfs_loader *, struct vnode **);
int vmmfs_loader_run(struct vmmfs_loader *, struct vnode *, struct ucred *);

struct vmmfs_loader {
	struct vmmfs_node node;
	/* A NUL-terminated, one-page shell script executed with boot fd 3. */
	char script[PAGE_SIZE];
};

#endif /* VMMFS_LOADER_H */
