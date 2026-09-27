/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly cgroupfs: shared declarations.
 *
 * A cgroupfs mount is a view of the kernel's global control group tree
 * (sys/cgroup.h).  Lifecycle and locking rules are described in
 * cgroupfs_node.c.
 */
#ifndef _VFS_CGROUPFS_CGROUPFS_H_
#define _VFS_CGROUPFS_CGROUPFS_H_

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/spinlock.h>
#include <sys/tree.h>

struct cgroup;
struct cgroup_control;
struct mount;
struct nchandle;
struct sbuf;
struct ucred;
struct uio;
struct vnode;
struct vop_ops;
struct cgroupfs_file;
struct cgroupfs_node;

RB_HEAD(cgroupfs_node_tree, cgroupfs_node);

struct cgroupfs_mount {
	struct mount			*mount;
	/* Top of this view; the global root for host mounts.  Held. */
	struct cgroup			*view_root;
	/* The view root's directory vnode; its base reference is ours. */
	struct vnode			*root_vnode;
	/* Leaf lock: the node tree and every node's vnode bookkeeping. */
	struct spinlock			node_spin;
	struct cgroupfs_node_tree	nodes;
	/* On the global mount list, under cgroupfs_mounts_lock. */
	LIST_ENTRY(cgroupfs_mount)	entry;
};

MALLOC_DECLARE(M_CGROUPFS);

extern struct vop_ops cgroupfs_vnode_vops;

/*
 * Views and nodes (cgroupfs_node.c).
 */

/* Creates the root vnode and publishes the view; undone by detach. */
int	cgroupfs_view_attach(struct cgroupfs_mount *);
/* After every vnode of the mount has been reclaimed. */
void	cgroupfs_view_detach(struct cgroupfs_mount *);

/* Directory VOP back ends; the node is the directory vnode's v_data. */
ino_t	cgroupfs_node_inode(const struct cgroupfs_node *);
/* Returns a referenced, unlocked vnode. */
int	cgroupfs_node_lookup(struct cgroupfs_node *, const char *, size_t,
	    struct vnode **);
int	cgroupfs_node_lookup_parent(struct cgroupfs_node *, struct vnode **);
int	cgroupfs_node_readdir(struct cgroupfs_node *, struct uio *, int *);
/* Returns the new directory vnode referenced and unlocked. */
int	cgroupfs_node_mkdir(struct cgroupfs_node *, const char *, size_t,
	    struct vnode **);
/* Unlinks nch in this mount and invalidates the group in every view. */
int	cgroupfs_node_rmdir(struct cgroupfs_node *, struct nchandle *);
bool	cgroupfs_node_is_dead(const struct cgroupfs_node *);
void	cgroupfs_node_reclaim(struct cgroupfs_node *, struct vnode *);

/* Regular file VOP back ends; the file is the file vnode's v_data. */
void	cgroupfs_node_file_attr(const struct cgroupfs_file *, ino_t *,
	    mode_t *);
int	cgroupfs_node_file_read(struct cgroupfs_file *, struct uio *);
int	cgroupfs_node_file_write(struct cgroupfs_file *, struct uio *,
	    struct ucred *);
bool	cgroupfs_node_file_is_dead(const struct cgroupfs_file *);
void	cgroupfs_node_file_reclaim(struct cgroupfs_file *, struct vnode *);

/* Makes the last vrele() deactivate the vnode instead of caching it. */
void	cgroupfs_vnode_finalize(struct vnode *);

/*
 * Interface file table (cgroupfs_file.c).  Pure translation between file
 * text and the kernel's control group interface.
 */

#define CGROUPFS_FILE_COUNT	6
/* Largest accepted write. */
#define CGROUPFS_FILE_SIZE_MAX	128

const char	*cgroupfs_file_name(u_int);
mode_t		cgroupfs_file_mode(u_int);
/* Index of the interface file with this name, or -1. */
int		cgroupfs_file_find(const char *, size_t);
bool		cgroupfs_file_present(u_int, const struct cgroup_control *);
/*
 * Formats the file content into an auto-extending sbuf.  Called without
 * the hierarchy lock, with a control snapshot taken under it.
 */
int		cgroupfs_file_load(u_int, struct cgroup *,
		    const struct cgroup_control *, struct sbuf *);
/*
 * Applies a write on behalf of cred; only for files whose mode grants
 * write access.  Reports whether the namespace-visible control changed.
 */
int		cgroupfs_file_store(u_int, struct cgroup *, const char *,
		    size_t, struct ucred *, bool *);

#endif /* _VFS_CGROUPFS_CGROUPFS_H_ */
