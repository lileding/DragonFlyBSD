/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared ownership for VMMFS namespace branches.
 */
#ifndef VMMFS_BRANCH_H
#define VMMFS_BRANCH_H

#include <sys/param.h>
#include <sys/thread.h>
#include <sys/types.h>

#include "vmmfs_node.h"

struct mount;
struct vop_nmkdir_args;
struct vop_nresolve_args;
struct vop_nrmdir_args;
struct vop_readdir_args;

struct vmmfs_branch_item {
	struct vnode *vnode;
	ino_t inode;
	char name[NAME_MAX + 1];
};

/*
 * Branch-specific collection operations.  get_item() and read_item() return
 * a held vnode.  remove_item() erases the branch registry entry and releases
 * the vnode Arc retained by that entry after the child was deactivated.
 */
struct vmmfs_branch_ops {
	int (*get_item)(struct vmmfs_branch *, const char *, size_t,
	    struct vnode **);
	int (*read_item)(struct vmmfs_branch *, uint64_t,
	    struct vmmfs_branch_item *);
	int (*create_item)(struct vmmfs_branch *, struct mount *,
	    const char *, size_t, struct vnode **);
	void (*remove_item)(struct vmmfs_branch *, const char *, size_t);
};

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
	const struct vmmfs_branch_ops *ops;
};

void vmmfs_branch_init(struct vmmfs_branch *, struct vmmfs_branch *,
	void (*)(struct vmmfs_node *));
void vmmfs_branch_hold(struct vmmfs_branch *);
void vmmfs_branch_put(struct vmmfs_branch *);
int vmmfs_branch_nmkdir(struct vop_nmkdir_args *);
int vmmfs_branch_nresolve(struct vop_nresolve_args *);
int vmmfs_branch_nrmdir(struct vop_nrmdir_args *);
int vmmfs_branch_readdir(struct vop_readdir_args *);

#endif /* VMMFS_BRANCH_H */
