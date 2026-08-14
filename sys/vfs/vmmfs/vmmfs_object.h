/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs object interface.
 */
#ifndef VMMFS_OBJECT_H
#define VMMFS_OBJECT_H

#include <sys/types.h>

struct vmmfs_item;
struct vmmfs_object;

/*
 * The sole vmmfs middle-layer interface.  A NULL callback means that the
 * object type does not support that collection operation.
 */
struct vmmfs_object_collection_ops {
	const char *type_name;
	int (*create_item)(struct vmmfs_object *, const char *, size_t,
	    struct vmmfs_object **);
	int (*read_item)(struct vmmfs_object *, uint64_t,
	    struct vmmfs_item *);
	int (*remove_item)(struct vmmfs_object *, const char *, size_t);
};

/* Every vmmfs object begins with this VFS-independent dispatch record. */
struct vmmfs_object {
	const struct vmmfs_object_collection_ops *ops;
};

#define VMMFS_TYPE_NAME(type) #type

#define VMMFS_OBJECT_COLLECTION_INIT(object, collection_ops) \
	((object).ops = &(collection_ops))

#endif /* VMMFS_OBJECT_H */
