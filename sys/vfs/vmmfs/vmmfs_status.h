/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs status object.
 */
#ifndef VMMFS_STATUS_H
#define VMMFS_STATUS_H

struct vmmfs_machine;

struct vmmfs_status {
	struct vmmfs_machine *machine;
};

#endif /* VMMFS_STATUS_H */
