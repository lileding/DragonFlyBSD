/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs events object.
 */
#ifndef VMMFS_EVENTS_H
#define VMMFS_EVENTS_H

struct vmmfs_machine;

struct vmmfs_events {
	struct vmmfs_machine *machine;
};

#endif /* VMMFS_EVENTS_H */
