/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Thin VFS ownership helper for one vmmfs object vnode.
 */
#ifndef VMMFS_NODE_H
#define VMMFS_NODE_H

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/lock.h>
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
/* Lookup and enumeration return an ordinary vnode reference to the caller. */
typedef int (*vmmfs_node_get_item_t)(struct vmmfs_node *, const char *,
	size_t, struct vnode **);
typedef int (*vmmfs_node_read_item_t)(struct vmmfs_node *, uint64_t,
	struct vmmfs_node_item *);
/* Success returns a vnode reference in addition to the registry reference. */
typedef int (*vmmfs_node_create_item_t)(struct vmmfs_node *, struct mount *,
	const char *, size_t, struct vnode **);
typedef int (*vmmfs_node_remove_item_t)(struct vmmfs_node *, const char *,
	size_t);

struct vmmfs_node_item {
	struct vnode *vnode;
	ino_t inode;
	char name[NAME_MAX + 1];
};

/* Every namespace object embeds this as its first field. */
struct vmmfs_node {
	struct vmmfs_node *parent;
	/* Non-owning backlink, cleared under exclusive lock at detachment. */
	struct vnode *vnode;
	struct vmmfs_mount *mount;
	/* Shared work admission versus exclusive deactivation. */
	struct lock lock;
	u_int references;
	ino_t inode;
	mode_t mode;
	off_t size;
	bool dead;
	/*
	 * True agrees to closure; false vetoes. Called with dead set, without
	 * the lifecycle lock. A veto must leave
	 * resources unchanged; admission is restored under exclusive lock.
	 * Once cleanup starts, it cannot veto.
	 */
	bool (*deactivate)(struct vmmfs_node *);
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

/*
 * The caller pins object storage. Work holds shared admission until return;
 * a callback must not deactivate itself or wait for its own deactivation.
 * Helpers inherit the caller's protection. Lifecycle callbacks use their
 * own ownership protocol and do not use this macro for new admission.
 */
#define VMMFS_WORK(object, ...) ({ \
	__typeof__(object) _vmmfs_object = (object); \
	struct vmmfs_node *_vmmfs_node = (struct vmmfs_node *)_vmmfs_object; \
	int _vmmfs_error; \
	_vmmfs_error = lockmgr(&_vmmfs_node->lock, LK_SHARED); \
	if (_vmmfs_error == 0) { \
		if (_vmmfs_node->dead) \
			_vmmfs_error = ENOENT; \
		else \
			_vmmfs_error = (__VA_ARGS__); \
		/* Releasing an acquired, non-cancelable lock cannot fail. */ \
		(void)lockmgr(&_vmmfs_node->lock, LK_RELEASE); \
	} \
	_vmmfs_error; \
})

#define VMMFS_CALL(object, method, ...) \
	VMMFS_WORK((object), _vmmfs_object->method == NULL ? EOPNOTSUPP : \
	    _vmmfs_object->method(_vmmfs_object, ##__VA_ARGS__))

/* Object references do not imply that its vnode or service remains active. */
void vmmfs_node_hold(struct vmmfs_node *);
void vmmfs_node_put(struct vmmfs_node *);
int vmmfs_node_nmkdir(struct vop_nmkdir_args *);
int vmmfs_node_nresolve(struct vop_nresolve_args *);
int vmmfs_node_nlookupdotdot(struct vop_nlookupdotdot_args *);
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

/* Creates a regular vnode and records it in node->vnode. */
int vmmfs_vnode_create_regular(struct mount *, struct vop_ops **,
	enum vtype, struct vmmfs_node *);

/* Creates a cdev-backed vnode and records it in node->vnode. */
int vmmfs_vnode_create_cdev(struct mount *, struct vop_ops **,
	struct cdev *, struct vmmfs_node *);

/*
 * Closes a complete object and consumes one caller-owned vnode reference.
 * NULL objects and objects without a deactivate callback need no rollback.
 * A veto (false) retains the reference; successful closure returns true.
 * Constructors install the callback only after vnode ownership is established.
 */
bool vmmfs_node_deactivate(struct vmmfs_node *);

/* Common VOP_RECLAIM handoff for every VMMFS namespace vnode. */
int vmmfs_node_reclaim(struct vop_reclaim_args *);

#endif /* VMMFS_NODE_H */
