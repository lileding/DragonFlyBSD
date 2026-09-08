/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot power state.
 */
#ifndef VMMFS_PCISLOT_POWERED_H
#define VMMFS_PCISLOT_POWERED_H

#include <sys/event.h>
#include <sys/thread.h>
#include "vmmfs_node.h"

struct vmmfs_pcislot_powered {
	struct vmmfs_node node;
	struct lwkt_token token;
	struct kqinfo kq;
	bool value;
	bool closed;
};

int vmmfs_pcislot_powered_init(struct vmmfs_node *, struct vmmfs_pcislot_powered *);
/* Publish current readiness; notify NOTE_WRITE only when the value changes. */
void vmmfs_pcislot_powered_set(struct vmmfs_pcislot_powered *, bool);

#endif /* VMMFS_PCISLOT_POWERED_H */
