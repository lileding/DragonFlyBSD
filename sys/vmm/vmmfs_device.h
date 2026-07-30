/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem-side device view.  The core fabric owns function identity, BDF,
 * and consumer attachment; this wrapper owns only vnode presentation.
 */
#ifndef VMMFS_DEVICE_H
#define VMMFS_DEVICE_H

#include "vmm_device.h"
#include "vmmfs.h"
#include "vmmfs_machine.h"

struct vmmfs_device {
	SLIST_ENTRY(vmmfs_device) dv_view_link;
	struct vmm_device	own_mut_device;
	struct vmmfs_node	node;		/* the device directory */
	struct vmmfs_node	link;		/* its symlink in /vmm/devices/ */
};

#define VMMFS_DEV_OF_NODE(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, node)))
#define VMMFS_DEV_OF_LINK(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, link)))
#define VMMFS_DEV_OF_CORE(d) \
	((struct vmmfs_device *)((char *)(d) - __offsetof(struct vmmfs_device, own_mut_device)))

struct vmmfs_device *vmmfs_find_device(struct vmmfs_mount *vmp,
	    struct vmmfs_machine *owner, const char *name, int nlen);
struct vmmfs_device *vmmfs_find_device_any(struct vmmfs_mount *vmp,
	    const char *name, int nlen);
int	vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
	    char *buf, size_t bufsize);
void	vmmfs_device_return_owner_locked(struct vmmfs_mount *vmp,
	    struct vmm_machine *owner);
void	vmmfs_device_destroy_all(struct vmmfs_mount *vmp);

#endif /* VMMFS_DEVICE_H */
