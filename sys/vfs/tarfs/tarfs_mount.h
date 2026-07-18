/*-
 * Copyright (c) 2026 The DragonFly Project.
 *
 * Mount argument structure for tarfs (userland + kernel).
 */

#ifndef _VFS_TARFS_TARFS_MOUNT_H_
#define _VFS_TARFS_TARFS_MOUNT_H_

#include <sys/param.h>
#include <sys/mount.h>

/*
 * mount(2) data: always sizeof(struct tarfs_args).
 * fspec is the absolute path to the tarball.
 * as[], if non-empty, becomes f_mntfromname (like FreeBSD "as=").
 * uid/gid/mode control synthetic root directory attributes when
 * the caller is allowed to set them (root).  mode==0 means default 0755.
 */
#define TARFS_ARGS_MAGIC	0x54415246u	/* 'TARF' */
#define TARFS_ARGS_VERSION	1
#define TARFSMNT_VERIFY		0x0001	/* reserved; DF has no O_VERIFY */

struct tarfs_args {
	uint32_t	magic;
	uint32_t	version;
	char		fspec[MAXPATHLEN];
	char		as[MNAMELEN];
	uid_t		uid;
	gid_t		gid;
	mode_t		mode;
	int		flags;
	struct export_args export;
};

#endif /* _VFS_TARFS_TARFS_MOUNT_H_ */
