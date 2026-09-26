/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly cgroupfs: control group objects.
 *
 * Lifecycle:
 *
 *   Every group owns one persistent directory vnode.  The base reference
 *   returned by getnewvnode() is owned by the registration of the group:
 *   the mount for the root, the parent's child set otherwise.  The vnode in
 *   turn owns one group reference, released when the vnode is reclaimed.
 *   A registered group therefore always has a live vnode and a non-zero
 *   reference count; when the count reaches zero the group has already been
 *   unregistered, so the final put only frees memory and drops the parent.
 *
 *   Interface files are embedded in their group and have no reference
 *   count of their own.  Each has at most one vnode, created on lookup and
 *   tracked in file->vnode (under the mount's vnode spinlock); the vnode
 *   holds a reference on the group until reclaimed.  While the group lives
 *   an unreferenced file vnode stays cached like any other.  When the group
 *   dies its file vnodes are marked VREF_FINALIZE and cycled, so
 *   VOP_INACTIVE recycles them now or at their last release: nothing is
 *   left to pin a removed group until vnlru.
 *
 *   rmdir unregisters and marks the group dead, then hands the base vnode
 *   reference to the caller, which unlinks the name and releases it with
 *   VREF_FINALIZE set.  Open descriptors and working directories inside a
 *   removed group keep the vnode and its namecache entry alive but see
 *   ENOENT.
 *
 * Locking:
 *
 *   The per-mount hierarchy lock protects topology, dead flags and control
 *   state.  It is ordered after every vnode and namecache lock: while it is
 *   held no cgroupfs vnode or namecache lock may be acquired and no vnode
 *   may be allocated, and the reclaim -> put path never takes it.  Taking
 *   or dropping plain vnode references (vref/vrele of a registered group's
 *   vnode, which cannot reach zero) is allowed.  The vnode spinlock is a
 *   leaf: only file->vnode is read or written, plus vhold(), under it.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/dirent.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/refcount.h>
#include <sys/spinlock2.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "cgroupfs.h"

MALLOC_DEFINE(M_CGROUPFS, "cgroupfs", "Control group filesystem");

/* Directory entry positions: ".", "..", interface files, child groups. */
#define CGROUPFS_ENTRY_FILES	2
#define CGROUPFS_ENTRY_CHILDREN	(CGROUPFS_ENTRY_FILES + CGROUPFS_FILE_COUNT)

struct cgroupfs_file {
	struct cgroupfs_group	*group;
	u_int			index;
	/* Current vnode, if any; protected by the mount's vnode_spin. */
	struct vnode		*vnode;
};

RB_HEAD(cgroupfs_children, cgroupfs_group);

struct cgroupfs_group {
	struct cgroupfs_mount	*mount;
	struct cgroupfs_group	*parent;
	RB_ENTRY(cgroupfs_group) sibling;
	struct cgroupfs_children children;
	u_int			nchildren;
	/* Cleared by reclaim; stable while the group is registered. */
	struct vnode		*vnode;
	u_int			references;
	/* Set under the exclusive hierarchy lock when unregistered. */
	bool			dead;
	/* Directory inode; file i uses inode + 1 + i. */
	ino_t			inode;
	uint32_t		subtree_control;
	struct cgroupfs_file	files[CGROUPFS_FILE_COUNT];
	char			name[NAME_MAX + 1];
};

/* Orders a counted name against a NUL-terminated one, bytewise. */
static int
cgroupfs_name_compare(const char *name, size_t namelen, const char *other)
{
	size_t otherlen;
	int cmp;

	otherlen = strlen(other);
	cmp = memcmp(name, other, MIN(namelen, otherlen));
	if (cmp != 0)
		return (cmp);
	return ((namelen > otherlen) - (namelen < otherlen));
}

static int
cgroupfs_group_compare(struct cgroupfs_group *left,
    struct cgroupfs_group *right)
{
	return (cgroupfs_name_compare(left->name, strlen(left->name),
	    right->name));
}

RB_PROTOTYPE_STATIC(cgroupfs_children, cgroupfs_group, sibling,
    cgroupfs_group_compare);
RB_GENERATE_STATIC(cgroupfs_children, cgroupfs_group, sibling,
    cgroupfs_group_compare);

/* Makes the last vrele() deactivate the vnode instead of caching it. */
void
cgroupfs_vnode_finalize(struct vnode *vp)
{
	atomic_set_int(&vp->v_refcnt, VREF_FINALIZE);
}

static void
cgroupfs_group_hold(struct cgroupfs_group *group)
{
	refcount_acquire(&group->references);
}

static void
cgroupfs_group_put(struct cgroupfs_group *group)
{
	struct cgroupfs_group *parent;

	while (group != NULL && refcount_release(&group->references)) {
		KKASSERT(group->vnode == NULL);
		KKASSERT(RB_EMPTY(&group->children));
		parent = group->parent;
		kfree(group, M_CGROUPFS);
		group = parent;
	}
}

static struct cgroupfs_group *
cgroupfs_group_child_find_locked(struct cgroupfs_group *group,
    const char *name, size_t namelen)
{
	struct cgroupfs_group *child;
	int cmp;

	child = RB_ROOT(&group->children);
	while (child != NULL) {
		cmp = cgroupfs_name_compare(name, namelen, child->name);
		if (cmp == 0)
			return (child);
		child = cmp < 0 ? RB_LEFT(child, sibling) :
		    RB_RIGHT(child, sibling);
	}
	return (NULL);
}

/* The child at a rank in name order; O(rank). */
static struct cgroupfs_group *
cgroupfs_group_child_at_locked(struct cgroupfs_group *group, off_t rank)
{
	struct cgroupfs_group *child;

	RB_FOREACH(child, cgroupfs_children, &group->children) {
		if (rank-- == 0)
			return (child);
	}
	return (NULL);
}

/* Index of the interface file with this name, or -1. */
static int
cgroupfs_group_file_find(const char *name, size_t namelen)
{
	u_int index;

	for (index = 0; index < CGROUPFS_FILE_COUNT; ++index) {
		if (cgroupfs_name_compare(name, namelen,
		    cgroupfs_file_name(index)) == 0)
			return (index);
	}
	return (-1);
}

static void
cgroupfs_group_control_locked(struct cgroupfs_group *group,
    struct cgroupfs_control *control)
{
	struct cgroupfs_group *child;

	control->is_root = group->parent == NULL;
	control->available = group->parent != NULL ?
	    group->parent->subtree_control : cgroupfs_controller_all();
	control->subtree_control = group->subtree_control;
	control->children_subtree_control = 0;
	RB_FOREACH(child, cgroupfs_children, &group->children)
		control->children_subtree_control |= child->subtree_control;
}

static bool
cgroupfs_group_file_visible_locked(struct cgroupfs_file *file)
{
	struct cgroupfs_control control;

	if (file->group->dead)
		return (false);
	cgroupfs_group_control_locked(file->group, &control);
	return (cgroupfs_file_present(file->index, &control));
}

/*
 * Allocates a group together with its persistent directory vnode.  On
 * success the caller owns the vnode's base reference (unlocked) and must
 * hand it to the registration that will hold it.
 */
static int
cgroupfs_group_alloc(struct cgroupfs_mount *kmp, struct cgroupfs_group *parent,
    const char *name, size_t namelen, struct cgroupfs_group **groupp)
{
	struct cgroupfs_group *group;
	struct vnode *vp;
	u_int index;
	int error;

	if (namelen > NAME_MAX)
		return (ENAMETOOLONG);
	group = kmalloc(sizeof(*group), M_CGROUPFS, M_WAITOK | M_ZERO);
	group->mount = kmp;
	group->references = 1;	/* owned by the directory vnode */
	RB_INIT(&group->children);
	group->inode = atomic_fetchadd_int(&kmp->next_inode,
	    1 + CGROUPFS_FILE_COUNT);
	for (index = 0; index < CGROUPFS_FILE_COUNT; ++index) {
		group->files[index].group = group;
		group->files[index].index = index;
	}
	bcopy(name, group->name, namelen);
	group->name[namelen] = '\0';

	error = getnewvnode(VT_SYNTH, kmp->mount, &vp, 0, 0);
	if (error != 0) {
		kfree(group, M_CGROUPFS);
		return (error);
	}
	vp->v_type = VDIR;
	vp->v_data = group;
	group->vnode = vp;
	if (parent != NULL) {
		cgroupfs_group_hold(parent);
		group->parent = parent;
	}
	vx_downgrade(vp);
	vn_unlock(vp);
	*groupp = group;
	return (0);
}

/*
 * Returns the interface file's vnode, referenced and unlocked, creating it
 * if needed (after tmpfs_alloc_vp()).  The caller pins the group through
 * the directory vnode it holds; an attached vnode takes its own reference.
 */
static int
cgroupfs_group_file_vnode(struct cgroupfs_file *file, struct vnode **vpp)
{
	struct cgroupfs_group *group = file->group;
	struct cgroupfs_mount *kmp = group->mount;
	struct vnode *vp;
	struct vnode *newvp;
	bool dead;
	int error;

	for (;;) {
		spin_lock(&kmp->vnode_spin);
		vp = file->vnode;
		if (vp != NULL) {
			vhold(vp);
			spin_unlock(&kmp->vnode_spin);
			/* Fails while the vnode is being reclaimed; retry. */
			if (vget(vp, LK_SHARED) == 0) {
				if (vp->v_data == file) {
					vn_unlock(vp);
					vdrop(vp);
					*vpp = vp;
					return (0);
				}
				vput(vp);
			}
			vdrop(vp);
			continue;
		}
		spin_unlock(&kmp->vnode_spin);

		error = getnewvnode(VT_SYNTH, kmp->mount, &newvp, 0, 0);
		if (error != 0)
			return (error);
		newvp->v_type = VREG;
		newvp->v_data = file;

		spin_lock(&kmp->vnode_spin);
		if (file->vnode == NULL) {
			file->vnode = newvp;
			cgroupfs_group_hold(group);	/* owned by the vnode */
			/* Ordered after rmdir's walk by the spinlock. */
			dead = group->dead;
			spin_unlock(&kmp->vnode_spin);
			if (dead)
				cgroupfs_vnode_finalize(newvp);
			vx_downgrade(newvp);
			vn_unlock(newvp);
			*vpp = newvp;
			return (0);
		}
		spin_unlock(&kmp->vnode_spin);
		/* Lost the race: discard ours, reclaim sees no v_data. */
		newvp->v_data = NULL;
		newvp->v_type = VBAD;
		vx_put(newvp);
	}
}

/*
 * A dead group's file vnodes must not stay cached: finalize them, and
 * cycle unreferenced ones through vget()/vput() to run VOP_INACTIVE now,
 * as cache_unlink() does for removed files.  Called without the hierarchy
 * lock after the group was marked dead.
 */
static void
cgroupfs_group_finalize_files(struct cgroupfs_group *group)
{
	struct cgroupfs_mount *kmp = group->mount;
	struct vnode *vp;
	u_int index;

	KKASSERT(group->dead);
	for (index = 0; index < CGROUPFS_FILE_COUNT; ++index) {
		spin_lock(&kmp->vnode_spin);
		vp = group->files[index].vnode;
		if (vp != NULL)
			vhold(vp);
		spin_unlock(&kmp->vnode_spin);
		if (vp == NULL)
			continue;
		cgroupfs_vnode_finalize(vp);
		if (VREFCNT(vp) <= 0 && vget(vp, LK_SHARED) == 0)
			vput(vp);
		vdrop(vp);
	}
}

int
cgroupfs_group_create_root(struct cgroupfs_mount *kmp,
    struct cgroupfs_group **rootp)
{
	struct cgroupfs_group *root;
	int error;

	error = cgroupfs_group_alloc(kmp, NULL, "", 0, &root);
	if (error != 0)
		return (error);
	vsetflags(root->vnode, VROOT);
	*rootp = root;
	return (0);
}

int
cgroupfs_group_root_vnode(struct cgroupfs_group *root, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	vp = root->vnode;
	if (vp == NULL)
		return (ENOENT);
	vhold(vp);
	error = vget(vp, LK_EXCLUSIVE | LK_RETRY);
	vdrop(vp);
	if (error != 0)
		return (error);
	*vpp = vp;
	return (0);
}

/* Child groups veto even a forced unmount; see the design notes. */
int
cgroupfs_group_unmount_begin(struct cgroupfs_group *root)
{
	struct cgroupfs_mount *kmp = root->mount;
	int error;

	lockmgr(&kmp->hierarchy_lock, LK_EXCLUSIVE);
	KKASSERT(!root->dead);
	if (!RB_EMPTY(&root->children)) {
		error = EBUSY;
	} else {
		root->dead = true;
		error = 0;
	}
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);
	return (error);
}

void
cgroupfs_group_unmount_abort(struct cgroupfs_group *root)
{
	struct cgroupfs_mount *kmp = root->mount;

	lockmgr(&kmp->hierarchy_lock, LK_EXCLUSIVE);
	KKASSERT(root->dead);
	root->dead = false;
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);
}

ino_t
cgroupfs_group_inode(const struct cgroupfs_group *group)
{
	return (group->inode);
}

/*
 * Unlocked read: only used as a recycling hint by VOP_INACTIVE.  A stale
 * answer merely leaves the vnode to ordinary LRU reclamation.
 */
bool
cgroupfs_group_is_dead(const struct cgroupfs_group *group)
{
	return (group->dead);
}

int
cgroupfs_group_lookup(struct cgroupfs_group *group, const char *name,
    size_t namelen, struct vnode **vpp)
{
	struct cgroupfs_mount *kmp = group->mount;
	struct cgroupfs_group *child;
	struct cgroupfs_file *found;
	int index;
	int error;

	found = NULL;
	error = ENOENT;
	lockmgr(&kmp->hierarchy_lock, LK_SHARED);
	if (group->dead)
		goto out;
	index = cgroupfs_group_file_find(name, namelen);
	if (index >= 0) {
		if (cgroupfs_group_file_visible_locked(&group->files[index])) {
			found = &group->files[index];
			error = 0;
		}
		goto out;
	}
	child = cgroupfs_group_child_find_locked(group, name, namelen);
	if (child != NULL) {
		*vpp = child->vnode;
		vref(*vpp);
		error = 0;
	}
out:
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);
	if (found != NULL)
		error = cgroupfs_group_file_vnode(found, vpp);
	return (error);
}

int
cgroupfs_group_lookup_parent(struct cgroupfs_group *group,
    struct vnode **vpp)
{
	struct cgroupfs_mount *kmp = group->mount;
	int error;

	if (group->parent == NULL)
		return (EOPNOTSUPP);
	lockmgr(&kmp->hierarchy_lock, LK_SHARED);
	/* A live group's parent is registered and so has a live vnode. */
	if (group->dead) {
		error = ENOENT;
	} else {
		*vpp = group->parent->vnode;
		vref(*vpp);
		error = 0;
	}
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);
	return (error);
}

/*
 * Interface file names are reserved even while hidden, so enabling a
 * controller can never collide with an existing child.
 */
int
cgroupfs_group_mkdir(struct cgroupfs_group *parent, const char *name,
    size_t namelen, struct vnode **vpp)
{
	struct cgroupfs_mount *kmp = parent->mount;
	struct cgroupfs_group *child;
	struct vnode *vp;
	int error;

	error = cgroupfs_group_alloc(kmp, parent, name, namelen, &child);
	if (error != 0)
		return (error);
	vp = child->vnode;

	lockmgr(&kmp->hierarchy_lock, LK_EXCLUSIVE);
	if (parent->dead) {
		error = ENOENT;
	} else if (cgroupfs_group_file_find(name, namelen) >= 0 ||
	    cgroupfs_group_child_find_locked(parent, name, namelen) != NULL) {
		error = EEXIST;
	} else {
		RB_INSERT(cgroupfs_children, &parent->children, child);
		++parent->nchildren;
		vref(vp);	/* the caller's, beside the registration's */
		error = 0;
	}
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);

	if (error != 0) {
		/* Never registered: recycled on release, freed on reclaim. */
		child->dead = true;
		cgroupfs_vnode_finalize(vp);
		vrele(vp);
		return (error);
	}
	*vpp = vp;
	return (0);
}

/*
 * On success the removed group's directory vnode is returned with the
 * registration's base reference, which the caller must release after
 * cleaning up the namecache.
 */
int
cgroupfs_group_rmdir(struct cgroupfs_group *parent, const char *name,
    size_t namelen, struct vnode **vpp)
{
	struct cgroupfs_mount *kmp = parent->mount;
	struct cgroupfs_group *child;
	int index;
	int error;

	lockmgr(&kmp->hierarchy_lock, LK_EXCLUSIVE);
	child = NULL;
	index = cgroupfs_group_file_find(name, namelen);
	if (parent->dead) {
		error = ENOENT;
	} else if (index >= 0) {
		error = cgroupfs_group_file_visible_locked(
		    &parent->files[index]) ? ENOTDIR : ENOENT;
	} else if ((child = cgroupfs_group_child_find_locked(parent, name,
	    namelen)) == NULL) {
		error = ENOENT;
	} else if (!RB_EMPTY(&child->children)) {
		error = EBUSY;
	} else {
		RB_REMOVE(cgroupfs_children, &parent->children, child);
		--parent->nchildren;
		child->dead = true;
		*vpp = child->vnode;
		error = 0;
	}
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);
	/* The returned base reference keeps the child alive. */
	if (error == 0)
		cgroupfs_group_finalize_files(child);
	return (error);
}

/*
 * The uio offset is the entry position.  Copying out under the shared
 * hierarchy lock is safe: cgroupfs files cannot be mapped, so a fault on
 * the user buffer never needs a cgroupfs vnode.
 */
int
cgroupfs_group_readdir(struct cgroupfs_group *group, struct uio *uio,
    int *eofp)
{
	struct cgroupfs_mount *kmp = group->mount;
	struct cgroupfs_group *child;
	struct cgroupfs_file *file;
	const char *name;
	ino_t inode;
	uint8_t type;
	off_t offset;
	int error;

	offset = uio->uio_offset;
	if (offset < 0)
		return (EINVAL);
	*eofp = 0;
	error = 0;
	child = NULL;
	lockmgr(&kmp->hierarchy_lock, LK_SHARED);
	if (group->dead)
		error = ENOENT;
	while (error == 0) {
		if (offset == 0) {
			inode = group->inode;
			type = DT_DIR;
			name = ".";
		} else if (offset == 1) {
			inode = group->parent != NULL ? group->parent->inode :
			    group->inode;
			type = DT_DIR;
			name = "..";
		} else if (offset < CGROUPFS_ENTRY_CHILDREN) {
			file = &group->files[offset - CGROUPFS_ENTRY_FILES];
			inode = group->inode + 1 + file->index;
			type = DT_REG;
			name = cgroupfs_group_file_visible_locked(file) ?
			    cgroupfs_file_name(file->index) : NULL;
		} else {
			child = child == NULL ?
			    cgroupfs_group_child_at_locked(group,
			    offset - CGROUPFS_ENTRY_CHILDREN) :
			    RB_NEXT(cgroupfs_children, &group->children, child);
			if (child == NULL) {
				*eofp = 1;
				break;
			}
			inode = child->inode;
			type = DT_DIR;
			name = child->name;
		}
		if (name != NULL && vop_write_dirent(&error, uio, inode, type,
		    strlen(name), name))
			break;
		++offset;
	}
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);
	uio->uio_offset = offset;
	return (error);
}

void
cgroupfs_group_reclaim(struct cgroupfs_group *group)
{
	group->vnode = NULL;
	cgroupfs_group_put(group);
}

void
cgroupfs_group_file_attr(const struct cgroupfs_file *file, ino_t *inodep,
    mode_t *modep)
{
	*inodep = file->group->inode + 1 + file->index;
	*modep = cgroupfs_file_mode(file->index);
}

/*
 * Stale vnodes (removed group, controller disabled) read as ENOENT.  The
 * content is formatted under the lock and copied out after releasing it.
 */
int
cgroupfs_group_file_read(struct cgroupfs_file *file, struct uio *uio)
{
	struct cgroupfs_mount *kmp = file->group->mount;
	struct cgroupfs_control control;
	char buffer[CGROUPFS_FILE_SIZE_MAX];
	size_t length;
	int error;

	if (uio->uio_offset < 0)
		return (EINVAL);
	lockmgr(&kmp->hierarchy_lock, LK_SHARED);
	if (!cgroupfs_group_file_visible_locked(file)) {
		error = ENOENT;
	} else {
		cgroupfs_group_control_locked(file->group, &control);
		error = cgroupfs_file_load(file->index, &control, buffer,
		    &length);
	}
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);
	if (error != 0)
		return (error);
	if (uio->uio_offset >= (off_t)length)
		return (0);
	return (uiomove(buffer + uio->uio_offset,
	    length - (size_t)uio->uio_offset, uio));
}

/*
 * The children's interface files change visibility with the group's
 * subtree_control.  Their directory vnodes are referenced under the lock
 * and invalidated after it is released (namecache locks rank above it).
 * A child's own entry becomes unresolved too, which is harmless: it is
 * still registered and resolves again through its parent.
 */
int
cgroupfs_group_file_write(struct cgroupfs_file *file, struct uio *uio)
{
	struct cgroupfs_group *group = file->group;
	struct cgroupfs_mount *kmp = group->mount;
	struct cgroupfs_control control;
	struct cgroupfs_group *child;
	struct vnode **stale;
	char buffer[CGROUPFS_FILE_SIZE_MAX];
	size_t length;
	u_int nstale;
	u_int index;
	int error;

	if ((cgroupfs_file_mode(file->index) & S_IWUSR) == 0)
		return (EPERM);
	if (uio->uio_offset != 0 ||
	    uio->uio_resid > (ssize_t)sizeof(buffer))
		return (EINVAL);
	length = uio->uio_resid;
	error = uiomove(buffer, length, uio);
	if (error != 0)
		return (error);

	stale = NULL;
	nstale = 0;
	lockmgr(&kmp->hierarchy_lock, LK_EXCLUSIVE);
	if (!cgroupfs_group_file_visible_locked(file)) {
		error = ENOENT;
		goto out;
	}
	cgroupfs_group_control_locked(group, &control);
	error = cgroupfs_file_store(file->index, &control, buffer, length);
	if (error != 0 || control.subtree_control == group->subtree_control)
		goto out;
	group->subtree_control = control.subtree_control;
	if (group->nchildren == 0)
		goto out;
	stale = kmalloc(sizeof(*stale) * group->nchildren, M_CGROUPFS,
	    M_WAITOK);
	RB_FOREACH(child, cgroupfs_children, &group->children) {
		stale[nstale] = child->vnode;
		vref(stale[nstale]);
		++nstale;
	}
	KKASSERT(nstale == group->nchildren);
out:
	lockmgr(&kmp->hierarchy_lock, LK_RELEASE);
	for (index = 0; index < nstale; ++index) {
		cache_inval_vp(stale[index], CINV_CHILDREN);
		vrele(stale[index]);
	}
	if (stale != NULL)
		kfree(stale, M_CGROUPFS);
	return (error);
}

/* Unlocked recycling hint, see cgroupfs_group_is_dead(). */
bool
cgroupfs_group_file_is_dead(const struct cgroupfs_file *file)
{
	return (file->group->dead);
}

void
cgroupfs_group_file_reclaim(struct cgroupfs_file *file, struct vnode *vp)
{
	struct cgroupfs_mount *kmp = file->group->mount;

	spin_lock(&kmp->vnode_spin);
	KKASSERT(file->vnode == vp);
	file->vnode = NULL;
	spin_unlock(&kmp->vnode_spin);
	cgroupfs_group_put(file->group);
}
