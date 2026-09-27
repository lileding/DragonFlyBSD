/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Control groups: the global hierarchy.
 *
 * Lifecycle: a group holds one reference for its registration in the
 * parent, and one on its parent.  Destroy unregisters an empty group and
 * marks it dead; its memory goes away with the last reference, whose
 * release only frees memory and walks up the parents without locking.
 *
 * Locking: a single hierarchy lock, ranked after every vnode and namecache
 * lock, protects topology, dead flags and control state.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cgroup.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/refcount.h>
#include <sys/tree.h>

static MALLOC_DEFINE(M_CGROUP, "cgroup", "Control groups");

const struct cgroup_controller cgroup_controllers[] = {
	{ "pids", CGROUP_CONTROLLER_PIDS },
};
const u_int cgroup_ncontrollers = nitems(cgroup_controllers);

RB_HEAD(cgroup_children, cgroup);

struct cgroup {
	struct cgroup		*parent;
	RB_ENTRY(cgroup)	sibling;
	struct cgroup_children	children;
	u_int			nchildren;
	u_int			refs;
	/* Written under the exclusive lock; only turns true. */
	volatile int		dead;
	uint64_t		id;
	uint32_t		subtree_control;
	char			name[NAME_MAX + 1];
};

static struct lock cgroup_hierarchy_lock =
    LOCK_INITIALIZER("cgroup", 0, 0);

/* The root: never registered anywhere, its one reference is never dropped. */
static struct cgroup cgroup_root_group = {
	.children = RB_INITIALIZER(cgroup_root_group.children),
	.refs = 1,
	.id = 0,
};

/* Protected by the exclusive hierarchy lock. */
static uint64_t cgroup_next_id = 1;

/* Orders a counted name against a NUL-terminated one, bytewise. */
static int
cgroup_name_compare(const char *name, size_t namelen, const char *other)
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
cgroup_compare(struct cgroup *left, struct cgroup *right)
{
	return (cgroup_name_compare(left->name, strlen(left->name),
	    right->name));
}

RB_PROTOTYPE_STATIC(cgroup_children, cgroup, sibling, cgroup_compare);
RB_GENERATE_STATIC(cgroup_children, cgroup, sibling, cgroup_compare);

void
cgroup_lock_shared(void)
{
	lockmgr(&cgroup_hierarchy_lock, LK_SHARED);
}

void
cgroup_lock_exclusive(void)
{
	lockmgr(&cgroup_hierarchy_lock, LK_EXCLUSIVE);
}

void
cgroup_unlock(void)
{
	lockmgr(&cgroup_hierarchy_lock, LK_RELEASE);
}

struct cgroup *
cgroup_root(void)
{
	return (&cgroup_root_group);
}

void
cgroup_hold(struct cgroup *cg)
{
	refcount_acquire(&cg->refs);
}

void
cgroup_drop(struct cgroup *cg)
{
	struct cgroup *parent;

	while (cg != NULL && refcount_release(&cg->refs)) {
		KKASSERT(cg != &cgroup_root_group);
		KKASSERT(cg->dead);
		KKASSERT(RB_EMPTY(&cg->children));
		parent = cg->parent;
		kfree(cg, M_CGROUP);
		cg = parent;
	}
}

uint64_t
cgroup_id(const struct cgroup *cg)
{
	return (cg->id);
}

struct cgroup *
cgroup_parent(const struct cgroup *cg)
{
	return (cg->parent);
}

bool
cgroup_is_dead(const struct cgroup *cg)
{
	return (cg->dead != 0);
}

uint32_t
cgroup_controller_all(void)
{
	uint32_t mask;
	u_int index;

	mask = 0;
	for (index = 0; index < cgroup_ncontrollers; ++index)
		mask |= cgroup_controllers[index].bit;
	return (mask);
}

uint32_t
cgroup_controller_find(const char *name, size_t namelen)
{
	u_int index;

	for (index = 0; index < cgroup_ncontrollers; ++index) {
		if (cgroup_name_compare(name, namelen,
		    cgroup_controllers[index].name) == 0)
			return (cgroup_controllers[index].bit);
	}
	return (0);
}

const char *
cgroup_name(const struct cgroup *cg)
{
	return (cg->name);
}

struct cgroup *
cgroup_child_find(struct cgroup *parent, const char *name, size_t namelen)
{
	struct cgroup *child;
	int cmp;

	child = RB_ROOT(&parent->children);
	while (child != NULL) {
		cmp = cgroup_name_compare(name, namelen, child->name);
		if (cmp == 0)
			return (child);
		child = cmp < 0 ? RB_LEFT(child, sibling) :
		    RB_RIGHT(child, sibling);
	}
	return (NULL);
}

struct cgroup *
cgroup_child_first(struct cgroup *parent)
{
	return (RB_MIN(cgroup_children, &parent->children));
}

struct cgroup *
cgroup_child_next(struct cgroup *parent, struct cgroup *child)
{
	return (RB_NEXT(cgroup_children, &parent->children, child));
}

void
cgroup_control(struct cgroup *cg, struct cgroup_control *control)
{
	struct cgroup *child;

	control->is_root = cg == &cgroup_root_group;
	control->available = cg->parent != NULL ?
	    cg->parent->subtree_control : cgroup_controller_all();
	control->subtree_control = cg->subtree_control;
	control->children_subtree_control = 0;
	RB_FOREACH(child, cgroup_children, &cg->children)
		control->children_subtree_control |= child->subtree_control;
}

int
cgroup_create(struct cgroup *parent, const char *name, size_t namelen,
    struct cgroup **childp)
{
	struct cgroup *child;
	int error;

	if (namelen == 0)
		return (EINVAL);
	if (namelen > NAME_MAX)
		return (ENAMETOOLONG);
	child = kmalloc(sizeof(*child), M_CGROUP, M_WAITOK | M_ZERO);
	RB_INIT(&child->children);
	bcopy(name, child->name, namelen);
	child->name[namelen] = '\0';
	child->refs = 2;	/* registration and caller */

	cgroup_lock_exclusive();
	if (parent->dead) {
		error = ENOENT;
	} else if (cgroup_child_find(parent, name, namelen) != NULL) {
		error = EEXIST;
	} else {
		child->id = cgroup_next_id++;
		child->parent = parent;
		cgroup_hold(parent);
		RB_INSERT(cgroup_children, &parent->children, child);
		++parent->nchildren;
		error = 0;
	}
	cgroup_unlock();

	if (error != 0) {
		kfree(child, M_CGROUP);
		return (error);
	}
	*childp = child;
	return (0);
}

int
cgroup_destroy(struct cgroup *parent, const char *name, size_t namelen,
    struct cgroup **childp)
{
	struct cgroup *child;
	int error;

	cgroup_lock_exclusive();
	child = NULL;
	if (parent->dead) {
		error = ENOENT;
	} else if ((child = cgroup_child_find(parent, name, namelen)) ==
	    NULL) {
		error = ENOENT;
	} else if (!RB_EMPTY(&child->children)) {
		error = EBUSY;
	} else {
		RB_REMOVE(cgroup_children, &parent->children, child);
		--parent->nchildren;
		child->dead = 1;
		error = 0;
	}
	cgroup_unlock();

	if (error == 0)
		*childp = child;
	return (error);
}

int
cgroup_control_update(struct cgroup *cg, uint32_t enable, uint32_t disable,
    bool *changedp)
{
	struct cgroup_control control;
	uint32_t updated;
	int error;

	KKASSERT((enable & disable) == 0);
	*changedp = false;
	cgroup_lock_exclusive();
	cgroup_control(cg, &control);
	if (cg->dead) {
		error = ENOENT;
	} else if ((enable & ~control.available) != 0) {
		error = ENOENT;
	} else if ((disable & control.children_subtree_control) != 0) {
		error = EBUSY;
	} else {
		updated = (cg->subtree_control | enable) & ~disable;
		*changedp = updated != cg->subtree_control;
		cg->subtree_control = updated;
		error = 0;
	}
	cgroup_unlock();
	return (error);
}
