/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 Juniper Networks, Inc.
 * Copyright (c) 2022-2023 Klara, Inc.
 * Copyright (c) 2026 The DragonFly Project (DragonFly namecache VOP port).
 *
 * Logic based on FreeBSD sys/fs/tarfs/tarfs_vnops.c
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/mutex2.h>
#include <sys/namecache.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/buf2.h>
#include <sys/vnode.h>
#include <vm/vm.h>
#include <vm/vnode_pager.h>


#include "tarfs.h"
#include "tarfs_dbg.h"

static int
tarfs_open(struct vop_open_args *ap)
{
	struct vnode *vp = ap->a_vp;

	if (vp->v_type != VREG && vp->v_type != VDIR)
		return (EOPNOTSUPP);
	/* VMIO object is created in tarfs_alloc_vp (NFS fhtovp skips open). */
	return (vop_stdopen(ap));
}

static int
tarfs_close(struct vop_close_args *ap)
{
	return (vop_stdclose(ap));
}

static int
tarfs_access(struct vop_access_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct tarfs_node *tnp = VP_TO_TARFS_NODE(vp);
	mode_t mode = ap->a_mode;

	switch (vp->v_type) {
	case VDIR:
	case VLNK:
	case VREG:
		if ((mode & VWRITE) != 0)
			return (EROFS);
		break;
	case VBLK:
	case VCHR:
	case VFIFO:
		break;
	default:
		return (EINVAL);
	}
	if ((mode & VWRITE) != 0)
		return (EROFS);
	return (vaccess(vp->v_type, tnp->mode, tnp->uid, tnp->gid,
	    mode, ap->a_cred));
}

static int
tarfs_getattr(struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct tarfs_node *tnp = VP_TO_TARFS_NODE(vp);
	struct tarfs_node *src = tnp;

	if (tnp->type == VREG && tnp->other != NULL)
		src = tnp->other;

	vap->va_type = vp->v_type;
	vap->va_mode = src->mode;
	vap->va_nlink = src->nlink;
	vap->va_uid = src->uid;
	vap->va_gid = src->gid;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = tnp->ino;
	vap->va_size = src->size;
	vap->va_blocksize = vp->v_mount->mnt_stat.f_iosize;
	vap->va_atime = src->atime;
	vap->va_ctime = src->ctime;
	vap->va_mtime = src->mtime;
	vap->va_gen = src->gen;
	vap->va_flags = src->flags;
	if (vp->v_type == VBLK || vp->v_type == VCHR) {
		/*
		 * DF st_rdev comes from vp->v_rdev (cdev), not va_rmajor.
		 * We still fill va_* for VFS consumers; open remains EOPNOTSUPP
		 * so no real cdev is attached for archive device nodes.
		 */
		vap->va_rmajor = umajor(src->rdev);
		vap->va_rminor = uminor(src->rdev);
	} else {
		vap->va_rmajor = VNOVAL;
		vap->va_rminor = VNOVAL;
	}
	vap->va_bytes = round_page(src->physize);
	vap->va_filerev = 0;
	return (0);
}

static int
tarfs_setattr(struct vop_setattr_args *ap __unused)
{
	return (EROFS);
}

static int
tarfs_nresolve(struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct tarfs_node *dirnode = VP_TO_TARFS_NODE(dvp);
	struct tarfs_node *tnp;
	struct vnode *vp;
	int error;

	error = VOP_ACCESS(dvp, VEXEC, ap->a_cred);
	if (error)
		return (error);

	if (dirnode->type != VDIR)
		return (ENOTDIR);

	tnp = tarfs_lookup_name(dirnode, ncp->nc_name, ncp->nc_nlen);
	if (tnp == NULL) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}

	error = tarfs_alloc_vp(dvp->v_mount, tnp, LK_EXCLUSIVE, &vp);
	if (error)
		return (error);
	vn_unlock(vp);
	cache_setvp(ap->a_nch, vp);
	vrele(vp);
	return (0);
}

static int
tarfs_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct tarfs_node *dnode = VP_TO_TARFS_NODE(dvp);
	struct tarfs_node *parent;
	int error;

	*ap->a_vpp = NULL;
	error = VOP_ACCESS(dvp, VEXEC, ap->a_cred);
	if (error)
		return (error);

	parent = dnode->parent;
	if (parent == NULL)
		return (ENOENT);

	error = tarfs_alloc_vp(dvp->v_mount, parent, LK_EXCLUSIVE, ap->a_vpp);
	if (error == 0 && *ap->a_vpp)
		vn_unlock(*ap->a_vpp);
	return (error);
}

/*
 * Regular-file read via the buffer cache so VOP_STRATEGY fills pages for
 * UIO_NOCOPY (mmap / getpages).  Direct tarfs_read_file is used only by
 * strategy and internal helpers.
 */
static int
tarfs_read(struct vop_read_args *ap)
{
	struct buf *bp;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct tarfs_node *tnp, *src;
	off_t base_offset;
	size_t offset, len;
	int bsize, error;

	if (vp->v_type != VREG)
		return (EISDIR);
	if (uio->uio_offset < 0)
		return (EINVAL);

	tnp = VP_TO_TARFS_NODE(vp);
	src = (tnp->other != NULL) ? tnp->other : tnp;
	bsize = (int)vp->v_mount->mnt_stat.f_iosize;
	if (bsize <= 0)
		bsize = PAGE_SIZE;

	error = 0;
	while (uio->uio_resid > 0 && uio->uio_offset < (off_t)src->size) {
		offset = (size_t)uio->uio_offset & (size_t)(bsize - 1);
		base_offset = uio->uio_offset - (off_t)offset;

		error = bread(vp, base_offset, bsize, &bp);
		if (error != 0) {
			if (bp != NULL)
				brelse(bp);
			break;
		}

		len = (size_t)bsize - offset;
		if (len > uio->uio_resid)
			len = uio->uio_resid;
		if (len > src->size - (size_t)uio->uio_offset)
			len = src->size - (size_t)uio->uio_offset;

		error = uiomovebp(bp, (char *)bp->b_data + offset, len, uio);
		brelse(bp);
		if (error != 0)
			break;
	}
	return (error);
}

static int
tarfs_readdir(struct vop_readdir_args *ap)
{
	struct dirent cde;
	struct tarfs_node *current, *tnp;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	int *eofflag = ap->a_eofflag;
	off_t off;
	u_int ndirents = 0;
	int error = 0;
	size_t reclen;

	if (vp->v_type != VDIR)
		return (ENOTDIR);

	tnp = VP_TO_TARFS_NODE(vp);
	off = uio->uio_offset;
	current = NULL;
	bzero(&cde, sizeof(cde));

	if (uio->uio_offset == TARFS_COOKIE_EOF) {
		if (eofflag)
			*eofflag = 1;
		return (0);
	}

	if (uio->uio_offset == TARFS_COOKIE_DOT) {
		cde.d_ino = tnp->ino;
		cde.d_type = DT_DIR;
		cde.d_namlen = 1;
		cde.d_name[0] = '.';
		cde.d_name[1] = '\0';
		reclen = _DIRENT_DIRSIZ(&cde);
		if (reclen > uio->uio_resid)
			goto full;
		error = uiomove((caddr_t)&cde, reclen, uio);
		if (error)
			return (error);
		uio->uio_offset = TARFS_COOKIE_DOTDOT;
		ndirents++;
	}

	if (uio->uio_offset == TARFS_COOKIE_DOTDOT) {
		cde.d_ino = tnp->parent->ino;
		cde.d_type = DT_DIR;
		cde.d_namlen = 2;
		cde.d_name[0] = '.';
		cde.d_name[1] = '.';
		cde.d_name[2] = '\0';
		reclen = _DIRENT_DIRSIZ(&cde);
		if (reclen > uio->uio_resid)
			goto full;
		error = uiomove((caddr_t)&cde, reclen, uio);
		if (error)
			return (error);
		current = TAILQ_FIRST(&tnp->dir.dirhead);
		if (current == NULL)
			goto done;
		uio->uio_offset = (off_t)current->ino;
		ndirents++;
	}

	if (current == NULL) {
		current = tarfs_lookup_dir(tnp, uio->uio_offset);
		if (current == NULL) {
			error = EINVAL;
			goto done;
		}
		uio->uio_offset = (off_t)current->ino;
	}

	for (;;) {
		/* Match getattr/stat: hardlink name follows target inode. */
		if (current->type == VREG && current->other != NULL)
			cde.d_ino = current->other->ino;
		else
			cde.d_ino = current->ino;
		switch (current->type) {
		case VBLK: cde.d_type = DT_BLK; break;
		case VCHR: cde.d_type = DT_CHR; break;
		case VDIR: cde.d_type = DT_DIR; break;
		case VFIFO: cde.d_type = DT_FIFO; break;
		case VLNK: cde.d_type = DT_LNK; break;
		case VREG: cde.d_type = DT_REG; break;
		default: cde.d_type = DT_UNKNOWN; break;
		}
		cde.d_namlen = (uint16_t)current->namelen;
		if (current->namelen >= sizeof(cde.d_name)) {
			error = ENAMETOOLONG;
			goto done;
		}
		bcopy(current->name, cde.d_name, current->namelen);
		cde.d_name[current->namelen] = '\0';
		reclen = _DIRENT_DIRSIZ(&cde);
		if (reclen > uio->uio_resid)
			goto full;
		error = uiomove((caddr_t)&cde, reclen, uio);
		if (error)
			goto done;
		ndirents++;
		current = TAILQ_NEXT(current, dirents);
		if (current == NULL)
			goto done;
		uio->uio_offset = (off_t)current->ino;
	}
full:
	if (reclen > uio->uio_resid)
		error = (ndirents == 0) ? EINVAL : 0;
done:
	if (current == NULL) {
		uio->uio_offset = TARFS_COOKIE_EOF;
		tnp->dir.lastcookie = 0;
		tnp->dir.lastnode = NULL;
	} else {
		tnp->dir.lastcookie = current->ino;
		tnp->dir.lastnode = current;
	}
	if (eofflag != NULL)
		*eofflag = (error == 0 && uio->uio_offset == TARFS_COOKIE_EOF);

	/* NFS readdir cookies: one cookie per dirent emitted. */
	if (error == 0 && ap->a_cookies != NULL && ap->a_ncookies != NULL) {
		off_t *cookies;
		off_t coff = off;
		u_int i;
		struct tarfs_node *cn;

		*ap->a_ncookies = (int)ndirents;
		if (ndirents == 0) {
			*ap->a_cookies = NULL;
		} else {
			cookies = kmalloc(ndirents * sizeof(off_t), M_TEMP,
			    M_WAITOK);
			*ap->a_cookies = cookies;
			/*
			 * Reconstruct cookie sequence from start offset.
			 * Cookies are the "next" uio_offset after each dent.
			 */
			for (i = 0; i < ndirents; i++) {
				if (coff == TARFS_COOKIE_DOT) {
					coff = TARFS_COOKIE_DOTDOT;
				} else if (coff == TARFS_COOKIE_DOTDOT) {
					cn = TAILQ_FIRST(&tnp->dir.dirhead);
					coff = (cn != NULL) ?
					    (off_t)cn->ino : TARFS_COOKIE_EOF;
				} else {
					cn = tarfs_lookup_dir(tnp, coff);
					if (cn != NULL)
						cn = TAILQ_NEXT(cn, dirents);
					coff = (cn != NULL) ?
					    (off_t)cn->ino : TARFS_COOKIE_EOF;
				}
				cookies[i] = coff;
			}
		}
	}
	return (error);
}


static int
tarfs_readlink(struct vop_readlink_args *ap)
{
	struct tarfs_node *tnp = VP_TO_TARFS_NODE(ap->a_vp);
	struct uio *uiop = ap->a_uio;
	size_t len;

	if (ap->a_vp->v_type != VLNK)
		return (EINVAL);
	len = tnp->link.namelen;
	if (len > uiop->uio_resid)
		len = uiop->uio_resid;
	return (uiomove(tnp->link.name, len, uiop));
}

static int
tarfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct tarfs_node *tnp = VP_TO_TARFS_NODE(vp);

	TARFS_NODE_LOCK(tnp);
	tnp->vnode = NULL;
	vp->v_data = NULL;
	TARFS_NODE_UNLOCK(tnp);
	return (0);
}

static int
tarfs_inactive(struct vop_inactive_args *ap __unused)
{
	return (0);
}


/*
 * Logical offset readahead hints (FreeBSD tarfs_bmap).  There is no real
 * device mapping; strategy fills buffers by reading the archive.
 */
static int
tarfs_bmap(struct vop_bmap_args *ap)
{
	struct tarfs_node *tnp, *src;
	struct vnode *vp = ap->a_vp;
	off_t off, iosize;
	int ra, rb, rmax;
	u_int i;

	if (ap->a_doffsetp != NULL)
		*ap->a_doffsetp = ap->a_loffset;
	if (ap->a_runp == NULL && ap->a_runb == NULL)
		return (0);
	if (vp->v_type != VREG)
		return (0);

	tnp = VP_TO_TARFS_NODE(vp);
	src = (tnp->type == VREG && tnp->other != NULL) ? tnp->other : tnp;
	iosize = vp->v_mount->mnt_stat.f_iosize;
	if (iosize <= 0)
		iosize = PAGE_SIZE;
	off = ap->a_loffset;
	ra = rb = 0;

	for (i = 0; i < src->nblk; i++) {
		off_t bs, be;

		bs = src->blk[i].o;
		be = src->blk[i].o + (off_t)src->blk[i].l;
		if (off > be)
			continue;
		else if (off < bs) {
			/* hole */
			ra = (bs - off < iosize) ? 0 :
			    (int)howmany(bs - (off + iosize), iosize);
			rb = (int)howmany(off - (i == 0 ? 0 :
			    src->blk[i - 1].o + (off_t)src->blk[i - 1].l),
			    iosize);
			break;
		} else {
			ra = (be - off < iosize) ? 0 :
			    (int)howmany(be - (off + iosize), iosize);
			rb = (int)howmany(off - bs, iosize);
			break;
		}
	}
	rmax = (int)(vp->v_mount->mnt_iosize_max / iosize) - 1;
	if (rmax < 0)
		rmax = 0;
	if (ap->a_runp != NULL)
		*ap->a_runp = (ra < rmax) ? ra : rmax;
	if (ap->a_runb != NULL)
		*ap->a_runb = (rb < rmax) ? rb : rmax;
	return (0);
}

/*
 * Buffer strategy: fill bp from tar content (not a disk device).
 */
static int
tarfs_strategy(struct vop_strategy_args *ap)
{
	struct bio *bio = ap->a_bio;
	struct buf *bp = bio->bio_buf;
	struct tarfs_node *tnp, *src;
	struct uio auio;
	struct iovec iov;
	off_t off;
	size_t len;
	int error;

	tnp = VP_TO_TARFS_NODE(ap->a_vp);
	src = (tnp->type == VREG && tnp->other != NULL) ? tnp->other : tnp;

	if (bp->b_cmd != BUF_CMD_READ) {
		bp->b_error = EOPNOTSUPP;
		bp->b_flags |= B_ERROR;
		biodone(bio);
		return (0);
	}

	off = bio->bio_offset;
	len = (size_t)bp->b_bcount;
	bp->b_resid = (int)len;
	if (off < 0 || (size_t)off > src->size) {
		error = EIO;
		goto out;
	}
	if ((size_t)off + len > src->size)
		len = src->size - (size_t)off;

	/* Zero whole buffer first so short EOF cannot leak prior contents. */
	bzero(bp->b_data, bp->b_bcount);

	iov.iov_base = bp->b_data;
	iov.iov_len = len;
	auio.uio_iov = &iov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = off;
	auio.uio_resid = len;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_td = curthread;
	error = tarfs_read_file(src, len, &auio);
	if (error == 0 && auio.uio_resid != 0)
		error = EIO;
out:
	if (error != 0) {
		bp->b_error = error;
		bp->b_flags |= B_ERROR;
	} else {
		/* Full buffer valid (short EOF already zero-filled). */
		bp->b_resid = 0;
	}
	biodone(bio);
	return (0);
}

static int
tarfs_print(struct vop_print_args *ap)
{
	struct tarfs_node *tnp = VP_TO_TARFS_NODE(ap->a_vp);

	kprintf("tag tarfs, tarfs_node %p, links %lu\n",
	    tnp, (unsigned long)tnp->nlink);
	kprintf("\tmode 0%o, owner %u, group %u, size %zu\n",
	    tnp->mode, tnp->uid, tnp->gid, tnp->size);
	return (0);
}

struct vop_ops tarfs_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_nresolve =		tarfs_nresolve,
	.vop_nlookupdotdot =	tarfs_nlookupdotdot,
	.vop_open =		tarfs_open,
	.vop_close =		tarfs_close,
	.vop_access =		tarfs_access,
	.vop_getattr =		tarfs_getattr,
	.vop_setattr =		tarfs_setattr,
	.vop_read =		tarfs_read,
	.vop_readdir =		tarfs_readdir,
	.vop_readlink =		tarfs_readlink,
	.vop_bmap =		tarfs_bmap,
	.vop_strategy =		tarfs_strategy,
	.vop_getpages =		vop_stdgetpages,
	.vop_putpages =		vop_stdputpages,
	.vop_inactive =		tarfs_inactive,
	.vop_reclaim =		tarfs_reclaim,
	.vop_print =		tarfs_print,
	.vop_pathconf =		vop_stdpathconf,
};
