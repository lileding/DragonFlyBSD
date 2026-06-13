/*
 * Copyright (c) 2019-2020 François Tigeot <ftigeot@wolfpond.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/poll.h>
#include <linux/reservation.h>
#include <linux/sync_file.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/ktime.h>

#include <sys/sysctl.h>

struct fileops dmabuf_fileops;

SYSCTL_DECL(_hw_dri);

static uint64_t dmabuf_export_sync_file_count;
static uint64_t dmabuf_export_sync_file_us;
static uint64_t dmabuf_import_sync_file_count;
static uint64_t dmabuf_import_sync_file_us;
static uint64_t dmabuf_fd_count;
static uint64_t dmabuf_fd_error_count;
static uint64_t dmabuf_fd_us;
static uint64_t dmabuf_export_count;
static uint64_t dmabuf_get_count;
static uint64_t dmabuf_get_error_count;
static uint64_t dmabuf_close_count;

SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_export_sync_file_count, CTLFLAG_RD,
    &dmabuf_export_sync_file_count, 0, "dma-buf export sync_file count");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_export_sync_file_us, CTLFLAG_RD,
    &dmabuf_export_sync_file_us, 0, "dma-buf export sync_file time");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_import_sync_file_count, CTLFLAG_RD,
    &dmabuf_import_sync_file_count, 0, "dma-buf import sync_file count");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_import_sync_file_us, CTLFLAG_RD,
    &dmabuf_import_sync_file_us, 0, "dma-buf import sync_file time");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_fd_count, CTLFLAG_RD,
    &dmabuf_fd_count, 0, "dma-buf fd export count");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_fd_error_count, CTLFLAG_RD,
    &dmabuf_fd_error_count, 0, "dma-buf fd export error count");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_fd_us, CTLFLAG_RD,
    &dmabuf_fd_us, 0, "dma-buf fd export time");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_export_count, CTLFLAG_RD,
    &dmabuf_export_count, 0, "dma-buf export count");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_get_count, CTLFLAG_RD,
    &dmabuf_get_count, 0, "dma-buf get count");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_get_error_count, CTLFLAG_RD,
    &dmabuf_get_error_count, 0, "dma-buf get error count");
SYSCTL_UQUAD(_hw_dri, OID_AUTO, dmabuf_close_count, CTLFLAG_RD,
    &dmabuf_close_count, 0, "dma-buf close count");

static uint64_t
dmabuf_now_us(void)
{
	return ((uint64_t)ktime_to_us(ktime_get()));
}

struct dmabuf_stub_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *
dmabuf_stub_fence_get_name(struct dma_fence *fence)
{
	return "dmabufstub";
}

static const struct dma_fence_ops dmabuf_stub_fence_ops = {
	.get_driver_name = dmabuf_stub_fence_get_name,
	.get_timeline_name = dmabuf_stub_fence_get_name,
};

static bool
dmabuf_sync_flags_valid(uint32_t flags)
{
	return (flags != 0 && (flags & ~DMA_BUF_SYNC_RW) == 0);
}

static struct dma_fence *
dmabuf_signaled_fence_create(void)
{
	struct dmabuf_stub_fence *fence;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (fence == NULL)
		return NULL;

	lockinit(&fence->lock, "dbsf", 0, 0);
	dma_fence_init(&fence->base, &dmabuf_stub_fence_ops,
	    &fence->lock, dma_fence_context_alloc(1), 0);
	dma_fence_signal(&fence->base);

	return (&fence->base);
}

static void
dmabuf_put_fences(struct dma_fence *excl, unsigned shared_count,
    struct dma_fence **shared)
{
	unsigned i;

	if (excl != NULL)
		dma_fence_put(excl);
	for (i = 0; i < shared_count; i++)
		dma_fence_put(shared[i]);
	kfree(shared);
}

static int
dmabuf_sync_file_install(struct dma_fence *fence, int *sync_fd)
{
	struct sync_file *sync_file;
	int fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return (-fd);

	sync_file = sync_file_create(fence);
	if (sync_file == NULL) {
		put_unused_fd(fd);
		return (ENOMEM);
	}

	fd_install(fd, sync_file->file);
	*sync_fd = fd;
	return (0);
}

static int
dmabuf_export_sync_file(struct dma_buf *dmabuf,
    struct dma_buf_export_sync_file *args)
{
	struct dma_fence *excl = NULL;
	struct dma_fence **shared = NULL;
	struct dma_fence **fences;
	struct dma_fence *fence = NULL;
	struct dma_fence_array *array;
	unsigned shared_count = 0;
	unsigned count, i, out;
	int ret;

	if (!dmabuf_sync_flags_valid(args->flags))
		return (EINVAL);
	if (dmabuf->resv == NULL)
		return (EINVAL);

	if ((args->flags & DMA_BUF_SYNC_WRITE) == 0) {
		fence = reservation_object_get_excl_rcu(dmabuf->resv);
		if (fence == NULL)
			fence = dmabuf_signaled_fence_create();
		if (fence == NULL)
			return (ENOMEM);

		ret = dmabuf_sync_file_install(fence, &args->fd);
		dma_fence_put(fence);
		return (ret);
	}

	ret = reservation_object_get_fences_rcu(dmabuf->resv, &excl,
	    &shared_count, &shared);
	if (ret < 0)
		return (-ret);

	count = shared_count + (excl != NULL ? 1 : 0);
	if (count == 0) {
		fence = dmabuf_signaled_fence_create();
		if (fence == NULL)
			return (ENOMEM);
		ret = dmabuf_sync_file_install(fence, &args->fd);
		dma_fence_put(fence);
		return (ret);
	}

	if (count == 1) {
		fence = excl != NULL ? excl : shared[0];
		ret = dmabuf_sync_file_install(fence, &args->fd);
		dmabuf_put_fences(excl, shared_count, shared);
		return (ret);
	}

	fences = kmalloc_array(count, sizeof(*fences), GFP_KERNEL);
	if (fences == NULL) {
		dmabuf_put_fences(excl, shared_count, shared);
		return (ENOMEM);
	}

	out = 0;
	if (excl != NULL)
		fences[out++] = excl;
	for (i = 0; i < shared_count; i++)
		fences[out++] = shared[i];
	kfree(shared);

	array = dma_fence_array_create(count, fences, dma_fence_context_alloc(1),
	    0, false);
	if (array == NULL) {
		for (i = 0; i < count; i++)
			dma_fence_put(fences[i]);
		kfree(fences);
		return (ENOMEM);
	}

	ret = dmabuf_sync_file_install(&array->base, &args->fd);
	dma_fence_put(&array->base);
	return (ret);
}

static int
dmabuf_import_sync_file(struct dma_buf *dmabuf,
    const struct dma_buf_import_sync_file *args)
{
	struct dma_fence *fence;
	int ret;

	if (!dmabuf_sync_flags_valid(args->flags))
		return (EINVAL);
	if (dmabuf->resv == NULL)
		return (EINVAL);

	fence = sync_file_get_fence(args->fd);
	if (fence == NULL)
		return (EINVAL);

	ret = reservation_object_lock(dmabuf->resv, NULL);
	if (ret < 0) {
		dma_fence_put(fence);
		return (-ret);
	}

	if ((args->flags & DMA_BUF_SYNC_WRITE) != 0) {
		reservation_object_add_excl_fence(dmabuf->resv, fence);
		ret = 0;
	} else {
		ret = reservation_object_reserve_shared(dmabuf->resv);
		if (ret == 0)
			reservation_object_add_shared_fence(dmabuf->resv, fence);
	}

	reservation_object_unlock(dmabuf->resv);
	dma_fence_put(fence);

	return (ret < 0 ? -ret : ret);
}

static int
dmabuf_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{
STUB();
	KASSERT(fp->f_type == DTYPE_DMABUF, ("fp is not DMABUF"));
	struct dma_buf *dmabuf = fp->private_data;

	memset(sb, 0, sizeof(*sb));
	sb->st_size = dmabuf->size;
	sb->st_mode = S_IFIFO;	/* XXX */

	return (0);
}

static int
dmabuf_close(struct file *fp)
{
	struct dma_buf *dmabuf;
	if (fp->f_ops != &dmabuf_fileops) {
		kprintf("dmabuf_close(): file->f_ops != &dmabuf_fileops\n");
		return EINVAL;
	}
	dmabuf_close_count++;
	dmabuf = fp->private_data;
	dmabuf->ops->release(dmabuf);
	kfree(dmabuf);

	//kprintf("dmabuf_close(): success\n");
	return 0;
}

static int
dmabuf_ioctl(struct file *fp, u_long com, caddr_t data,
	    struct ucred *cred, struct sysmsg *msgv)
{
	struct dma_buf *dmabuf;

	if (fp->f_ops != &dmabuf_fileops)
		return (EBADF);
	dmabuf = fp->private_data;
	if (dmabuf == NULL)
		return (EBADF);

	switch (com) {
	case DMA_BUF_IOCTL_EXPORT_SYNC_FILE: {
		uint64_t start = dmabuf_now_us();
		int ret = dmabuf_export_sync_file(dmabuf,
		    (struct dma_buf_export_sync_file *)data);

		dmabuf_export_sync_file_count++;
		dmabuf_export_sync_file_us += dmabuf_now_us() - start;
		return (ret);
	}
	case DMA_BUF_IOCTL_IMPORT_SYNC_FILE: {
		uint64_t start = dmabuf_now_us();
		int ret = dmabuf_import_sync_file(dmabuf,
		    (const struct dma_buf_import_sync_file *)data);

		dmabuf_import_sync_file_count++;
		dmabuf_import_sync_file_us += dmabuf_now_us() - start;
		return (ret);
	}
	default:
		return (ENOTTY);
	}
}

static int
dmabuf_seek(struct file *fp, off_t offset, int whence, off_t *res)
{
	KASSERT(fp->f_type == DTYPE_DMABUF, ("fp is not DMABUF"));
	struct dma_buf *dmabuf = fp->private_data;
	off_t newoff;

	if (offset != 0) {
		return EINVAL;
	}

	switch (whence) {
	case SEEK_SET:
		newoff = 0;
		break;
	case SEEK_END:
		newoff = dmabuf->size;
		break;
	default:
		return EINVAL;
	}
	spin_lock(&fp->f_spin);
	fp->f_offset = newoff;
	spin_unlock(&fp->f_spin);
	*res = newoff;
	return 0;
	
}

struct fileops dmabuf_fileops = {
	.fo_read	= badfo_readwrite,
	.fo_write	= badfo_readwrite,
	.fo_ioctl	= dmabuf_ioctl,
	.fo_kqfilter	= badfo_kqfilter,
	.fo_stat	= dmabuf_stat,
	.fo_close	= dmabuf_close,
	.fo_seek	= dmabuf_seek,
};

struct dma_buf *
dma_buf_export(const struct dma_buf_export_info *exp_info)
{
	struct dma_buf *dmabuf;
	struct file *fp;

	falloc(curthread->td_lwp, &fp, NULL);
	if (fp == NULL)
		return ERR_PTR(-ENFILE);

	dmabuf_export_count++;
	dmabuf = kmalloc(sizeof(struct dma_buf), M_DRM, M_WAITOK);
	fp->f_type = DTYPE_DMABUF;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &dmabuf_fileops;
	fp->private_data = dmabuf;
	dmabuf->priv = exp_info->priv;
	dmabuf->ops = exp_info->ops;
	dmabuf->size = exp_info->size;
	dmabuf->file = fp;
	dmabuf->resv = exp_info->resv;

	return dmabuf;
}

int
dma_buf_fd(struct dma_buf *dmabuf, int flags)
{
	uint64_t start = dmabuf_now_us();
	int fd;
	int ret;

	dmabuf_fd_count++;

	if (dmabuf == NULL) {
		ret = -EINVAL;
		goto out;
	}

	if (dmabuf->file == NULL) {
		ret = -EINVAL;
		goto out;
	}

	fd = get_unused_fd_flags(flags);
	if (fd < 0) {
		ret = fd;
		goto out;
	}

	fd_install(fd, dmabuf->file);
	ret = fd;
out:
	if (ret < 0)
		dmabuf_fd_error_count++;
	dmabuf_fd_us += dmabuf_now_us() - start;
	return ret;
}

struct dma_buf *
dma_buf_get(int fd)
{
	struct file *fp;
	struct dma_buf *dmabuf;

	dmabuf_get_count++;
	if ((fp = holdfp(curthread, fd, -1)) == NULL) {
		dmabuf_get_error_count++;
		return ERR_PTR(-EBADF);
	}

	if (fp->f_ops != &dmabuf_fileops) {
		kprintf("dma_buf_get(): file->f_ops != &dmabuf_fileops\n");
		dropfp(curthread, fd, fp);
		dmabuf_get_error_count++;
		return ERR_PTR(-EBADF);
	}

	dmabuf = fp->private_data;
	/* Keep holdfp()'s reference; the caller releases it with dma_buf_put(). */

	return dmabuf;
}

struct sg_table *
dma_buf_map_attachment(struct dma_buf_attachment *attach,
				enum dma_data_direction direction)
{
STUB();
	struct sg_table *sg_table;

	if (attach == NULL)
		return ERR_PTR(-EINVAL);

	if (attach->dmabuf == NULL)
		return ERR_PTR(-EINVAL);

	sg_table = attach->dmabuf->ops->map_dma_buf(attach, direction);
	if (sg_table == NULL)
		return ERR_PTR(-ENOMEM);

	return sg_table;
}

void dma_buf_unmap_attachment(struct dma_buf_attachment *attach,
				struct sg_table *sg_table,
				enum dma_data_direction direction)
{
STUB();
}
