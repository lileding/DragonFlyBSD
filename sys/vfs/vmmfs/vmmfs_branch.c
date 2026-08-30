/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared ownership for VMMFS namespace branches.
 */
#include <sys/systm.h>

#include <machine/atomic.h>

#include "vmmfs_branch.h"

static void
vmmfs_branch_drop(struct vmmfs_node *node)
{
	KKASSERT(node != NULL);
	vmmfs_branch_put((struct vmmfs_branch *)node);
}

void
vmmfs_branch_init(struct vmmfs_branch *branch, struct vmmfs_branch *parent,
	void (*drop)(struct vmmfs_node *))
{
	KKASSERT(branch != NULL);
	KKASSERT(drop != NULL);
	bzero(branch, sizeof(*branch));
	vmmfs_node_setup(&branch->node, parent, vmmfs_branch_drop);
	branch->final_drop = drop;
	lwkt_token_init(&branch->token, "vmmfsbranch");
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
	struct vmmfs_branch *parent;
	void (*final_drop)(struct vmmfs_node *);

	KKASSERT(branch != NULL);
	if (atomic_fetchadd_int(&branch->references, -1) != 1)
		return;
	node = &branch->node;
	parent = node->parent;
	final_drop = branch->final_drop;
	KKASSERT(final_drop != NULL);
	lwkt_token_uninit(&branch->token);
	final_drop(node);
	if (parent != NULL)
		vmmfs_branch_put(parent);
}
