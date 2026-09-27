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
 *
 * Membership: p_cgroup holds a group reference from process creation until
 * the zombie is reaped; nprocs counts live members (until exit1()).  A
 * process's p_cgroup changes only under its p_token without blocking, so
 * fork's read-and-hold of the parent's group is atomic against migration
 * and exit.  A live parent is itself counted in its group, which therefore
 * cannot be destroyed underneath fork.
 *
 * Kill: every non-root group has a fork gate.  fork1() holds the gate of
 * the child's group shared from charging until the child runs; kill takes
 * the gates of its subtree exclusively, which waits out forks in flight,
 * closes the groups, and then signals every member in a single scan.  A
 * closed group refuses forks and migration until its subtree has no live
 * process left (nlive), because a process already sent SIGKILL may still
 * enter fork() before the signal is acted upon.
 *
 * pids: each group counts the processes of its subtree, from fork until
 * reap.  Charging increments every level up to the root and backs out if
 * any exceeds its limit.  A limit other than unlimited only exists where
 * the parent enables the pids controller: disabling it resets the
 * children's limits, and setting one requires it.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cgroup.h>
#include <sys/event.h>
#include <sys/kernel.h>
#include <sys/socket.h>		/* struct prison embeds sockaddr_storage */
#include <sys/jail.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/refcount.h>
#include <sys/signalvar.h>
#include <sys/tree.h>

#include <machine/atomic.h>

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
	/* Live member processes; atomic. */
	u_int			nprocs;
	/* Processes of the subtree until reaped; atomic. */
	u_int			pids;
	/* Processes of the subtree until they exit; atomic. */
	u_int			nlive;
	/* Held shared by forks in flight, exclusive by kill; unused by root. */
	struct lock		gate;
	/* Set by kill, cleared when nlive drops to zero; atomic. */
	volatile u_int		closed;
	/* Prisons whose group this is; atomic. */
	u_int			pins;
	/* cgroup.events waiters. */
	struct kqinfo		events;
	/* Written under the exclusive lock, read locklessly by fork. */
	u_int			pids_max;
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
	.pids_max = CGROUP_PIDS_UNLIMITED,
	.gate = LOCK_INITIALIZER("cgrpgate", 0, 0),
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
		KKASSERT(SLIST_EMPTY(&cg->events.ki_note));
		parent = cg->parent;
		lockuninit(&cg->gate);
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
	child->pids_max = CGROUP_PIDS_UNLIMITED;
	lockinit(&child->gate, "cgrpgate", 0, 0);

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
		lockuninit(&child->gate);
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
	} else if (!RB_EMPTY(&child->children) || child->nprocs != 0 ||
	    child->pins != 0) {
		error = EBUSY;
	} else {
		RB_REMOVE(cgroup_children, &parent->children, child);
		--parent->nchildren;
		child->dead = 1;
		error = 0;
	}
	cgroup_unlock();

	if (error == 0) {
		KNOTE(&child->events.ki_note, NOTE_DELETE);
		*childp = child;
	}
	return (error);
}

int
cgroup_control_update(struct cgroup *cg, uint32_t enable, uint32_t disable,
    bool *changedp)
{
	struct cgroup_control control;
	struct cgroup *child;
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
		/* A disabled controller forgets the children's limits. */
		if ((cg->subtree_control & ~updated &
		    CGROUP_CONTROLLER_PIDS) != 0) {
			RB_FOREACH(child, cgroup_children, &cg->children)
				child->pids_max = CGROUP_PIDS_UNLIMITED;
		}
		cg->subtree_control = updated;
		error = 0;
	}
	cgroup_unlock();
	return (error);
}

/* Adds one process to every level from cg up to, not including, stop. */
static void
cgroup_pids_add(struct cgroup *cg, struct cgroup *stop)
{
	for (; cg != stop; cg = cg->parent)
		atomic_add_int(&cg->pids, 1);
}

static void
cgroup_pids_sub(struct cgroup *cg, struct cgroup *stop)
{
	for (; cg != stop; cg = cg->parent)
		atomic_add_int(&cg->pids, -1);
}

/*
 * nlive flipping between zero and non-zero is a populated change.  Notes
 * may arrive out of order under concurrency; each only means "re-read".
 * KNOTE takes only pool tokens, so any caller's context is fine.
 */
static void
cgroup_nlive_add(struct cgroup *cg, struct cgroup *stop)
{
	for (; cg != stop; cg = cg->parent) {
		if (atomic_fetchadd_int(&cg->nlive, 1) == 0)
			KNOTE(&cg->events.ki_note, NOTE_WRITE);
	}
}

/*
 * The last live process leaving a closed subtree reopens it.  Kill closes
 * before it samples nlive and we decrement before we sample closed, both
 * with locked instructions, so one of the two always reopens.
 */
static void
cgroup_nlive_sub(struct cgroup *cg, struct cgroup *stop)
{
	for (; cg != stop; cg = cg->parent) {
		if (atomic_fetchadd_int(&cg->nlive, -1) != 1)
			continue;
		if (cg->closed)
			atomic_clear_int(&cg->closed, 1);
		KNOTE(&cg->events.ki_note, NOTE_WRITE);
	}
}

/*
 * Charges one new process to cg and all its ancestors, or none of them if
 * a level would exceed its limit.  Concurrent charges may both overshoot
 * transiently and back out, as Linux's pids_try_charge() does.
 */
static int
cgroup_pids_try_charge(struct cgroup *cg)
{
	struct cgroup *level;

	for (level = cg; level != NULL; level = level->parent) {
		if (atomic_fetchadd_int(&level->pids, 1) + 1 >
		    level->pids_max) {
			cgroup_pids_sub(cg, level->parent);
			return (EAGAIN);
		}
	}
	return (0);
}

/* Whether g is top or one of its descendants. */
static bool
cgroup_is_within(struct cgroup *g, struct cgroup *top)
{
	for (; g != NULL; g = g->parent) {
		if (g == top)
			return (true);
	}
	return (false);
}

/* Caller holds the hierarchy lock, which pins both ancestries. */
static struct cgroup *
cgroup_common_ancestor(struct cgroup *left, struct cgroup *right)
{
	struct cgroup *a;
	struct cgroup *b;

	for (a = left; a != NULL; a = a->parent) {
		for (b = right; b != NULL; b = b->parent) {
			if (a == b)
				return (a);
		}
	}
	panic("cgroup: groups %p and %p share no root", left, right);
}

u_int
cgroup_pids_current(const struct cgroup *cg)
{
	/* An aligned word read; a snapshot is all readers need. */
	return (cg->pids);
}

u_int
cgroup_pids_max(const struct cgroup *cg)
{
	return (cg->pids_max);
}

int
cgroup_pids_set_max(struct cgroup *cg, u_int max)
{
	int error;

	cgroup_lock_exclusive();
	if (cg->dead || cg->parent == NULL ||
	    (cg->parent->subtree_control & CGROUP_CONTROLLER_PIDS) == 0) {
		error = ENOENT;
	} else {
		cg->pids_max = max;
		error = 0;
	}
	cgroup_unlock();
	return (error);
}

void
cgroup_proc_init0(struct proc *p)
{
	struct cgroup *root = &cgroup_root_group;

	KKASSERT(p->p_cgroup == NULL);
	cgroup_hold(root);
	atomic_add_int(&root->nprocs, 1);
	cgroup_pids_add(root, NULL);
	cgroup_nlive_add(root, NULL);
	p->p_cgroup = root;
}

int
cgroup_proc_fork(struct proc *parent, struct cgroup **cgp)
{
	struct cgroup *root = &cgroup_root_group;
	struct cgroup *cg;
	int error;

	/*
	 * Taking a gate may block, which releases parent's p_token: pin the
	 * group, then recheck that the parent still belongs to it.  The
	 * pinning reference becomes the child's.
	 */
	for (;;) {
		if (parent->p_flags & P_WEXIT)
			return (EAGAIN);
		cg = parent->p_cgroup;
		KKASSERT(cg != NULL);
		cgroup_hold(cg);
		if (cg == root)
			break;
		lockmgr(&cg->gate, LK_SHARED);
		if (parent->p_cgroup == cg && (parent->p_flags & P_WEXIT) == 0)
			break;
		lockmgr(&cg->gate, LK_RELEASE);
		cgroup_drop(cg);
	}

	/* Nothing below blocks. */
	if (cg->closed)
		error = EAGAIN;
	else
		error = cgroup_pids_try_charge(cg);
	if (error != 0) {
		if (cg != root)
			lockmgr(&cg->gate, LK_RELEASE);
		cgroup_drop(cg);
		return (error);
	}
	cgroup_nlive_add(cg, NULL);
	atomic_add_int(&cg->nprocs, 1);
	*cgp = cg;
	return (0);
}

/* The child runs: kill may now see it on the allproc list. */
void
cgroup_proc_started(struct proc *p)
{
	if (p->p_cgroup != &cgroup_root_group)
		lockmgr(&p->p_cgroup->gate, LK_RELEASE);
}

void
cgroup_proc_fork_abort(struct cgroup *cg)
{
	if (cg != &cgroup_root_group)
		lockmgr(&cg->gate, LK_RELEASE);
}

void
cgroup_proc_exit(struct proc *p)
{
	KKASSERT(p->p_flags & P_WEXIT);
	atomic_add_int(&p->p_cgroup->nprocs, -1);
	cgroup_nlive_sub(p->p_cgroup, NULL);
}

void
cgroup_proc_reap(struct proc *p)
{
	struct cgroup *cg = p->p_cgroup;

	p->p_cgroup = NULL;
	/* A removed group's parent chain stays valid while it is held. */
	cgroup_pids_sub(cg, NULL);
	cgroup_drop(cg);
}

/*
 * The hierarchy lock is taken before p_token: an LWKT token is released
 * while its holder blocks, so nothing may block inside the token section.
 * Holding the hierarchy lock serializes against destroy.
 */
int
cgroup_proc_migrate(struct cgroup *cg, pid_t pid, struct ucred *cred)
{
	struct cgroup *common;
	struct cgroup *old;
	struct proc *p;
	int error;

	if (pid == 0) {
		p = curproc;
		PHOLD(p);
	} else {
		p = pfind(pid);
		if (p == NULL)
			return (ESRCH);
	}
	if (p->p_flags & P_SYSTEM) {
		error = EINVAL;
		goto out;
	}
	error = p_trespass(cred, p->p_ucred);
	if (error != 0)
		goto out;

	old = NULL;
	cgroup_lock_exclusive();
	if (cg->dead) {
		error = ENOENT;
	} else {
		lwkt_gettoken(&p->p_token);
		if (p->p_flags & P_WEXIT) {
			error = ESRCH;
		} else if (p->p_stat == SIDL) {
			/* Its fork still holds the gate of its group. */
			error = EBUSY;
		} else if (p->p_ucred->cr_prison != NULL &&
		    !cgroup_is_within(cg, p->p_ucred->cr_prison->pr_cgroup)) {
			/* A jail's processes stay in its group's subtree. */
			error = EPERM;
		} else if (p->p_cgroup != cg &&
		    (p->p_cgroup->closed || cg->closed)) {
			/* Neither escaping nor joining a group being killed. */
			error = EBUSY;
		} else if (p->p_cgroup != cg) {
			old = p->p_cgroup;
			/*
			 * Move the pids charge exactly: only the levels below
			 * the common ancestor change, so no level ever over-
			 * or under-counts.  Migration is not limited.
			 */
			common = cgroup_common_ancestor(old, cg);
			cgroup_pids_sub(old, common);
			cgroup_pids_add(cg, common);
			cgroup_nlive_add(cg, common);
			cgroup_nlive_sub(old, common);
			cgroup_hold(cg);
			atomic_add_int(&cg->nprocs, 1);
			p->p_cgroup = cg;
			atomic_add_int(&old->nprocs, -1);
		}
		lwkt_reltoken(&p->p_token);
	}
	cgroup_unlock();
	if (old != NULL)
		cgroup_drop(old);
out:
	PRELE(p);
	return (error);
}

struct cgroup_procs_scan {
	struct cgroup	*cgroup;
	pid_t		*pids;
	u_int		count;
	u_int		capacity;
	bool		overflow;
};

/* p_cgroup is only compared, so an unlocked snapshot suffices. */
static int
cgroup_procs_scan_callback(struct proc *p, void *data)
{
	struct cgroup_procs_scan *scan = data;

	if (p->p_cgroup != scan->cgroup || p->p_stat == SIDL)
		return (0);
	if (scan->count == scan->capacity) {
		scan->overflow = true;
		return (-1);
	}
	scan->pids[scan->count++] = p->p_pid;
	return (0);
}

void
cgroup_procs_snapshot(struct cgroup *cg, pid_t **pidsp, u_int *countp)
{
	struct cgroup_procs_scan scan;

	scan.cgroup = cg;
	scan.capacity = cg->nprocs + 16;
	for (;;) {
		scan.pids = kmalloc(sizeof(*scan.pids) * scan.capacity,
		    M_CGROUP, M_WAITOK);
		scan.count = 0;
		scan.overflow = false;
		allproc_scan(cgroup_procs_scan_callback, &scan, 0);
		if (!scan.overflow)
			break;
		kfree(scan.pids, M_CGROUP);
		scan.capacity *= 2;
	}
	*pidsp = scan.pids;
	*countp = scan.count;
}

void
cgroup_procs_free(pid_t *pids)
{
	kfree(pids, M_CGROUP);
}

/* Preorder successor of g within the subtree rooted at top, or NULL. */
static struct cgroup *
cgroup_subtree_next(struct cgroup *top, struct cgroup *g)
{
	struct cgroup *next;

	if ((next = RB_MIN(cgroup_children, &g->children)) != NULL)
		return (next);
	while (g != top) {
		next = RB_NEXT(cgroup_children, &g->parent->children, g);
		if (next != NULL)
			return (next);
		g = g->parent;
	}
	return (NULL);
}

/* The hierarchy lock keeps every p_cgroup and parent chain stable. */
static int
cgroup_kill_callback(struct proc *p, void *data)
{
	struct cgroup *target = data;
	struct cgroup *level;

	if (p->p_flags & P_SYSTEM)
		return (0);
	for (level = p->p_cgroup; level != NULL; level = level->parent) {
		if (level == target) {
			ksignal(p, SIGKILL);
			break;
		}
	}
	return (0);
}

int
cgroup_kill(struct cgroup *cg)
{
	struct cgroup **groups;
	struct cgroup *g;
	u_int count;
	u_int index;

	if (cg == &cgroup_root_group)
		return (EINVAL);
	/* Shared: keeps migration, mkdir and rmdir out until we are done. */
	cgroup_lock_shared();
	if (cg->dead) {
		cgroup_unlock();
		return (ENOENT);
	}
	count = 0;
	for (g = cg; g != NULL; g = cgroup_subtree_next(cg, g))
		++count;
	groups = kmalloc(sizeof(*groups) * count, M_CGROUP, M_WAITOK);
	index = 0;
	for (g = cg; g != NULL; g = cgroup_subtree_next(cg, g))
		groups[index++] = g;

	/*
	 * Parents first.  The exclusive gate waits for forks in flight, whose
	 * children are then running and visible to the scan; once closed, no
	 * further fork or migration adds a member.
	 */
	for (index = 0; index < count; ++index) {
		lockmgr(&groups[index]->gate, LK_EXCLUSIVE);
		atomic_set_int(&groups[index]->closed, 1);
		lockmgr(&groups[index]->gate, LK_RELEASE);
	}
	allproc_scan(cgroup_kill_callback, cg, 0);
	/* Groups that were already empty will see no exit to reopen them. */
	for (index = 0; index < count; ++index) {
		if (groups[index]->nlive == 0)
			atomic_clear_int(&groups[index]->closed, 1);
	}
	cgroup_unlock();
	kfree(groups, M_CGROUP);
	return (0);
}

/*
 * The caller is a live member of its group, so the group cannot be
 * destroyed before the pin is counted.
 */
struct cgroup *
cgroup_pin_current(void)
{
	struct proc *p = curproc;
	struct cgroup *cg;

	lwkt_gettoken(&p->p_token);
	cg = p->p_cgroup;
	cgroup_hold(cg);
	atomic_add_int(&cg->pins, 1);
	lwkt_reltoken(&p->p_token);
	return (cg);
}

void
cgroup_unpin(struct cgroup *cg)
{
	atomic_add_int(&cg->pins, -1);
	cgroup_drop(cg);
}

bool
cgroup_events_populated(const struct cgroup *cg)
{
	return (cg->nlive != 0);
}

static void
filt_cgroup_detach(struct knote *kn)
{
	struct cgroup *cg = (struct cgroup *)kn->kn_hook;

	knote_remove(&cg->events.ki_note, kn);
}

static int
filt_cgroup_vnode(struct knote *kn, long hint)
{
	if (kn->kn_sfflags & hint)
		kn->kn_fflags |= hint;
	return (kn->kn_fflags != 0);
}

/* poll(POLLPRI) arrives as EVFILT_EXCEPT: any change is exceptional. */
static int
filt_cgroup_except(struct knote *kn, long hint)
{
	if (hint != 0)
		kn->kn_fflags |= NOTE_OOB;
	return (kn->kn_fflags != 0);
}

static int
filt_cgroup_ready(struct knote *kn, long hint)
{
	(void)kn;
	(void)hint;
	return (1);
}

static struct filterops cgroup_vnode_filtops =
	{ FILTEROP_ISFD | FILTEROP_MPSAFE,
	  NULL, filt_cgroup_detach, filt_cgroup_vnode };
static struct filterops cgroup_except_filtops =
	{ FILTEROP_ISFD | FILTEROP_MPSAFE,
	  NULL, filt_cgroup_detach, filt_cgroup_except };
static struct filterops cgroup_ready_filtops =
	{ FILTEROP_ISFD | FILTEROP_MPSAFE,
	  NULL, filt_cgroup_detach, filt_cgroup_ready };

/*
 * The knote's file keeps its vnode, the view node and so this group alive
 * until the knote is detached.
 */
int
cgroup_events_kqfilter(struct cgroup *cg, struct knote *kn)
{
	switch (kn->kn_filter) {
	case EVFILT_VNODE:
		kn->kn_fop = &cgroup_vnode_filtops;
		break;
	case EVFILT_EXCEPT:
		kn->kn_fop = &cgroup_except_filtops;
		break;
	case EVFILT_READ:
	case EVFILT_WRITE:
		kn->kn_fop = &cgroup_ready_filtops;
		break;
	default:
		return (EOPNOTSUPP);
	}
	kn->kn_hook = (caddr_t)cg;
	knote_insert(&cg->events.ki_note, kn);
	return (0);
}
