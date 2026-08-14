/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs loader declaration object.
 */
#ifndef VMMFS_LOADER_H
#define VMMFS_LOADER_H

#include <sys/param.h>

struct vmmfs_machine;

struct vmmfs_loader_spec {
	char path[MAXPATHLEN];
};

struct vmmfs_loader {
	struct vmmfs_machine *machine;
	struct vmmfs_loader_spec spec;
};

#endif /* VMMFS_LOADER_H */
