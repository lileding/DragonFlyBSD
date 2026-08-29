/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared ownership for VMMFS namespace branches.
 */
#include <sys/systm.h>

#include <machine/atomic.h>

#include "vmmfs_branch.h"

void
vmmfs_branch_init(struct vmmfs_branch *branch, struct vmmfs_node *parent,
	void (*drop)(struct vmmfs_node *),
	bool (*is_dead)(struct vmmfs_node *))
{
	KKASSERT(branch != NULL);
	KKASSERT(drop != NULL);
	bzero(branch, sizeof(*branch));
	vmmfs_node_setup(&branch->node, parent, drop, is_dead);
	branch->references = 1;
}

void
vmmfs_branch_hold(struct vmmfs_branch *branch)
{
	KKASSERT(branch != NULL);
	KKASSERT(atomic_fetchadd_int(&branch->references, 0) != 0);
	atomic_add_int(&branch->references, 1);
}

void
vmmfs_branch_put(struct vmmfs_branch *branch)
{
	struct vmmfs_node *node;

	KKASSERT(branch != NULL);
	if (atomic_fetchadd_int(&branch->references, -1) != 1)
		return;
	node = &branch->node;
	vmmfs_node_drop(node);
}
