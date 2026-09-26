/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly cgroupfs: shared declarations.
 *
 * Lifecycle and locking rules are described in cgroupfs_group.c.
 */
#ifndef _VFS_CGROUPFS_CGROUPFS_H_
#define _VFS_CGROUPFS_CGROUPFS_H_

#include <sys/types.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/spinlock.h>

struct mount;
struct uio;
struct vnode;
struct vop_ops;
struct cgroupfs_file;
struct cgroupfs_group;

struct cgroupfs_mount {
	struct mount		*mount;
	/*
	 * Protects hierarchy topology, dead flags and control state.
	 * Ordered after every vnode and namecache lock.
	 */
	struct lock		hierarchy_lock;
	/* Protects each interface file's vnode pointer; never blocks. */
	struct spinlock		vnode_spin;
	struct cgroupfs_group	*root;
	u_int			next_inode;
};

MALLOC_DECLARE(M_CGROUPFS);

extern struct vop_ops cgroupfs_vnode_vops;

/*
 * Group objects (cgroupfs_group.c).
 */

/* Root group: creates its persistent, VROOT-flagged directory vnode. */
int	cgroupfs_group_create_root(struct cgroupfs_mount *,
	    struct cgroupfs_group **);
/* Returns the root directory vnode exclusively locked and referenced. */
int	cgroupfs_group_root_vnode(struct cgroupfs_group *, struct vnode **);
/* Unmount admission: vetoes while child groups exist, blocks mkdir. */
int	cgroupfs_group_unmount_begin(struct cgroupfs_group *);
void	cgroupfs_group_unmount_abort(struct cgroupfs_group *);

/* Directory VOP back ends; the group is the directory vnode's v_data. */
ino_t	cgroupfs_group_inode(const struct cgroupfs_group *);
/* Returns a referenced, unlocked vnode. */
int	cgroupfs_group_lookup(struct cgroupfs_group *, const char *, size_t,
	    struct vnode **);
int	cgroupfs_group_lookup_parent(struct cgroupfs_group *,
	    struct vnode **);
int	cgroupfs_group_readdir(struct cgroupfs_group *, struct uio *, int *);
/* Returns the new directory vnode referenced and unlocked. */
int	cgroupfs_group_mkdir(struct cgroupfs_group *, const char *, size_t,
	    struct vnode **);
/* Hands back the removed group's vnode with its base reference. */
int	cgroupfs_group_rmdir(struct cgroupfs_group *, const char *, size_t,
	    struct vnode **);
bool	cgroupfs_group_is_dead(const struct cgroupfs_group *);
/* Consumes the reference owned by the group's directory vnode. */
void	cgroupfs_group_reclaim(struct cgroupfs_group *);
/* Makes the last vrele() deactivate the vnode instead of caching it. */
void	cgroupfs_vnode_finalize(struct vnode *);

/* Regular file VOP back ends; the file is the file vnode's v_data. */
void	cgroupfs_group_file_attr(const struct cgroupfs_file *, ino_t *,
	    mode_t *);
int	cgroupfs_group_file_read(struct cgroupfs_file *, struct uio *);
int	cgroupfs_group_file_write(struct cgroupfs_file *, struct uio *);
/* Detaches the vnode and consumes the group reference it owned. */
void	cgroupfs_group_file_reclaim(struct cgroupfs_file *, struct vnode *);
bool	cgroupfs_group_file_is_dead(const struct cgroupfs_file *);

/*
 * Interface file and controller tables (cgroupfs_file.c).  Pure functions
 * of a control state snapshot taken under the hierarchy lock.
 */

#define CGROUPFS_FILE_COUNT	5
#define CGROUPFS_FILE_SIZE_MAX	128

struct cgroupfs_control {
	bool		is_root;
	/* Controllers this group may use: all for the root. */
	uint32_t	available;
	uint32_t	subtree_control;
	/* Union of the children's subtree_control. */
	uint32_t	children_subtree_control;
};

uint32_t	cgroupfs_controller_all(void);
const char	*cgroupfs_file_name(u_int);
mode_t		cgroupfs_file_mode(u_int);
bool		cgroupfs_file_present(u_int, const struct cgroupfs_control *);
/* Formats the file content; the buffer holds CGROUPFS_FILE_SIZE_MAX. */
int		cgroupfs_file_load(u_int, const struct cgroupfs_control *,
		    char *, size_t *);
/*
 * Applies a write to the control state, all or nothing.  Only files whose
 * mode grants write access may be stored to.
 */
int		cgroupfs_file_store(u_int, struct cgroupfs_control *,
		    const char *, size_t);

#endif /* _VFS_CGROUPFS_CGROUPFS_H_ */
