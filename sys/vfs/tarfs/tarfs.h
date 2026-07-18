/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 Juniper Networks, Inc.
 * Copyright (c) 2022-2023 Klara, Inc.
 * Copyright (c) 2026 The DragonFly Project (DragonFly port).
 */

#ifndef _VFS_TARFS_TARFS_H_
#define	_VFS_TARFS_TARFS_H_

#ifndef _KERNEL
#error Should only be included by kernel
#endif

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/vnode.h>

MALLOC_DECLARE(M_TARFSMNT);
MALLOC_DECLARE(M_TARFSNODE);
MALLOC_DECLARE(M_TARFSNAME);
MALLOC_DECLARE(M_TARFSBLK);

#ifdef SYSCTL_DECL
SYSCTL_DECL(_vfs_tarfs);
#endif

struct mount;
struct vnode;
struct unrhdr;
struct tarfs_zio;

struct tarfs_node {
	TAILQ_ENTRY(tarfs_node)	entries;
	TAILQ_ENTRY(tarfs_node)	dirents;
	struct mtx		 lock;
	struct vnode		*vnode;
	struct tarfs_mount	*tmp;
	enum vtype		 type;
	ino_t			 ino;
	off_t			 offset;
	size_t			 size;
	size_t			 physize;
	char			*name;
	size_t			 namelen;
	uid_t			 uid;
	gid_t			 gid;
	mode_t			 mode;
	unsigned int		 flags;
	nlink_t			 nlink;
	struct timespec		 atime;
	struct timespec		 mtime;
	struct timespec		 ctime;
	struct timespec		 birthtime;
	uint32_t		 gen;
	size_t			 nblk;
	struct tarfs_blk	*blk;
	struct tarfs_node	*parent;
	union {
		struct {
			TAILQ_HEAD(, tarfs_node) dirhead;
			off_t			 lastcookie;
			struct tarfs_node	*lastnode;
		} dir;
		struct {
			char			*name;
			size_t			 namelen;
		} link;
		dev_t			 rdev;
		struct tarfs_node	*other;
	};
};

struct tarfs_blk {
	off_t	 i;
	off_t	 o;
	size_t	 l;
};

struct tarfs_mount {
	TAILQ_HEAD(, tarfs_node) allnodes;
	struct lock		allnode_lock;
	struct tarfs_node	*root;
	struct vnode		*vp;
	struct mount		*vfs;
	struct unrhdr		*ino_unr;
	size_t			iosize;
	size_t			nblocks;
	size_t			nfiles;
	time_t			mtime;
	struct tarfs_zio	*zio; /* zstd stream state; NULL if uncompressed */
	struct netexport	 export; /* NFS export info */
};

/*
 * File handle overlay (must fit in struct fid, MAXFIDSZ data + header).
 * sizeof(struct fid) == 2+2+MAXFIDSZ on DragonFly.
 */
struct tarfs_fid {
	uint16_t	len;	/* total sizeof(struct tarfs_fid) */
	uint16_t	pad;
	ino_t		ino;
	uint64_t	gen;	/* zero-extended node gen (tmpfs-sized FH) */
} __packed;



#define	TARFS_NODE_LOCK(tnp)		mtx_lock(&(tnp)->lock)
#define	TARFS_NODE_UNLOCK(tnp)		mtx_unlock(&(tnp)->lock)
#define	TARFS_ALLNODES_LOCK(tmp)	lockmgr(&(tmp)->allnode_lock, LK_EXCLUSIVE)
#define	TARFS_ALLNODES_UNLOCK(tmp)	lockmgr(&(tmp)->allnode_lock, LK_RELEASE)

#define	TARFS_BSHIFT		9
#define	TARFS_BLOCKSIZE		((size_t)(1U << TARFS_BSHIFT))
#define	TARFS_SZ2BLKS(sz)	(((sz) + TARFS_BLOCKSIZE - 1) / TARFS_BLOCKSIZE)

extern unsigned int tarfs_ioshift;
#define	TARFS_IOSHIFT_DEFAULT	PAGE_SHIFT

#define	TARFS_ROOTINO		((ino_t)3)
#define	TARFS_MININO		((ino_t)65535)

#define	TARFS_COOKIE_DOT	0
#define	TARFS_COOKIE_DOTDOT	1
#define	TARFS_COOKIE_EOF	((off_t)0x7fffffffffffffffLL)

extern struct vop_ops tarfs_vnode_vops;

static __inline struct tarfs_mount *
MP_TO_TARFS_MOUNT(struct mount *mp)
{
	KKASSERT(mp != NULL && mp->mnt_data != NULL);
	return ((struct tarfs_mount *)(uintptr_t)mp->mnt_data);
}

static __inline struct tarfs_node *
VP_TO_TARFS_NODE(struct vnode *vp)
{
	KKASSERT(vp != NULL && vp->v_data != NULL);
	return ((struct tarfs_node *)vp->v_data);
}

int	tarfs_alloc_node(struct tarfs_mount *tmp, const char *name,
	    size_t namelen, enum vtype type, off_t off, size_t sz,
	    time_t mtime, uid_t uid, gid_t gid, mode_t mode,
	    unsigned int flags, const char *linkname, dev_t rdev,
	    struct tarfs_node *parent, struct tarfs_node **node);
int	tarfs_load_blockmap(struct tarfs_node *tnp, size_t realsize);
void	tarfs_free_node(struct tarfs_node *tnp);
struct tarfs_node *tarfs_lookup_dir(struct tarfs_node *tnp, off_t cookie);
struct tarfs_node *tarfs_lookup_name(struct tarfs_node *tnp,
	    const char *name, size_t namelen);
int	tarfs_read_file(struct tarfs_node *tnp, size_t len, struct uio *uiop);
int	tarfs_alloc_vp(struct mount *mp, struct tarfs_node *tnp,
	    int lkflags, struct vnode **vpp);

int	tarfs_io_init(struct tarfs_mount *tmp);
int	tarfs_io_fini(struct tarfs_mount *tmp);
int	tarfs_io_read(struct tarfs_mount *tmp, int raw, struct uio *uiop);
ssize_t	tarfs_io_read_buf(struct tarfs_mount *tmp, int raw,
	    void *buf, off_t off, size_t len);
unsigned int tarfs_strtofflags(const char *str, char **end);

#endif
