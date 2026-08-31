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
struct vmmfs_branch;

/* Every namespace object embeds this as its first field. */
struct vmmfs_node {
	struct vmmfs_branch *parent;
	ino_t inode;
	mode_t mode;
	off_t size;
	bool dead;
	int (*deactivate)(struct vmmfs_node *);
	void (*drop)(struct vmmfs_node *);
};

void vmmfs_node_parent_put(struct vmmfs_node *);
int vmmfs_node_default_deactivate(struct vmmfs_node *);
void vmmfs_node_set_metadata(struct vmmfs_node *, ino_t, mode_t, off_t);
off_t vmmfs_node_decimal_size(uint64_t);
int vmmfs_node_open(struct vop_open_args *);
int vmmfs_node_access(struct vop_access_args *);
int vmmfs_node_getattr(struct vop_getattr_args *);
int vmmfs_node_getattr_lite(struct vop_getattr_lite_args *);
int vmmfs_node_inactive(struct vop_inactive_args *);

/* Initializes object ownership before the object becomes namespace-visible. */
void vmmfs_node_setup(struct vmmfs_node *, struct vmmfs_branch *,
	void (*)(struct vmmfs_node *));

/* Creates a regular vnode without storing it in the semantic node. */
int vmmfs_vnode_create_regular(struct mount *, struct vop_ops **,
	enum vtype, struct vmmfs_node *, struct vnode **);

/* Creates a cdev-backed vnode without storing it in the semantic node. */
int vmmfs_vnode_create_cdev(struct mount *, struct vop_ops **,
	struct cdev *, struct vmmfs_node *, struct vnode **);

/* Stops one namespace object and revokes its open file descriptors. */
int vmmfs_vnode_deactivate(struct vnode *);

/* Drops a vnode that was never attached to the namespace. */
void vmmfs_vnode_discard(struct vnode *);


/* Runs a terminal object destructor exactly once. */
void vmmfs_node_drop(struct vmmfs_node *);

/* Common VOP_RECLAIM handoff for every VMMFS namespace vnode. */
int vmmfs_node_reclaim(struct vop_reclaim_args *);

#endif /* VMMFS_NODE_H */
