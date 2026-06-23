/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The PCIe device-pool interface: lookups over the per-mount device list and
 * the symlink-target formatter the device classes use.  vmm.ko only.  Include
 * after vmmfs.h (it uses struct vmmfs_device / vmmfs_mount).
 */
#ifndef VMMFS_DEVICE_H
#define VMMFS_DEVICE_H

struct vmmfs_device *vmmfs_find_device(struct vmmfs_mount *vmp,
	    struct vmmfs_machines *owner, const char *name, int nlen);
struct vmmfs_device *vmmfs_find_device_any(struct vmmfs_mount *vmp,
	    const char *name, int nlen);
int	vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
	    char *buf, size_t bufsize);

#endif /* VMMFS_DEVICE_H */
