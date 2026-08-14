/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs console object.
 */
#ifndef VMMFS_CONSOLE_H
#define VMMFS_CONSOLE_H

struct vmmfs_machine;

struct vmmfs_console {
	struct vmmfs_machine *machine;
};

#endif /* VMMFS_CONSOLE_H */
