/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared ownership for VMMFS namespace branches.
 */
#ifndef VMMFS_BRANCH_H
#define VMMFS_BRANCH_H

#include <sys/types.h>

#include "vmmfs_node.h"

/*
 * A branch owns a namespace collection.  Its initial reference belongs to
 * the creator or collection; published vnodes and children hold additional
 * references.  The final put calls node.drop().
 */
struct vmmfs_branch {
	struct vmmfs_node node;
	volatile u_int references;
};

void vmmfs_branch_init(struct vmmfs_branch *, struct vmmfs_node *,
	void (*)(struct vmmfs_node *), bool (*)(struct vmmfs_node *));
void vmmfs_branch_hold(struct vmmfs_branch *);
void vmmfs_branch_put(struct vmmfs_branch *);

/* Rolls back a branch before it is inserted into its parent collection. */
void vmmfs_branch_abort(struct vmmfs_branch *);

#endif /* VMMFS_BRANCH_H */
