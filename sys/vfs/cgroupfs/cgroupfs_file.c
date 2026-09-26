/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly cgroupfs: interface file and controller tables.
 *
 * Everything here is a pure function of a control state snapshot; object
 * lifetime and locking belong to cgroupfs_group.c.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>

#include "cgroupfs.h"

#define CGROUPFS_CONTROLLER_PIDS	0x00000001U

struct cgroupfs_controller {
	const char	*name;
	uint32_t	bit;
};

static const struct cgroupfs_controller cgroupfs_controllers[] = {
	{ "pids", CGROUPFS_CONTROLLER_PIDS },
};

typedef int (*cgroupfs_file_load_t)(const struct cgroupfs_control *, char *,
	size_t *);
typedef int (*cgroupfs_file_store_t)(struct cgroupfs_control *, const char *,
	size_t);

struct cgroupfs_file_desc {
	const char		*name;
	mode_t			mode;
	/* Zero for core files present in every group. */
	uint32_t		controller;
	cgroupfs_file_load_t	load;
	/* NULL exactly when the mode grants no write access. */
	cgroupfs_file_store_t	store;
};

static int cgroupfs_load_controllers(const struct cgroupfs_control *, char *,
	size_t *);
static int cgroupfs_load_subtree_control(const struct cgroupfs_control *,
	char *, size_t *);
static int cgroupfs_load_empty(const struct cgroupfs_control *, char *,
	size_t *);
static int cgroupfs_load_max(const struct cgroupfs_control *, char *,
	size_t *);
static int cgroupfs_load_zero(const struct cgroupfs_control *, char *,
	size_t *);
static int cgroupfs_store_subtree_control(struct cgroupfs_control *,
	const char *, size_t);

static const struct cgroupfs_file_desc cgroupfs_files[] = {
	{ "cgroup.controllers", 0444, 0, cgroupfs_load_controllers, NULL },
	{ "cgroup.procs", 0444, 0, cgroupfs_load_empty, NULL },
	{ "cgroup.subtree_control", 0644, 0, cgroupfs_load_subtree_control,
	    cgroupfs_store_subtree_control },
	{ "pids.current", 0444, CGROUPFS_CONTROLLER_PIDS, cgroupfs_load_zero,
	    NULL },
	{ "pids.max", 0444, CGROUPFS_CONTROLLER_PIDS, cgroupfs_load_max,
	    NULL },
};

CTASSERT(nitems(cgroupfs_files) == CGROUPFS_FILE_COUNT);

uint32_t
cgroupfs_controller_all(void)
{
	uint32_t mask;
	u_int index;

	mask = 0;
	for (index = 0; index < nitems(cgroupfs_controllers); ++index)
		mask |= cgroupfs_controllers[index].bit;
	return (mask);
}

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

/*
 * Controller interface files follow Linux: never on the root, and only
 * where the parent has enabled the controller for its subtree.
 */
bool
cgroupfs_file_present(u_int index, const struct cgroupfs_control *control)
{
	uint32_t controller;

	KKASSERT(index < nitems(cgroupfs_files));
	controller = cgroupfs_files[index].controller;
	if (controller == 0)
		return (true);
	return (!control->is_root && (control->available & controller) != 0);
}

int
cgroupfs_file_load(u_int index, const struct cgroupfs_control *control,
    char *buffer, size_t *lengthp)
{
	KKASSERT(index < nitems(cgroupfs_files));
	return (cgroupfs_files[index].load(control, buffer, lengthp));
}

int
cgroupfs_file_store(u_int index, struct cgroupfs_control *control,
    const char *buffer, size_t length)
{
	KKASSERT(index < nitems(cgroupfs_files));
	KKASSERT(cgroupfs_files[index].store != NULL);
	return (cgroupfs_files[index].store(control, buffer, length));
}

/* Controller bit by name, or zero. */
static uint32_t
cgroupfs_controller_find(const char *name, size_t namelen)
{
	u_int index;

	for (index = 0; index < nitems(cgroupfs_controllers); ++index) {
		if (strlen(cgroupfs_controllers[index].name) == namelen &&
		    bcmp(cgroupfs_controllers[index].name, name, namelen) == 0)
			return (cgroupfs_controllers[index].bit);
	}
	return (0);
}

static bool
cgroupfs_is_space(char c)
{
	return (c == ' ' || c == '\t' || c == '\n');
}

/* Space-separated controller names in table order, newline terminated. */
static int
cgroupfs_format_mask(uint32_t mask, char *buffer, size_t *lengthp)
{
	size_t length;
	u_int index;
	int written;

	length = 0;
	for (index = 0; index < nitems(cgroupfs_controllers); ++index) {
		if ((mask & cgroupfs_controllers[index].bit) == 0)
			continue;
		written = ksnprintf(buffer + length,
		    CGROUPFS_FILE_SIZE_MAX - length, "%s%s",
		    length == 0 ? "" : " ", cgroupfs_controllers[index].name);
		if (written < 0 ||
		    (size_t)written >= CGROUPFS_FILE_SIZE_MAX - length)
			return (EOVERFLOW);
		length += written;
	}
	if (length + 1 >= CGROUPFS_FILE_SIZE_MAX)
		return (EOVERFLOW);
	buffer[length++] = '\n';
	*lengthp = length;
	return (0);
}

static int
cgroupfs_format_text(const char *text, char *buffer, size_t *lengthp)
{
	size_t length;

	length = strlen(text);
	KKASSERT(length < CGROUPFS_FILE_SIZE_MAX);
	bcopy(text, buffer, length);
	*lengthp = length;
	return (0);
}

/*
 * Linux semantics: whitespace-separated "+name" / "-name" tokens, the last
 * token for a controller wins, and nothing changes unless every token is
 * valid.  Enabling requires the controller to be available to this group;
 * disabling fails while a child still enables it for its own subtree.
 */
static int
cgroupfs_store_subtree_control(struct cgroupfs_control *control,
    const char *buffer, size_t length)
{
	uint32_t enable;
	uint32_t disable;
	uint32_t bit;
	size_t position;
	size_t start;
	char sign;

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
		bit = cgroupfs_controller_find(buffer + start, position - start);
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
	if ((enable & ~control->available) != 0)
		return (ENOENT);
	if ((disable & control->children_subtree_control) != 0)
		return (EBUSY);
	control->subtree_control = (control->subtree_control | enable) &
	    ~disable;
	return (0);
}

static int
cgroupfs_load_controllers(const struct cgroupfs_control *control,
    char *buffer, size_t *lengthp)
{
	return (cgroupfs_format_mask(control->available, buffer, lengthp));
}

static int
cgroupfs_load_subtree_control(const struct cgroupfs_control *control,
    char *buffer, size_t *lengthp)
{
	return (cgroupfs_format_mask(control->subtree_control, buffer,
	    lengthp));
}

/* Process membership does not exist yet. */
static int
cgroupfs_load_empty(const struct cgroupfs_control *control, char *buffer,
    size_t *lengthp)
{
	(void)control;
	return (cgroupfs_format_text("", buffer, lengthp));
}

static int
cgroupfs_load_max(const struct cgroupfs_control *control, char *buffer,
    size_t *lengthp)
{
	(void)control;
	return (cgroupfs_format_text("max\n", buffer, lengthp));
}

static int
cgroupfs_load_zero(const struct cgroupfs_control *control, char *buffer,
    size_t *lengthp)
{
	(void)control;
	return (cgroupfs_format_text("0\n", buffer, lengthp));
}
