/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly cgroupfs: views of the kernel's control group tree.
 *
 * Objects:
 *
 *   A node pairs one mount with one kernel group and records the vnodes
 *   that exist for that group in that mount: at most one directory vnode
 *   and one vnode per interface file.  A node is created with its first
 *   vnode and freed with its last (node->vnodes); it holds a reference on
 *   its group.  Vnodes are created on lookup and cached like any other.
 *   Only the mount's root vnode is persistent: its base reference belongs
 *   to the mount and is released by vflush() at unmount.
 *
 * Group removal:
 *
 *   The kernel unregisters the group; every view then finalizes the vnodes
 *   it has for it (VREF_FINALIZE, and a vget()/vput() cycle for idle ones)
 *   so VOP_INACTIVE recycles them now or at their last release, and the
 *   node and its group reference go away without waiting for vnlru.
 *
 * Namecache coherence across views (after devfs):
 *
 *   Any view may change the shared tree, so every view's namecache must
 *   follow.  Negative entries: when a name may have appeared, bump each
 *   mount's mnt_namecache_gen; VFS_NCPGEN_TEST then discards stale
 *   negative hits.  Positive entries: a resolved entry always refers to a
 *   live vnode, and all vnodes are recorded in nodes, so when a name goes
 *   away its vnodes are found and invalidated.
 *
 * Locking, in order:
 *
 *   vnode and namecache locks
 *   the kernel hierarchy lock (cgroup_lock_*) or cgroupfs_mounts_lock,
 *     never both, and neither held while taking a vnode or namecache lock
 *     or allocating a vnode
 *   node_spin (per mount, leaf): node trees and vnode bookkeeping
 *
 *   Work on other views' vnodes therefore happens on a snapshot: vnodes
 *   are collected and held under the locks and processed after release.
 *   Reclaim only takes node_spin, and releasing a group takes no lock.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cgroup.h>
#include <sys/dirent.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/sbuf.h>
#include <sys/spinlock2.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "cgroupfs.h"

MALLOC_DEFINE(M_CGROUPFS, "cgroupfs", "Control group filesystem");

/* Inodes 0 and 1 are conventionally reserved; the global root gets 2. */
#define CGROUPFS_ROOT_INODE	2
/* Slot of a node's directory vnode; files use their table index. */
#define CGROUPFS_SLOT_DIR	(-1)
#define CGROUPFS_SLOT_COUNT	(1 + CGROUPFS_FILE_COUNT)
/* Directory entry positions: ".", "..", interface files, child groups. */
#define CGROUPFS_ENTRY_FILES	2
#define CGROUPFS_ENTRY_CHILDREN	(CGROUPFS_ENTRY_FILES + CGROUPFS_FILE_COUNT)

struct cgroupfs_file {
	struct cgroupfs_node	*node;
	u_int			index;
	struct vnode		*vnode;		/* node_spin */
};

struct cgroupfs_node {
	RB_ENTRY(cgroupfs_node)	entry;
	struct cgroupfs_mount	*mount;
	struct cgroup		*cgroup;	/* held */
	u_int			vnodes;		/* node_spin */
	struct vnode		*dir;		/* node_spin */
	struct cgroupfs_file	files[CGROUPFS_FILE_COUNT];
};

/* A held vnode from a snapshot, with the view it belongs to. */
struct cgroupfs_held {
	struct vnode		*vnode;
	struct cgroupfs_mount	*mount;
	bool			dir;
};

static struct lock cgroupfs_mounts_lock =
    LOCK_INITIALIZER("cgrpfsmnt", 0, 0);
static LIST_HEAD(, cgroupfs_mount) cgroupfs_mounts =
    LIST_HEAD_INITIALIZER(cgroupfs_mounts);

static int
cgroupfs_node_compare(struct cgroupfs_node *left, struct cgroupfs_node *right)
{
	uintptr_t a = (uintptr_t)left->cgroup;
	uintptr_t b = (uintptr_t)right->cgroup;

	return ((a > b) - (a < b));
}

RB_PROTOTYPE_STATIC(cgroupfs_node_tree, cgroupfs_node, entry,
    cgroupfs_node_compare);
RB_GENERATE_STATIC(cgroupfs_node_tree, cgroupfs_node, entry,
    cgroupfs_node_compare);

void
cgroupfs_vnode_finalize(struct vnode *vp)
{
	atomic_set_int(&vp->v_refcnt, VREF_FINALIZE);
}

static ino_t
cgroupfs_inode(const struct cgroup *cg, int slot)
{
	return (CGROUPFS_ROOT_INODE + cgroup_id(cg) * CGROUPFS_SLOT_COUNT +
	    (slot + 1));
}

static struct vnode **
cgroupfs_node_slot(struct cgroupfs_node *node, int slot)
{
	return (slot == CGROUPFS_SLOT_DIR ? &node->dir :
	    &node->files[slot].vnode);
}

static void *
cgroupfs_node_slot_data(struct cgroupfs_node *node, int slot)
{
	return (slot == CGROUPFS_SLOT_DIR ? (void *)node :
	    (void *)&node->files[slot]);
}

static struct cgroupfs_node *
cgroupfs_node_find_locked(struct cgroupfs_mount *kmp, struct cgroup *cg)
{
	struct cgroupfs_node key;

	key.cgroup = cg;
	return (RB_FIND(cgroupfs_node_tree, &kmp->nodes, &key));
}

static struct cgroupfs_node *
cgroupfs_node_alloc(struct cgroupfs_mount *kmp, struct cgroup *cg)
{
	struct cgroupfs_node *node;
	u_int index;

	node = kmalloc(sizeof(*node), M_CGROUPFS, M_WAITOK | M_ZERO);
	node->mount = kmp;
	node->cgroup = cg;
	for (index = 0; index < CGROUPFS_FILE_COUNT; ++index) {
		node->files[index].node = node;
		node->files[index].index = index;
	}
	return (node);
}

static void
cgroupfs_vnode_discard(struct vnode *vp)
{
	vp->v_type = VBAD;
	vx_put(vp);
}

/*
 * Returns the vnode for a slot of (view, group), referenced and unlocked,
 * creating the node and the vnode as needed (after tmpfs_alloc_vp()).
 * The caller keeps the group alive for the duration.
 */
static int
cgroupfs_vnode_get(struct cgroupfs_mount *kmp, struct cgroup *cg, int slot,
    struct vnode **vpp)
{
	struct cgroupfs_node *node;
	struct cgroupfs_node *spare;
	struct vnode *vp;
	void *data;
	bool dead;
	int error;

	spare = NULL;
	for (;;) {
		spin_lock(&kmp->node_spin);
		node = cgroupfs_node_find_locked(kmp, cg);
		vp = node != NULL ? *cgroupfs_node_slot(node, slot) : NULL;
		if (vp != NULL) {
			data = cgroupfs_node_slot_data(node, slot);
			vhold(vp);
			spin_unlock(&kmp->node_spin);
			/* Fails while the vnode is being reclaimed; retry. */
			if (vget(vp, LK_SHARED) == 0) {
				if (vp->v_data == data) {
					vn_unlock(vp);
					vdrop(vp);
					*vpp = vp;
					error = 0;
					break;
				}
				vput(vp);
			}
			vdrop(vp);
			continue;
		}
		spin_unlock(&kmp->node_spin);
		if (node == NULL && spare == NULL) {
			spare = cgroupfs_node_alloc(kmp, cg);
			continue;
		}

		error = getnewvnode(VT_SYNTH, kmp->mount, &vp, 0, 0);
		if (error != 0)
			break;
		vp->v_type = slot == CGROUPFS_SLOT_DIR ? VDIR : VREG;

		spin_lock(&kmp->node_spin);
		node = cgroupfs_node_find_locked(kmp, cg);
		if (node == NULL && spare != NULL) {
			node = spare;
			spare = NULL;
			cgroup_hold(cg);	/* owned by the node */
			RB_INSERT(cgroupfs_node_tree, &kmp->nodes, node);
		}
		if (node == NULL || *cgroupfs_node_slot(node, slot) != NULL) {
			/* Raced with another creator or a node release. */
			spin_unlock(&kmp->node_spin);
			cgroupfs_vnode_discard(vp);
			continue;
		}
		*cgroupfs_node_slot(node, slot) = vp;
		vp->v_data = cgroupfs_node_slot_data(node, slot);
		++node->vnodes;
		/* Ordered after a removal's finalization walk by node_spin. */
		dead = cgroup_is_dead(cg);
		spin_unlock(&kmp->node_spin);
		if (dead)
			cgroupfs_vnode_finalize(vp);
		vx_downgrade(vp);
		vn_unlock(vp);
		*vpp = vp;
		error = 0;
		break;
	}
	if (spare != NULL)
		kfree(spare, M_CGROUPFS);
	return (error);
}

/* Reclaim: detaches the vnode; the last one frees the node. */
static void
cgroupfs_vnode_release(struct cgroupfs_node *node, int slot,
    struct vnode *vp)
{
	struct cgroupfs_mount *kmp = node->mount;
	struct cgroup *cg;
	bool last;

	spin_lock(&kmp->node_spin);
	KKASSERT(*cgroupfs_node_slot(node, slot) == vp);
	*cgroupfs_node_slot(node, slot) = NULL;
	last = --node->vnodes == 0;
	if (last)
		RB_REMOVE(cgroupfs_node_tree, &kmp->nodes, node);
	spin_unlock(&kmp->node_spin);
	if (last) {
		cg = node->cgroup;
		kfree(node, M_CGROUPFS);
		cgroup_drop(cg);
	}
}

/*
 * Snapshots, across every view, the vnodes of the given groups: their
 * directories if dirs is set, their files if files is set.  Each vnode
 * is held; the caller processes and vdrop()s them, then frees the array.
 */
static struct cgroupfs_held *
cgroupfs_views_collect(struct cgroup **groups, u_int ngroups, bool dirs,
    bool files, u_int *countp)
{
	struct cgroupfs_mount *kmp;
	struct cgroupfs_node *node;
	struct cgroupfs_held *held;
	struct vnode *vp;
	u_int nmounts;
	u_int count;
	u_int group;
	u_int index;

	lockmgr(&cgroupfs_mounts_lock, LK_SHARED);
	nmounts = 0;
	LIST_FOREACH(kmp, &cgroupfs_mounts, entry)
		++nmounts;
	held = kmalloc(sizeof(*held) * (nmounts * ngroups *
	    CGROUPFS_SLOT_COUNT + 1), M_CGROUPFS, M_WAITOK);
	count = 0;
	LIST_FOREACH(kmp, &cgroupfs_mounts, entry) {
		spin_lock(&kmp->node_spin);
		for (group = 0; group < ngroups; ++group) {
			node = cgroupfs_node_find_locked(kmp, groups[group]);
			if (node == NULL)
				continue;
			if (dirs && (vp = node->dir) != NULL) {
				vhold(vp);
				held[count].vnode = vp;
				held[count].mount = kmp;
				held[count].dir = true;
				++count;
			}
			for (index = 0; files && index < CGROUPFS_FILE_COUNT;
			    ++index) {
				if ((vp = node->files[index].vnode) == NULL)
					continue;
				vhold(vp);
				held[count].vnode = vp;
				held[count].mount = kmp;
				held[count].dir = false;
				++count;
			}
		}
		spin_unlock(&kmp->node_spin);
	}
	lockmgr(&cgroupfs_mounts_lock, LK_RELEASE);
	*countp = count;
	return (held);
}

/* A name may have appeared: drop every view's negative entries. */
static void
cgroupfs_views_names_appeared(void)
{
	struct cgroupfs_mount *kmp;

	lockmgr(&cgroupfs_mounts_lock, LK_SHARED);
	LIST_FOREACH(kmp, &cgroupfs_mounts, entry)
		atomic_add_int(&kmp->mount->mnt_namecache_gen, 1);
	lockmgr(&cgroupfs_mounts_lock, LK_RELEASE);
}

/*
 * A group was unregistered.  In the view that removed it the caller has
 * already unlinked the name; elsewhere destroy the directory's entries.
 * Every vnode of the group is finalized in every view.
 */
static void
cgroupfs_views_group_removed(struct cgroup *cg, struct cgroupfs_mount *origin)
{
	struct cgroupfs_held *held;
	struct vnode *vp;
	u_int count;
	u_int index;

	held = cgroupfs_views_collect(&cg, 1, true, true, &count);
	for (index = 0; index < count; ++index) {
		vp = held[index].vnode;
		if (held[index].dir && held[index].mount != origin)
			cache_inval_vp(vp, CINV_DESTROY);
		cgroupfs_vnode_finalize(vp);
		if (VREFCNT(vp) <= 0 && vget(vp, LK_SHARED) == 0)
			vput(vp);
		vdrop(vp);
	}
	kfree(held, M_CGROUPFS);
}

/*
 * The group's subtree_control changed, so its children's controller files
 * appeared or disappeared: drop negative entries everywhere and the
 * children's file entries in every view.
 */
static void
cgroupfs_views_control_changed(struct cgroup *cg)
{
	struct cgroupfs_held *held;
	struct cgroup **children;
	struct cgroup *child;
	u_int nchildren;
	u_int count;
	u_int index;

	cgroupfs_views_names_appeared();

	cgroup_lock_shared();
	nchildren = 0;
	for (child = cgroup_child_first(cg); child != NULL;
	    child = cgroup_child_next(cg, child))
		++nchildren;
	children = kmalloc(sizeof(*children) * (nchildren + 1), M_CGROUPFS,
	    M_WAITOK);
	nchildren = 0;
	for (child = cgroup_child_first(cg); child != NULL;
	    child = cgroup_child_next(cg, child)) {
		cgroup_hold(child);
		children[nchildren++] = child;
	}
	cgroup_unlock();

	held = cgroupfs_views_collect(children, nchildren, false, true,
	    &count);
	for (index = 0; index < count; ++index) {
		cache_inval_vp(held[index].vnode, 0);
		vdrop(held[index].vnode);
	}
	kfree(held, M_CGROUPFS);
	for (index = 0; index < nchildren; ++index)
		cgroup_drop(children[index]);
	kfree(children, M_CGROUPFS);
}

int
cgroupfs_view_attach(struct cgroupfs_mount *kmp)
{
	struct vnode *vp;
	int error;

	spin_init(&kmp->node_spin, "cgrpfsnode");
	RB_INIT(&kmp->nodes);
	error = cgroupfs_vnode_get(kmp, kmp->view_root, CGROUPFS_SLOT_DIR,
	    &vp);
	if (error != 0) {
		spin_uninit(&kmp->node_spin);
		return (error);
	}
	vsetflags(vp, VROOT);
	kmp->root_vnode = vp;
	lockmgr(&cgroupfs_mounts_lock, LK_EXCLUSIVE);
	LIST_INSERT_HEAD(&cgroupfs_mounts, kmp, entry);
	lockmgr(&cgroupfs_mounts_lock, LK_RELEASE);
	return (0);
}

void
cgroupfs_view_detach(struct cgroupfs_mount *kmp)
{
	lockmgr(&cgroupfs_mounts_lock, LK_EXCLUSIVE);
	LIST_REMOVE(kmp, entry);
	lockmgr(&cgroupfs_mounts_lock, LK_RELEASE);
	KKASSERT(RB_EMPTY(&kmp->nodes));
	kmp->root_vnode = NULL;
	spin_uninit(&kmp->node_spin);
}

ino_t
cgroupfs_node_inode(const struct cgroupfs_node *node)
{
	return (cgroupfs_inode(node->cgroup, CGROUPFS_SLOT_DIR));
}

/* Unlocked hint for VOP_INACTIVE; the flag only turns true. */
bool
cgroupfs_node_is_dead(const struct cgroupfs_node *node)
{
	return (cgroup_is_dead(node->cgroup));
}

/* Caller holds the hierarchy lock. */
static bool
cgroupfs_file_visible_locked(struct cgroup *cg, u_int index)
{
	struct cgroup_control control;

	if (cgroup_is_dead(cg))
		return (false);
	cgroup_control(cg, &control);
	return (cgroupfs_file_present(index, &control));
}

int
cgroupfs_node_lookup(struct cgroupfs_node *node, const char *name,
    size_t namelen, struct vnode **vpp)
{
	struct cgroup *cg = node->cgroup;
	struct cgroup *child;
	int index;
	int slot;
	int error;

	child = NULL;
	slot = CGROUPFS_SLOT_DIR;
	error = ENOENT;
	cgroup_lock_shared();
	if (cgroup_is_dead(cg))
		goto out;
	index = cgroupfs_file_find(name, namelen);
	if (index >= 0) {
		if (cgroupfs_file_visible_locked(cg, index)) {
			slot = index;
			error = 0;
		}
		goto out;
	}
	child = cgroup_child_find(cg, name, namelen);
	if (child != NULL) {
		cgroup_hold(child);
		error = 0;
	}
out:
	cgroup_unlock();
	if (error != 0)
		return (error);
	/* The directory vnode our caller holds keeps cg alive. */
	error = cgroupfs_vnode_get(node->mount, child != NULL ? child : cg,
	    slot, vpp);
	if (child != NULL)
		cgroup_drop(child);
	return (error);
}

int
cgroupfs_node_lookup_parent(struct cgroupfs_node *node, struct vnode **vpp)
{
	struct cgroup *parent;
	int error;

	/* Above the view root lies the covered filesystem. */
	if (node->cgroup == node->mount->view_root)
		return (EOPNOTSUPP);
	cgroup_lock_shared();
	if (cgroup_is_dead(node->cgroup)) {
		parent = NULL;
	} else {
		parent = cgroup_parent(node->cgroup);
		cgroup_hold(parent);
	}
	cgroup_unlock();
	if (parent == NULL)
		return (ENOENT);
	error = cgroupfs_vnode_get(node->mount, parent, CGROUPFS_SLOT_DIR,
	    vpp);
	cgroup_drop(parent);
	return (error);
}

/*
 * The uio offset is the entry position.  Copying out under the shared
 * hierarchy lock is safe: cgroupfs files cannot be mapped, so a fault on
 * the user buffer never needs a cgroupfs vnode.
 */
int
cgroupfs_node_readdir(struct cgroupfs_node *node, struct uio *uio,
    int *eofp)
{
	struct cgroup *cg = node->cgroup;
	struct cgroup *child;
	const char *name;
	ino_t inode;
	uint8_t type;
	off_t offset;
	off_t rank;
	u_int index;
	int error;

	offset = uio->uio_offset;
	if (offset < 0)
		return (EINVAL);
	*eofp = 0;
	error = 0;
	child = NULL;
	cgroup_lock_shared();
	if (cgroup_is_dead(cg))
		error = ENOENT;
	while (error == 0) {
		if (offset == 0) {
			inode = cgroupfs_inode(cg, CGROUPFS_SLOT_DIR);
			type = DT_DIR;
			name = ".";
		} else if (offset == 1) {
			inode = cgroupfs_inode(cg == node->mount->view_root ?
			    cg : cgroup_parent(cg), CGROUPFS_SLOT_DIR);
			type = DT_DIR;
			name = "..";
		} else if (offset < CGROUPFS_ENTRY_CHILDREN) {
			index = offset - CGROUPFS_ENTRY_FILES;
			inode = cgroupfs_inode(cg, index);
			type = DT_REG;
			name = cgroupfs_file_visible_locked(cg, index) ?
			    cgroupfs_file_name(index) : NULL;
		} else {
			if (child == NULL) {
				child = cgroup_child_first(cg);
				for (rank = offset - CGROUPFS_ENTRY_CHILDREN;
				    child != NULL && rank > 0; --rank)
					child = cgroup_child_next(cg, child);
			} else {
				child = cgroup_child_next(cg, child);
			}
			if (child == NULL) {
				*eofp = 1;
				break;
			}
			inode = cgroupfs_inode(child, CGROUPFS_SLOT_DIR);
			type = DT_DIR;
			name = cgroup_name(child);
		}
		if (name != NULL && vop_write_dirent(&error, uio, inode, type,
		    strlen(name), name))
			break;
		++offset;
	}
	cgroup_unlock();
	uio->uio_offset = offset;
	return (error);
}

/* Interface file names are reserved even while hidden. */
int
cgroupfs_node_mkdir(struct cgroupfs_node *node, const char *name,
    size_t namelen, struct vnode **vpp)
{
	struct cgroup *child;
	struct cgroup *removed;
	int error;

	if (cgroupfs_file_find(name, namelen) >= 0)
		return (EEXIST);
	error = cgroup_create(node->cgroup, name, namelen, &child);
	if (error != 0)
		return (error);
	cgroupfs_views_names_appeared();
	error = cgroupfs_vnode_get(node->mount, child, CGROUPFS_SLOT_DIR, vpp);
	if (error != 0 &&
	    cgroup_destroy(node->cgroup, name, namelen, &removed) == 0) {
		cgroupfs_views_group_removed(removed, NULL);
		cgroup_drop(removed);
	}
	cgroup_drop(child);
	return (error);
}

int
cgroupfs_node_rmdir(struct cgroupfs_node *node, struct nchandle *nch)
{
	struct cgroup *cg = node->cgroup;
	struct cgroup *child;
	const char *name = nch->ncp->nc_name;
	size_t namelen = nch->ncp->nc_nlen;
	int index;
	int error;

	index = cgroupfs_file_find(name, namelen);
	if (index >= 0) {
		cgroup_lock_shared();
		error = cgroupfs_file_visible_locked(cg, index) ? ENOTDIR :
		    ENOENT;
		cgroup_unlock();
		return (error);
	}
	error = cgroup_destroy(cg, name, namelen, &child);
	if (error != 0)
		return (error);
	/*
	 * The unlinked entry stays resolved so a working directory inside
	 * the removed group still reaches our VOPs and gets ENOENT.
	 */
	cache_unlink(nch);
	cgroupfs_views_group_removed(child, node->mount);
	cgroup_drop(child);
	return (0);
}

void
cgroupfs_node_reclaim(struct cgroupfs_node *node, struct vnode *vp)
{
	cgroupfs_vnode_release(node, CGROUPFS_SLOT_DIR, vp);
}

void
cgroupfs_node_file_attr(const struct cgroupfs_file *file, ino_t *inodep,
    mode_t *modep)
{
	*inodep = cgroupfs_inode(file->node->cgroup, file->index);
	*modep = cgroupfs_file_mode(file->index);
}

/*
 * Stale vnodes (removed group, controller disabled) read as ENOENT.
 * Visibility and the control snapshot are taken under the lock; the
 * content, which may be long (cgroup.procs), is formatted after it is
 * released.  Each read regenerates the whole file.
 */
int
cgroupfs_node_file_read(struct cgroupfs_file *file, struct uio *uio)
{
	struct cgroup *cg = file->node->cgroup;
	struct cgroup_control control;
	struct sbuf *sb;
	ssize_t length;
	bool visible;
	int error;

	if (uio->uio_offset < 0)
		return (EINVAL);
	cgroup_lock_shared();
	visible = cgroupfs_file_visible_locked(cg, file->index);
	if (visible)
		cgroup_control(cg, &control);
	cgroup_unlock();
	if (!visible)
		return (ENOENT);

	sb = sbuf_new_auto();
	error = cgroupfs_file_load(file->index, cg, &control, sb);
	if (error == 0 && sbuf_finish(sb) != 0)
		error = ENOMEM;
	if (error == 0) {
		length = sbuf_len(sb);
		if (uio->uio_offset < length)
			error = uiomove(sbuf_data(sb) + uio->uio_offset,
			    length - uio->uio_offset, uio);
	}
	sbuf_delete(sb);
	return (error);
}

int
cgroupfs_node_file_write(struct cgroupfs_file *file, struct uio *uio,
    struct ucred *cred)
{
	struct cgroup *cg = file->node->cgroup;
	char buffer[CGROUPFS_FILE_SIZE_MAX];
	size_t length;
	bool changed;
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
	error = cgroupfs_file_store(file->index, cg, buffer, length, cred,
	    &changed);
	if (error == 0 && changed)
		cgroupfs_views_control_changed(cg);
	return (error);
}

/* Unlocked hint for VOP_INACTIVE; the flag only turns true. */
bool
cgroupfs_node_file_is_dead(const struct cgroupfs_file *file)
{
	return (cgroup_is_dead(file->node->cgroup));
}

void
cgroupfs_node_file_reclaim(struct cgroupfs_file *file, struct vnode *vp)
{
	cgroupfs_vnode_release(file->node, file->index, vp);
}
