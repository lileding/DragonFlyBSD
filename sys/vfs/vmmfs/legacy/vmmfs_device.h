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

#define VMMFS_DEVICE_LEAF_COUNT	4

enum vmmfs_device_leaf_kind {
	VMMFS_DEVICE_LEAF_PROVIDER,
	VMMFS_DEVICE_LEAF_CONSUMER,
	VMMFS_DEVICE_LEAF_STATE,
	VMMFS_DEVICE_LEAF_BDF,
};

struct vmmfs_device;

struct vmmfs_device_leaf {
	struct vmmfs_node	node;
	struct vmmfs_device	*borrow_imm_device;
	enum vmmfs_device_leaf_kind imm_kind;
};

struct vmmfs_device {
	SLIST_ENTRY(vmmfs_device) dv_view_link;
	struct vmm_device	own_mut_device;
	struct vmmfs_node	node;		/* the device directory */
	struct vmmfs_device_leaf own_mut_leaves[VMMFS_DEVICE_LEAF_COUNT];
};

#define VMMFS_DEV_OF_NODE(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, node)))
#define VMMFS_DEV_OF_CORE(d) \
	((struct vmmfs_device *)((char *)(d) - __offsetof(struct vmmfs_device, own_mut_device)))
#define VMMFS_DEVICE_LEAF_OF_NODE(n) \
	((struct vmmfs_device_leaf *)((char *)(n) - \
	__offsetof(struct vmmfs_device_leaf, node)))

struct vmmfs_device *vmmfs_find_device(struct vmmfs_mount *vmp,
	    struct vmmfs_machine *owner, const char *name, int nlen);
void	vmmfs_device_init(struct vmmfs_device *d, struct vmmfs_node *parent,
	    ino_t index);
void	vmmfs_device_revoke(struct vmmfs_device *d);
void	vmmfs_device_uninit(struct vmmfs_device *d);
int	vmmfs_device_destroy_owner_locked(struct vmmfs_mount *vmp,
	    struct vmm_machine *owner);
void	vmmfs_device_destroy_all(struct vmmfs_mount *vmp);

#endif /* VMMFS_DEVICE_H */
