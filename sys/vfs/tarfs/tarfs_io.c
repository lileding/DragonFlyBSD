/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 Juniper Networks, Inc.
 * Copyright (c) 2022-2023 Klara, Inc.
 * Copyright (c) 2026 The DragonFly Project (DragonFly port + zstd).
 *
 * Based on FreeBSD sys/fs/tarfs/tarfs_io.c
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/mutex2.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "tarfs.h"
#include "tarfs_dbg.h"

MALLOC_DEFINE(M_TARFSZSTATE, "tarfs zstate", "tarfs zstd state");
MALLOC_DEFINE(M_TARFSZBUF, "tarfs zbuf", "tarfs zstd buffers");

/* ZSTD I/O counters (FreeBSD vfs.tarfs.zio.*) */
static u_long tarfs_zio_inflated;
static u_long tarfs_zio_consumed;
static u_long tarfs_zio_bounced;

SYSCTL_NODE(_vfs_tarfs, OID_AUTO, zio, CTLFLAG_RD, 0,
    "Tar filesystem decompression layer");
SYSCTL_ULONG(_vfs_tarfs_zio, OID_AUTO, inflated, CTLFLAG_RW,
    &tarfs_zio_inflated, 0, "Bytes inflated from zstd stream");
SYSCTL_ULONG(_vfs_tarfs_zio, OID_AUTO, consumed, CTLFLAG_RW,
    &tarfs_zio_consumed, 0, "Logical bytes returned to readers");
SYSCTL_ULONG(_vfs_tarfs_zio, OID_AUTO, bounced, CTLFLAG_RW,
    &tarfs_zio_bounced, 0, "Bytes copied via bounce buffer");


static uint8_t XZ_MAGIC[] = { 0xfd, 0x37, 0x7a, 0x58, 0x5a };
static uint8_t ZLIB_MAGIC[] = { 0x1f, 0x8b, 0x08 };
static uint8_t ZSTD_MAGIC[] = { 0x28, 0xb5, 0x2f, 0xfd };

struct tarfs_idx {
	off_t i;
	off_t o;
};

struct tarfs_zio {
	struct tarfs_mount *tmp;
	struct mtx lock;	/* exclusive: DStream + index (FreeBSD znode LK_EXCLUSIVE) */
	ZSTD_DStream *zds;
	off_t ipos;
	off_t opos;
	unsigned int curidx;
	unsigned int nidx;
	unsigned int szidx;
	struct tarfs_idx *idx;
};

static void *
tarfs_zstate_alloc(void *opaque, size_t size)
{
	(void)opaque;
	return (kmalloc(size, M_TARFSZSTATE, M_WAITOK));
}

static void
tarfs_zstate_free(void *opaque, void *address)
{
	(void)opaque;
	kfree(address, M_TARFSZSTATE);
}

static ZSTD_customMem tarfs_zstd_mem = {
	tarfs_zstate_alloc,
	tarfs_zstate_free,
	NULL,
};

static void
tarfs_zio_update_index(struct tarfs_zio *zio, off_t i, off_t o)
{
	if (++zio->curidx >= zio->nidx) {
		if (++zio->nidx > zio->szidx) {
			zio->szidx *= 2;
			zio->idx = krealloc(zio->idx,
			    zio->szidx * sizeof(*zio->idx),
			    M_TARFSZSTATE, M_WAITOK);
			bzero(&zio->idx[zio->nidx - 1],
			    (zio->szidx - (zio->nidx - 1)) * sizeof(*zio->idx));
		}
		zio->idx[zio->curidx].i = i;
		zio->idx[zio->curidx].o = o;
	}
	KKASSERT(zio->idx[zio->curidx].i == i);
	KKASSERT(zio->idx[zio->curidx].o == o);
}

/*
 * Read decompressed bytes at logical offset from a zstd-compressed tarball.
 * Frame index allows rewind/skip without full re-inflate from 0 every time.
 */
static int
tarfs_zread_zstd(struct tarfs_zio *zio, struct uio *uiop)
{
	void *ibuf = NULL, *obuf = NULL;
	struct uio auio;
	struct iovec aiov;
	struct tarfs_mount *tmp = zio->tmp;
	struct vnode *vp = tmp->vp;
	struct vattr va;
	ZSTD_inBuffer zib;
	ZSTD_outBuffer zob;
	off_t zsize;
	off_t ipos, opos;
	size_t ilen, olen;
	size_t zerror;
	off_t off = uiop->uio_offset;
	size_t len = uiop->uio_resid;
	size_t resid = uiop->uio_resid;
	size_t bsize;
	int error;
	int reset = 0;

	mtx_lock(&zio->lock);

	if (off < zio->opos) {
		while (zio->curidx > 0 && off < zio->idx[zio->curidx].o)
			zio->curidx--;
		reset = 1;
	}
	if (off > zio->opos) {
		while (zio->curidx < zio->nidx - 1 &&
		    off >= zio->idx[zio->curidx + 1].o) {
			zio->curidx++;
			reset = 1;
		}
	}
	if (reset) {
		zio->ipos = zio->idx[zio->curidx].i;
		zio->opos = zio->idx[zio->curidx].o;
		ZSTD_DCtx_reset(zio->zds, ZSTD_reset_session_only);
	}

	bsize = roundup(ZSTD_DStreamInSize(), PAGE_SIZE);
	if (bsize > MAXBSIZE)
		bsize = MAXBSIZE;
	ibuf = kmalloc(bsize, M_TEMP, M_WAITOK);
	zib.src = NULL;
	zib.size = 0;
	zib.pos = 0;

	/*
	 * Bounce buffer for userland destinations; kernel callers
	 * (mount scan, file read into sysspace) use UIO_SYSSPACE.
	 */
	if (uiop->uio_segflg == UIO_SYSSPACE && uiop->uio_iovcnt == 1) {
		zob.dst = uiop->uio_iov->iov_base;
	} else {
		zob.dst = obuf = kmalloc(len, M_TEMP, M_WAITOK);
	}
	zob.size = len;
	zob.pos = 0;

	error = vn_lock(vp, LK_SHARED);
	if (error != 0)
		goto out_nolock;

	error = VOP_GETATTR(vp, &va);
	if (error != 0)
		goto out;
	zsize = (off_t)va.va_size;
	if (zio->ipos >= zsize)
		goto out;

	while (resid > 0) {
		if (zib.pos == zib.size) {
			aiov.iov_base = ibuf;
			aiov.iov_len = bsize;
			auio.uio_iov = &aiov;
			auio.uio_iovcnt = 1;
			auio.uio_offset = zio->ipos;
			auio.uio_segflg = UIO_SYSSPACE;
			auio.uio_rw = UIO_READ;
			auio.uio_resid = aiov.iov_len;
			auio.uio_td = curthread;
			error = VOP_READ(vp, &auio, IO_NODELOCKED,
			    curthread->td_ucred);
			if (error != 0)
				goto out;
			zib.src = ibuf;
			zib.size = bsize - auio.uio_resid;
			zib.pos = 0;
		}
		if (zib.pos == zib.size)
			break;

		if (zio->opos < off) {
			zob.size = MIN(off - zio->opos, (off_t)len);
			zob.pos = 0;
		} else {
			zob.size = len;
			zob.pos = (size_t)(zio->opos - off);
		}
		ipos = zib.pos;
		opos = zob.pos;
		zerror = ZSTD_decompressStream(zio->zds, &zob, &zib);
		ilen = zib.pos - (size_t)ipos;
		olen = zob.pos - (size_t)opos;
		zio->ipos += (off_t)ilen;
		zio->opos += (off_t)olen;
		if (zio->opos > off)
			resid -= olen;
		if (ZSTD_isError(zerror)) {
			kprintf("tarfs: zstd inflate failed: %s\n",
			    ZSTD_getErrorName(zerror));
			error = EIO;
			goto out;
		}
		if (zerror == 0 && olen == 0)
			break;
		if (zerror == 0)
			tarfs_zio_update_index(zio, zio->ipos, zio->opos);
		tarfs_zio_inflated += olen;
	}
out:
	vn_unlock(vp);
out_nolock:
	if (error == 0) {
		if (obuf == NULL) {
			/* in-place into single iov */
			size_t got = len - resid;
			tarfs_zio_consumed += got;
			uiop->uio_resid -= got;
			uiop->uio_offset += got;
			if (uiop->uio_iovcnt == 1) {
				uiop->uio_iov->iov_base =
				    (char *)uiop->uio_iov->iov_base + got;
				uiop->uio_iov->iov_len -= got;
			}
		} else if (len > resid) {
			error = uiomove(obuf, len - resid, uiop);
			tarfs_zio_consumed += (len - resid);
			tarfs_zio_bounced += (len - resid);
		}
	}
	if (obuf != NULL)
		kfree(obuf, M_TEMP);
	if (ibuf != NULL)
		kfree(ibuf, M_TEMP);
	if (error != 0) {
		zio->curidx = 0;
		zio->ipos = zio->idx[0].i;
		zio->opos = zio->idx[0].o;
		if (zio->zds != NULL)
			ZSTD_DCtx_reset(zio->zds, ZSTD_reset_session_only);
	}
	mtx_unlock(&zio->lock);
	return (error);
}

int
tarfs_io_read(struct tarfs_mount *tmp, int raw, struct uio *uiop)
{
	struct vnode *vp = tmp->vp;
	int error;

	if (!raw && tmp->zio != NULL)
		return (tarfs_zread_zstd(tmp->zio, uiop));

	error = vn_lock(vp, LK_SHARED);
	if (error)
		return (error);
	error = VOP_READ(vp, uiop, IO_NODELOCKED,
	    (uiop->uio_td != NULL) ? uiop->uio_td->td_ucred : proc0.p_ucred);
	vn_unlock(vp);
	TARFS_DPF(IO, "%s residual %zu error %d\n", __func__,
	    uiop->uio_resid, error);
	return (error);
}

ssize_t
tarfs_io_read_buf(struct tarfs_mount *tmp, int raw,
    void *buf, off_t off, size_t len)
{
	struct uio auio;
	struct iovec aiov;
	ssize_t res;
	int error;

	if (len == 0)
		return (0);

	aiov.iov_base = buf;
	aiov.iov_len = len;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = off;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_resid = len;
	auio.uio_td = curthread;
	error = tarfs_io_read(tmp, raw, &auio);
	if (error != 0)
		return (-error);
	res = (ssize_t)(len - auio.uio_resid);
	return (res);
}

static struct tarfs_zio *
tarfs_zio_alloc(struct tarfs_mount *tmp)
{
	struct tarfs_zio *zio;

	zio = kmalloc(sizeof(*zio), M_TARFSZSTATE, M_WAITOK | M_ZERO);
	zio->tmp = tmp;
	mtx_init(&zio->lock, "tarfszio");
	zio->szidx = 128;
	zio->idx = kmalloc(zio->szidx * sizeof(*zio->idx), M_TARFSZSTATE,
	    M_WAITOK | M_ZERO);
	zio->curidx = 0;
	zio->nidx = 1;
	zio->idx[0].i = zio->ipos = 0;
	zio->idx[0].o = zio->opos = 0;
	zio->zds = ZSTD_createDStream_advanced(tarfs_zstd_mem);
	if (zio->zds == NULL) {
		kfree(zio->idx, M_TARFSZSTATE);
		kfree(zio, M_TARFSZSTATE);
		return (NULL);
	}
	(void)ZSTD_initDStream(zio->zds);
	tmp->zio = zio;
	return (zio);
}

int
tarfs_io_init(struct tarfs_mount *tmp)
{
	uint8_t *block;
	ssize_t res;
	int error = 0;

	tmp->zio = NULL;
	block = kmalloc(tmp->iosize, M_TEMP, M_WAITOK | M_ZERO);
	res = tarfs_io_read_buf(tmp, 1, block, 0, tmp->iosize);
	if (res < 0) {
		error = (int)(-res);
		goto out;
	}
	if (res >= (ssize_t)sizeof(XZ_MAGIC) &&
	    bcmp(block, XZ_MAGIC, sizeof(XZ_MAGIC)) == 0) {
		kprintf("tarfs: xz compression not supported "
		    "(use zstd or uncompressed tar)\n");
		error = EOPNOTSUPP;
	} else if (res >= (ssize_t)sizeof(ZLIB_MAGIC) &&
	    bcmp(block, ZLIB_MAGIC, sizeof(ZLIB_MAGIC)) == 0) {
		kprintf("tarfs: gzip/zlib compression not supported "
		    "(use zstd or uncompressed tar)\n");
		error = EOPNOTSUPP;
	} else if (res >= (ssize_t)sizeof(ZSTD_MAGIC) &&
	    bcmp(block, ZSTD_MAGIC, sizeof(ZSTD_MAGIC)) == 0) {
		if (tarfs_zio_alloc(tmp) == NULL) {
			kprintf("tarfs: zstd init failed\n");
			error = ENOMEM;
		} else {
			kprintf("tarfs: zstd compression enabled\n");
		}
	}
out:
	kfree(block, M_TEMP);
	return (error);
}

int
tarfs_io_fini(struct tarfs_mount *tmp)
{
	struct tarfs_zio *zio = tmp->zio;

	if (zio == NULL)
		return (0);
	if (zio->zds != NULL)
		ZSTD_freeDStream(zio->zds);
	if (zio->idx != NULL)
		kfree(zio->idx, M_TARFSZSTATE);
	mtx_uninit(&zio->lock);
	kfree(zio, M_TARFSZSTATE);
	tmp->zio = NULL;
	return (0);
}
