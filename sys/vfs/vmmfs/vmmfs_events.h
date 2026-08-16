/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs events object.
 */
#ifndef VMMFS_EVENTS_H
#define VMMFS_EVENTS_H

#include <sys/thread.h>
#include <sys/types.h>

struct vmmfs_machine;
struct vnode;
struct vop_ops;

#define VMMFS_EVENTS_BUFFER_SIZE (64 * 1024)

struct vmmfs_events {
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	ino_t inode;
	struct lwkt_token token;
	char *buffer;
	size_t start;
	size_t length;
	uint64_t sequence;
	uint64_t last_tsc;
	bool closed;
};

extern struct vop_ops vmmfs_events_vops;

int vmmfs_events_create(struct vmmfs_machine *, struct vmmfs_events *);
int vmmfs_events_destroy(struct vmmfs_events *);
void vmmfs_events_log(struct vmmfs_events *, const char *);

#endif /* VMMFS_EVENTS_H */
