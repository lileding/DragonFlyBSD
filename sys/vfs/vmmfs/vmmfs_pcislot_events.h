/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot events stream.
 */
#ifndef VMMFS_PCISLOT_EVENTS_H
#define VMMFS_PCISLOT_EVENTS_H

#include <sys/time.h>
#include <sys/event.h>
#include <sys/thread.h>
#include <sys/types.h>

#include "vmmfs_node.h"
#include <sys/vmmfs.h>

#define VMMFS_PCISLOT_EVENTS_BUFFER_SIZE (16 * 1024)

struct vmmfs_pcislot;
struct vnode;
struct vop_ops;

struct vmmfs_pcislot_events {
	struct vmmfs_node node;
	ino_t inode;
	struct lwkt_token token;
	struct kqinfo kq;
	char *buffer;
	size_t start;
	size_t length;
	uint64_t sequence;
	uint64_t last_tsc;
	bool closed;
};

int vmmfs_pcislot_events_init(struct vmmfs_mount *, struct vmmfs_branch *,
	struct vmmfs_pcislot_events *, struct vnode **);
void vmmfs_pcislot_events_revoke(struct vmmfs_pcislot_events *);
void vmmfs_pcislot_events_reset(struct vmmfs_pcislot_events *);
void vmmfs_pcislot_events_log(struct vmmfs_pcislot_events *,
	enum vmmfs_pci_event, const char *, ...);

#endif /* VMMFS_PCISLOT_EVENTS_H */
