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
	bool published;
	bool base_reference;
};

/* Creates a regular vnode ready for parent namespace publication. */
int vmmfs_node_init(struct vmmfs_node *, struct mount *,
	struct vop_ops **, enum vtype, void *);

/* Creates a cdev vnode ready for parent namespace publication. */
int vmmfs_node_init_cdev(struct vmmfs_node *, struct mount *,
	struct vop_ops **, struct cdev *, void *);

/* Disposes of an unpublished regular or cdev vnode during error rollback. */
void vmmfs_node_abort(struct vmmfs_node *);

/* Publishes a fully initialized node after its parent owns the object. */
void vmmfs_node_publish(struct vmmfs_node *);

/* Drops the base reference after the owner has removed its namespace entry. */
void vmmfs_node_unpublish(struct vmmfs_node *);

/* Drops the base reference of a transient node after cache_setunresolved(). */
void vmmfs_node_release_transient(struct vmmfs_node *);

/* Reports whether the node remains reachable through its parent namespace. */
bool vmmfs_node_is_published(const struct vmmfs_node *);



/* Common VOP_INACTIVE tail after the object's semantic dead gate. */
void vmmfs_node_inactive(struct vmmfs_node *, struct vnode *);

/* Common VOP_RECLAIM handoff.  The caller holds its object lock if needed. */
bool vmmfs_node_reclaim(struct vmmfs_node *, struct vnode *);

#endif /* VMMFS_NODE_H */
