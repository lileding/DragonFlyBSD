/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem-side loader helpers.
 */
#ifndef VMMFS_LOADER_H
#define VMMFS_LOADER_H

struct ucred;
struct vmmfs_machine;

int	vmmfs_loader_validate(struct vmmfs_machine *m, struct ucred *cred);

#endif /* VMMFS_LOADER_H */
