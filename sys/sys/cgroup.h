/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Control groups: the kernel's single, global control group hierarchy.
 * Filesystem views (cgroupfs) and process accounting build on this
 * interface; the tree itself does not depend on any view being mounted.
 */
#ifndef _SYS_CGROUP_H_
#define _SYS_CGROUP_H_

#if !defined(_KERNEL)
#error "This file should not be included by userland programs."
#endif

#include <sys/types.h>
#include <machine/limits.h>

struct cgroup;
struct proc;
struct ucred;

#define CGROUP_CONTROLLER_PIDS	0x00000001U

/* pids.max value meaning "max": no limit. */
#define CGROUP_PIDS_UNLIMITED	UINT_MAX

struct cgroup_controller {
	const char	*name;
	uint32_t	bit;
};

extern const struct cgroup_controller cgroup_controllers[];
extern const u_int cgroup_ncontrollers;

/* Controller state of one group, as seen by its interface files. */
struct cgroup_control {
	bool		is_root;
	/* Controllers this group may use: all for the root. */
	uint32_t	available;
	uint32_t	subtree_control;
	/* Union of the children's subtree_control. */
	uint32_t	children_subtree_control;
};

/*
 * The hierarchy lock protects topology, dead flags and control state.  It
 * ranks after every vnode and namecache lock, and releasing a group
 * reference never takes it.
 */
void		cgroup_lock_shared(void);
void		cgroup_lock_exclusive(void);
void		cgroup_unlock(void);

/* The root group always exists and is never destroyed. */
struct cgroup	*cgroup_root(void);
void		cgroup_hold(struct cgroup *);
/* Frees the group and releases its parent on the last reference. */
void		cgroup_drop(struct cgroup *);

/* Immutable after creation; no lock needed. */
uint64_t	cgroup_id(const struct cgroup *);
struct cgroup	*cgroup_parent(const struct cgroup *);
/* Only turns from false to true; unlocked reads are hints. */
bool		cgroup_is_dead(const struct cgroup *);
uint32_t	cgroup_controller_all(void);
uint32_t	cgroup_controller_find(const char *, size_t);

/* Caller holds the hierarchy lock; results are valid until released. */
const char	*cgroup_name(const struct cgroup *);
struct cgroup	*cgroup_child_find(struct cgroup *, const char *, size_t);
struct cgroup	*cgroup_child_first(struct cgroup *);
struct cgroup	*cgroup_child_next(struct cgroup *, struct cgroup *);
void		cgroup_control(struct cgroup *, struct cgroup_control *);

/*
 * The following take the hierarchy lock themselves.
 *
 * create returns the new child with a reference for the caller.  destroy
 * unregisters an empty child and hands the registration's reference to
 * the caller.  control_update applies enable/disable masks with cgroup v2
 * rules (ENOENT: not available; EBUSY: still enabled by a child) and
 * reports whether subtree_control changed.
 */
int		cgroup_create(struct cgroup *, const char *, size_t,
		    struct cgroup **);
int		cgroup_destroy(struct cgroup *, const char *, size_t,
		    struct cgroup **);
int		cgroup_control_update(struct cgroup *, uint32_t, uint32_t,
		    bool *);

/*
 * Process membership.  Every process belongs to exactly one group through
 * p_cgroup, which holds a reference from creation until the zombie is
 * reaped.  p_cgroup changes only under the process's p_token, without
 * blocking in between.
 */

/* proc0 joins the root group. */
void		cgroup_proc_init0(struct proc *);
/*
 * fork1(), parent's p_token held: returns the parent's group, held and
 * counted, for the child.  Fails only for an exiting parent.
 */
int		cgroup_proc_fork(struct proc *, struct cgroup **);
/* exit1(), p_token held and P_WEXIT set: the process stops counting. */
void		cgroup_proc_exit(struct proc *);
/* The zombie is reaped: releases its group. */
void		cgroup_proc_reap(struct proc *);
/* Moves process pid (0: the caller) into the group. */
int		cgroup_proc_migrate(struct cgroup *, pid_t, struct ucred *);
/*
 * pids controller.  current counts the processes of the whole subtree,
 * including forks in progress and unreaped zombies.  A fork fails with
 * EAGAIN when any group from the child's up to the root would exceed its
 * max; migration moves the count without checking.  set_max requires the
 * parent to have the controller enabled (ENOENT otherwise).
 */
u_int		cgroup_pids_current(const struct cgroup *);
u_int		cgroup_pids_max(const struct cgroup *);
int		cgroup_pids_set_max(struct cgroup *, u_int);

/* Live members' pids; free the array with cgroup_procs_free(). */
void		cgroup_procs_snapshot(struct cgroup *, pid_t **, u_int *);
void		cgroup_procs_free(pid_t *);

#endif /* _SYS_CGROUP_H_ */
