/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared ownership for VMMFS namespace branches.
 */
#ifndef VMMFS_BRANCH_H
#define VMMFS_BRANCH_H

#include <sys/thread.h>
#include <sys/types.h>

#include "vmmfs_node.h"

/*
 * A branch owns a namespace collection. A named branch begins with the
 * reference retained by its vnode; an internal branch begins with the
 * reference retained by its direct owner. The final put invokes final_drop.
 */
struct vmmfs_branch {
	struct vmmfs_node node;
	struct lwkt_token token;
	volatile u_int references;
	void (*final_drop)(struct vmmfs_node *);
};

void vmmfs_branch_init(struct vmmfs_branch *, struct vmmfs_branch *,
	void (*)(struct vmmfs_node *));
void vmmfs_branch_hold(struct vmmfs_branch *);
void vmmfs_branch_put(struct vmmfs_branch *);


#endif /* VMMFS_BRANCH_H */
