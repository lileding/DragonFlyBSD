/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly cgroupfs: interface file table.
 *
 * Translates between file text and the kernel's control group state: load
 * formats a control snapshot, store parses a write and hands it to the
 * kernel, which applies the cgroup rules.  Object lifetime and locking
 * belong to cgroupfs_node.c.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cgroup.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/sbuf.h>

#include "cgroupfs.h"

typedef int (*cgroupfs_file_load_t)(struct cgroup *,
	const struct cgroup_control *, struct sbuf *);
typedef int (*cgroupfs_file_store_t)(struct cgroup *, const char *, size_t,
	struct ucred *, bool *);

struct cgroupfs_file_desc {
	const char		*name;
	mode_t			mode;
	/* Zero for core files present in every group. */
	uint32_t		controller;
	/* Absent from the root group. */
	bool			nonroot;
	/* Supports change notification through kqueue and poll. */
	bool			notify;
	cgroupfs_file_load_t	load;
	/* NULL exactly when the mode grants no write access. */
	cgroupfs_file_store_t	store;
};

static int cgroupfs_load_controllers(struct cgroup *,
	const struct cgroup_control *, struct sbuf *);
static int cgroupfs_load_unreadable(struct cgroup *,
	const struct cgroup_control *, struct sbuf *);
static int cgroupfs_load_events(struct cgroup *,
	const struct cgroup_control *, struct sbuf *);
static int cgroupfs_load_procs(struct cgroup *, const struct cgroup_control *,
	struct sbuf *);
static int cgroupfs_load_subtree_control(struct cgroup *,
	const struct cgroup_control *, struct sbuf *);
static int cgroupfs_load_pids_current(struct cgroup *,
	const struct cgroup_control *, struct sbuf *);
static int cgroupfs_load_pids_max(struct cgroup *,
	const struct cgroup_control *, struct sbuf *);
static int cgroupfs_store_kill(struct cgroup *, const char *, size_t,
	struct ucred *, bool *);
static int cgroupfs_store_procs(struct cgroup *, const char *, size_t,
	struct ucred *, bool *);
static int cgroupfs_store_subtree_control(struct cgroup *, const char *,
	size_t, struct ucred *, bool *);
static int cgroupfs_store_pids_max(struct cgroup *, const char *, size_t,
	struct ucred *, bool *);

static const struct cgroupfs_file_desc cgroupfs_files[] = {
	{ "cgroup.controllers", 0444, 0, false, false,
	    cgroupfs_load_controllers, NULL },
	{ "cgroup.events", 0444, 0, true, true,
	    cgroupfs_load_events, NULL },
	{ "cgroup.kill", 0200, 0, true, false,
	    cgroupfs_load_unreadable, cgroupfs_store_kill },
	{ "cgroup.procs", 0644, 0, false, false,
	    cgroupfs_load_procs, cgroupfs_store_procs },
	{ "cgroup.subtree_control", 0644, 0, false, false,
	    cgroupfs_load_subtree_control, cgroupfs_store_subtree_control },
	{ "pids.current", 0444, CGROUP_CONTROLLER_PIDS, true, false,
	    cgroupfs_load_pids_current, NULL },
	{ "pids.max", 0644, CGROUP_CONTROLLER_PIDS, true, false,
	    cgroupfs_load_pids_max, cgroupfs_store_pids_max },
};

CTASSERT(nitems(cgroupfs_files) == CGROUPFS_FILE_COUNT);

const char *
cgroupfs_file_name(u_int index)
{
	KKASSERT(index < nitems(cgroupfs_files));
	return (cgroupfs_files[index].name);
}

mode_t
cgroupfs_file_mode(u_int index)
{
	KKASSERT(index < nitems(cgroupfs_files));
	return (cgroupfs_files[index].mode);
}

bool
cgroupfs_file_notifies(u_int index)
{
	KKASSERT(index < nitems(cgroupfs_files));
	return (cgroupfs_files[index].notify);
}

int
cgroupfs_file_find(const char *name, size_t namelen)
{
	u_int index;

	for (index = 0; index < nitems(cgroupfs_files); ++index) {
		if (strlen(cgroupfs_files[index].name) == namelen &&
		    bcmp(cgroupfs_files[index].name, name, namelen) == 0)
			return (index);
	}
	return (-1);
}

/*
 * Follows Linux: controller interface files, like cgroup.kill, never
 * appear on the root, and controller files only where the parent has
 * enabled the controller for its subtree.
 */
bool
cgroupfs_file_present(u_int index, const struct cgroup_control *control)
{
	const struct cgroupfs_file_desc *desc;

	KKASSERT(index < nitems(cgroupfs_files));
	desc = &cgroupfs_files[index];
	if (desc->nonroot && control->is_root)
		return (false);
	return (desc->controller == 0 ||
	    (control->available & desc->controller) != 0);
}

int
cgroupfs_file_load(u_int index, struct cgroup *cg,
    const struct cgroup_control *control, struct sbuf *sb)
{
	KKASSERT(index < nitems(cgroupfs_files));
	return (cgroupfs_files[index].load(cg, control, sb));
}

int
cgroupfs_file_store(u_int index, struct cgroup *cg, const char *buffer,
    size_t length, struct ucred *cred, bool *changedp)
{
	KKASSERT(index < nitems(cgroupfs_files));
	KKASSERT(cgroupfs_files[index].store != NULL);
	*changedp = false;
	return (cgroupfs_files[index].store(cg, buffer, length, cred,
	    changedp));
}

static bool
cgroupfs_is_space(char c)
{
	return (c == ' ' || c == '\t' || c == '\n');
}

/* Space-separated controller names in table order, newline terminated. */
static int
cgroupfs_format_mask(uint32_t mask, struct sbuf *sb)
{
	const char *separator;
	u_int index;

	separator = "";
	for (index = 0; index < cgroup_ncontrollers; ++index) {
		if ((mask & cgroup_controllers[index].bit) == 0)
			continue;
		sbuf_printf(sb, "%s%s", separator,
		    cgroup_controllers[index].name);
		separator = " ";
	}
	sbuf_printf(sb, "\n");
	return (0);
}

static int
cgroupfs_load_controllers(struct cgroup *cg,
    const struct cgroup_control *control, struct sbuf *sb)
{
	(void)cg;
	return (cgroupfs_format_mask(control->available, sb));
}

static int
cgroupfs_load_events(struct cgroup *cg, const struct cgroup_control *control,
    struct sbuf *sb)
{
	(void)control;
	sbuf_printf(sb, "populated %d\n", cgroup_events_populated(cg) ? 1 : 0);
	return (0);
}

/* Write-only files. */
static int
cgroupfs_load_unreadable(struct cgroup *cg,
    const struct cgroup_control *control, struct sbuf *sb)
{
	(void)cg;
	(void)control;
	(void)sb;
	return (EINVAL);
}

static int
cgroupfs_load_subtree_control(struct cgroup *cg,
    const struct cgroup_control *control, struct sbuf *sb)
{
	(void)cg;
	return (cgroupfs_format_mask(control->subtree_control, sb));
}

static int
cgroupfs_load_procs(struct cgroup *cg, const struct cgroup_control *control,
    struct sbuf *sb)
{
	pid_t *pids;
	u_int count;
	u_int index;

	(void)control;
	cgroup_procs_snapshot(cg, &pids, &count);
	for (index = 0; index < count; ++index)
		sbuf_printf(sb, "%d\n", pids[index]);
	cgroup_procs_free(pids);
	return (0);
}

static int
cgroupfs_load_pids_current(struct cgroup *cg,
    const struct cgroup_control *control, struct sbuf *sb)
{
	(void)control;
	sbuf_printf(sb, "%u\n", cgroup_pids_current(cg));
	return (0);
}

static int
cgroupfs_load_pids_max(struct cgroup *cg, const struct cgroup_control *control,
    struct sbuf *sb)
{
	u_int max;

	(void)control;
	max = cgroup_pids_max(cg);
	if (max == CGROUP_PIDS_UNLIMITED)
		sbuf_printf(sb, "max\n");
	else
		sbuf_printf(sb, "%u\n", max);
	return (0);
}

/* "max" or a decimal count up to INT_MAX, optionally newline terminated. */
static int
cgroupfs_store_pids_max(struct cgroup *cg, const char *buffer, size_t length,
    struct ucred *cred, bool *changedp)
{
	u_int max;
	size_t position;

	(void)cred;
	(void)changedp;
	if (length > 0 && buffer[length - 1] == '\n')
		--length;
	if (length == sizeof("max") - 1 &&
	    bcmp(buffer, "max", sizeof("max") - 1) == 0)
		return (cgroup_pids_set_max(cg, CGROUP_PIDS_UNLIMITED));
	if (length == 0)
		return (EINVAL);
	max = 0;
	for (position = 0; position < length; ++position) {
		if (buffer[position] < '0' || buffer[position] > '9')
			return (EINVAL);
		max = max * 10 + (buffer[position] - '0');
		if (max > INT_MAX)
			return (EINVAL);
	}
	return (cgroup_pids_set_max(cg, max));
}

/* "1", optionally newline terminated, kills the subtree. */
static int
cgroupfs_store_kill(struct cgroup *cg, const char *buffer, size_t length,
    struct ucred *cred, bool *changedp)
{
	(void)cred;
	(void)changedp;
	if (length > 0 && buffer[length - 1] == '\n')
		--length;
	if (length != 1 || buffer[0] != '1')
		return (EINVAL);
	return (cgroup_kill(cg));
}

/*
 * One decimal pid per write, optionally newline terminated; 0 names the
 * writer.  Migration changes no directory entry.
 */
static int
cgroupfs_store_procs(struct cgroup *cg, const char *buffer, size_t length,
    struct ucred *cred, bool *changedp)
{
	pid_t pid;
	size_t position;

	(void)changedp;
	if (length > 0 && buffer[length - 1] == '\n')
		--length;
	if (length == 0)
		return (EINVAL);
	pid = 0;
	for (position = 0; position < length; ++position) {
		if (buffer[position] < '0' || buffer[position] > '9')
			return (EINVAL);
		pid = pid * 10 + (buffer[position] - '0');
		if (pid > PID_MAX)
			return (ESRCH);
	}
	return (cgroup_proc_migrate(cg, pid, cred));
}

/*
 * Linux syntax: whitespace-separated "+name" / "-name" tokens, the last
 * token for a controller wins, and nothing changes unless every token is
 * valid.  The availability and child-usage rules are the kernel's.
 */
static int
cgroupfs_store_subtree_control(struct cgroup *cg, const char *buffer,
    size_t length, struct ucred *cred, bool *changedp)
{
	uint32_t enable;
	uint32_t disable;
	uint32_t bit;
	size_t position;
	size_t start;
	char sign;

	(void)cred;
	enable = 0;
	disable = 0;
	position = 0;
	while (position < length) {
		if (cgroupfs_is_space(buffer[position])) {
			++position;
			continue;
		}
		sign = buffer[position++];
		if (sign != '+' && sign != '-')
			return (EINVAL);
		start = position;
		while (position < length &&
		    !cgroupfs_is_space(buffer[position]))
			++position;
		bit = cgroup_controller_find(buffer + start, position - start);
		if (bit == 0)
			return (EINVAL);
		if (sign == '+') {
			enable |= bit;
			disable &= ~bit;
		} else {
			disable |= bit;
			enable &= ~bit;
		}
	}
	return (cgroup_control_update(cg, enable, disable, changedp));
}
