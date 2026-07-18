/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 Juniper Networks, Inc.
 * Copyright (c) 2022-2023 Klara, Inc.
 * Copyright (c) 2026 The DragonFly Project (DragonFly port).
 *
 * Based on FreeBSD sys/fs/tarfs/tarfs_subr.c
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/mutex2.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <vm/vm.h>
#include <vm/vnode_pager.h>
#include <sys/uio.h>

#include "tarfs.h"
#include "tarfs_dbg.h"

MALLOC_DEFINE(M_TARFSNODE, "tarfs node", "tarfs node structures");
MALLOC_DEFINE(M_TARFSNAME, "tarfs name", "tarfs path element");
MALLOC_DEFINE(M_TARFSBLK, "tarfs blk", "tarfs sparse block map");

unsigned int tarfs_ioshift = TARFS_IOSHIFT_DEFAULT;

SYSCTL_NODE(_vfs, OID_AUTO, tarfs, CTLFLAG_RW, 0, "tar filesystem");
SYSCTL_UINT(_vfs_tarfs, OID_AUTO, ioshift, CTLFLAG_RW,
    &tarfs_ioshift, 0, "Preferred I/O size shift for tarfs mounts");

#ifdef TARFS_DEBUG
int tarfs_debug;
SYSCTL_INT(_vfs_tarfs, OID_AUTO, debug, CTLFLAG_RW, &tarfs_debug, 0,
    "tarfs debug flags");
#endif

struct tarfs_node *
tarfs_lookup_name(struct tarfs_node *tnp, const char *name, size_t namelen)
{
	struct tarfs_node *entry;

	TAILQ_FOREACH(entry, &tnp->dir.dirhead, dirents) {
		if (entry->namelen == namelen &&
		    bcmp(entry->name, name, namelen) == 0) {
			if (entry->type == VREG && entry->other != NULL)
				entry = entry->other;
			return (entry);
		}
	}
	return (NULL);
}

struct tarfs_node *
tarfs_lookup_dir(struct tarfs_node *tnp, off_t cookie)
{
	struct tarfs_node *current;

	if (cookie == tnp->dir.lastcookie && tnp->dir.lastnode != NULL)
		return (tnp->dir.lastnode);

	TAILQ_FOREACH(current, &tnp->dir.dirhead, dirents) {
		if ((off_t)current->ino == cookie)
			return (current);
	}
	return (NULL);
}

int
tarfs_alloc_node(struct tarfs_mount *tmp, const char *name, size_t namelen,
    enum vtype type, off_t off, size_t sz, time_t mtime, uid_t uid, gid_t gid,
    mode_t mode, unsigned int flags, const char *linkname, dev_t rdev,
    struct tarfs_node *parent, struct tarfs_node **retnode)
{
	struct tarfs_node *tnp;

	if (parent != NULL && parent->type != VDIR)
		return (ENOTDIR);

	tnp = kmalloc(sizeof(*tnp), M_TARFSNODE, M_WAITOK | M_ZERO);
	mtx_init(&tnp->lock, "tarfsnode");
	tnp->gen = karc4random();
	tnp->tmp = tmp;
	if (namelen > 0) {
		tnp->name = kmalloc(namelen + 1, M_TARFSNAME, M_WAITOK);
		tnp->namelen = namelen;
		bcopy(name, tnp->name, namelen);
		tnp->name[namelen] = '\0';
	}
	tnp->type = type;
	tnp->uid = uid;
	tnp->gid = gid;
	tnp->mode = mode;
	tnp->flags = flags;
	tnp->nlink = 1;
	vfs_timestamp(&tnp->atime);
	tnp->mtime.tv_sec = mtime;
	tnp->birthtime = tnp->atime;
	tnp->ctime = tnp->mtime;
	if (parent != NULL)
		tnp->ino = (ino_t)alloc_unr(tmp->ino_unr);
	tnp->offset = off;
	tnp->size = tnp->physize = sz;

	switch (type) {
	case VDIR:
		TAILQ_INIT(&tnp->dir.dirhead);
		tnp->nlink++;
		if (parent == NULL)
			tnp->ino = TARFS_ROOTINO;
		tnp->physize = 0;
		break;
	case VLNK:
		tnp->link.name = kmalloc(sz + 1, M_TARFSNAME, M_WAITOK);
		tnp->link.namelen = sz;
		bcopy(linkname, tnp->link.name, sz);
		tnp->link.name[sz] = '\0';
		break;
	case VREG:
		tnp->nblk = 1;
		tnp->blk = kmalloc(sizeof(*tnp->blk), M_TARFSBLK, M_WAITOK);
		tnp->blk[0].i = 0;
		tnp->blk[0].o = 0;
		tnp->blk[0].l = tnp->physize;
		break;
	case VFIFO:
		break;
	case VBLK:
	case VCHR:
		tnp->rdev = rdev;
		tnp->physize = 0;
		break;
	default:
		panic("tarfs_alloc_node: type %d", type);
	}

	if (parent != NULL) {
		TARFS_NODE_LOCK(parent);
		TAILQ_INSERT_TAIL(&parent->dir.dirhead, tnp, dirents);
		parent->size += sizeof(struct tarfs_node);
		tnp->parent = parent;
		if (type == VDIR)
			parent->nlink++;
		TARFS_NODE_UNLOCK(parent);
	} else {
		tnp->parent = tnp;
	}

	TARFS_ALLNODES_LOCK(tmp);
	TAILQ_INSERT_TAIL(&tmp->allnodes, tnp, entries);
	TARFS_ALLNODES_UNLOCK(tmp);

	*retnode = tnp;
	tmp->nfiles++;
	return (0);
}

int
tarfs_load_blockmap(struct tarfs_node *tnp, size_t realsize)
{
	struct tarfs_blk *blk = NULL;
	char *map = NULL;
	size_t nmap = 0, nblk = 0;
	char *p, *q;
	ssize_t res;
	unsigned int i;
	long n;
	int error;

	do {
		nmap++;
		if (tnp->size < nmap * TARFS_BLOCKSIZE)
			goto bad;
		/* DragonFly krealloc rejects M_ZERO; new tail is filled by read + NUL. */
		map = krealloc(map, nmap * TARFS_BLOCKSIZE + 1, M_TARFSBLK,
		    M_WAITOK);
		res = tarfs_io_read_buf(tnp->tmp, 0,
		    map + (nmap - 1) * TARFS_BLOCKSIZE,
		    tnp->offset + (nmap - 1) * TARFS_BLOCKSIZE,
		    TARFS_BLOCKSIZE);
		if (res < 0) {
			error = (int)(-res);
			goto fail;
		}
		if (res < (ssize_t)TARFS_BLOCKSIZE) {
			error = EIO;
			goto fail;
		}
		map[nmap * TARFS_BLOCKSIZE] = '\0';
		if (nblk == 0) {
			n = strtol(p = map, &q, 10);
			if (q == p || *q != '\n' || n < 1)
				goto syntax;
			nblk = (size_t)n;
		}
		for (n = 0, p = map; *p != '\0'; ++p) {
			if (*p == '\n')
				++n;
		}
	} while ((size_t)n < nblk * 2 + 1);

	blk = kmalloc(sizeof(*blk) * nblk, M_TARFSBLK, M_WAITOK | M_ZERO);
	p = strchr(map, '\n') + 1;
	for (i = 0; i < nblk; i++) {
		if (i == 0)
			blk[i].i = nmap * TARFS_BLOCKSIZE;
		else
			blk[i].i = blk[i - 1].i + blk[i - 1].l;
		n = strtol(p, &q, 10);
		if (q == p || *q != '\n' || n < 0)
			goto syntax;
		p = q + 1;
		blk[i].o = n;
		n = strtol(p, &q, 10);
		if (q == p || *q != '\n' || n < 0)
			goto syntax;
		p = q + 1;
		blk[i].l = (size_t)n;
		if (blk[i].l != 0) {
			if (blk[i].i % TARFS_BLOCKSIZE != 0 ||
			    blk[i].o % TARFS_BLOCKSIZE != 0)
				goto bad;
		}
		if (i > 0 && blk[i].o < blk[i - 1].o + (off_t)blk[i - 1].l)
			goto bad;
		if ((size_t)blk[i].i + blk[i].l > tnp->physize ||
		    (size_t)blk[i].o + blk[i].l > realsize)
			goto bad;
	}
	kfree(map, M_TARFSBLK);
	kfree(tnp->blk, M_TARFSBLK);
	tnp->nblk = nblk;
	tnp->blk = blk;
	tnp->size = realsize;
	return (0);
syntax:
bad:
	error = EINVAL;
fail:
	if (map)
		kfree(map, M_TARFSBLK);
	if (blk)
		kfree(blk, M_TARFSBLK);
	return (error);
}

void
tarfs_free_node(struct tarfs_node *tnp)
{
	struct tarfs_mount *tmp = tnp->tmp;

	switch (tnp->type) {
	case VREG:
		if (tnp->nlink-- > 1)
			return;
		break;
	case VDIR:
		if (tnp->nlink-- > 2)
			return;
		break;
	case VLNK:
		if (tnp->link.name)
			kfree(tnp->link.name, M_TARFSNAME);
		break;
	default:
		break;
	}
	if (tnp->name != NULL)
		kfree(tnp->name, M_TARFSNAME);
	if (tnp->blk != NULL)
		kfree(tnp->blk, M_TARFSBLK);
	if (tnp->ino >= TARFS_MININO)
		free_unr(tmp->ino_unr, (u_int)tnp->ino);
	TAILQ_REMOVE(&tmp->allnodes, tnp, entries);
	mtx_uninit(&tnp->lock);
	kfree(tnp, M_TARFSNODE);
	tmp->nfiles--;
}

int
tarfs_read_file(struct tarfs_node *tnp, size_t len, struct uio *uiop)
{
	size_t resid = len;
	size_t copylen;
	unsigned int i;
	int error;
	struct uio auio;

	for (i = 0; i < tnp->nblk && resid > 0; ++i) {
		if (uiop->uio_offset > tnp->blk[i].o + (off_t)tnp->blk[i].l)
			continue;
		while (resid > 0 &&
		    uiop->uio_offset < tnp->blk[i].o) {
			copylen = (size_t)(tnp->blk[i].o - uiop->uio_offset);
			if (copylen > resid)
				copylen = resid;
			{
				static const char zbuf[512];
				size_t n;
				while (copylen > 0) {
					n = copylen > sizeof(zbuf) ? sizeof(zbuf) : copylen;
					error = uiomove(__DECONST(void *, zbuf), n, uiop);
					if (error != 0)
						return (error);
					copylen -= n;
					resid -= n;
				}
			}
		}
		while (resid > 0 &&
		    uiop->uio_offset < tnp->blk[i].o + (off_t)tnp->blk[i].l) {
			off_t into = uiop->uio_offset - tnp->blk[i].o;

			if (into < 0)
				break;
			/* remaining data bytes in this map entry */
			copylen = tnp->blk[i].l - (size_t)into;
			if (copylen > resid)
				copylen = resid;
			if (copylen == 0)
				break;
			auio = *uiop;
			auio.uio_offset = tnp->offset + tnp->blk[i].i + into;
			auio.uio_resid = copylen;
			error = tarfs_io_read(tnp->tmp, 0, &auio);
			if (error != 0)
				return (error);
			{
				size_t got = copylen - auio.uio_resid;
				uiop->uio_offset += got;
				uiop->uio_resid -= got;
				resid -= got;
				if (got == 0)
					return (EIO);
			}
		}
	}
	/* trailing hole after last map entry */
	while (resid > 0 && uiop->uio_offset < (off_t)tnp->size) {
		static const char zbuf[512];
		size_t n;

		copylen = (size_t)((off_t)tnp->size - uiop->uio_offset);
		if (copylen > resid)
			copylen = resid;
		while (copylen > 0) {
			n = copylen > sizeof(zbuf) ? sizeof(zbuf) : copylen;
			error = uiomove(__DECONST(void *, zbuf), n, uiop);
			if (error != 0)
				return (error);
			copylen -= n;
			resid -= n;
		}
	}
	return (0);
}

/* SCHILY.fflags subset — from FreeBSD tarfs_subr.c */
static const struct {
	const char *name;
	unsigned int flag;
} tarfs_flags[] = {
	{ "nodump", UF_NODUMP },
	{ "uchg", UF_IMMUTABLE },
	{ "uappnd", UF_APPEND },
	{ "opaque", UF_OPAQUE },
	{ "uunlnk", UF_NOUNLINK },
	{ "arch", SF_ARCHIVED },
	{ "schg", SF_IMMUTABLE },
	{ "sappnd", SF_APPEND },
	{ "sunlnk", SF_NOUNLINK },
	{ NULL, 0 },
};

unsigned int
tarfs_strtofflags(const char *str, char **end)
{
	const char *p, *q;
	unsigned int ret = 0;
	int i;

	for (p = q = str; *q != '\0'; p = q + 1) {
		for (q = p; *q != '\0' && *q != ','; ++q)
			;
		for (i = 0; tarfs_flags[i].name != NULL; i++) {
			size_t nlen = strlen(tarfs_flags[i].name);
			if ((size_t)(q - p) == nlen &&
			    bcmp(tarfs_flags[i].name, p, nlen) == 0) {
				ret |= tarfs_flags[i].flag;
				break;
			}
		}
		if (*q == '\0')
			break;
	}
	if (end)
		*end = __DECONST(char *, q);
	return (ret);
}

/*
 * Allocate or reget a vnode for tarfs_node.
 * Inspired by tmpfs_alloc_vp (simplified for read-only tree).
 */
int
tarfs_alloc_vp(struct mount *mp, struct tarfs_node *tnp, int lkflags,
    struct vnode **vpp)
{
	struct vnode *vp;
	int error;

loop:
	TARFS_NODE_LOCK(tnp);
	if (tnp->vnode != NULL) {
		vp = tnp->vnode;
		vhold(vp);
		TARFS_NODE_UNLOCK(tnp);
		error = vget(vp, lkflags | LK_EXCLUSIVE);
		vdrop(vp);
		if (error) {
			if (error == ENOENT)
				goto loop;
			return (error);
		}
		if (VP_TO_TARFS_NODE(vp) != tnp) {
			vput(vp);
			goto loop;
		}
		*vpp = vp;
		return (0);
	}
	TARFS_NODE_UNLOCK(tnp);

	error = getnewvnode(VT_NON, mp, &vp, VLKTIMEOUT, LK_CANRECURSE);
	if (error)
		return (error);

	TARFS_NODE_LOCK(tnp);
	if (tnp->vnode != NULL) {
		/* lost race */
		TARFS_NODE_UNLOCK(tnp);
		vp->v_type = VBAD;
		vx_put(vp);
		goto loop;
	}
	vp->v_data = tnp;
	vp->v_type = tnp->type;
	tnp->vnode = vp;
	TARFS_NODE_UNLOCK(tnp);

	/*
	 * VMIO is mandatory for VREG: bread/getblk asserts v_object.
	 * NFS fhtovp -> VOP_READ never calls VOP_OPEN, so init here
	 * (tmpfs_alloc_vp pattern), not in open.
	 */
	if (vp->v_type == VREG) {
		struct tarfs_node *src = tnp;
		off_t vmsize;

		if (tnp->other != NULL)
			src = tnp->other;
		vmsize = (off_t)src->size;
		vinitvmio(vp, vmsize, PAGE_SIZE, -1);
	}

	/* vnops already on mount via vfs_add_vnodeops */
	vx_downgrade(vp);
	*vpp = vp;
	KKASSERT(vn_islocked(vp));
	return (0);
}
