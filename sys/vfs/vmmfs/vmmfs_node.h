/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Thin VFS ownership helper for one vmmfs object vnode.
 */
#ifndef VMMFS_NODE_H
#define VMMFS_NODE_H

#include <sys/param.h>
#include <sys/thread.h>
#include <sys/vnode.h>

struct mount;
struct vmmfs_mount;
struct cdev;
struct vop_ops;
struct vmmfs_node;
struct vmmfs_node_item;

typedef int (*vmmfs_node_load_t)(struct vmmfs_node *, char *, size_t,
	size_t *);
typedef int (*vmmfs_node_store_t)(struct vmmfs_node *, const char *, size_t);
typedef int (*vmmfs_node_get_item_t)(struct vmmfs_node *, const char *,
	size_t, struct vnode **);
typedef int (*vmmfs_node_read_item_t)(struct vmmfs_node *, uint64_t,
	struct vmmfs_node_item *);
/* Success returns a vnode reference in addition to the registry reference. */
typedef int (*vmmfs_node_create_item_t)(struct vmmfs_node *, struct mount *,
	const char *, size_t, struct vnode **);
typedef void (*vmmfs_node_remove_item_t)(struct vmmfs_node *, const char *,
	size_t);

struct vmmfs_node_item {
	struct vnode *vnode;
	ino_t inode;
	char name[NAME_MAX + 1];
};

/* Every namespace object embeds this as its first field. */
struct vmmfs_node {
	struct vmmfs_node *parent;
	struct vmmfs_mount *mount;
	struct lwkt_token token;
	u_int references;
	ino_t inode;
	mode_t mode;
	off_t size;
	bool dead;
	/*
	 * Called with token held and dead set. Veto must not block or change
	 * resources. Once cleanup can block, it must complete without veto.
	 */
	int (*deactivate)(struct vmmfs_node *);
	void (*drop)(struct vmmfs_node *);
	size_t load_limit;
	size_t store_limit;
	vmmfs_node_load_t load;
	vmmfs_node_store_t store;
	vmmfs_node_get_item_t get_item;
	vmmfs_node_read_item_t read_item;
	vmmfs_node_create_item_t create_item;
	vmmfs_node_remove_item_t remove_item;
};

/* Object references do not imply that its vnode or service remains active. */
void vmmfs_node_hold(struct vmmfs_node *);
void vmmfs_node_put(struct vmmfs_node *);
int vmmfs_node_nmkdir(struct vop_nmkdir_args *);
int vmmfs_node_nresolve(struct vop_nresolve_args *);
int vmmfs_node_nrmdir(struct vop_nrmdir_args *);
int vmmfs_node_readdir(struct vop_readdir_args *);
off_t vmmfs_node_decimal_size(uint64_t);
int vmmfs_node_open(struct vop_open_args *);
int vmmfs_node_access(struct vop_access_args *);
int vmmfs_node_getattr(struct vop_getattr_args *);
int vmmfs_node_getattr_lite(struct vop_getattr_lite_args *);
int vmmfs_node_inactive(struct vop_inactive_args *);
int vmmfs_node_read(struct vop_read_args *);
int vmmfs_node_setattr(struct vop_setattr_args *);
int vmmfs_node_write(struct vop_write_args *);

/* Creates a regular vnode without storing it in the semantic node. */
int vmmfs_vnode_create_regular(struct mount *, struct vop_ops **,
	enum vtype, struct vmmfs_node *, struct vnode **);

/* Creates a cdev-backed vnode without storing it in the semantic node. */
int vmmfs_vnode_create_cdev(struct mount *, struct vop_ops **,
	struct cdev *, struct vmmfs_node *, struct vnode **);

/*
 * Closes admission and revokes file descriptors; does not consume the
 * caller's vnode reference. A NULL callback accepts closure. A veto restores
 * admission under the same token; an already closed gate returns EBUSY.
 */
int vmmfs_vnode_deactivate(struct vnode *);

/* Drops a vnode that was never attached to the namespace. */
void vmmfs_vnode_discard(struct vnode *);


/* Common VOP_RECLAIM handoff for every VMMFS namespace vnode. */
int vmmfs_node_reclaim(struct vop_reclaim_args *);

#endif /* VMMFS_NODE_H */
