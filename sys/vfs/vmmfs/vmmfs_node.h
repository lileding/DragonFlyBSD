/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Thin VFS ownership helper for one vmmfs object vnode.
 */
#ifndef VMMFS_NODE_H
#define VMMFS_NODE_H

#include <sys/vnode.h>

struct mount;
struct cdev;
struct vop_ops;

/* Every namespace object embeds this as its first field. */
struct vmmfs_node {
	struct vnode *vnode;
	struct vmmfs_node *parent;
	void (*drop)(struct vmmfs_node *);
	bool (*is_dead)(struct vmmfs_node *);
};

/* Initializes object ownership before the object becomes namespace-visible. */
void vmmfs_node_setup(struct vmmfs_node *, struct vmmfs_node *,
	void (*)(struct vmmfs_node *), bool (*)(struct vmmfs_node *));

/* Publishes a fully initialized regular vnode for a namespace object. */
int vmmfs_node_publish_regular(struct vmmfs_node *, struct mount *,
	struct vop_ops **, enum vtype, void *);

/* Publishes a fully initialized special vnode for a cdev-backed object. */
int vmmfs_node_publish_cdev(struct vmmfs_node *, struct mount *,
	struct vop_ops **, struct cdev *, void *);

/* Runs a terminal object destructor exactly once. */
void vmmfs_node_drop(struct vmmfs_node *);

/* Evaluates the local and direct-parent service gates. */
bool vmmfs_node_is_dead(struct vmmfs_node *);

/* Disposes of an unpublished regular or cdev vnode during error rollback. */
void vmmfs_node_abort(struct vmmfs_node *);

/* Atomically rolls back a partially published node and releases its object. */
void vmmfs_node_abort_drop(struct vmmfs_node *);


/* Drops the base reference after the owner has removed its namespace entry. */
void vmmfs_node_unpublish(struct vmmfs_node *);

/* Drops the base reference of a transient node after cache_setunresolved(). */
void vmmfs_node_release_transient(struct vmmfs_node *);




/* Common VOP_INACTIVE tail after the object's semantic dead gate. */
void vmmfs_node_inactive(struct vmmfs_node *, struct vnode *);

/* Common VOP_RECLAIM handoff.  The caller holds its object lock if needed. */
bool vmmfs_node_reclaim(struct vmmfs_node *, struct vnode *);

#endif /* VMMFS_NODE_H */
