/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI root declaration object.
 */
#ifndef VMMFS_PCIROOT_H
#define VMMFS_PCIROOT_H

#include <sys/param.h>
#include <sys/tree.h>

struct vmmfs_machine;
struct vmmfs_pcidev;

struct vmmfs_pcislot_spec {
	RB_ENTRY(vmmfs_pcislot_spec) entry;
	char name[NAME_MAX + 1];
};

RB_HEAD(vmmfs_pcislot_spec_tree, vmmfs_pcislot_spec);

struct vmmfs_pciroot_spec {
	struct vmmfs_pcislot_spec_tree slots;
};

struct vmmfs_pcislot {
	struct vmmfs_machine *machine;
	struct vmmfs_pcislot_spec spec;
	struct vmmfs_pcidev *device;
};

struct vmmfs_pciroot {
	struct vmmfs_machine *machine;
	struct vmmfs_pciroot_spec spec;
};

#endif /* VMMFS_PCIROOT_H */
