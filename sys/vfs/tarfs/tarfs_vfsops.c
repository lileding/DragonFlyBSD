/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 Juniper Networks, Inc.
 * Copyright (c) 2022-2024 Klara, Inc.
 * Copyright (c) 2026 The DragonFly Project (DragonFly VFS glue).
 *
 * USTAR/PAX scan logic based on FreeBSD sys/fs/tarfs/tarfs_vfsops.c
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/fcntl.h>
#include <sys/libkern.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/nlookup.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/sbuf.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

#include <sys/socket.h>
#include "tarfs.h"
#include "tarfs_mount.h"

#include "tarfs_dbg.h"

MALLOC_DEFINE(M_TARFSMNT, "tarfs mount", "tarfs mount structures");

/* Ensure zero page is large enough for header compares */
CTASSERT(PAGE_SIZE >= TARFS_BLOCKSIZE);

struct ustar_header {
	char	name[100];
	char	mode[8];
	char	uid[8];
	char	gid[8];
	char	size[12];
	char	mtime[12];
	char	checksum[8];
	char	typeflag[1];
	char	linkname[100];
	char	magic[6];
	char	version[2];
	char	uname[32];
	char	gname[32];
	char	major[8];
	char	minor[8];
	char	prefix[155];
	char	_pad[12];
};

CTASSERT(sizeof(struct ustar_header) == TARFS_BLOCKSIZE);

#define	TAR_EOF			((size_t)-1)
#define	TAR_TYPE_FILE		'0'
#define	TAR_TYPE_HARDLINK	'1'
#define	TAR_TYPE_SYMLINK	'2'
#define	TAR_TYPE_CHAR		'3'
#define	TAR_TYPE_BLOCK		'4'
#define	TAR_TYPE_DIRECTORY	'5'
#define	TAR_TYPE_FIFO		'6'
#define	TAR_TYPE_CONTIG		'7'
#define	TAR_TYPE_GLOBAL_EXTHDR	'g'
#define	TAR_TYPE_EXTHDR		'x'

static const uint8_t USTAR_MAGIC[] = { 'u', 's', 't', 'a', 'r', 0 };
static const uint8_t USTAR_VERSION[] = { '0', '0' };
static const uint8_t GNUTAR_MAGIC[] = { 'u', 's', 't', 'a', 'r', ' ' };

static int tarfs_mount(struct mount *, char *, caddr_t, struct ucred *);
static int tarfs_unmount(struct mount *, int);
static int tarfs_root(struct mount *, struct vnode **);
static int tarfs_statfs(struct mount *, struct statfs *, struct ucred *);

/*
 * Read a len-width signed octal number.  Leading spaces are skipped so
 * space-padded fields (" 777") parse as octal, not decimal.
 */
static int
tarfs_str2octal(const char *strp, size_t len, int64_t *num)
{
	int64_t val;
	size_t idx;
	int sign;

	idx = 0;
	while (idx < len && strp[idx] == ' ')
		idx++;
	if (idx >= len)
		return (EINVAL);

	if (strp[idx] == '-') {
		sign = -1;
		idx++;
	} else {
		sign = 1;
	}

	val = 0;
	for (; idx < len && strp[idx] != '\0' && strp[idx] != ' '; idx++) {
		if (strp[idx] < '0' || strp[idx] > '7')
			return (EINVAL);
		if (val > INT64_MAX / 8)
			return (ERANGE);
		val <<= 3;
		val += strp[idx] - '0';
	}

	*num = val * sign;
	return (0);
}

/*
 * GNU base-256 extended numeric field: first byte has bit 7 set; remaining
 * 7 bits of that byte plus the following (len-1) bytes are big-endian
 * signed two's complement.
 */
static int
tarfs_str2base256(const char *strp, size_t len, int64_t *num)
{
	int64_t val;
	size_t idx;

	/* Sign-extend the first byte (bit 6 is the sign of the 7-bit payload). */
	if ((strp[0] & 0x40) != 0)
		val = (int64_t)-1;
	else
		val = 0;
	val <<= 6;
	val |= (strp[0] & 0x3f);

	for (idx = 1; idx < len; idx++) {
		if (val > INT64_MAX / 256 || val < INT64_MIN / 256)
			return (ERANGE);
		val <<= 8;
		val |= (0xff & (int64_t)(unsigned char)strp[idx]);
	}

	*num = val;
	return (0);
}

/*
 * Read a len-byte numeric field.  High bit of the first byte selects
 * GNU base-256; otherwise signed octal (POSIX ustar).
 */
static int
tarfs_str2int64(const char *strp, size_t len, int64_t *num)
{
	if (len < 1)
		return (EINVAL);
	if ((strp[0] & 0x80) != 0)
		return (tarfs_str2base256(strp, len, num));
	return (tarfs_str2octal(strp, len, num));
}

/*
 * Verify header checksum.  Accept unsigned sum (POSIX) and signed-byte
 * sum (broken older archives), matching FreeBSD tarfs.
 */
static int
tarfs_checksum(struct ustar_header *hdrp)
{
	const unsigned char *ptr;
	int64_t checksum, hdrsum;

	if (tarfs_str2int64(hdrp->checksum, sizeof(hdrp->checksum),
	    &hdrsum) != 0)
		return (0);

	/* Treat checksum field as eight spaces without mutating the header. */
	checksum = 0;
	for (ptr = (const unsigned char *)hdrp;
	    ptr < (const unsigned char *)hdrp->checksum; ptr++)
		checksum += *ptr;
	for (; ptr < (const unsigned char *)hdrp->typeflag; ptr++)
		checksum += 0x20;
	for (; ptr < (const unsigned char *)(hdrp + 1); ptr++)
		checksum += *ptr;
	if (hdrsum == checksum)
		return (1);

	/* Signed-byte variant used by some older writers. */
	checksum = 0;
	for (ptr = (const unsigned char *)hdrp;
	    ptr < (const unsigned char *)hdrp->checksum; ptr++)
		checksum += *((const signed char *)ptr);
	for (; ptr < (const unsigned char *)hdrp->typeflag; ptr++)
		checksum += 0x20;
	for (; ptr < (const unsigned char *)(hdrp + 1); ptr++)
		checksum += *((const signed char *)ptr);
	if (hdrsum == checksum)
		return (1);

	return (0);
}

/*
 * Walk/create parent path components for a member name.
 * Closely follows FreeBSD tarfs_lookup_path.
 */
static int
tarfs_lookup_path(struct tarfs_mount *tmp, char *name, size_t namelen,
    char **namepp, char **sepp, struct tarfs_node **parentp,
    struct tarfs_node **tnpp, int create)
{
	struct tarfs_node *parent, *tnp;
	char *p, *sep;
	size_t seglen;
	int error;

	parent = tmp->root;
	p = name;
	if (tnpp)
		*tnpp = NULL;

	/* strip leading slashes and ./ prefixes */
	for (;;) {
		while (namelen > 0 && *p == '/') {
			p++;
			namelen--;
		}
		if (namelen >= 2 && p[0] == '.' && p[1] == '/') {
			p += 2;
			namelen -= 2;
			continue;
		}
		if (namelen == 1 && p[0] == '.') {
			p++;
			namelen--;
			continue;
		}
		break;
	}
	if (namelen == 0) {
		if (parentp)
			*parentp = parent;
		if (namepp)
			*namepp = p;
		if (sepp)
			*sepp = p;
		return (0);
	}

	for (;;) {
		sep = p;
		while ((size_t)(sep - name) < namelen && *sep != '/')
			sep++;
		seglen = (size_t)(sep - p);
		if (seglen == 1 && p[0] == '.')
			goto nextcomp;
		if (seglen == 2 && p[0] == '.' && p[1] == '.')
			goto nextcomp;
		if (seglen == 0) {
			/* trailing slash — directory */
			if (parentp)
				*parentp = parent;
			if (namepp)
				*namepp = p;
			if (sepp)
				*sepp = sep;
			return (0);
		}
		if ((size_t)(sep - name) >= namelen) {
			/* final component */
			tnp = tarfs_lookup_name(parent, p, seglen);
			if (parentp)
				*parentp = parent;
			if (namepp)
				*namepp = p;
			if (sepp)
				*sepp = sep;
			if (tnpp)
				*tnpp = tnp;
			return (0);
		}
		/* intermediate directory */
		tnp = tarfs_lookup_name(parent, p, seglen);
		if (tnp == NULL) {
			if (!create)
				return (ENOENT);
			error = tarfs_alloc_node(tmp, p, seglen, VDIR, 0, 0,
			    tmp->mtime, tmp->root->uid, tmp->root->gid,
			    0755, 0, NULL, (dev_t)(-1), parent, &tnp);
			if (error)
				return (error);
		} else if (tnp->type != VDIR) {
			return (ENOTDIR);
		}
		parent = tnp;
nextcomp:
		p = sep + 1;
	}
}

static void
tarfs_free_mount(struct tarfs_mount *tmp)
{
	struct tarfs_node *tnp, *ntnp;

	if (tmp == NULL)
		return;
	tarfs_io_fini(tmp);
	if (tmp->vp != NULL) {
		vn_lock(tmp->vp, LK_EXCLUSIVE | LK_RETRY);
		(void)VOP_CLOSE(tmp->vp, FREAD, NULL);
		vn_unlock(tmp->vp);
		vrele(tmp->vp);
		tmp->vp = NULL;
	}
	TAILQ_FOREACH_MUTABLE(tnp, &tmp->allnodes, entries, ntnp) {
		/* force free */
		tnp->nlink = 1;
		if (tnp->type == VDIR)
			tnp->nlink = 2;
		tarfs_free_node(tnp);
	}
	if (tmp->ino_unr != NULL)
		delete_unrhdr(tmp->ino_unr);
	lockuninit(&tmp->allnode_lock);
	kfree(tmp, M_TARFSMNT);
}

static int
tarfs_alloc_one(struct tarfs_mount *tmp, size_t *blknump)
{
	char block[TARFS_BLOCKSIZE];
	struct ustar_header *hdrp = (struct ustar_header *)(void *)block;
	struct sbuf *namebuf = NULL;
	char *exthdr = NULL, *name = NULL, *link = NULL;
	size_t blknum = *blknump;
	int64_t num;
	int endmarker = 0;
	char *namep, *sep;
	struct tarfs_node *parent, *tnp, *other;
	size_t namelen = 0, linklen = 0, realsize = 0, extsize = 0, sz;
	ssize_t res;
	dev_t rdev;
	gid_t gid;
	mode_t mode;
	time_t mtime;
	uid_t uid;
	long major = -1, minor = -1;
	unsigned int flags = 0;
	int error;
	int sparse = 0;
	static const char zeros[TARFS_BLOCKSIZE];

again:
	res = tarfs_io_read_buf(tmp, 0, block,
	    (off_t)TARFS_BLOCKSIZE * (off_t)blknum, TARFS_BLOCKSIZE);
	if (res < 0) {
		error = (int)(-res);
		goto bad;
	} else if (res < (ssize_t)TARFS_BLOCKSIZE) {
		goto eof;
	}
	blknum++;

	if (bcmp(block, zeros, TARFS_BLOCKSIZE) == 0) {
		if (endmarker++) {
			if (exthdr != NULL)
				kfree(exthdr, M_TEMP);
			tmp->nblocks = blknum;
			*blknump = TAR_EOF;
			return (0);
		}
		goto again;
	}

	if (bcmp(hdrp->magic, USTAR_MAGIC, sizeof(USTAR_MAGIC)) == 0 &&
	    bcmp(hdrp->version, USTAR_VERSION, sizeof(USTAR_VERSION)) == 0) {
		/* POSIX ustar */
	} else if (bcmp(hdrp->magic, GNUTAR_MAGIC, sizeof(GNUTAR_MAGIC)) == 0) {
		error = EFTYPE;
		goto bad;
	} else {
		error = EINVAL;
		goto bad;
	}

	if (!tarfs_checksum(hdrp)) {
		error = EINVAL;
		goto bad;
	}

	if (tarfs_str2int64(hdrp->mode, sizeof(hdrp->mode), &num) != 0 ||
	    num < 0 || num > (S_IFMT | ALLPERMS))
		mode = S_IRUSR;
	else
		mode = (mode_t)num & ALLPERMS;

	if (tarfs_str2int64(hdrp->uid, sizeof(hdrp->uid), &num) != 0 ||
	    num < 0 || num > UID_MAX) {
		uid = tmp->root->uid;
		mode &= ~S_ISUID;
	} else {
		uid = (uid_t)num;
	}
	if (tarfs_str2int64(hdrp->gid, sizeof(hdrp->gid), &num) != 0 ||
	    num < 0 || num > GID_MAX) {
		gid = tmp->root->gid;
		mode &= ~S_ISGID;
	} else {
		gid = (gid_t)num;
	}
	if (tarfs_str2int64(hdrp->size, sizeof(hdrp->size), &num) != 0 ||
	    num < 0) {
		error = EINVAL;
		goto bad;
	}
	sz = (size_t)num;
	if (tarfs_str2int64(hdrp->mtime, sizeof(hdrp->mtime), &num) != 0) {
		error = EINVAL;
		goto bad;
	}
	mtime = (time_t)num;
	rdev = (dev_t)(-1);

	if (hdrp->typeflag[0] == TAR_TYPE_GLOBAL_EXTHDR)
		goto skip;

	if (hdrp->typeflag[0] == TAR_TYPE_EXTHDR) {
		if (exthdr != NULL) {
			error = EFTYPE;
			goto bad;
		}
		exthdr = kmalloc(sz + 1, M_TEMP, M_WAITOK);
		res = tarfs_io_read_buf(tmp, 0, exthdr,
		    (off_t)TARFS_BLOCKSIZE * (off_t)blknum, sz);
		if (res < 0) {
			error = (int)(-res);
			goto bad;
		}
		if (res < (ssize_t)sz)
			goto eof;
		exthdr[sz] = '\0';
		blknum += TARFS_SZ2BLKS(res);
		/* PAX parser (FreeBSD style) */
		{
			char *line = exthdr;
			while (line < exthdr + sz) {
				char *eol, *key, *value, *sp;
				size_t len = strtoul(line, &sp, 10);
				if (len == 0 || sp == line || *sp != ' ')
					goto syntax;
				if (line + len > exthdr + sz) {
					error = EINVAL;
					goto bad;
				}
				eol = line + len - 1;
				*eol = '\0';
				line += len;
				key = sp + 1;
				sp = strchr(key, '=');
				if (sp == NULL)
					goto syntax;
				*sp = '\0';
				value = sp + 1;
				if (strcmp(key, "size") == 0) {
					extsize = (size_t)strtol(value, &sp, 10);
					if (sp != eol)
						goto syntax;
				} else if (strcmp(key, "path") == 0) {
					name = value;
					namelen = (size_t)(eol - value);
				} else if (strcmp(key, "linkpath") == 0) {
					link = value;
					linklen = (size_t)(eol - value);
				} else if (strcmp(key, "GNU.sparse.major") == 0) {
					sparse = 1;
					major = strtol(value, &sp, 10);
					if (sp != eol)
						goto syntax;
				} else if (strcmp(key, "GNU.sparse.minor") == 0) {
					sparse = 1;
					minor = strtol(value, &sp, 10);
					if (sp != eol)
						goto syntax;
				} else if (strcmp(key, "GNU.sparse.name") == 0) {
					sparse = 1;
					name = value;
					namelen = (size_t)(eol - value);
				} else if (strcmp(key, "GNU.sparse.realsize") == 0) {
					sparse = 1;
					realsize = (size_t)strtoul(value, &sp, 10);
					if (sp != eol)
						goto syntax;
				} else if (strcmp(key, "SCHILY.fflags") == 0) {
					flags |= tarfs_strtofflags(value, &sp);
					if (sp != eol)
						goto syntax;
				}
			}
		}
		goto again;
	}

	if (extsize > 0)
		sz = extsize;

	if (sparse) {
		if (major != 1 || minor != 0 || name == NULL || realsize == 0 ||
		    hdrp->typeflag[0] != TAR_TYPE_FILE) {
			error = EINVAL;
			goto bad;
		}
	}

	if (name == NULL) {
		if (hdrp->prefix[0] != '\0') {
			namebuf = sbuf_new_auto();
			sbuf_printf(namebuf, "%.*s/%.*s",
			    (int)sizeof(hdrp->prefix), hdrp->prefix,
			    (int)sizeof(hdrp->name), hdrp->name);
			sbuf_finish(namebuf);
			name = sbuf_data(namebuf);
			namelen = sbuf_len(namebuf);
		} else {
			name = hdrp->name;
			namelen = strnlen(hdrp->name, sizeof(hdrp->name));
		}
	}

	error = tarfs_lookup_path(tmp, name, namelen, &namep, &sep,
	    &parent, &tnp, 1);
	if (error != 0) {
		error = EINVAL;
		goto bad;
	}
	/* skip ./ or trailing-slash-only members */
	if ((size_t)(sep - namep) == 0) {
		if (hdrp->typeflag[0] == TAR_TYPE_DIRECTORY)
			goto skip;
		error = EINVAL;
		goto bad;
	}
	if (tnp != NULL) {
		if (hdrp->typeflag[0] == TAR_TYPE_DIRECTORY)
			goto skip;
		error = EINVAL;
		goto bad;
	}

	switch (hdrp->typeflag[0]) {
	case TAR_TYPE_DIRECTORY:
		error = tarfs_alloc_node(tmp, namep, (size_t)(sep - namep),
		    VDIR, 0, 0, mtime, uid, gid, mode, flags, NULL, 0,
		    parent, &tnp);
		break;
	case TAR_TYPE_FILE:
	case TAR_TYPE_CONTIG:
	case '\0':
		error = tarfs_alloc_node(tmp, namep, (size_t)(sep - namep),
		    VREG, (off_t)blknum * TARFS_BLOCKSIZE, sz, mtime, uid, gid,
		    mode, flags, NULL, 0, parent, &tnp);
		if (error == 0 && sparse)
			error = tarfs_load_blockmap(tnp, realsize);
		break;
	case TAR_TYPE_HARDLINK:
		if (link == NULL) {
			link = hdrp->linkname;
			linklen = strnlen(link, sizeof(hdrp->linkname));
		}
		if (linklen == 0) {
			error = EINVAL;
			goto bad;
		}
		error = tarfs_lookup_path(tmp, link, linklen, NULL, NULL,
		    NULL, &other, 0);
		if (error != 0 || other == NULL ||
		    other->type != VREG || other->other != NULL) {
			error = EINVAL;
			goto bad;
		}
		error = tarfs_alloc_node(tmp, namep, (size_t)(sep - namep),
		    VREG, 0, 0, mtime, uid, gid, mode, flags, NULL, 0,
		    parent, &tnp);
		if (error == 0) {
			/* Name entry; lookup follows other (FreeBSD model). */
			tnp->other = other;
			other->nlink++;
		}
		break;
	case TAR_TYPE_SYMLINK:
		if (link == NULL) {
			link = hdrp->linkname;
			linklen = strnlen(link, sizeof(hdrp->linkname));
		}
		if (linklen == 0) {
			error = EINVAL;
			goto bad;
		}
		error = tarfs_alloc_node(tmp, namep, (size_t)(sep - namep),
		    VLNK, 0, linklen, mtime, uid, gid, mode, flags, link, 0,
		    parent, &tnp);
		break;
	case TAR_TYPE_BLOCK:
	case TAR_TYPE_CHAR: {
		int64_t maj, minn;
		if (tarfs_str2int64(hdrp->major, sizeof(hdrp->major), &maj) ||
		    tarfs_str2int64(hdrp->minor, sizeof(hdrp->minor), &minn) ||
		    maj < 0 || minn < 0) {
			error = EINVAL;
			goto bad;
		}
		rdev = makeudev((int)maj, (int)minn);
			if (rdev == NOUDEV) {
				error = EINVAL;
				goto bad;
			}
		error = tarfs_alloc_node(tmp, namep, (size_t)(sep - namep),
		    hdrp->typeflag[0] == TAR_TYPE_BLOCK ? VBLK : VCHR,
		    0, 0, mtime, uid, gid, mode, flags, NULL, rdev,
		    parent, &tnp);
		break;
	}
	case TAR_TYPE_FIFO:
		error = tarfs_alloc_node(tmp, namep, (size_t)(sep - namep),
		    VFIFO, 0, 0, mtime, uid, gid, mode, flags, NULL, 0,
		    parent, &tnp);
		break;
	default:
		error = EINVAL;
		break;
	}
	if (error != 0)
		goto bad;

skip:
	blknum += TARFS_SZ2BLKS(sz);
	tmp->nblocks = blknum;
	*blknump = blknum;
	if (exthdr != NULL)
		kfree(exthdr, M_TEMP);
	if (namebuf != NULL)
		sbuf_delete(namebuf);
	return (0);

syntax:
	error = EINVAL;
	goto bad;
eof:
	error = EIO;
	goto bad;
bad:
	if (exthdr != NULL)
		kfree(exthdr, M_TEMP);
	if (namebuf != NULL)
		sbuf_delete(namebuf);
	return (error);
}

static int
tarfs_alloc_mount(struct mount *mp, struct vnode *vp,
    uid_t root_uid, gid_t root_gid, mode_t root_mode,
    struct tarfs_mount **tmpp)
{
	struct vattr va;
	struct tarfs_mount *tmp;
	struct tarfs_node *root;
	size_t blknum;
	time_t mtime;
	int error;

	/*
	 * Take ownership of the open backing vnode immediately so every
	 * failure path can release it via tarfs_free_mount().
	 */
	tmp = kmalloc(sizeof(*tmp), M_TARFSMNT, M_WAITOK | M_ZERO);
	mp->mnt_data = (qaddr_t)(uintptr_t)tmp;
	lockinit(&tmp->allnode_lock, "tarfsall", 0, 0);
	TAILQ_INIT(&tmp->allnodes);
	tmp->ino_unr = new_unrhdr((int)TARFS_MININO, INT_MAX, &tmp->allnode_lock);
	tmp->vp = vp;
	tmp->vfs = mp;
	{
		unsigned int shift = tarfs_ioshift;

		/* Keep bread() bsize in int range and >= PAGE_SIZE. */
		if (shift < (unsigned int)PAGE_SHIFT)
			shift = (unsigned int)PAGE_SHIFT;
		if (shift > 20)	/* 1 MiB */
			shift = 20;
		tmp->iosize = 1U << shift;
	}

	error = VOP_GETATTR(vp, &va);
	if (error != 0)
		goto bad;
	mtime = va.va_mtime.tv_sec;
	tmp->mtime = mtime;

	error = tarfs_io_init(tmp);
	if (error != 0)
		goto bad;

	error = tarfs_alloc_node(tmp, NULL, 0, VDIR, 0, 0, mtime, root_uid,
	    root_gid, root_mode & ALLPERMS, 0, NULL, (dev_t)(-1), NULL, &root);
	if (error != 0 || root == NULL)
		goto bad;
	tmp->root = root;

	blknum = 0;
	do {
		error = tarfs_alloc_one(tmp, &blknum);
		if (error != 0) {
			kprintf("tarfs: unsupported or corrupt tar at block %zu\n",
			    blknum);
			goto bad;
		}
	} while (blknum != TAR_EOF);

	*tmpp = tmp;
	return (0);
bad:
	tarfs_free_mount(tmp);
	mp->mnt_data = NULL;
	return (error);
}

static int
tarfs_mount(struct mount *mp, char *path, caddr_t data, struct ucred *cred)
{
	struct tarfs_args args;
	char from[MAXPATHLEN];
	const char *asname;
	struct nlookupdata nd;
	struct tarfs_mount *tmp = NULL;
	struct vnode *vp = NULL;
	uid_t root_uid;
	gid_t root_gid;
	mode_t root_mode;
	int error, export_error;
	size_t n;

	if (mp->mnt_flag & MNT_UPDATE) {
		/*
		 * Only NFS export updates are meaningful for a RO tarfs.
		 * Accept export_args-style updates via full tarfs_args.
		 */
		error = copyin(data, &args, sizeof(args));
		if (error)
			return (error);
		tmp = MP_TO_TARFS_MOUNT(mp);
		export_error = vfs_export(mp, &tmp->export, &args.export);
		return (export_error);
	}

	/*
	 * Structured tarfs_args is identified by magic/version.
	 * Bare path strings (legacy) use copyinstr only.
	 * Never guess ABI from a large copyin appearing to succeed.
	 */
	bzero(&args, sizeof(args));
	error = copyin(data, &args, sizeof(args));
	if (error == 0 &&
	    args.magic == TARFS_ARGS_MAGIC &&
	    args.version == TARFS_ARGS_VERSION &&
	    args.fspec[0] != '\0') {
		strlcpy(from, args.fspec, sizeof(from));
	} else {
		error = copyinstr(data, from, sizeof(from), &n);
		if (error)
			return (error);
		bzero(&args, sizeof(args));
		strlcpy(args.fspec, from, sizeof(args.fspec));
	}


	root_uid = 0;
	root_gid = 0;
	root_mode = 0755;
	/* Only root may override synthetic root ownership (FreeBSD policy). */
	if (cred != NULL && cred->cr_ruid == 0) {
		if (args.mode != 0)
			root_mode = args.mode;
		root_uid = args.uid;
		root_gid = args.gid;
	}

	/* Open tarball */
	error = nlookup_init(&nd, from, UIO_SYSSPACE, NLC_FOLLOW);
	if (error == 0)
		error = nlookup(&nd);
	if (error == 0)
		error = cache_vget(&nd.nl_nch, nd.nl_cred, LK_EXCLUSIVE, &vp);
	nlookup_done(&nd);
	if (error)
		return (error);

	if (vp->v_type != VREG) {
		vput(vp);
		return (EOPNOTSUPP);
	}

	error = VOP_OPEN(vp, FREAD, FSCRED, NULL);
	if (error) {
		vput(vp);
		return (error);
	}
	if (vn_islocked(vp))
		vn_unlock(vp);

	error = tarfs_alloc_mount(mp, vp, root_uid, root_gid, root_mode, &tmp);
	if (error != 0)
		return (error);

	/* Optional NFS export (initial). */
	if (args.export.ex_flags != 0) {
		export_error = vfs_export(mp, &tmp->export, &args.export);
		if (export_error != 0) {
			tarfs_free_mount(tmp);
			mp->mnt_data = NULL;
			return (export_error);
		}
	}

	vfs_getnewfsid(mp);
	vfs_add_vnodeops(mp, &tarfs_vnode_vops, &mp->mnt_vn_norm_ops);

	mp->mnt_flag |= MNT_LOCAL | MNT_RDONLY;
	mp->mnt_kern_flag |= MNTK_ALL_MPSAFE;
	(void)args.flags; /* TARFSMNT_VERIFY reserved (no O_VERIFY on DF) */

	bzero(mp->mnt_stat.f_mntfromname, sizeof(mp->mnt_stat.f_mntfromname));
	bzero(mp->mnt_stat.f_mntonname, sizeof(mp->mnt_stat.f_mntonname));
	asname = (args.as[0] != '\0') ? args.as : from;
	strlcpy(mp->mnt_stat.f_mntfromname, asname,
	    sizeof(mp->mnt_stat.f_mntfromname));
	if (path != NULL)
		copyinstr(path, mp->mnt_stat.f_mntonname,
		    sizeof(mp->mnt_stat.f_mntonname), NULL);
	(void)tarfs_statfs(mp, &mp->mnt_stat, cred);

	return (0);
}

static int
tarfs_unmount(struct mount *mp, int mntflags)
{
	struct tarfs_mount *tmp;
	int error, flags = 0;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	error = vflush(mp, 0, flags);
	if (error)
		return (error);

	tmp = MP_TO_TARFS_MOUNT(mp);
	tarfs_free_mount(tmp);
	mp->mnt_data = NULL;
	return (0);
}

static int
tarfs_root(struct mount *mp, struct vnode **vpp)
{
	struct tarfs_mount *tmp = MP_TO_TARFS_MOUNT(mp);
	int error;

	error = tarfs_alloc_vp(mp, tmp->root, LK_EXCLUSIVE, vpp);
	if (error == 0)
		(*vpp)->v_flag |= VROOT;
	return (error);
}

static int
tarfs_statfs(struct mount *mp, struct statfs *sbp, struct ucred *cred __unused)
{
	struct tarfs_mount *tmp = MP_TO_TARFS_MOUNT(mp);

	sbp->f_bsize = TARFS_BLOCKSIZE;
	sbp->f_iosize = (long)tmp->iosize;
	sbp->f_blocks = tmp->nblocks;
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = (long)tmp->nfiles;
	sbp->f_ffree = 0;
	if (sbp != &mp->mnt_stat) {
		sbp->f_type = mp->mnt_vfc->vfc_typenum;
		bcopy(&mp->mnt_stat.f_fsid, &sbp->f_fsid, sizeof(sbp->f_fsid));
		bcopy(mp->mnt_stat.f_mntonname, sbp->f_mntonname,
		    MNAMELEN);
		bcopy(mp->mnt_stat.f_mntfromname, sbp->f_mntfromname,
		    MNAMELEN);
	}
	return (0);
}


/*
 * Look up a tarfs_node by inode number (FreeBSD tarfs_vget without vfs_hash).
 */
static struct tarfs_node *
tarfs_find_ino(struct tarfs_mount *tmp, ino_t ino)
{
	struct tarfs_node *tnp;

	TARFS_ALLNODES_LOCK(tmp);
	TAILQ_FOREACH(tnp, &tmp->allnodes, entries) {
		if (tnp->ino == ino)
			break;
	}
	TARFS_ALLNODES_UNLOCK(tmp);
	return (tnp);
}

static int
tarfs_vget(struct mount *mp, struct vnode *dvp __unused, ino_t ino,
    struct vnode **vpp)
{
	struct tarfs_mount *tmp = MP_TO_TARFS_MOUNT(mp);
	struct tarfs_node *tnp;
	int error;

	if (ino == TARFS_ROOTINO)
		tnp = tmp->root;
	else
		tnp = tarfs_find_ino(tmp, ino);
	if (tnp == NULL) {
		*vpp = NULL;
		return (ENOENT);
	}
	error = tarfs_alloc_vp(mp, tnp, LK_EXCLUSIVE, vpp);
	if (error == 0 && ino == TARFS_ROOTINO)
		(*vpp)->v_flag |= VROOT;
	return (error);
}

static int
tarfs_fhtovp(struct mount *mp, struct vnode *rootvp __unused,
    struct fid *fhp, struct vnode **vpp)
{
	struct tarfs_fid *tfp = (struct tarfs_fid *)(void *)fhp;
	struct tarfs_node *tnp;
	struct vnode *nvp;
	int error;

	if (tfp->len != sizeof(struct tarfs_fid))
		return (ESTALE);
	if (tfp->ino < TARFS_ROOTINO)
		return (ESTALE);

	error = tarfs_vget(mp, NULL, tfp->ino, &nvp);
	if (error != 0) {
		*vpp = NULL;
		return (error);
	}
	tnp = VP_TO_TARFS_NODE(nvp);
	if (tnp->gen != tfp->gen || tnp->nlink <= 0) {
		vput(nvp);
		*vpp = NULL;
		return (ESTALE);
	}
	*vpp = nvp;
	return (0);
}

static int
tarfs_vptofh(struct vnode *vp, struct fid *fhp)
{
	struct tarfs_fid tfh;
	struct tarfs_node *tnp = VP_TO_TARFS_NODE(vp);

	bzero(&tfh, sizeof(tfh));
	tfh.len = sizeof(struct tarfs_fid);
	tfh.pad = 0;
	tfh.ino = tnp->ino;
	tfh.gen = (uint64_t)tnp->gen;
	bcopy(&tfh, fhp, sizeof(tfh));
	return (0);
}

static int
tarfs_checkexp(struct mount *mp, struct sockaddr *nam, int *exflagsp,
    struct ucred **credanonp)
{
	struct tarfs_mount *tmp = MP_TO_TARFS_MOUNT(mp);
	struct netcred *np;

	np = vfs_export_lookup(mp, &tmp->export, nam);
	if (np == NULL)
		return (EACCES);
	*exflagsp = np->netc_exflags;
	*credanonp = &np->netc_anon;
	return (0);
}

static struct vfsops tarfs_vfsops = {
	.vfs_mount =	tarfs_mount,
	.vfs_unmount =	tarfs_unmount,
	.vfs_root =	tarfs_root,
	.vfs_statfs =	tarfs_statfs,
	.vfs_vget =	tarfs_vget,
	.vfs_fhtovp =	tarfs_fhtovp,
	.vfs_vptofh =	tarfs_vptofh,
	.vfs_checkexp =	tarfs_checkexp,
};
VFS_SET(tarfs_vfsops, tarfs, VFCF_MPSAFE | VFCF_READONLY);
