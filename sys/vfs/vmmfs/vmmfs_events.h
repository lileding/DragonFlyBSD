/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs events object.
 */
#ifndef VMMFS_EVENTS_H
#define VMMFS_EVENTS_H

#include "vmmfs_node.h"

#include <sys/time.h>
#include <sys/event.h>
#include <sys/thread.h>
#include <sys/types.h>
#include <sys/vmmfs.h>

struct vmmfs_machine;
struct vnode;
struct vop_ops;

#define VMMFS_EVENTS_BUFFER_SIZE (64 * 1024)

struct vmmfs_events {
	struct vmmfs_node node;
	struct vmmfs_machine *machine;
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

extern struct vop_ops vmmfs_events_vops;

int vmmfs_events_init(struct vmmfs_machine *, struct vmmfs_events *);
void vmmfs_events_fini(struct vmmfs_events *);
void vmmfs_events_revoke(struct vmmfs_events *);
void vmmfs_events_log(struct vmmfs_events *, enum vmmfs_machine_event,
	const char *, ...);

#endif /* VMMFS_EVENTS_H */
