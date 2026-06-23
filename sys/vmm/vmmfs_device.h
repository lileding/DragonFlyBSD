/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem-side device object and device-pool helpers.  The core identity and
 * binding state are in struct vmm_device; this wrapper owns the device file and
 * the /vmm/devices symlink node.
 */
#ifndef VMMFS_DEVICE_H
#define VMMFS_DEVICE_H

#include "vmm_device.h"
#include "vmmfs.h"
#include "vmmfs_machine.h"

struct vmmfs_device {
	SLIST_ENTRY(vmmfs_device) dv_link;
	struct vmm_device	dev;		/* core: bdf, owner, host/user kind */
	struct vmmfs_node	node;		/* the device file */
	struct vmmfs_node	link;		/* its symlink in /vmm/devices/ */
};

#define VMMFS_DEV_OF_NODE(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, node)))
#define VMMFS_DEV_OF_LINK(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, link)))

struct vmmfs_device *vmmfs_find_device(struct vmmfs_mount *vmp,
	    struct vmmfs_machine *owner, const char *name, int nlen);
struct vmmfs_device *vmmfs_find_device_any(struct vmmfs_mount *vmp,
	    const char *name, int nlen);
int	vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
	    char *buf, size_t bufsize);
void	vmmfs_device_init_host_pool(struct vmmfs_mount *vmp);
void	vmmfs_device_unbind_owner_locked(struct vmmfs_mount *vmp,
	    struct vmm_machine *owner, struct vmmfs_devlist *tofree);
void	vmmfs_device_free_list(struct vmmfs_devlist *list);
void	vmmfs_device_destroy_all(struct vmmfs_mount *vmp);

#endif /* VMMFS_DEVICE_H */
