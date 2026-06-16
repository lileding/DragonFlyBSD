/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM driver registration. Phase 3 step 1: just get the driver
 * registered so that /dev/dri/cardN and /dev/dri/renderDN appear.
 * No nouveau-specific ioctls yet — those come in subsequent steps.
 *
 * Pattern follows dfly amdgpu (drm/amd/amdgpu/amdgpu_drv.c) but
 * stripped to render-only essentials (no modeset, no vblank, no
 * connector/crtc/encoder).
 */

#include "nvkm_priv.h"
#include "nvkm_bo.h"
#include "nvkm_gsp_vmm.h"
#include "nvkm_ttm.h"

#include <drm/drmP.h>
#include <drm/drm_drv.h>
#include <drm/drm_prime.h>
#include <drm/drm_syncobj.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/reservation.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <machine/pmap.h>

/* Identify ourselves as "nouveau" so unmodified Mesa NVK accepts us.
 * Version >= 1.0.3.1 is what NVK requires (winsys/nouveau_device.c). */
#define NVKM_DRM_NAME		"nouveau"
#define NVKM_DRM_DESC		"nVidia Riva/TNT/GeForce (dfly GSP-RM)"
#define NVKM_DRM_DATE		"20260522"
#define NVKM_DRM_MAJOR		1
#define NVKM_DRM_MINOR		3
#define NVKM_DRM_PATCH		1
#define NVKM_DRM_MAX_CHANNELS	64
#define NVKM_DRM_MAX_CHAN_OBJS	16

/*
 * Minimal file_operations. On DragonFly the cdevsw layer (drm_cdevsw in
 * drm_drv.c) is what actually services /dev/dri reads/ioctls; the Linux
 * file_operations struct here is just for compat shape. Keep empty.
 */
static const struct file_operations nvkm_drm_fops = {
	.owner = THIS_MODULE,
};

/* Forward decl: ioctl table defined at end of file. */
static const struct drm_ioctl_desc nvkm_drm_ioctls[];
struct nvkm_drm_file;
struct nvkm_drm_job;
struct nvkm_drm_vm_binding;
TAILQ_HEAD(nvkm_drm_job_list, nvkm_drm_job);
static struct nvkm_softc *nvkm_drm_sc(struct drm_device *ddev);
static int nvkm_drm_open(struct drm_device *ddev, struct drm_file *file_priv);
static void nvkm_drm_postclose(struct drm_device *ddev,
    struct drm_file *file_priv);
static void nvkm_drm_lastclose(struct drm_device *ddev);
static unsigned int nvkm_drm_primary_client_count(struct drm_device *ddev);
static void nvkm_drm_exec_pending_cancel_channel(struct nvkm_softc *sc,
    struct nvkm_gsp_chan *chan, int error);
static void nvkm_drm_job_work(struct work_struct *work);
static void nvkm_drm_jobs_flush(struct nvkm_drm_file *nfile);
static void nvkm_drm_jobs_close(struct nvkm_drm_file *nfile);

/* NVIF ioctl is variable-size; encode with size=0 since dispatch
 * matches by NR only and the actual copy size comes from userspace. */
#define DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_ALLOC, struct drm_nouveau_channel_alloc)
#define DRM_IOCTL_NOUVEAU_CHANNEL_FREE \
    DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_FREE, struct drm_nouveau_channel_free)
#define DRM_IOCTL_NOUVEAU_GEM_NEW \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_NEW, struct drm_nouveau_gem_new)
#define DRM_IOCTL_NOUVEAU_GEM_INFO \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_INFO, struct drm_nouveau_gem_info)
#define DRM_IOCTL_NOUVEAU_GEM_CPU_PREP \
    DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_CPU_PREP, struct drm_nouveau_gem_cpu_prep)
#define DRM_IOCTL_NOUVEAU_GEM_CPU_FINI \
    DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_CPU_FINI, struct drm_nouveau_gem_cpu_fini)
#define DRM_IOCTL_NOUVEAU_VM_BIND \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_BIND, struct drm_nouveau_vm_bind)
#define DRM_IOCTL_NOUVEAU_EXEC \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_EXEC, struct drm_nouveau_exec)
#define DRM_IOCTL_NOUVEAU_NVIF \
    _IOC(IOC_INOUT, DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_NOUVEAU_NVIF, 0)

/* Raster scanout position for precise vblank timestamps (TU102 = gv100):
 * armed head timing at 0x6820xx, raster generator line at 0x616330/4. Lets
 * the drm vblank core place page-flip events on the correct vblank. */
static bool
nvkm_drm_get_scanout_position(struct drm_device *dev, unsigned int pipe,
    bool in_vblank_irq, int *vpos, int *hpos, ktime_t *stime, ktime_t *etime,
    const struct drm_display_mode *mode)
{
	struct nvkm_softc *sc = nvkm_drm_sc(dev);
	uint32_t rg = pipe * 0x800u;
	uint32_t st = 0x8000u + pipe * 0x400u;	/* armed head state */
	int vtotal, vblanks, vblanke, line;

	(void)in_vblank_irq;
	(void)mode;
	if (sc == NULL)
		return false;

	vtotal  = (nvkm_rd32(sc, 0x682064 + st) >> 16) & 0xffff;
	vblanke = (nvkm_rd32(sc, 0x68206c + st) >> 16) & 0xffff;
	vblanks = (nvkm_rd32(sc, 0x682070 + st) >> 16) & 0xffff;
	if (vtotal == 0)
		return false;

	if (stime != NULL)
		*stime = ktime_get();
	/* Reading vline (0x616330) latches hline (0x616334). */
	line  = nvkm_rd32(sc, 0x616330 + rg) & 0xffff;
	*hpos = nvkm_rd32(sc, 0x616334 + rg) & 0xffff;
	if (etime != NULL)
		*etime = ktime_get();

	/* nouveau calc(): raster line -> vpos relative to active scanout. */
	if (vblanke >= vblanks) {
		if (line >= vblanks)
			line -= vtotal;
	} else {
		if (line >= vblanks)
			line -= vtotal;
		line -= vblanke + 1;
	}
	*vpos = line;
	return true;
}

static int
nvkm_drm_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file_priv,
    uint32_t handle, uint32_t flags, int *prime_fd)
{
	struct nvkm_softc *sc = nvkm_drm_sc(dev);
	int ret;

	if (sc != NULL)
		sc->prime_handle_to_fd_count++;
	ret = drm_gem_prime_handle_to_fd(dev, file_priv, handle, flags,
	    prime_fd);
	if (ret != 0 && sc != NULL)
		sc->prime_handle_to_fd_error_count++;
	return (ret);
}

static int
nvkm_drm_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
    int prime_fd, uint32_t *handle)
{
	struct nvkm_softc *sc = nvkm_drm_sc(dev);
	int ret;

	if (sc != NULL)
		sc->prime_fd_to_handle_count++;
	ret = drm_gem_prime_fd_to_handle(dev, file_priv, prime_fd, handle);
	if (ret != 0 && sc != NULL)
		sc->prime_fd_to_handle_error_count++;
	return (ret);
}

static struct dma_buf *
nvkm_drm_gem_prime_export(struct drm_device *dev, struct drm_gem_object *obj,
    int flags)
{
	struct nvkm_softc *sc = nvkm_drm_sc(dev);
	struct nvkm_bo *bo = to_nvkm_bo(obj);
	struct dma_buf *dmabuf;

	if (bo->no_share)
		return (ERR_PTR(-EPERM));

	dmabuf = drm_gem_prime_export(dev, obj, flags);
	if (!IS_ERR(dmabuf) && sc != NULL)
		sc->prime_dma_buf_export_count++;

	return (dmabuf);
}

static struct reservation_object *
nvkm_drm_gem_prime_res_obj(struct drm_gem_object *obj)
{
	return (nvkm_bo_resv(to_nvkm_bo(obj)));
}

static struct drm_driver nvkm_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ |
	    DRIVER_PRIME | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops    = &nvkm_drm_fops,
	.get_scanout_position = nvkm_drm_get_scanout_position,
	.get_vblank_timestamp = drm_calc_vbltimestamp_from_scanoutpos,
	.ioctls  = nvkm_drm_ioctls,
	.num_ioctls = 0x45 /* sparse: max index DRM_NOUVEAU_GEM_INFO(0x44)+1 */,
	.name    = NVKM_DRM_NAME,
	.desc    = NVKM_DRM_DESC,
	.date    = NVKM_DRM_DATE,
	.major   = NVKM_DRM_MAJOR,
	.minor   = NVKM_DRM_MINOR,
	.patchlevel = NVKM_DRM_PATCH,
	.open = nvkm_drm_open,
	.postclose = nvkm_drm_postclose,
	.lastclose = nvkm_drm_lastclose,
	.gem_vm_ops = &nvkm_gem_pager_ops,
	.gem_free_object_unlocked = nvkm_bo_gem_free,
	/*
	 * PRIME self-import only (wlroots requires DRM_PRIME_CAP_IMPORT even
	 * for software rendering): same-device dmabuf round-trips resolve to
	 * the original GEM object in drm_gem_prime_import_dev().  Cross-device
	 * sharing needs gem_prime_get_sg_table/import_sg_table, which are
	 * deliberately absent -- those paths fail with an errno, not a crash.
	 */
	.prime_handle_to_fd = nvkm_drm_prime_handle_to_fd,
	.prime_fd_to_handle = nvkm_drm_prime_fd_to_handle,
	.gem_prime_export = nvkm_drm_gem_prime_export,
	.gem_prime_res_obj = nvkm_drm_gem_prime_res_obj,
	.gem_prime_import = drm_gem_prime_import,
	.dumb_create = nvkm_bo_dumb_create,
	.dumb_map_offset = nvkm_bo_dumb_map_offset,
	.dumb_destroy = nvkm_bo_dumb_destroy,
};

static uint64_t
nvkm_drm_profile_now_us(void)
{
	return ((uint64_t)ktime_to_us(ktime_get()));
}

static void
nvkm_drm_profile_add_us(uint64_t *total, uint64_t start_us)
{
	uint64_t end_us = nvkm_drm_profile_now_us();

	if (end_us >= start_us)
		*total += end_us - start_us;
}

struct nvkm_drm_vm_binding {
	LIST_ENTRY(nvkm_drm_vm_binding) link;
	uint64_t addr;
	uint64_t size;
	uint64_t bo_offset;
	struct nvkm_drm_file *owner;
	struct drm_gem_object *obj;
	bool pte_installed;
	bool bo_pinned;
};
LIST_HEAD(nvkm_drm_vm_binding_list, nvkm_drm_vm_binding);

struct nvkm_drm_chan_obj {
	uint32_t handle;
	uint32_t oclass;
	uint64_t nvif_object;	/* NVIF object id (nvif_ioctl_v0.object) for DEL */
	struct nvkm_gsp_object object;
};

struct nvkm_drm_chan {
	LIST_ENTRY(nvkm_drm_chan) link;
	uint32_t id;
	uint32_t engine_type;
	struct nvkm_gsp_chan *chan;
	struct nvkm_drm_chan_obj obj[NVKM_DRM_MAX_CHAN_OBJS];
};
LIST_HEAD(nvkm_drm_chan_list, nvkm_drm_chan);

struct nvkm_drm_file {
	/* Per-file GPU address space. Each drm_file owns its own VMM (RM
	 * vaspace + page-table tree) so concurrent NVK processes cannot
	 * collide on GPU VAs. NULL only if vmm_ctor failed during open. */
	struct nvkm_gsp_vmm *vmm;
	struct nvkm_drm_vm_binding_list vm_bindings;
	uint64_t vm_bindings_max_end;
	struct nvkm_drm_chan_list channels;
	/* Serializes VM_BIND page-table mutation against other remaps. EXEC
	 * jobs take the same token before ringing the doorbell so they never
	 * observe a half-written page-table update, but VM_BIND does not drain
	 * already submitted GPU work. Nouveau relies on userspace fences for
	 * that ordering, and synchronous VM_BIND only waits for its own bind job.
	 * Order is always vm_token -> gsp_tok. */
	struct lwkt_token vm_token;

	/* Private GPUVM reservation object.
	 *
	 * Linux nouveau publishes EXEC fences through drm_gpuvm_exec using
	 * dma_resv usage metadata. DragonFly's current reservation_object has no
	 * usage classes, so VM-wide EXEC fences live here instead of being
	 * exported as per-BO dma-buf content fences. */
	struct reservation_object vm_resv;

	/*
	 * Serializes the userspace-visible submit point for EXEC and VM_BIND.
	 *
	 * Ownership:
	 *   Owned by the drm_file; callers borrow it only for the duration of a
	 *   single ioctl submit sequence.
	 *
	 * Lifetime:
	 *   Initialized at open and valid until postclose has drained and
	 *   cancelled this file's job queue.
	 *
	 * Threading:
	 *   Guards the order in which a job publishes reservation fences, publishes
	 *   out-sync fences, and becomes visible to the per-file job queue.  Worker
	 *   execution does not take this lock, matching Linux nouveau's scheduler
	 *   submit mutex rather than turning execution itself into a global lock.
	 */
	struct lock job_submit_lock;

	/* Ordered per-file GPU job queue.  IOCTL handlers copy user data and
	 * publish output fences before enqueueing; the worker waits dependency
	 * fences and performs the actual EXEC/async VM_BIND work. */
	struct work_struct job_work;
	struct lwkt_token job_token;
	struct nvkm_drm_job_list job_queue;
	bool job_work_queued;
	bool job_closing;
	uint64_t job_epoch;
};

struct drm_nouveau_exec_push {
	uint64_t va;
	uint32_t va_len;
	uint32_t flags;
};

static volatile u_int nvkm_drm_next_channel = 1;

/* Per-file RM client handle allocator. Each drm_file's VMM needs a unique
 * client handle; child object handles are derived from it. Kept clear of the
 * kernel VMM (0xc1d00001) and golden VMM (0xc1d00002). Monotonic; reuse only
 * after 2^16 opens, by which point earlier files are long gone. */
static volatile uint32_t nvkm_drm_next_client_handle = 0xc1d10000u;

static struct nvkm_drm_file *
nvkm_drm_file_priv(struct drm_file *file_priv)
{
	return (file_priv->driver_priv);
}

/* Exposed to nvkm_bo so a no_share BO can alias its fence-wait resv to this
 * file's VM-wide EXEC completion set (vm_resv). */
struct reservation_object *
nvkm_drm_file_vm_resv(struct drm_file *file_priv)
{
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);

	if (nfile == NULL)
		return (NULL);
	return (&nfile->vm_resv);
}

static bool
nvkm_drm_vm_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs)
{
	return (a < b + bs && b < a + as);
}

static bool
nvkm_drm_gpu_va_fits_binding(const struct nvkm_drm_vm_binding *binding,
    uint64_t va, uint64_t size, uint64_t *binding_offset)
{
	uint64_t offset;

	if (va < binding->addr)
		return (false);
	offset = va - binding->addr;
	if (offset > binding->size || size > binding->size - offset)
		return (false);
	*binding_offset = offset;
	return (true);
}

static void
nvkm_drm_vm_binding_assert(const struct nvkm_drm_vm_binding *binding)
{
	KASSERT(binding->owner != NULL,
	    ("nvkm_drm: VM binding without owner"));
	KASSERT(binding->obj != NULL,
	    ("nvkm_drm: VM binding without GEM object"));
}

static struct nvkm_drm_vm_binding *
nvkm_drm_vm_binding_alloc(struct nvkm_drm_file *nfile, uint64_t addr,
    uint64_t size, struct drm_gem_object *obj, uint64_t bo_offset)
{
	struct nvkm_drm_vm_binding *binding;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (binding == NULL)
		return (NULL);

	binding->addr = addr;
	binding->size = size;
	binding->bo_offset = bo_offset;
	binding->owner = nfile;
	binding->obj = obj;
	binding->pte_installed = true;
	return (binding);
}

static void
nvkm_drm_vm_binding_insert_sorted(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_drm_vm_binding *pos, *prev = NULL;
	uint64_t end = binding->addr + binding->size;

	if (nfile->vm_bindings_max_end < end)
		nfile->vm_bindings_max_end = end;

	LIST_FOREACH(pos, &nfile->vm_bindings, link) {
		if (binding->addr < pos->addr) {
			if (prev != NULL)
				LIST_INSERT_AFTER(prev, binding, link);
			else
				LIST_INSERT_HEAD(&nfile->vm_bindings,
				    binding, link);
			return;
		}
		prev = pos;
	}

	if (prev != NULL)
		LIST_INSERT_AFTER(prev, binding, link);
	else
		LIST_INSERT_HEAD(&nfile->vm_bindings, binding, link);
}

static void
nvkm_drm_vm_bindings_recalc_max_end(struct nvkm_drm_file *nfile)
{
	struct nvkm_drm_vm_binding *binding;
	uint64_t max_end = 0;

	LIST_FOREACH(binding, &nfile->vm_bindings, link) {
		uint64_t end = binding->addr + binding->size;

		if (max_end < end)
			max_end = end;
	}
	nfile->vm_bindings_max_end = max_end;
}

static int
nvkm_drm_vm_binding_pin(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_bo *bo;
	int err;

	nvkm_drm_vm_binding_assert(binding);
	if (binding->bo_pinned)
		return (0);

	bo = to_nvkm_bo(binding->obj);
	err = nvkm_bo_vm_bind_pin(bo);
	if (err != 0)
		return (err);
	binding->bo_pinned = true;
	return (0);
}

static int
nvkm_drm_vm_binding_unpin(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_bo *bo;
	int err;

	nvkm_drm_vm_binding_assert(binding);
	if (!binding->bo_pinned)
		return (0);

	bo = to_nvkm_bo(binding->obj);
	err = nvkm_bo_vm_bind_unpin(bo);
	if (err == 0)
		binding->bo_pinned = false;
	return (err);
}

static void
nvkm_drm_vm_binding_free(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_softc *sc = binding->obj->dev->dev_private;
	int err;

	nvkm_drm_vm_binding_assert(binding);
	err = nvkm_drm_vm_binding_unpin(binding);
	if (err != 0)
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND unpin failed obj=%p err=%d\n",
		    binding->obj, err);
	drm_gem_object_put_unlocked(binding->obj);
	kfree(binding);
}

static void
nvkm_drm_vm_binding_unlink_free(struct nvkm_drm_vm_binding *binding)
{
	LIST_REMOVE(binding, link);
	nvkm_drm_vm_binding_free(binding);
}

static int
nvkm_drm_vm_binding_reclaim_noflush(struct nvkm_softc *sc,
    struct nvkm_drm_vm_binding *binding)
{
	int err;

	nvkm_drm_vm_binding_assert(binding);

	if (binding->pte_installed) {
		err = nvkm_gsp_vmm_unmap_valid_noflush(binding->owner->vmm,
		    binding->addr, binding->size);
		if (err != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND unmap failed addr=0x%016jx size=0x%016jx obj=%p err=%d\n",
			    (uintmax_t)binding->addr,
			    (uintmax_t)binding->size, binding->obj, err);
			return (-err);
		}
		binding->pte_installed = false;
	}

	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND reclaim addr=0x%016jx size=0x%016jx obj=%p\n",
	    (uintmax_t)binding->addr, (uintmax_t)binding->size,
	    binding->obj);
	nvkm_drm_vm_binding_unlink_free(binding);
	return (0);
}

/*
 * nvkm_drm_vm_bind_note_empty_clear_skip()
 *
 * Ownership:
 *   Borrows sc for counter updates only; it does not acquire references to the
 *   VMM, GEM objects, or VM binding records.
 *
 * Lifetime:
 *   The range is an ioctl-local value that remains owned by the caller.  The
 *   helper stores only aggregate diagnostic counters.
 *
 * Threading:
 *   Called from VM_BIND mutation paths while the caller holds the drm_file's
 *   vm_token.  Counters follow the driver's existing best-effort debug counter
 *   model and are not part of the ABI.
 */
static void
nvkm_drm_vm_bind_note_empty_clear_skip(struct nvkm_softc *sc, uint64_t size)
{
	sc->vm_bind_empty_clear_skip_count++;
	sc->vm_bind_empty_clear_skip_pages += size / NVKM_GMMU_PT_PAGE_SIZE;
}

/*
 * nvkm_drm_vm_bind_note_replace_clear_skip()
 *
 * Ownership:
 *   Borrows sc for diagnostic counter updates only.  It does not acquire or
 *   release VM binding, VMM, or GEM ownership.
 *
 * Lifetime:
 *   The byte range is caller-owned and used only to update aggregate counters.
 *
 * Threading:
 *   Called while the drm_file VM token serializes VM_BIND mutations.  Counters
 *   follow the driver's best-effort debug accounting and are not UAPI.
 */
static void
nvkm_drm_vm_bind_note_replace_clear_skip(struct nvkm_softc *sc, uint64_t size)
{
	sc->vm_bind_replace_clear_skip_count++;
	sc->vm_bind_replace_clear_skip_pages += size / NVKM_GMMU_PT_PAGE_SIZE;
}

/*
 * nvkm_drm_vm_bind_note_op()
 *
 * Ownership:
 *   Borrows sc for aggregate diagnostics.  No VM_BIND, BO, or VMM ownership is
 *   changed.
 *
 * Lifetime:
 *   The range is caller-owned and not retained.
 *
 * Threading:
 *   Called while a VM_BIND ioctl/job is applying under the per-file VM token.
 */
static void
nvkm_drm_vm_bind_note_op(struct nvkm_softc *sc, uint32_t action,
    uint64_t size)
{
	uint64_t pages = size / NVKM_GMMU_PT_PAGE_SIZE;

	switch (action) {
	case NVKM_DRM_VM_TRACE_MAP:
		sc->vm_bind_map_count++;
		sc->vm_bind_map_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_UNMAP:
		sc->vm_bind_unmap_count++;
		sc->vm_bind_unmap_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_MAP_NULL:
		sc->vm_bind_map_null_count++;
		sc->vm_bind_map_null_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_MAP_SPARSE:
		sc->vm_bind_map_sparse_count++;
		sc->vm_bind_map_sparse_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_UNMAP_SPARSE:
		sc->vm_bind_unmap_sparse_count++;
		sc->vm_bind_unmap_sparse_pages += pages;
		break;
	}
}

/*
 * nvkm_drm_vm_bind_note_clear()
 *
 * Ownership:
 *   Borrows sc for aggregate diagnostics only.
 *
 * Lifetime:
 *   The range is caller-owned and not retained.
 *
 * Threading:
 *   Called immediately before remove_range writes invalid/sparse PTEs for a
 *   tracked overlap, while VM_BIND serialization is held.
 */
static void
nvkm_drm_vm_bind_note_clear(struct nvkm_softc *sc, uint32_t action,
    uint64_t size)
{
	uint64_t pages = size / NVKM_GMMU_PT_PAGE_SIZE;

	switch (action) {
	case NVKM_DRM_VM_TRACE_UNMAP:
		sc->vm_bind_clear_unmap_count++;
		sc->vm_bind_clear_unmap_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_MAP_NULL:
		sc->vm_bind_clear_map_null_count++;
		sc->vm_bind_clear_map_null_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_MAP_SPARSE:
		sc->vm_bind_clear_map_sparse_count++;
		sc->vm_bind_clear_map_sparse_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_UNMAP_SPARSE:
		sc->vm_bind_clear_unmap_sparse_count++;
		sc->vm_bind_clear_unmap_sparse_pages += pages;
		break;
	}
}

/*
 * nvkm_drm_vm_bindings_free_prepared()
 *
 * Ownership:
 *   Consumes every binding currently linked on bindings.  Each binding owns a
 *   GEM reference and may own a VM_BIND BO pin; both are released here.
 *
 * Lifetime:
 *   The list head remains owned by the caller and is empty on return.
 *
 * Threading:
 *   Called from VM_BIND prepare/abort paths while the caller owns the per-file
 *   VM token.  It does not touch hardware page tables.
 */
static void
nvkm_drm_vm_bindings_free_prepared(
    struct nvkm_drm_vm_binding_list *bindings)
{
	struct nvkm_drm_vm_binding *binding;

	while ((binding = LIST_FIRST(bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_free(binding);
	}
}

/*
 * nvkm_drm_vm_bindings_prepare_replace_range()
 *
 * Ownership:
 *   Borrows nfile and the existing binding list.  On success, tail_bindings
 *   owns any preallocated tail bindings needed to split old mappings.  The
 *   caller must later pass the list to commit or abort.
 *
 * Lifetime:
 *   No existing binding is mutated here.  The prepared tail bindings remain
 *   valid until commit/abort while the caller keeps the VM token held.
 *
 * Threading:
 *   Requires nfile->vm_token.  It may acquire BO pin references for prepared
 *   tails, but it does not write PTEs and cannot expose a partial VA update.
 */
static int
nvkm_drm_vm_bindings_prepare_replace_range(struct nvkm_drm_file *nfile,
    uint64_t addr, uint64_t size,
    struct nvkm_drm_vm_binding_list *tail_bindings)
{
	struct nvkm_drm_vm_binding *binding;
	uint64_t end = addr + size;
	int err;

	LIST_INIT(tail_bindings);
	if (addr >= nfile->vm_bindings_max_end)
		return (0);

	LIST_FOREACH(binding, &nfile->vm_bindings, link) {
		struct nvkm_drm_vm_binding *tail;
		uint64_t old_start, old_end, cut_start, cut_end;
		uint64_t head_size, tail_size, tail_bo_offset;

		if (binding->addr >= end)
			break;
		if (binding->addr + binding->size <= addr)
			continue;
		if (!nvkm_drm_vm_ranges_overlap(addr, size,
		    binding->addr, binding->size))
			continue;

		old_start = binding->addr;
		old_end = binding->addr + binding->size;
		cut_start = old_start > addr ? old_start : addr;
		cut_end = old_end < end ? old_end : end;
		head_size = cut_start - old_start;
		tail_size = old_end - cut_end;
		if (head_size == 0 || tail_size == 0)
			continue;

		tail_bo_offset = binding->bo_offset + (cut_end - old_start);
		drm_gem_object_get(binding->obj);
		tail = nvkm_drm_vm_binding_alloc(nfile, cut_end, tail_size,
		    binding->obj, tail_bo_offset);
		if (tail == NULL) {
			drm_gem_object_put_unlocked(binding->obj);
			err = -ENOMEM;
			goto fail;
		}
		err = nvkm_drm_vm_binding_pin(tail);
		if (err != 0) {
			nvkm_drm_vm_binding_free(tail);
			err = -err;
			goto fail;
		}
		LIST_INSERT_HEAD(tail_bindings, tail, link);
	}
	return (0);

fail:
	nvkm_drm_vm_bindings_free_prepared(tail_bindings);
	return (err);
}

/*
 * nvkm_drm_vm_bindings_abort_replace_range()
 *
 * Ownership:
 *   Consumes the prepared tail bindings produced by prepare_replace_range().
 *   Existing live bindings and hardware PTEs are untouched.
 *
 * Lifetime:
 *   The prepared list must not be used after this call except as an empty list.
 *
 * Threading:
 *   Requires the same VM_BIND serialization as prepare/commit.
 */
static void
nvkm_drm_vm_bindings_abort_replace_range(
    struct nvkm_drm_vm_binding_list *tail_bindings)
{
	nvkm_drm_vm_bindings_free_prepared(tail_bindings);
}

/*
 * nvkm_drm_vm_bindings_commit_replace_range()
 *
 * Ownership:
 *   Consumes prepared tail bindings and mutates nfile's binding tracker to
 *   describe the final VA state after a successful MAP replacement.
 *
 * Lifetime:
 *   The caller must have already installed the new PTEs for [addr, addr+size).
 *   Old bindings covering that exact range are removed from the software
 *   tracker without first writing invalid PTEs, because the new valid PTEs have
 *   already overwritten them and the caller will flush once before ioctl return.
 *
 * Threading:
 *   Requires nfile->vm_token.  This function does not acquire the VMM token and
 *   does not touch BAR1; it is a no-fail software state transition.
 */
static void
nvkm_drm_vm_bindings_commit_replace_range(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size,
    struct nvkm_drm_vm_binding_list *tail_bindings)
{
	struct nvkm_drm_vm_binding *binding, *next;
	uint64_t end = addr + size;
	bool changed = false;

	LIST_FOREACH_MUTABLE(binding, &nfile->vm_bindings, link, next) {
		uint64_t old_start, old_end, cut_start, cut_end;
		uint64_t head_size, tail_size;

		if (binding->addr >= end)
			break;
		if (binding->addr + binding->size <= addr)
			continue;
		if (!nvkm_drm_vm_ranges_overlap(addr, size,
		    binding->addr, binding->size))
			continue;

		changed = true;
		old_start = binding->addr;
		old_end = binding->addr + binding->size;
		cut_start = old_start > addr ? old_start : addr;
		cut_end = old_end < end ? old_end : end;
		head_size = cut_start - old_start;
		tail_size = old_end - cut_end;
		nvkm_drm_vm_bind_note_replace_clear_skip(sc, cut_end - cut_start);

		if (head_size != 0 && tail_size != 0) {
			binding->size = head_size;
		} else if (head_size != 0) {
			binding->size = head_size;
		} else if (tail_size != 0) {
			binding->addr = cut_end;
			binding->size = tail_size;
			binding->bo_offset += cut_end - old_start;
		} else {
			binding->pte_installed = false;
			nvkm_drm_vm_binding_unlink_free(binding);
		}
	}

	while ((binding = LIST_FIRST(tail_bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_insert_sorted(nfile, binding);
	}
	if (changed)
		nvkm_drm_vm_bindings_recalc_max_end(nfile);
}

static int
nvkm_drm_vm_bindings_remove_range(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size,
    bool clear_empty_range, uint32_t clear_action, uint32_t *punmapped)
{
	struct nvkm_drm_vm_binding *binding, *next;
	struct nvkm_drm_vm_binding_list tail_bindings;
	uint64_t end = addr + size;
	int err;

	LIST_INIT(&tail_bindings);
	*punmapped = 0;
	if (addr >= nfile->vm_bindings_max_end) {
		if (clear_empty_range)
			nvkm_drm_vm_bind_note_empty_clear_skip(sc, size);
		return (0);
	}

	LIST_FOREACH(binding, &nfile->vm_bindings, link) {
		struct nvkm_drm_vm_binding *tail;
		uint64_t old_start, old_end, cut_start, cut_end;
		uint64_t head_size, tail_size, tail_bo_offset;

		if (binding->addr >= end)
			break;
		if (binding->addr + binding->size <= addr)
			continue;
		if (!nvkm_drm_vm_ranges_overlap(addr, size,
		    binding->addr, binding->size))
			continue;

		(*punmapped)++;
		old_start = binding->addr;
		old_end = binding->addr + binding->size;
		cut_start = old_start > addr ? old_start : addr;
		cut_end = old_end < end ? old_end : end;
		head_size = cut_start - old_start;
		tail_size = old_end - cut_end;
		if (head_size == 0 || tail_size == 0)
			continue;

		tail_bo_offset = binding->bo_offset + (cut_end - old_start);
		drm_gem_object_get(binding->obj);
		tail = nvkm_drm_vm_binding_alloc(nfile, cut_end, tail_size,
		    binding->obj, tail_bo_offset);
		if (tail == NULL) {
			drm_gem_object_put_unlocked(binding->obj);
			err = -ENOMEM;
			goto fail_tails;
		}
		err = nvkm_drm_vm_binding_pin(tail);
		if (err != 0) {
			nvkm_drm_vm_binding_free(tail);
			err = -err;
			goto fail_tails;
		}
		LIST_INSERT_HEAD(&tail_bindings, tail, link);
	}

	if (*punmapped != 0) {
		LIST_FOREACH(binding, &nfile->vm_bindings, link) {
			uint64_t old_start, old_end, cut_start, cut_end;

			if (binding->addr >= end)
				break;
			if (binding->addr + binding->size <= addr)
				continue;
			if (!nvkm_drm_vm_ranges_overlap(addr, size,
			    binding->addr, binding->size))
				continue;

			old_start = binding->addr;
			old_end = binding->addr + binding->size;
			cut_start = old_start > addr ? old_start : addr;
			cut_end = old_end < end ? old_end : end;
			nvkm_drm_vm_bind_note_clear(sc, clear_action,
			    cut_end - cut_start);
			err = nvkm_gsp_vmm_unmap_valid_noflush(nfile->vmm,
			    cut_start, cut_end - cut_start);
			if (err != 0) {
				err = -err;
				goto fail_tails;
			}
		}
	} else if (clear_empty_range) {
		nvkm_drm_vm_bind_note_empty_clear_skip(sc, size);
	}

	LIST_FOREACH_MUTABLE(binding, &nfile->vm_bindings, link, next) {
		uint64_t old_start, old_end, cut_start, cut_end;
		uint64_t head_size, tail_size;

		if (binding->addr >= end)
			break;
		if (binding->addr + binding->size <= addr)
			continue;
		if (!nvkm_drm_vm_ranges_overlap(addr, size,
		    binding->addr, binding->size))
			continue;

		old_start = binding->addr;
		old_end = binding->addr + binding->size;
		cut_start = old_start > addr ? old_start : addr;
		cut_end = old_end < end ? old_end : end;
		head_size = cut_start - old_start;
		tail_size = old_end - cut_end;

		if (head_size != 0 && tail_size != 0) {
			binding->size = head_size;
		} else if (head_size != 0) {
			binding->size = head_size;
		} else if (tail_size != 0) {
			binding->addr = cut_end;
			binding->size = tail_size;
			binding->bo_offset += cut_end - old_start;
		} else {
			binding->pte_installed = false;
			nvkm_drm_vm_binding_unlink_free(binding);
		}
	}

	while ((binding = LIST_FIRST(&tail_bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_insert_sorted(nfile, binding);
	}
	if (*punmapped != 0)
		nvkm_drm_vm_bindings_recalc_max_end(nfile);
	return (0);

fail_tails:
	while ((binding = LIST_FIRST(&tail_bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_free(binding);
	}
	return (err);
}

static int
nvkm_drm_vm_binding_add(struct nvkm_drm_file *nfile, uint64_t addr,
    uint64_t size, struct drm_gem_object *obj, uint64_t bo_offset,
    bool bo_pinned)
{
	struct nvkm_drm_vm_binding *binding;
	int err;

	binding = nvkm_drm_vm_binding_alloc(nfile, addr, size, obj, bo_offset);
	if (binding == NULL)
		return (ENOMEM);

	binding->bo_pinned = bo_pinned;
	err = nvkm_drm_vm_binding_pin(binding);
	if (err != 0) {
		kfree(binding);
		return (err);
	}
	nvkm_drm_vm_binding_insert_sorted(nfile, binding);
	return (0);
}


static void
nvkm_drm_vm_bind_record_error(struct nvkm_softc *sc, uint32_t op,
    uint32_t flags, uint32_t handle, uint64_t addr, uint64_t range,
    uint64_t bo_offset, int err)
{
	sc->vm_bind_error_count++;
	sc->vm_bind_last_error = err;
	sc->vm_bind_last_op = op;
	sc->vm_bind_last_flags = flags;
	sc->vm_bind_last_handle = handle;
	sc->vm_bind_last_addr = addr;
	sc->vm_bind_last_range = range;
	sc->vm_bind_last_bo_offset = bo_offset;
}

static void
nvkm_drm_vm_trace_record(struct nvkm_softc *sc, uint32_t action,
    uint32_t flags, uint32_t handle, uint64_t addr, uint64_t range,
    uint64_t bo_offset, struct drm_gem_object *obj, int error)
{
	struct nvkm_drm_vm_trace *trace;

	if (nvkm_debug == 0)
		return;

	trace = &sc->vm_trace[sc->vm_trace_next % NVKM_DRM_VM_TRACE_COUNT];
	memset(trace, 0, sizeof(*trace));
	trace->seq = ++sc->vm_trace_seq;
	trace->action = action;
	trace->flags = flags;
	trace->handle = handle;
	trace->addr = addr;
	trace->range = range;
	trace->bo_offset = bo_offset;
	trace->error = error;
	if (obj != NULL) {
		struct nvkm_bo *bo = to_nvkm_bo(obj);

		trace->obj = (uintptr_t)obj;
		trace->bo_size = obj->size;
		trace->bo_paddr = bo->paddr;
		trace->bo_domain = bo->domain;
		trace->cpu_mapped = nvkm_bo_cpu_mappable(bo);
	}
	sc->vm_trace_next++;
}

static void
nvkm_drm_dump_push_buffer(struct nvkm_softc *sc, struct nvkm_drm_file *nfile,
    uint64_t va, uint32_t va_len)
{
	struct nvkm_drm_vm_binding *binding;

	LIST_FOREACH(binding, &nfile->vm_bindings, link) {
		struct nvkm_bo *bo;
		uint64_t offset;
		uint32_t count;

		if (va < binding->addr ||
		    va + va_len > binding->addr + binding->size)
			continue;

		bo = to_nvkm_bo(binding->obj);
		offset = binding->bo_offset + (va - binding->addr);
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC dump push va=0x%016jx len=0x%08x binding=0x%016jx+0x%016jx obj=%p domain=0x%x paddr=0x%016jx offset=0x%jx cpu_map=%u\n",
		    (uintmax_t)va, va_len, (uintmax_t)binding->addr,
		    (uintmax_t)binding->size, binding->obj, bo->domain,
		    (uintmax_t)bo->paddr, (uintmax_t)offset,
		    nvkm_bo_has_sysmem(bo));
		if (!nvkm_bo_has_sysmem(bo))
			return;
		if (offset > bo->base.size ||
		    va_len > bo->base.size - offset)
			return;

		count = va_len / sizeof(uint32_t);
		if (count > 48)
			count = 48;
		for (uint32_t i = 0; i < count; i += 4) {
			uint32_t word[4] = {};

			for (uint32_t j = 0; j < 4 && i + j < count; j++)
				(void)nvkm_bo_read32(bo, offset +
				    (uint64_t)(i + j) * sizeof(uint32_t),
				    &word[j]);
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC push[%02u]=%08x %08x %08x %08x\n",
			    i, word[0], word[1], word[2], word[3]);
		}
		return;
	}

	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC dump push va=0x%016jx len=0x%08x no binding\n",
	    (uintmax_t)va, va_len);
}

static void
nvkm_drm_dump_large_push(struct nvkm_softc *sc, struct nvkm_drm_file *nfile,
    uint64_t va, uint32_t va_len)
{
	if (nvkm_debug != 0 && va_len >= 0xa0)
		nvkm_drm_dump_push_buffer(sc, nfile, va, va_len);
}

static void
nvkm_drm_exec_trace_record(struct nvkm_softc *sc, uint64_t seq,
    uint32_t channel, uint32_t chid, uint32_t post_slot,
    uint32_t gpf_index, uint32_t push_index, uint32_t push_count,
    const struct drm_nouveau_exec_push *push)
{
	struct nvkm_drm_exec_trace *trace;

	if (nvkm_debug == 0)
		return;

	trace = &sc->exec_trace[sc->exec_trace_next %
	    NVKM_DRM_EXEC_TRACE_COUNT];
	memset(trace, 0, sizeof(*trace));
	trace->seq = seq;
	trace->channel = channel;
	trace->chid = chid;
	trace->post_slot = post_slot;
	trace->gpf_index = gpf_index;
	trace->push_index = push_index;
	trace->push_count = push_count;
	trace->flags = push->flags;
	trace->va = push->va;
	trace->va_len = push->va_len;
	sc->exec_trace_next++;
}

static void
nvkm_drm_exec_trace_complete(struct nvkm_softc *sc,
    const struct nvkm_drm_exec_pending *pending, int error)
{
	for (uint32_t i = 0; i < pending->trace_count; i++) {
		struct nvkm_drm_exec_trace *trace;
		uint32_t idx = (pending->trace_first + i) %
		    NVKM_DRM_EXEC_TRACE_COUNT;

		trace = &sc->exec_trace[idx];
		if (trace->seq != pending->trace_seq ||
		    trace->post_slot != pending->post_slot)
			continue;
		trace->completed = 1;
		trace->error = error;
	}
}

int
nvkm_drm_register(struct nvkm_softc *sc)
{
	struct pci_dev *pdev = NULL;
	struct drm_device *ddev;
	int err;

	/* Wrap the BSD device_t into a Linux pci_dev shim that dfly drm
	 * core understands. drm_init_pdev mallocs *pdev for us. */
	drm_init_pdev(sc->dev, &pdev);
	if (pdev == NULL) {
		nvkm_debugf(sc->dev,
		    "drm: drm_init_pdev failed\n");
		return (ENOMEM);
	}
	sc->drm_pdev = pdev;

	ddev = drm_dev_alloc(&nvkm_drm_driver, &pdev->dev);
	if (IS_ERR(ddev)) {
		nvkm_debugf(sc->dev,
		    "drm: drm_dev_alloc failed (%ld)\n", PTR_ERR(ddev));
		return (ENOMEM);
	}

	ddev->dev_private = sc;
	ddev->pdev        = pdev;
	sc->drm_dev       = ddev;

	err = nvkm_ttm_init(sc, ddev);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "drm: nvkm_ttm_init failed (%d)\n", err);
		drm_dev_put(ddev);
		sc->drm_dev = NULL;
		return (err);
	}

	/* Prepare KMS mode_config/connectors before drm_dev_register(), which
	 * registers the objects created here.  The imported display engine owns
	 * GSP display discovery; this layer owns DragonFly DRM object setup. */
	(void)nvkm_drm_kms_init(ddev, sc);

	err = drm_dev_register(ddev, 0);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "drm: drm_dev_register failed (%d)\n", err);
		nvkm_drm_kms_fini(sc);
		nvkm_ttm_fini(sc);
		drm_dev_put(ddev);
		sc->drm_dev = NULL;
		return (err);
	}

	nvkm_debugf(sc->dev,
	    "drm: registered as %s; check /dev/dri/renderD* and card*\n",
	    NVKM_DRM_NAME);
	return (0);
}

void
nvkm_drm_unregister(struct nvkm_softc *sc)
{
	nvkm_drm_kms_fini(sc);
	nvkm_dispnv50_fini(sc);
	if (sc->drm_dev != NULL) {
		drm_dev_unregister(sc->drm_dev);
		nvkm_ttm_fini(sc);
		drm_dev_put(sc->drm_dev);
		sc->drm_dev = NULL;
	}
}

/* ============================================================
 * Nouveau-uAPI ioctl handlers (Phase 3 Step 2).
 * Mirrors Linux include/uapi/drm/nouveau_drm.h subset that Mesa
 * NVK uses during VkPhysicalDevice probe.
 * ============================================================ */

/* ---- nouveau_drm.h subset (from Linux uapi) ---- */
#define DRM_NOUVEAU_GETPARAM		0x00
#define DRM_NOUVEAU_CHANNEL_ALLOC	0x02
#define DRM_NOUVEAU_CHANNEL_FREE	0x03
#define DRM_NOUVEAU_NVIF		0x07
#define DRM_NOUVEAU_VM_INIT		0x10
#define DRM_NOUVEAU_VM_BIND		0x11
#define DRM_NOUVEAU_EXEC		0x12
#define DRM_NOUVEAU_GEM_NEW		0x40
#define DRM_NOUVEAU_GEM_PUSHBUF		0x41
#define DRM_NOUVEAU_GEM_CPU_PREP	0x42
#define DRM_NOUVEAU_GEM_CPU_FINI	0x43
#define DRM_NOUVEAU_GEM_INFO		0x44

#define NOUVEAU_GETPARAM_PCI_VENDOR	3
#define NOUVEAU_GETPARAM_PCI_DEVICE	4
#define NOUVEAU_GETPARAM_BUS_TYPE	5
#define NOUVEAU_GETPARAM_FB_SIZE	8
#define NOUVEAU_GETPARAM_AGP_SIZE	9
#define NOUVEAU_GETPARAM_CHIPSET_ID	11
#define NOUVEAU_GETPARAM_VM_VRAM_BASE	12
#define NOUVEAU_GETPARAM_GRAPH_UNITS	13
#define NOUVEAU_GETPARAM_PTIMER_TIME	14
#define NOUVEAU_GETPARAM_HAS_BO_USAGE	15
#define NOUVEAU_GETPARAM_HAS_PAGEFLIP	16
#define NOUVEAU_GETPARAM_EXEC_PUSH_MAX	17
#define NOUVEAU_GETPARAM_VRAM_BAR_SIZE	18
#define NOUVEAU_GETPARAM_VRAM_USED	19
#define NOUVEAU_GETPARAM_HAS_VMA_TILEMODE 20

struct drm_nouveau_getparam {
	uint64_t param;
	uint64_t value;
};

struct drm_nouveau_vm_init {
	uint64_t kernel_managed_addr;
	uint64_t kernel_managed_size;
};

/* ---- NVIF wire format ---- */
struct nvif_ioctl_v0 {
	uint8_t  version;
	uint8_t  type;
#define NVIF_IOCTL_V0_NEW	0x02
#define NVIF_IOCTL_V0_DEL	0x03
#define NVIF_IOCTL_V0_MTHD	0x04
	uint8_t  path_nr;
	uint8_t  pad03[3];
	uint8_t  owner;
	uint8_t  route;
	uint64_t token;
	uint64_t object;
	uint8_t  data[];
} __packed;

struct nvif_ioctl_new_v0 {
	uint8_t  version;
	uint8_t  pad01[2];
	uint8_t  route;
	uint32_t pad04;        /* explicit pad: linux uses NV_DECLARE_ALIGNED u64 */
	uint64_t token;
	uint64_t object;
	uint32_t handle;
#define NV_DEVICE	0x0080
	uint32_t oclass;
	uint8_t  data[];
} __packed;

struct nv_device_v0 {
	uint8_t  version;
	uint8_t  pad01[7];
	uint64_t device;
	uint32_t priv;
	uint32_t pad14;
} __packed;

struct nvif_ioctl_mthd_v0 {
	uint8_t  version;
	uint8_t  method;
#define NV_DEVICE_V0_INFO	0x00
	uint8_t  pad02[6];
	uint8_t  data[];
} __packed;

struct nv_device_info_v0 {
	uint8_t  version;
	uint8_t  platform;
#define NV_DEVICE_INFO_V0_PCIE	0x03
	uint16_t chipset;
	uint8_t  revision;
	uint8_t  family;
	uint8_t  pad06[2];
	uint64_t ram_size;
	uint64_t ram_user;
	char     chip[16];
	char     name[64];
} __packed;

struct drm_nouveau_channel_alloc {
	uint32_t fb_ctxdma_handle;
	uint32_t tt_ctxdma_handle;
	int32_t  channel;
	uint32_t pushbuf_domains;
	uint32_t notifier_handle;
	struct {
		uint32_t handle;
		uint32_t grclass;
	} subchan[8];
	uint32_t nr_subchan;
};

struct nvif_ioctl_sclass_oclass_v0 {
	int32_t  oclass;
	int16_t  minver;
	int16_t  maxver;
};
struct nvif_ioctl_sclass_v0 {
	uint8_t  version;
	uint8_t  count;
	uint8_t  pad02[6];
	struct nvif_ioctl_sclass_oclass_v0 oclass[];
};
#define NVIF_IOCTL_V0_SCLASS	0x01

/* TU102 supported object classes (low byte = engine type). */
static const struct nvif_ioctl_sclass_oclass_v0 nvkm_tu102_classes[] = {
	{ .oclass = 0xc597 },    /* TURING_A          — 3D */
	{ .oclass = 0xc5c0 },    /* TURING_COMPUTE_A  — compute */
	{ .oclass = 0xc5b5 },    /* TURING_DMA_COPY_A — copy */
	{ .oclass = 0x902d },    /* FERMI_TWOD_A      — 2D */
	{ .oclass = 0xa140 },    /* KEPLER_INLINE_TO_MEMORY_B — m2mf; Turing reports this, not 0x9039 (NVK queue init asserts if M2MF<=FERMI) */
};
#define NVKM_TU102_NUM_CLASSES \
	(sizeof(nvkm_tu102_classes) / sizeof(nvkm_tu102_classes[0]))

#define NOUVEAU_FIFO_ENGINE_GR	0x01
#define NOUVEAU_FIFO_ENGINE_CE	0x30
static struct nvkm_drm_chan *
nvkm_drm_channel_find(struct nvkm_drm_file *nfile, uint32_t id)
{
	struct nvkm_drm_chan *dchan;

	LIST_FOREACH(dchan, &nfile->channels, link) {
		if (dchan->id == id)
			return (dchan);
	}
	return (NULL);
}

static struct nvkm_drm_chan_obj *
nvkm_drm_channel_obj_slot(struct nvkm_drm_chan *dchan)
{
	for (uint32_t i = 0; i < NVKM_DRM_MAX_CHAN_OBJS; i++) {
		if (dchan->obj[i].oclass == 0)
			return (&dchan->obj[i]);
	}
	return (NULL);
}

static void
nvkm_drm_channel_clear(struct nvkm_softc *sc, struct nvkm_drm_chan *dchan)
{
	if (dchan->chan != NULL) {
		lwkt_gettoken(&sc->gsp_tok);
		nvkm_drm_exec_pending_cancel_channel(sc, dchan->chan, -ENODEV);
		lwkt_reltoken(&sc->gsp_tok);
		for (uint32_t i = 0; i < NVKM_DRM_MAX_CHAN_OBJS; i++) {
			if (dchan->obj[i].oclass != 0 &&
			    dchan->obj[i].object.handle != 0)
				(void)nvkm_gsp_rm_free(&dchan->obj[i].object);
		}
		(void)nvkm_gsp_chan_dtor(dchan->chan);
		kfree(dchan->chan);
	}
	kfree(dchan);
}

static void
nvkm_drm_file_release(struct drm_device *ddev, struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct nvkm_drm_vm_binding *binding, *binding_next;
	struct nvkm_drm_chan *dchan, *dchan_next;
	uint32_t binding_count = 0, channel_count = 0;
	bool vmm_dirty = false;

	if (nfile == NULL)
		return;

	nvkm_drm_jobs_close(nfile);

	LIST_FOREACH_MUTABLE(dchan, &nfile->channels, link, dchan_next) {
		LIST_REMOVE(dchan, link);
		nvkm_drm_channel_clear(sc, dchan);
		channel_count++;
	}

	lwkt_gettoken(&sc->gsp_tok);
	LIST_FOREACH_MUTABLE(binding, &nfile->vm_bindings, link,
	    binding_next) {
		if (binding->pte_installed)
			vmm_dirty = true;
		(void)nvkm_drm_vm_binding_reclaim_noflush(sc, binding);
		binding_count++;
	}
	if (vmm_dirty && nfile->vmm != NULL)
		nvkm_gsp_vmm_flush(nfile->vmm);
	lwkt_reltoken(&sc->gsp_tok);

	/* Tear down the per-file address space only after its channels and
	 * bindings (which borrow this VMM) are gone. */
	if (nfile->vmm != NULL) {
		lwkt_gettoken(&sc->gsp_tok);
		nvkm_gsp_vmm_dtor(nfile->vmm);
		lwkt_reltoken(&sc->gsp_tok);
		kfree(nfile->vmm);
		nfile->vmm = NULL;
	}

	reservation_object_fini(&nfile->vm_resv);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: postclose released bindings=%u channels=%u\n",
	    binding_count, channel_count);
	kfree(nfile);
	file_priv->driver_priv = NULL;
}

/* Lazily build this file's per-file VMM.  Device open no longer creates
 * it: libdrm probes the node (drmGetDevices2) with throwaway open/close
 * cycles that never touch the GPU, and paying the multi-RPC VMM ctor
 * (~0.4s) on each made enumeration cost tens of seconds.  The first
 * VM_BIND or channel alloc that actually uses the GPU builds it here. */
static int
nvkm_drm_file_ensure_vmm(struct nvkm_softc *sc, struct nvkm_drm_file *nfile)
{
	struct nvkm_gsp_vmm *vmm;
	uint32_t client_handle;
	int err;

	if (nfile->vmm != NULL)
		return (0);

	lwkt_gettoken(&nfile->vm_token);
	if (nfile->vmm != NULL) {
		lwkt_reltoken(&nfile->vm_token);
		return (0);
	}

	vmm = kzalloc(sizeof(*vmm), GFP_KERNEL);
	if (vmm == NULL) {
		lwkt_reltoken(&nfile->vm_token);
		return (-ENOMEM);
	}
	client_handle = atomic_fetchadd_int(&nvkm_drm_next_client_handle, 1);
	/* Hold gsp_tok across the whole multi-RPC ctor so the client/device/
	 * vaspace/PDE-copy sequence is atomic against other GSP users; the
	 * token auto-releases during each RPC reply wait. */
	lwkt_gettoken(&sc->gsp_tok);
	err = nvkm_gsp_vmm_ctor(sc, client_handle, vmm);
	lwkt_reltoken(&sc->gsp_tok);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "nvkm_drm: per-file VMM ctor failed handle=0x%x err=%d\n",
		    client_handle, err);
		kfree(vmm);
		lwkt_reltoken(&nfile->vm_token);
		return (-err);
	}
	/* Publish only after the VMM is fully built so the lock-free fast
	 * path above never observes a half-constructed vmm. */
	nfile->vmm = vmm;
	lwkt_reltoken(&nfile->vm_token);
	return (0);
}

static int
nvkm_drm_open(struct drm_device *ddev, struct drm_file *file_priv)
{
	struct nvkm_drm_file *nfile;

	nfile = kzalloc(sizeof(*nfile), GFP_KERNEL);
	if (nfile == NULL)
		return (-ENOMEM);
	LIST_INIT(&nfile->vm_bindings);
	LIST_INIT(&nfile->channels);
	TAILQ_INIT(&nfile->job_queue);
	lwkt_token_init(&nfile->vm_token, "nvkm-vm");
	lwkt_token_init(&nfile->job_token, "nvkm-job");
	lockinit(&nfile->job_submit_lock, "nvkjsb", 0, 0);
	reservation_object_init(&nfile->vm_resv);
	INIT_WORK(&nfile->job_work, nvkm_drm_job_work);

	/* nfile->vmm stays NULL; nvkm_drm_file_ensure_vmm builds it on the
	 * first GPU use so probe-only opens stay cheap (see that helper). */
	file_priv->driver_priv = nfile;
	return (0);
}

static void
nvkm_drm_postclose(struct drm_device *ddev, struct drm_file *file_priv)
{
	nvkm_drm_file_release(ddev, file_priv);
}

static void
nvkm_drm_restore_console(struct drm_device *ddev, const char *reason)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	unsigned int primary_clients;
	int err;

	if (sc == NULL)
		return;

	primary_clients = nvkm_drm_primary_client_count(ddev);
	if (primary_clients != 0) {
		sc->kms_restore_skip_primary_count++;
		sc->kms_restore_last_primary_count = primary_clients;
		sc->kms_restore_last_open_count = ddev->open_count;
		nvkm_debugf(sc->dev,
		    "drm: skip %s console restore: primary_clients=%u "
		    "open_count=%d\n",
		    reason, primary_clients, ddev->open_count);
		return;
	}

	err = nvkm_drm_kms_schedule(sc, reason);
	if (err != 0)
		nvkm_infof(sc->dev,
		    "drm: %s console restore schedule failed err=%d\n",
		    reason, err);
}

static void
nvkm_drm_lastclose(struct drm_device *ddev)
{
	nvkm_drm_restore_console(ddev, "lastclose");
}

static unsigned int
nvkm_drm_primary_client_count(struct drm_device *ddev)
{
	struct drm_file *file_priv;
	unsigned int count = 0;

	if (ddev == NULL)
		return (0);

	mutex_lock(&ddev->filelist_mutex);
	list_for_each_entry(file_priv, &ddev->filelist, lhead) {
		if (file_priv->minor != NULL &&
		    file_priv->minor->type == DRM_MINOR_PRIMARY)
			count++;
	}
	mutex_unlock(&ddev->filelist_mutex);

	return (count);
}

/* ---- Helper: locate nvkm_softc from drm_file ---- */
static struct nvkm_softc *
nvkm_drm_sc(struct drm_device *ddev)
{
	return (struct nvkm_softc *)ddev->dev_private;
}

/* ---- DRM_NOUVEAU_GETPARAM ---- */
static int
nvkm_drm_ioctl_getparam(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct drm_nouveau_getparam *gp = data;

	switch (gp->param) {
	case NOUVEAU_GETPARAM_PCI_VENDOR:
		gp->value = pci_get_vendor(sc->dev);
		break;
	case NOUVEAU_GETPARAM_PCI_DEVICE:
		gp->value = pci_get_device(sc->dev);
		break;
	case NOUVEAU_GETPARAM_CHIPSET_ID:
		/* TU102 chipset id 0x162. We could query GSP-RM but it's
		 * a known value for our hardware. */
		gp->value = 0x162;
		break;
	case NOUVEAU_GETPARAM_BUS_TYPE:
		/* Legacy GETPARAM bus enum (Linux nouveau_abi16): AGP=0, PCI=1,
		 * PCIE=2, SOC=3 — distinct from the NVIF NV_DEVICE_INFO_V0_PCIE
		 * (=3) enum. We are a discrete PCIe card. */
		gp->value = 2;
		break;
	case NOUVEAU_GETPARAM_FB_SIZE:
		gp->value = sc->fb_usable_size;
		break;
	case NOUVEAU_GETPARAM_VRAM_BAR_SIZE:
		gp->value = sc->bar_res[1] != NULL ?
		    rman_get_size(sc->bar_res[1]) : 0;
		break;
	case NOUVEAU_GETPARAM_EXEC_PUSH_MAX:
		/*
		 * Max pushes NVK may pack into one EXEC ioctl. Match nouveau's
		 * nouveau_exec_push_max_from_ib_max(): half the GPFIFO ring minus
		 * one, so a single submit cannot outrun the ring once the
		 * completion trailer, fetch-window NOPs, and the get != put
		 * headroom are accounted for, and two max submits still fit.
		 */
		gp->value = NVKM_DRM_GPFIFO_ENTRIES / 2 - 1;
		break;
	case NOUVEAU_GETPARAM_GRAPH_UNITS:
		/* NVK interprets low 8 bits as GPC count and bits 8..23 as TPC count.
		 * TU102 / RTX 2080 Ti has 6 GPCs and 34 TPCs (68 SMs / 2 MPs per TPC). */
		gp->value = (34ULL << 8) | 6ULL;
		break;
	case NOUVEAU_GETPARAM_VRAM_USED:
		/* NVK asserts >0 on success; force fail so NVK uses 0 fallback. */
		return (-EINVAL);
	case NOUVEAU_GETPARAM_PTIMER_TIME: {
		/* 64-bit nanosecond counter at NV_PTIMER_TIME_0/_1 (BAR0 MMIO).
		 * Re-read the high word to guard against low-word rollover
		 * between the two reads. Mesa treats 0 as a valid timestamp, so
		 * a real value is required. */
		uint32_t hi, lo, hi2;

		do {
			hi = nvkm_rd32(sc, 0x009410);
			lo = nvkm_rd32(sc, 0x009400);
			hi2 = nvkm_rd32(sc, 0x009410);
		} while (hi != hi2);
		gp->value = ((uint64_t)hi << 32) | lo;
		break;
	}
	case NOUVEAU_GETPARAM_HAS_VMA_TILEMODE:
		/*
		 * VM_BIND carries the PTE kind in op->flags bits 7:0 and the
		 * map paths write it into GMMU PTE bits 63:56.  This is what
		 * NVK gates VK_EXT_image_drm_format_modifier on.  The sysctl
		 * is a kill switch while tiled-rendering wedges are debugged.
		 */
		gp->value = sc->vma_tilemode != 0 ? 1 : 0;
		break;
	default:
		nvkm_debugf(sc->dev,
		    "nvkm_drm: GETPARAM 0x%llx unhandled\n",
		    (unsigned long long)gp->param);
		return (-EINVAL);
	}
	return (0);
}

/* ---- DRM_NOUVEAU_VM_INIT ---- */
static int
nvkm_drm_ioctl_vm_init(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct drm_nouveau_vm_init *vminit = data;

	sc->vm_init_kernel_addr = vminit->kernel_managed_addr;
	sc->vm_init_kernel_size = vminit->kernel_managed_size;
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_INIT addr=0x%llx size=0x%llx (stub success)\n",
	    (unsigned long long)vminit->kernel_managed_addr,
	    (unsigned long long)vminit->kernel_managed_size);
	return (0);
}

/* ---- DRM_NOUVEAU_NVIF ---- */
static int
nvkm_drm_ioctl_nvif(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvif_ioctl_v0 *hdr = data;

	if (hdr->version != 0) {
		nvkm_debugf(sc->dev, "nvkm_drm: NVIF bad version %u\n",
		    hdr->version);
		return (-ENOSYS);
	}

	switch (hdr->type) {
	case NVIF_IOCTL_V0_NEW: {
		struct nvif_ioctl_new_v0 *new_ = (void *)hdr->data;
		struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
		struct nvkm_drm_chan *dchan;
		struct nvkm_drm_chan_obj *obj;
		int err;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: NVIF NEW oclass=0x%x handle=0x%x token=0x%llx\n",
		    new_->oclass, new_->handle,
		    (unsigned long long)new_->token);
		if (new_->oclass == NV_DEVICE) {
			/* stub: accept the NV_DEVICE allocation. */
			return (0);
		}
		switch (new_->oclass) {
		case 0xc597: case 0xc5c0: case 0xc5b5:
		case 0x902d: case 0xa140:
			if (nfile == NULL)
				return (-ENXIO);
			dchan = nvkm_drm_channel_find(nfile,
			    (uint32_t)hdr->token);
			if (dchan == NULL || dchan->chan == NULL)
				return (-ENOENT);
			if (new_->oclass == 0xc597 || new_->oclass == 0xc5c0 ||
			    new_->oclass == 0x902d || new_->oclass == 0xa140) {
				err = nvkm_gsp_chan_promote_gr_ctx(nfile->vmm,
				    dchan->chan, 0);
				if (err != 0)
					return (-err);
			}
			obj = nvkm_drm_channel_obj_slot(dchan);
			if (obj == NULL)
				return (-ENOMEM);
			err = nvkm_gsp_chan_alloc_obj(dchan->chan, new_->handle,
			    new_->oclass, &obj->object);
			if (err != 0)
				return (-err);
			obj->handle = new_->handle;
			obj->oclass = new_->oclass;
			obj->nvif_object = new_->object;
			return (0);
		}
		return (-EINVAL);
	}
	case NVIF_IOCTL_V0_MTHD: {
		struct nvif_ioctl_mthd_v0 *mthd = (void *)hdr->data;
		if (mthd->method == NV_DEVICE_V0_INFO) {
			struct nv_device_info_v0 *info = (void *)mthd->data;
			memset(info, 0, sizeof(*info));
			info->version  = 0;
			info->platform = NV_DEVICE_INFO_V0_PCIE;
			info->chipset  = 0x162;	/* TU102 */
			info->revision = pci_get_revid(sc->dev);
			info->family   = 0x0a;	/* TURING per nv_device.h family enum */
			info->ram_size = sc->fb_usable_size;
			info->ram_user = sc->fb_usable_size;
			strncpy(info->chip, "TU102", sizeof(info->chip));
			strncpy(info->name, "NVIDIA GeForce RTX 2080 Ti",
			    sizeof(info->name));
			nvkm_debugf(sc->dev,
			    "nvkm_drm: NVIF MTHD DEVICE_INFO -> TU102 ram=0x%llx user=0x%llx bar1=0x%llx\n",
			    (unsigned long long)info->ram_size,
			    (unsigned long long)info->ram_user,
			    (unsigned long long)(sc->bar_res[1] != NULL ?
			    rman_get_size(sc->bar_res[1]) : 0));
			return (0);
		}
		return (-EINVAL);
	}
	case NVIF_IOCTL_V0_SCLASS: {
		struct nvif_ioctl_sclass_v0 *sc_ = (void *)hdr->data;
		uint32_t want = sc_->count;
		uint32_t fill = want < NVKM_TU102_NUM_CLASSES ?
		    want : NVKM_TU102_NUM_CLASSES;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: NVIF SCLASS want=%u fill=%u\n", want, fill);
		for (uint32_t i = 0; i < fill; i++)
			sc_->oclass[i] = nvkm_tu102_classes[i];
		sc_->count = fill;
		return (0);
	}
	case NVIF_IOCTL_V0_DEL: {
		struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
		struct nvkm_drm_chan *dchan;

		/* Free the engine object created by a matching NVIF NEW and
		 * clear its slot (channel teardown then skips it). Untracked
		 * handles — e.g. the NV_DEVICE stub, which allocates nothing —
		 * succeed without action, as on Linux. */
		if (nfile != NULL) {
			LIST_FOREACH(dchan, &nfile->channels, link) {
				for (uint32_t i = 0; i < NVKM_DRM_MAX_CHAN_OBJS;
				    i++) {
					struct nvkm_drm_chan_obj *o =
					    &dchan->obj[i];

					if (o->oclass == 0 ||
					    o->nvif_object != hdr->object)
						continue;
					if (o->object.handle != 0)
						(void)nvkm_gsp_rm_free(
						    &o->object);
					o->oclass = 0;
					o->handle = 0;
					o->nvif_object = 0;
					return (0);
				}
			}
		}
		return (0);
	}
	default:
		nvkm_debugf(sc->dev,
		    "nvkm_drm: NVIF type %u unhandled\n", hdr->type);
		return (-EINVAL);
	}
}

/* ---- DRM_NOUVEAU_CHANNEL_ALLOC ---- */
static int
nvkm_drm_ioctl_channel_alloc(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_channel_alloc *req = data;
	struct nvkm_drm_chan *dchan = NULL;
	uint32_t engine_type;
	uint32_t channel_count = 0;
	int err;

	if (nfile == NULL)
		return (-ENXIO);
	err = nvkm_drm_file_ensure_vmm(sc, nfile);
	if (err != 0)
		return (err);
	LIST_FOREACH(dchan, &nfile->channels, link)
		channel_count++;
	if (channel_count >= NVKM_DRM_MAX_CHANNELS)
		return (-ENOMEM);

	/* Engine selector matches nouveau (Kepler+): tt_ctxdma_handle is an
	 * engine id only when fb_ctxdma_handle == ~0, and an unrecognised
	 * engine is rejected rather than silently mapped to graphics. We
	 * implement GR and CE; the video engines are absent. Determined before
	 * any allocation so the reject path leaks nothing. */
	engine_type = NV2080_ENGINE_TYPE_GRAPHICS;
	if (req->fb_ctxdma_handle == ~0u) {
		switch (req->tt_ctxdma_handle) {
		case NOUVEAU_FIFO_ENGINE_GR:
			engine_type = NV2080_ENGINE_TYPE_GRAPHICS;
			break;
		case NOUVEAU_FIFO_ENGINE_CE:
			engine_type = NV2080_ENGINE_TYPE_COPY0;
			break;
		default:
			nvkm_debugf(sc->dev,
			    "nvkm_drm: CHANNEL_ALLOC unsupported engine tt=0x%x\n",
			    req->tt_ctxdma_handle);
			return (-ENOSYS);
		}
	}

	dchan = kzalloc(sizeof(*dchan), GFP_KERNEL);
	if (dchan == NULL)
		return (-ENOMEM);
	dchan->chan = kzalloc(sizeof(*dchan->chan), GFP_KERNEL);
	if (dchan->chan == NULL) {
		kfree(dchan);
		return (-ENOMEM);
	}

	lwkt_gettoken(&sc->gsp_tok);
	if (engine_type == NV2080_ENGINE_TYPE_GRAPHICS) {
		err = nvkm_gsp_gr_oneinit(sc->gsp_vmm);
		if (err != 0) {
			lwkt_reltoken(&sc->gsp_tok);
			kfree(dchan->chan);
			kfree(dchan);
			return (-err);
		}
	}
	err = nvkm_gsp_chan_ctor(nfile->vmm, engine_type, dchan->chan);
	lwkt_reltoken(&sc->gsp_tok);
	if (err != 0) {
		kfree(dchan->chan);
		kfree(dchan);
		return (-err);
	}

	/* Atomic: concurrent drm_file opens must not collide on a channel id. */
	dchan->id = atomic_fetchadd_int(&nvkm_drm_next_channel, 1);
	dchan->engine_type = engine_type;
	LIST_INSERT_HEAD(&nfile->channels, dchan, link);
	req->channel = dchan->id;
	req->pushbuf_domains = 2;
	req->notifier_handle = 0;
	req->nr_subchan = 0;
	nvkm_debugf(sc->dev,
	    "nvkm_drm: CHANNEL_ALLOC -> channel=%d chid=%d engine=0x%x req_tt=0x%x\n",
	    req->channel, dchan->chan->chid, engine_type, req->tt_ctxdma_handle);
	return (0);
}

/* ---- DRM_NOUVEAU_CHANNEL_FREE ---- */
struct drm_nouveau_channel_free { int32_t channel; };
static int
nvkm_drm_ioctl_channel_free(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_channel_free *req = data;
	struct nvkm_drm_chan *dchan;

	if (nfile == NULL)
		return (-ENXIO);
	nvkm_drm_jobs_flush(nfile);
	dchan = nvkm_drm_channel_find(nfile, (uint32_t)req->channel);
	if (dchan == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: CHANNEL_FREE unknown channel=%d\n", req->channel);
		return (-ENOENT);
	}
	LIST_REMOVE(dchan, link);
	nvkm_drm_channel_clear(sc, dchan);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: CHANNEL_FREE channel=%d\n", req->channel);
	return (0);
}

#define DRM_NOUVEAU_SYNC_SYNCOBJ	0x0
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ	0x1
#define DRM_NOUVEAU_SYNC_TYPE_MASK	0xf

struct drm_nouveau_sync {
	uint32_t flags;
	uint32_t handle;
	uint64_t timeline_value;
};

#define DRM_NOUVEAU_VM_BIND_OP_MAP	0x0
#define DRM_NOUVEAU_VM_BIND_OP_UNMAP	0x1
#define DRM_NOUVEAU_VM_BIND_SPARSE	(1 << 8)
#define DRM_NOUVEAU_VM_BIND_RUN_ASYNC	0x1

struct drm_nouveau_vm_bind_op {
	uint32_t op;
	uint32_t flags;
	uint32_t handle;
	uint32_t pad;
	uint64_t addr;
	uint64_t bo_offset;
	uint64_t range;
};

struct drm_nouveau_vm_bind {
	uint32_t op_count;
	uint32_t flags;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t op_ptr;
};

struct nvkm_drm_exec_signal;

static int nvkm_drm_queue_vm_bind_job(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    const struct drm_nouveau_vm_bind *req,
    struct drm_nouveau_vm_bind_op *ops, bool sync);

static int nvkm_drm_vm_bind_attach_resv_fence(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct drm_nouveau_vm_bind_op *ops,
    uint32_t op_count, struct drm_gem_object **objects,
    struct dma_fence *fence);

static int
nvkm_drm_vm_bind_apply(struct nvkm_softc *sc, struct drm_file *file_priv,
    struct nvkm_drm_file *nfile, struct drm_nouveau_vm_bind_op *ops,
    uint32_t op_count, struct drm_gem_object **objects)
{
	struct drm_nouveau_vm_bind_op *current_op = NULL;
	int err = 0;
	bool gsp_tok_held = false;
	bool remap_started = false;
	uint64_t profile_start;

	/* vm_token (outer) serializes this remap against other remaps and
	 * prevents EXEC from doorbelling while the PTE update is half-written.
	 * It deliberately does not wait for already submitted GPU work; nouveau
	 * exposes that ordering through syncobjs and VM_BIND/EXEC fences. */
	profile_start = nvkm_drm_profile_now_us();
	lwkt_gettoken(&nfile->vm_token);
	remap_started = true;
	lwkt_gettoken(&sc->gsp_tok);
	gsp_tok_held = true;
	nvkm_drm_profile_add_us(&sc->vm_bind_profile_token_wait_us,
	    profile_start);
	profile_start = nvkm_drm_profile_now_us();

	for (uint32_t i = 0; i < op_count; i++) {
		struct drm_nouveau_vm_bind_op *op = &ops[i];

		current_op = op;
		if (op->range == 0 ||
		    ((op->addr | op->bo_offset | op->range) &
		     (NVKM_GMMU_PT_PAGE_SIZE - 1))) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND invalid idx=%u op=%u flags=0x%08x handle=%u addr=0x%016jx bo_off=0x%016jx range=0x%016jx\n",
			    i, op->op, op->flags, op->handle,
			    (uintmax_t)op->addr, (uintmax_t)op->bo_offset,
			    (uintmax_t)op->range);
			err = -EINVAL;
			break;
		}

		if (op->op == DRM_NOUVEAU_VM_BIND_OP_UNMAP) {
			uint32_t action =
			    (op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0 ?
			    NVKM_DRM_VM_TRACE_UNMAP_SPARSE :
			    NVKM_DRM_VM_TRACE_UNMAP;
			uint32_t unmapped = 0;

			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND unmap idx=%u op=%u flags=0x%08x handle=%u addr=0x%016jx range=0x%016jx\n",
			    i, op->op, op->flags, op->handle,
			    (uintmax_t)op->addr, (uintmax_t)op->range);
			nvkm_drm_vm_bind_note_op(sc, action, op->range);
			err = nvkm_drm_vm_bindings_remove_range(sc, nfile,
			    op->addr, op->range,
			    (op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) == 0,
			    action, &unmapped);
			if (err != 0) {
				nvkm_drm_vm_trace_record(sc, action,
				    op->flags, op->handle, op->addr,
				    op->range, op->bo_offset, NULL, err);
				break;
			}
			if (unmapped == 0 &&
			    (op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0) {
				err = nvkm_gsp_vmm_unmap_sparse_noflush(nfile->vmm,
				    op->addr, op->range);
			} else {
				err = 0;
			}
			nvkm_drm_vm_trace_record(sc, action, op->flags,
			    op->handle, op->addr, op->range, op->bo_offset,
			    NULL, err != 0 ? -err : 0);
			if (err != 0) {
				err = -err;
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND unmap failed idx=%u err=%d\n",
				    i, err);
			}
			continue;
		}

		if (op->op == DRM_NOUVEAU_VM_BIND_OP_MAP) {
			struct drm_gem_object *obj;
			struct nvkm_bo *bo;
			struct nvkm_drm_vm_binding *new_binding = NULL;
			struct nvkm_drm_vm_binding_list replace_tails;
			bool bo_pinned = false;
			bool job_object = false;
			uint32_t unmapped = 0;

			LIST_INIT(&replace_tails);

			if ((op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0) {
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND map sparse idx=%u flags=0x%08x handle=%u addr=0x%016jx range=0x%016jx\n",
				    i, op->flags, op->handle,
				    (uintmax_t)op->addr, (uintmax_t)op->range);
				nvkm_drm_vm_bind_note_op(sc,
				    NVKM_DRM_VM_TRACE_MAP_SPARSE, op->range);
				err = nvkm_drm_vm_bindings_remove_range(sc,
				    nfile, op->addr, op->range, false,
				    NVKM_DRM_VM_TRACE_MAP_SPARSE, &unmapped);
				if (err == 0) {
					err = nvkm_gsp_vmm_map_sparse_noflush(
					    nfile->vmm, op->addr, op->range);
					if (err != 0)
						err = -err;
				}
				nvkm_drm_vm_trace_record(sc,
				    NVKM_DRM_VM_TRACE_MAP_SPARSE, op->flags,
				    op->handle, op->addr, op->range,
				    op->bo_offset, NULL, err);
				if (err != 0) {
					nvkm_debugf(sc->dev,
					    "nvkm_drm: VM_BIND map sparse failed idx=%u err=%d\n",
					    i, err);
					break;
				}
				continue;
			}

			if (op->handle == 0) {
				uint32_t unmapped = 0;

				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND map-null idx=%u flags=0x%08x addr=0x%016jx range=0x%016jx\n",
				    i, op->flags, (uintmax_t)op->addr,
				    (uintmax_t)op->range);
				nvkm_drm_vm_bind_note_op(sc,
				    NVKM_DRM_VM_TRACE_MAP_NULL, op->range);
				err = nvkm_drm_vm_bindings_remove_range(sc,
				    nfile, op->addr, op->range, true,
				    NVKM_DRM_VM_TRACE_MAP_NULL, &unmapped);
				if (err != 0)
					break;
				nvkm_drm_vm_trace_record(sc,
				    NVKM_DRM_VM_TRACE_MAP_NULL, op->flags,
				    op->handle, op->addr, op->range,
				    op->bo_offset, NULL, 0);
				continue;
			}

			if (objects != NULL && objects[i] != NULL) {
				obj = objects[i];
				job_object = true;
			} else {
				obj = drm_gem_object_lookup(file_priv,
				    op->handle);
			}
			if (obj == NULL) {
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND missing BO idx=%u handle=%u\n",
				    i, op->handle);
				err = -ENOENT;
				break;
			}
			bo = to_nvkm_bo(obj);
			if (op->bo_offset > obj->size ||
			    op->range > obj->size - op->bo_offset) {
				if (!job_object)
					drm_gem_object_put_unlocked(obj);
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND BO range invalid idx=%u handle=%u bo_off=0x%016jx range=0x%016jx size=0x%016jx\n",
				    i, op->handle, (uintmax_t)op->bo_offset,
				    (uintmax_t)op->range, (uintmax_t)obj->size);
				err = -EINVAL;
				break;
			}
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND map idx=%u flags=0x%08x handle=%u obj=%p domain=0x%x addr=0x%016jx bo_off=0x%016jx range=0x%016jx paddr=0x%016jx\n",
			    i, op->flags, op->handle, obj, bo->domain,
			    (uintmax_t)op->addr, (uintmax_t)op->bo_offset,
			    (uintmax_t)op->range, (uintmax_t)bo->paddr);
			nvkm_drm_vm_bind_note_op(sc, NVKM_DRM_VM_TRACE_MAP,
			    op->range);
			err = nvkm_drm_vm_bindings_prepare_replace_range(nfile,
			    op->addr, op->range, &replace_tails);
			if (err != 0) {
				if (!job_object)
					drm_gem_object_put_unlocked(obj);
				break;
			}
			err = nvkm_bo_vm_bind_pin(bo);
			if (err != 0) {
				nvkm_drm_vm_bindings_abort_replace_range(&replace_tails);
				if (!job_object)
					drm_gem_object_put_unlocked(obj);
				err = -err;
				break;
			}
			bo_pinned = true;
			if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) == 0) {
				err = nvkm_bo_ensure_ttm_populated(bo);
				if (err != 0) {
					nvkm_drm_vm_bindings_abort_replace_range(&replace_tails);
					(void)nvkm_bo_vm_bind_unpin(bo);
					if (!job_object)
						drm_gem_object_put_unlocked(obj);
					err = -err;
					break;
				}
			}
			new_binding = nvkm_drm_vm_binding_alloc(nfile, op->addr,
			    op->range, obj, op->bo_offset);
			if (new_binding == NULL) {
				nvkm_drm_vm_bindings_abort_replace_range(&replace_tails);
				(void)nvkm_bo_vm_bind_unpin(bo);
				if (!job_object)
					drm_gem_object_put_unlocked(obj);
				err = -ENOMEM;
				break;
			}
			new_binding->bo_pinned = bo_pinned;
			bo_pinned = false;
			if (job_object)
				objects[i] = NULL;
			err = nvkm_gsp_vmm_ensure_pt_range(nfile->vmm, op->addr,
			    op->range);
			if (err != 0) {
				nvkm_drm_vm_binding_free(new_binding);
				nvkm_drm_vm_bindings_abort_replace_range(&replace_tails);
				err = -err;
				break;
			}
			/*
			 * NVK's VMA-tilemode protocol: bits 7:0 of op->flags
			 * carry the PTE kind (bit 8 is SPARSE).  Kinds are
			 * passed through unvalidated -- NVK only emits
			 * hardware-valid kinds, and a bogus one faults the
			 * offending context just like any other bad mapping.
			 */
			if (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) {
				err = nvkm_gsp_vmm_map_vram_flags_noflush(nfile->vmm,
				    op->addr, bo->paddr + op->bo_offset,
				    op->range, 0, 0, op->flags & 0xff);
			} else {
				err = nvkm_gsp_vmm_map_sysmem_bo_noflush(nfile->vmm,
				    op->addr, bo, op->bo_offset, op->range,
				    op->flags & 0xff);
			}
			nvkm_drm_vm_trace_record(sc, NVKM_DRM_VM_TRACE_MAP,
			    op->flags, op->handle, op->addr, op->range,
			    op->bo_offset, obj, err != 0 ? -err : 0);
			if (err != 0) {
				nvkm_drm_vm_binding_free(new_binding);
				nvkm_drm_vm_bindings_abort_replace_range(&replace_tails);
				err = -err;
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND map failed idx=%u err=%d\n",
				    i, err);
				break;
			}
			/*
			 * Mark the bo itself: scanout fb creation happens on
			 * a different drm file (compositor master fd) but
			 * same-device dmabuf import resolves to this same
			 * GEM object, so the flag travels with the buffer.
			 */
			if ((op->flags & 0xff) != 0) {
				uint8_t kind = op->flags & 0xff;

				if (!bo->vm_bound_tiled)
					bo->vm_bound_kind = kind;
				else if (bo->vm_bound_kind != kind)
					bo->vm_bound_mixed_kind = true;
				bo->vm_bound_tiled = true;
			}
			nvkm_drm_vm_bindings_commit_replace_range(sc, nfile,
			    op->addr, op->range, &replace_tails);
			nvkm_drm_vm_binding_insert_sorted(nfile, new_binding);
			new_binding = NULL;
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND track addr=0x%016jx size=0x%016jx obj=%p\n",
			    (uintmax_t)op->addr, (uintmax_t)op->range, obj);
			continue;
		}

		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND unknown op idx=%u op=%u flags=0x%08x\n",
		    i, op->op, op->flags);
		err = -EINVAL;
		break;
	}

out_unlock:
	nvkm_drm_profile_add_us(&sc->vm_bind_profile_apply_us,
	    profile_start);
	/* One TLB invalidate publishes every PTE the loop wrote,
	 * instead of one per op (the _noflush calls skip it). */
	if (gsp_tok_held) {
		profile_start = nvkm_drm_profile_now_us();
		nvkm_gsp_vmm_flush(nfile->vmm);
		nvkm_drm_profile_add_us(&sc->vm_bind_profile_flush_us,
		    profile_start);
	}
	/* Release inner (gsp_tok) before outer (vm_token). */
	if (gsp_tok_held)
		lwkt_reltoken(&sc->gsp_tok);
	if (remap_started)
		lwkt_reltoken(&nfile->vm_token);
	if (err != 0 && current_op != NULL)
		nvkm_drm_vm_bind_record_error(sc, current_op->op,
		    current_op->flags, current_op->handle, current_op->addr,
		    current_op->range, current_op->bo_offset, err);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND complete ops=%u err=%d\n",
	    op_count, err);
	return (err);
}

static int
nvkm_drm_vm_bind_prepare_objects(struct drm_file *file_priv,
    struct drm_nouveau_vm_bind_op *ops, uint32_t op_count,
    struct drm_gem_object ***pobjects)
{
	struct drm_gem_object **objects;
	int err = 0;

	*pobjects = NULL;
	if (op_count == 0)
		return (0);

	objects = kcalloc(op_count, sizeof(*objects), GFP_KERNEL);
	if (objects == NULL)
		return (-ENOMEM);

	for (uint32_t i = 0; i < op_count; i++) {
		struct drm_nouveau_vm_bind_op *op = &ops[i];
		struct drm_gem_object *obj;

		if (op->range == 0 ||
		    ((op->addr | op->bo_offset | op->range) &
		     (NVKM_GMMU_PT_PAGE_SIZE - 1))) {
			err = -EINVAL;
			break;
		}
		switch (op->op) {
		case DRM_NOUVEAU_VM_BIND_OP_MAP:
			break;
		case DRM_NOUVEAU_VM_BIND_OP_UNMAP:
			continue;
		default:
			err = -EINVAL;
			goto out;
		}
		if ((op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0 ||
		    op->handle == 0)
			continue;

		obj = drm_gem_object_lookup(file_priv, op->handle);
		if (obj == NULL) {
			err = -ENOENT;
			break;
		}
		if (op->bo_offset > obj->size ||
		    op->range > obj->size - op->bo_offset) {
			drm_gem_object_put_unlocked(obj);
			err = -EINVAL;
			break;
		}
		objects[i] = obj;
	}

out:
	if (err != 0) {
		for (uint32_t i = 0; i < op_count; i++) {
			if (objects[i] != NULL)
				drm_gem_object_put_unlocked(objects[i]);
		}
		kfree(objects);
		return (err);
	}

	*pobjects = objects;
	return (0);
}

static void
nvkm_drm_vm_bind_objects_put(struct drm_gem_object **objects,
    uint32_t op_count)
{
	if (objects == NULL)
		return;
	for (uint32_t i = 0; i < op_count; i++) {
		if (objects[i] != NULL)
			drm_gem_object_put_unlocked(objects[i]);
	}
	kfree(objects);
}

static int
nvkm_drm_ioctl_vm_bind(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_vm_bind *req = data;
	struct drm_nouveau_vm_bind_op *ops = NULL;
	int err = 0;
	bool async_bind;
	uint64_t profile_start;
	uint64_t profile_total_start;

	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND begin ops=%u waits=%u sigs=%u flags=0x%08x\n",
	    req->op_count, req->wait_count, req->sig_count, req->flags);
	if (sc->gsp_vmm == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND no gsp_vmm\n");
		return (-ENXIO);
	}
	if (nfile == NULL)
		return (-ENXIO);
	if ((req->flags & ~DRM_NOUVEAU_VM_BIND_RUN_ASYNC) != 0)
		return (-EINVAL);
	async_bind = (req->flags & DRM_NOUVEAU_VM_BIND_RUN_ASYNC) != 0;
	if (!async_bind && (req->wait_count != 0 || req->sig_count != 0))
		return (-EINVAL);
	if (req->op_count > 1024) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND too many ops=%u\n", req->op_count);
		return (-EINVAL);
	}

	profile_total_start = nvkm_drm_profile_now_us();
	err = nvkm_drm_file_ensure_vmm(sc, nfile);
	if (err != 0)
		return (err);
	sc->vm_bind_ioctl_count++;
	sc->vm_bind_op_count += req->op_count;
	if (req->op_count > sc->vm_bind_max_op_count)
		sc->vm_bind_max_op_count = req->op_count;
	if (async_bind)
		sc->vm_bind_async_count++;
	else
		sc->vm_bind_sync_count++;

	if (req->op_count != 0) {
		profile_start = nvkm_drm_profile_now_us();
		ops = kmalloc_array(req->op_count, sizeof(*ops), GFP_KERNEL);
		if (ops == NULL)
			return (-ENOMEM);
		err = copyin((const void *)(uintptr_t)req->op_ptr, ops,
		    sizeof(*ops) * req->op_count);
		nvkm_drm_profile_add_us(&sc->vm_bind_profile_copyin_us,
		    profile_start);
		if (err != 0) {
			kfree(ops);
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND copyin failed ops=%u err=%d\n",
			    req->op_count, err);
			return (-EFAULT);
		}
	}

	if (async_bind || req->op_count != 0) {
		err = nvkm_drm_queue_vm_bind_job(sc, file_priv, nfile,
		    req, ops, !async_bind);
		ops = NULL;
		nvkm_drm_profile_add_us(&sc->vm_bind_profile_total_us,
		    profile_total_start);
		return (err);
	}

	kfree(ops);
	nvkm_drm_profile_add_us(&sc->vm_bind_profile_total_us,
	    profile_total_start);
	return (0);
}

/* ---- DRM_NOUVEAU_EXEC ---- */
struct drm_nouveau_exec {
	uint32_t channel;
	uint32_t push_count;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t push_ptr;
};

#define NV_USERD_SLOT_SIZE	0x200
#define NV_USERD_GP_GET		0x88
#define NV_USERD_GP_PUT		0x8c
#define NV_USERMODE_BASE	0xbb0000u
#define NV_USERMODE_DOORBELL	(NV_USERMODE_BASE + 0x90)
#define NVC06F_GP_ENTRY1_LENGTH_SHIFT	10
#define NVC06F_GP_ENTRY1_NO_PREFETCH	(1u << 31)
#define DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH	0x1u
#define DRM_NOUVEAU_EXEC_PUSH_FLAGS_KNOWN	\
	DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH
#define NVC36F_DMA_SEC_OP_INC_METHOD	1u
#define NVC36F_MEM_OP_A_OFFSET		0x28
#define NVC36F_SEM_ADDR_LO_OFFSET	0x5c
#define NVC36F_SEM_EXECUTE_OFFSET	0x6c
#define NVC36F_NON_STALL_INTERRUPT_OFFSET	0x20
#define NVC36F_PUSH_HDR_MEM_OP_A_TO_D \
	((NVC36F_DMA_SEC_OP_INC_METHOD << 29) | (4u << 16) | \
	 (NVC36F_MEM_OP_A_OFFSET >> 2))
#define NVC36F_PUSH_HDR_SEM_ADDR_TRIPLET \
	((NVC36F_DMA_SEC_OP_INC_METHOD << 29) | (3u << 16) | \
	 (NVC36F_SEM_ADDR_LO_OFFSET >> 2))
#define NVC36F_PUSH_HDR_SEM_EXECUTE \
	((NVC36F_DMA_SEC_OP_INC_METHOD << 29) | (1u << 16) | \
	 (NVC36F_SEM_EXECUTE_OFFSET >> 2))
#define NVC36F_PUSH_HDR_NON_STALL_INTERRUPT \
	((NVC36F_DMA_SEC_OP_INC_METHOD << 29) | (1u << 16) | \
	 (NVC36F_NON_STALL_INTERRUPT_OFFSET >> 2))
#define NVC36F_SEM_EXECUTE_RELEASE	1u
#define NVC36F_SEM_EXECUTE_RELEASE_WFI	(1u << 20)
#define NVC36F_MEM_OP_C_MEMBAR_SYS_MEMBAR	0u
#define NVC36F_MEM_OP_D_OPERATION_MEMBAR	(0x5u << 27)
#define NVKM_DRM_SUBMIT_GVA_PUSHBUF	(NVKM_VMM_CLIENT_BASE + 0x0000ULL)
#define NVKM_DRM_SUBMIT_GVA_GPFIFO	(NVKM_VMM_CLIENT_BASE + 0x1000ULL)
#define NVKM_DRM_SUBMIT_GVA_SEMA	(NVKM_VMM_CLIENT_BASE + 0x2000ULL)
#define NVKM_DRM_POST_PUSH_DWORDS	13
#define NVKM_DRM_POST_RING_SLOTS	64
#define NVKM_DRM_GPFIFO_FETCH_WINDOW	0x40
#define NVKM_DRM_EXEC_POLL_US		5000000

struct nvkm_drm_exec_fence {
	struct dma_fence base;
	spinlock_t lock;
	volatile uint32_t *sema;
	uint32_t payload;
};

struct nvkm_drm_exec_signal {
	struct dma_fence *fence;
	struct drm_syncobj *syncobj;
	struct dma_fence_chain *chain;
	uint64_t timeline_value;
	uint32_t type;
};

static const char *
nvkm_drm_fence_name(struct dma_fence *fence __unused)
{
	return ("nvkm-drm");
}

static bool
nvkm_drm_fence_is_signaled(struct dma_fence *fence)
{
	struct nvkm_drm_exec_fence *f =
	    container_of(fence, struct nvkm_drm_exec_fence, base);
	volatile uint32_t *sema = f->sema;

	if (sema == NULL)
		return (false);
	cpu_lfence();
	return (*sema == f->payload);
}

static const struct dma_fence_ops nvkm_drm_fence_ops = {
	.get_driver_name = nvkm_drm_fence_name,
	.get_timeline_name = nvkm_drm_fence_name,
	.signaled = nvkm_drm_fence_is_signaled,
	.wait = dma_fence_default_wait,
};

/*
 * nvkm_drm_fence_flag_signaled()
 *
 * Ownership:
 *   Borrows fence.  It does not take, drop, or publish a dma_fence
 *   reference.
 *
 * Lifetime:
 *   The caller must hold a valid fence reference for the duration of the
 *   call.  This helper samples only the software completion flag and must
 *   not be used for functional wait decisions.
 *
 * Threading:
 *   Lockless diagnostic read.  The value is best-effort telemetry and may
 *   race with a concurrent signal path.
 */
static bool
nvkm_drm_fence_flag_signaled(struct dma_fence *fence)
{
	return (fence != NULL &&
	    test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags));
}

/*
 * nvkm_drm_exec_fence_hw_ready()
 *
 * Ownership:
 *   Borrows fence and reads the nvkm EXEC fence payload in place.  It never
 *   takes ownership and never calls dma_fence_signal().
 *
 * Lifetime:
 *   The caller must hold a valid fence reference.  For nvkm EXEC fences the
 *   semaphore pointer remains owned by the pending EXEC/job object; a NULL
 *   pointer means the fence has not been armed yet.
 *
 * Threading:
 *   Lockless diagnostic read paired with a CPU load fence before sampling the
 *   GPU-written semaphore value.  It is telemetry only; completion is still
 *   published by the normal dma_fence signal path.
 */
static bool
nvkm_drm_exec_fence_hw_ready(struct dma_fence *fence)
{
	struct nvkm_drm_exec_fence *f;
	volatile uint32_t *sema;

	if (fence == NULL || fence->ops != &nvkm_drm_fence_ops)
		return (false);
	f = container_of(fence, struct nvkm_drm_exec_fence, base);
	sema = f->sema;
	if (sema == NULL)
		return (false);
	cpu_lfence();
	return (*sema == f->payload);
}

static struct dma_fence *
nvkm_drm_exec_fence_create(struct nvkm_softc *sc, unsigned seqno)
{
	struct nvkm_drm_exec_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (f == NULL)
		return (NULL);

	lockinit(&f->lock, "nvdfen", 0, 0);
	dma_fence_init(&f->base, &nvkm_drm_fence_ops, &f->lock,
	    sc->fence_context, seqno);
	return (&f->base);
}

static void
nvkm_drm_wait_fences_put(struct dma_fence **fences, uint32_t count)
{
	if (fences == NULL)
		return;
	for (uint32_t i = 0; i < count; i++)
		dma_fence_put(fences[i]);
	kfree(fences);
}

static int
nvkm_drm_collect_wait_syncobjs(struct nvkm_softc *sc,
    struct drm_file *file_priv, uint32_t count, uint64_t wait_ptr,
    struct dma_fence ***pfences, uint32_t *pretained_count)
{
	struct drm_nouveau_sync *waits;
	struct dma_fence **fences;
	uint32_t retained = 0;
	int err = 0;

	*pfences = NULL;
	*pretained_count = 0;
	if (count == 0)
		return (0);
	sc->sync_wait_count += count;
	if (count > 64) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync wait invalid count=%u\n", count);
		return (-EINVAL);
	}

	waits = kmalloc_array(count, sizeof(*waits), GFP_KERNEL);
	fences = kcalloc(count, sizeof(*fences), GFP_KERNEL);
	if (waits == NULL) {
		return (-ENOMEM);
	}
	if (fences == NULL) {
		kfree(waits);
		return (-ENOMEM);
	}
	err = copyin((const void *)(uintptr_t)wait_ptr, waits,
	    sizeof(*waits) * count);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync wait copyin failed count=%u err=%d\n",
		    count, err);
		err = -EFAULT;
		goto out;
	}

	for (uint32_t i = 0; i < count; i++) {
		struct dma_fence *fence;
		uint32_t type = waits[i].flags & DRM_NOUVEAU_SYNC_TYPE_MASK;

		if (type != DRM_NOUVEAU_SYNC_SYNCOBJ &&
		    type != DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync wait unsupported flags idx=%u flags=0x%08x\n",
			    i, waits[i].flags);
			err = -EINVAL;
			break;
		}
		err = drm_syncobj_find_fence(file_priv, waits[i].handle,
		    type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ ?
		    waits[i].timeline_value : 0, &fence);
		if (err != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync wait missing fence idx=%u handle=%u err=%d\n",
			    i, waits[i].handle, err);
			break;
		}
		if (fence->ops == &nvkm_drm_fence_ops)
			sc->sync_wait_local_count++;
		else
			sc->sync_wait_external_count++;
		if (dma_fence_is_signaled(fence)) {
			sc->sync_wait_already_signaled_count++;
			dma_fence_put(fence);
			continue;
		}
		fences[retained++] = fence;
	}

out:
	kfree(waits);
	if (err != 0) {
		nvkm_drm_wait_fences_put(fences, retained);
		sc->sync_wait_error_count++;
		*pfences = NULL;
		*pretained_count = 0;
		return (err);
	}
	if (retained == 0) {
		kfree(fences);
		fences = NULL;
	}
	*pfences = fences;
	*pretained_count = retained;
	return (0);
}

static void
nvkm_drm_exec_signals_put(struct nvkm_drm_exec_signal *signals,
    uint32_t count)
{
	if (signals == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		dma_fence_put(signals[i].fence);
		if (signals[i].chain != NULL)
			dma_fence_chain_free(signals[i].chain);
		if (signals[i].syncobj != NULL)
			drm_syncobj_put(signals[i].syncobj);
	}
	kfree(signals);
}

/*
 * nvkm_drm_exec_signals_publish()
 *
 * Ownership:
 *   Consumes each prepared syncobj reference stored in signals.  For timeline
 *   syncobjs it also transfers ownership of the prepared dma_fence_chain node
 *   to drm_syncobj_add_point().  The per-signal fence reference remains owned
 *   by signals and is released by nvkm_drm_exec_signals_put().
 *
 * Lifetime:
 *   Must be called after the job done fence exists and before the job can be
 *   observed by the worker queue.  After this returns, userspace may wait on
 *   the published syncobj while the job object still owns the fence reference
 *   needed to signal completion.
 *
 * Threading:
 *   Called under nfile->job_submit_lock so out-sync publication is ordered with
 *   reservation-fence publication and per-file job enqueue.
 */
static void
nvkm_drm_exec_signals_publish(struct nvkm_drm_exec_signal *signals,
    uint32_t count)
{
	if (signals == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		if (signals[i].syncobj == NULL)
			continue;
		if (signals[i].type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			drm_syncobj_add_point(signals[i].syncobj,
			    signals[i].chain, dma_fence_get(signals[i].fence),
			    signals[i].timeline_value);
			signals[i].chain = NULL;
		} else {
			drm_syncobj_replace_fence(signals[i].syncobj, 0,
			    signals[i].fence);
		}
		drm_syncobj_put(signals[i].syncobj);
		signals[i].syncobj = NULL;
	}
}

static void
nvkm_drm_fence_set_error(struct dma_fence *fence, int error)
{
	if (fence == NULL || error == 0)
		return;
	fence->error = error;
}

static void
nvkm_drm_exec_signals_signal(struct nvkm_drm_exec_signal *signals,
    uint32_t count, int error)
{
	if (signals == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		if (error != 0)
			nvkm_drm_fence_set_error(signals[i].fence, error);
		(void)dma_fence_signal(signals[i].fence);
	}
}

static void
nvkm_drm_probe_pending_signal(struct nvkm_softc *sc,
    const struct nvkm_drm_exec_pending *pending, int error)
{
	if (sc == NULL || pending == NULL)
		return;

	sc->exec_pending_signal_count++;
	if (error != 0)
		sc->exec_pending_signal_error_count++;
	sc->exec_pending_signal_fence_count += pending->fence_count;
	for (uint32_t i = 0; i < pending->fence_count; i++) {
		if (nvkm_drm_fence_flag_signaled(pending->fences[i]))
			sc->exec_pending_signal_already_signaled_count++;
		if (nvkm_drm_exec_fence_hw_ready(pending->fences[i]))
			sc->exec_pending_signal_hw_ready_count++;
	}
}

static void
nvkm_drm_exec_pending_signal(struct nvkm_drm_exec_pending *pending, int error)
{
	for (uint32_t i = 0; i < pending->fence_count; i++) {
		if (error != 0)
			nvkm_drm_fence_set_error(pending->fences[i], error);
		(void)dma_fence_signal(pending->fences[i]);
	}
}

static void
nvkm_drm_exec_fence_arm(struct dma_fence *fence, volatile uint32_t *sema,
    uint32_t payload)
{
	struct nvkm_drm_exec_fence *f;

	if (fence == NULL || fence->ops != &nvkm_drm_fence_ops)
		return;

	f = container_of(fence, struct nvkm_drm_exec_fence, base);
	f->payload = payload;
	cpu_mfence();
	f->sema = sema;
}

static void
nvkm_drm_exec_pending_arm_fences(struct nvkm_drm_exec_pending *pending)
{
	for (uint32_t i = 0; i < pending->fence_count; i++)
		nvkm_drm_exec_fence_arm(pending->fences[i], pending->sema,
		    pending->payload);
}

static int
nvkm_drm_submit_slot_alloc(struct nvkm_gsp_chan *chan, uint32_t *slot)
{
	for (uint32_t i = 0; i < NVKM_DRM_POST_RING_SLOTS; i++) {
		uint32_t candidate = (chan->submit_post_slot + i) %
		    NVKM_DRM_POST_RING_SLOTS;
		uint64_t bit = 1ULL << candidate;

		if ((chan->submit_post_slots_busy & bit) != 0)
			continue;

		chan->submit_post_slots_busy |= bit;
		chan->submit_post_slot = (candidate + 1) %
		    NVKM_DRM_POST_RING_SLOTS;
		*slot = candidate;
		return (0);
	}
	return (-EAGAIN);
}

static void
nvkm_drm_submit_slot_release(struct nvkm_gsp_chan *chan, uint32_t slot)
{
	chan->submit_post_slots_busy &= ~(1ULL << slot);
	wakeup(chan);
}

static uint32_t
nvkm_drm_gpfifo_required(uint32_t put, uint32_t push_count)
{
	uint32_t required = 0;

	for (uint32_t i = 0; i < push_count; i++) {
		if ((put & (NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)) ==
		    NVKM_DRM_GPFIFO_FETCH_WINDOW - 1) {
			required++;
			put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
		}
		required++;
		put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	}
	if ((put & (NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)) ==
	    NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)
		required++;
	required++;
	return (required);
}

static uint32_t
nvkm_drm_gpfifo_free(uint32_t get, uint32_t put)
{
	return ((get - put - 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1));
}

static int
nvkm_drm_gpfifo_wait_space(struct nvkm_softc *sc, struct nvkm_gsp_chan *chan,
    uint64_t slot_bar1, uint32_t push_count, uint32_t *put,
    uint32_t *required_out)
{
	uint32_t required;
	int timeout_ticks = 5 * hz;
	int wait_ticks = hz / 100;

	if (wait_ticks < 1)
		wait_ticks = 1;

	*put = chan->gpf_put & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	required = nvkm_drm_gpfifo_required(*put, push_count);
	if (required >= NVKM_DRM_GPFIFO_ENTRIES)
		return (-EINVAL);
	if (required_out != NULL)
		*required_out = required;
	if (chan->gpf_free >= required)
		return (0);

	bool mismatch_logged = false;

	while (timeout_ticks > 0) {
		uint32_t get;
		uint32_t free;
		uint32_t put_rb;

		/*
		 * Match Linux nvif_chan_gpfifo_wait(): cache the free count
		 * and only refresh GP_GET when the software count is exhausted.
		 * GP_PUT is written only by this driver; the shadow value is
		 * authoritative, and USERD GP_PUT readback is debug-only because
		 * it can be stale during channel bring-up.
		 */
		if (nvkm_debug != 0) {
			put_rb = nvkm_gsp_bar1_rd32(sc,
			    slot_bar1 + NV_USERD_GP_PUT) &
			    (NVKM_DRM_GPFIFO_ENTRIES - 1);
			if (put_rb != *put && !mismatch_logged) {
				mismatch_logged = true;
				nvkm_infof(sc->dev,
				    "EXEC GP_PUT readback mismatch chid=%d readback=%u shadow=%u\n",
				    chan->chid, put_rb, *put);
			}
		}
		get = nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET) &
		    (NVKM_DRM_GPFIFO_ENTRIES - 1);
		free = nvkm_drm_gpfifo_free(get, *put);
		chan->gpf_free = free;
		if (free >= required)
			return (0);

		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC waits for GPFIFO space chid=%d get=%u put=%u free=%u required=%u\n",
		    chan->chid, get, *put, free, required);
		lwkt_reltoken(&sc->gsp_tok);
		(void)tsleep(chan, 0, "nvkgpf", wait_ticks);
		lwkt_gettoken(&sc->gsp_tok);
		timeout_ticks -= wait_ticks;
	}

	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC GPFIFO space timeout chid=%d put=%u required=%u\n",
	    chan->chid, *put, nvkm_drm_gpfifo_required(*put, push_count));
	return (-ETIME);
}

static struct nvkm_drm_exec_pending *
nvkm_drm_exec_pending_create(struct nvkm_gsp_chan *chan,
    volatile uint32_t *sema, uint32_t payload, uint32_t post_slot,
    uint64_t trace_seq, uint32_t trace_first, uint32_t trace_count,
    struct nvkm_drm_exec_signal *signals, uint32_t sig_count,
    struct dma_fence *exec_fence)
{
	struct nvkm_drm_exec_pending *pending;

	pending = kzalloc(sizeof(*pending), GFP_KERNEL);
	if (pending == NULL)
		return (NULL);

	pending->chan = chan;
	pending->sema = sema;
	pending->payload = payload;
	pending->post_slot = post_slot;
	pending->trace_seq = trace_seq;
	pending->trace_first = trace_first;
	pending->trace_count = trace_count;
	(void)signals;
	(void)sig_count;
	if (exec_fence == NULL) {
		kfree(pending);
		return (NULL);
	}
	pending->fence_count = 1;
	pending->fences[0] = dma_fence_get(exec_fence);
	return (pending);
}

static void
nvkm_drm_exec_pending_put(struct nvkm_softc *sc,
    struct nvkm_drm_exec_pending *pending)
{
	if (pending == NULL)
		return;
	for (uint32_t i = 0; i < pending->fence_count; i++)
		dma_fence_put(pending->fences[i]);
	kfree(pending);
}

static void
nvkm_drm_exec_pending_cancel_channel(struct nvkm_softc *sc,
    struct nvkm_gsp_chan *chan, int error)
{
	struct nvkm_drm_exec_pending *pending, *next;

	for (pending = LIST_FIRST(&sc->exec_pending); pending != NULL;
	    pending = next) {
		next = LIST_NEXT(pending, link);
		if (pending->chan != chan)
			continue;

		LIST_REMOVE(pending, link);
		nvkm_drm_exec_trace_complete(sc, pending, error);
		nvkm_drm_probe_pending_signal(sc, pending, error);
		nvkm_drm_exec_pending_signal(pending, error);
		atomic_store_rel_int(&pending->done, 1);
		wakeup(pending);
		nvkm_drm_submit_slot_release(chan, pending->post_slot);
		nvkm_drm_exec_pending_put(sc, pending);
	}
}

#define NVKM_DRM_FAULT_DATA_SCAN_MAX_SIZE	(16ULL * 1024ULL * 1024ULL)

static void
nvkm_drm_fault_scan_pushes(struct nvkm_softc *sc,
    struct nvkm_drm_exec_pending *pending, uint64_t fault_addr)
{
	uint32_t fault_lo = (uint32_t)fault_addr;
	uint32_t fault_hi = (uint32_t)(fault_addr >> 32);

	if (pending->nfile == NULL)
		return;

	for (uint32_t n = 0; n < pending->trace_count; n++) {
		struct nvkm_drm_exec_trace *trace;
		struct nvkm_drm_vm_binding *binding = NULL;
		struct nvkm_bo *bo;
		uint64_t binding_offset = 0;
		uint64_t offset;
		uint32_t count;

		trace = &sc->exec_trace[(pending->trace_first + n) %
		    NVKM_DRM_EXEC_TRACE_COUNT];
		if (trace->seq == 0)
			continue;
		LIST_FOREACH(binding, &pending->nfile->vm_bindings, link) {
			if (!nvkm_drm_gpu_va_fits_binding(binding, trace->va,
			    trace->va_len, &binding_offset))
				continue;
			break;
		}
		if (binding == NULL)
			continue;

		bo = to_nvkm_bo(binding->obj);
		if (!nvkm_bo_has_sysmem(bo))
			continue;
		if (binding->bo_offset > bo->base.size ||
		    binding_offset > bo->base.size - binding->bo_offset ||
		    trace->va_len >
		    bo->base.size - binding->bo_offset - binding_offset)
			continue;

		offset = binding->bo_offset + binding_offset;
		count = trace->va_len / sizeof(uint32_t);
		sc->rc_fault_push_scan_count++;
		for (uint32_t i = 0; i + 1 < count; i++) {
			uint32_t lo;
			uint32_t hi;
			uint64_t value;

			if (nvkm_bo_read32(bo, offset +
			    (uint64_t)i * sizeof(uint32_t), &lo) != 0 ||
			    nvkm_bo_read32(bo, offset +
			    (uint64_t)(i + 1) * sizeof(uint32_t), &hi) != 0)
				break;
			value = (uint64_t)lo | ((uint64_t)hi << 32);

			if (value != fault_addr &&
			    !(lo == fault_lo && (hi & 0xffu) == fault_hi))
				continue;
			sc->rc_fault_push_hit_count++;
			if (sc->rc_fault_push_hit_seq == 0) {
				sc->rc_fault_push_hit_seq = trace->seq;
				sc->rc_fault_push_hit_va = trace->va;
				sc->rc_fault_push_hit_dword = i;
			}
		}
	}
}

void
nvkm_drm_exec_fault_channel_locked(struct nvkm_softc *sc, uint32_t chid,
    int error, uint64_t fault_addr)
{
	struct nvkm_drm_exec_pending *pending, *next;

	sc->rc_fault_pending_count = 0;
	sc->rc_fault_binding_count = 0;
	sc->rc_fault_binding_addr = 0;
	sc->rc_fault_binding_size = 0;
	sc->rc_fault_binding_grefcnt = 0;
	sc->rc_fault_nearest_lo_addr = 0;
	sc->rc_fault_nearest_lo_size = 0;
	sc->rc_fault_nearest_hi_addr = 0;
	sc->rc_fault_nearest_hi_size = 0;
	sc->rc_fault_push_scan_count = 0;
	sc->rc_fault_push_hit_count = 0;
	sc->rc_fault_push_hit_seq = 0;
	sc->rc_fault_push_hit_va = 0;
	sc->rc_fault_push_hit_dword = 0;
	sc->rc_fault_data_scan_count = 0;
	sc->rc_fault_data_hit_count = 0;
	sc->rc_fault_data_hit_addr = 0;
	sc->rc_fault_data_hit_size = 0;
	sc->rc_fault_data_hit_offset = 0;
	sc->rc_fault_data_hit_value = 0;

	for (pending = LIST_FIRST(&sc->exec_pending); pending != NULL;
	    pending = next) {
		next = LIST_NEXT(pending, link);
		if (pending->chan == NULL ||
		    (uint32_t)pending->chan->chid != chid)
			continue;

		sc->rc_fault_pending_count++;
		nvkm_drm_fault_scan_pushes(sc, pending, fault_addr);

		pending->chan->faulted = 1;
		pending->chan->fault_error = error;
		LIST_REMOVE(pending, link);
		nvkm_drm_exec_trace_complete(sc, pending, error);
		nvkm_drm_probe_pending_signal(sc, pending, error);
		nvkm_drm_exec_pending_signal(pending, error);
		sc->exec_async_wait_error_count++;
		atomic_store_rel_int(&pending->done, 1);
		wakeup(pending);
		nvkm_drm_submit_slot_release(pending->chan, pending->post_slot);
		nvkm_drm_exec_pending_put(sc, pending);
	}
}

void
nvkm_drm_exec_complete_intr(struct nvkm_softc *sc)
{
	struct nvkm_drm_exec_pending *pending, *next;

	lwkt_gettoken(&sc->gsp_tok);
	for (pending = LIST_FIRST(&sc->exec_pending); pending != NULL;
	    pending = next) {
		next = LIST_NEXT(pending, link);
		if (*(pending->sema) != pending->payload)
			continue;

		LIST_REMOVE(pending, link);
		nvkm_drm_exec_trace_complete(sc, pending, 0);
		nvkm_drm_probe_pending_signal(sc, pending, 0);
		nvkm_drm_exec_pending_signal(pending, 0);
		sc->exec_async_complete_count++;
		atomic_store_rel_int(&pending->done, 1);
		wakeup(pending);
		nvkm_drm_submit_slot_release(pending->chan, pending->post_slot);
		nvkm_drm_exec_pending_put(sc, pending);
	}
	lwkt_reltoken(&sc->gsp_tok);
}

static int
nvkm_drm_prepare_signal_syncobjs(struct nvkm_softc *sc,
    struct drm_file *file_priv, uint32_t count, uint64_t sig_ptr,
    struct dma_fence *done_fence, struct nvkm_drm_exec_signal **psignals)
{
	struct drm_nouveau_sync *sigs;
	struct nvkm_drm_exec_signal *signals;
	int err = 0;

	*psignals = NULL;
	if (count == 0)
		return (0);
	if (done_fence == NULL)
		return (-EINVAL);
	sc->sync_signal_count += count;
	if (count > 64) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal invalid count=%u\n", count);
		return (-EINVAL);
	}

	sigs = kmalloc_array(count, sizeof(*sigs), GFP_KERNEL);
	signals = kcalloc(count, sizeof(*signals), GFP_KERNEL);
	if (sigs == NULL || signals == NULL) {
		err = -ENOMEM;
		goto out_free_arrays;
	}

	err = copyin((const void *)(uintptr_t)sig_ptr, sigs,
	    sizeof(*sigs) * count);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal copyin failed count=%u err=%d\n",
		    count, err);
		err = -EFAULT;
		goto out_free_arrays;
	}

	for (uint32_t i = 0; i < count; i++) {
		uint32_t type = sigs[i].flags & DRM_NOUVEAU_SYNC_TYPE_MASK;

		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal prepare idx=%u flags=0x%08x handle=%u timeline=0x%016jx\n",
		    i, sigs[i].flags, sigs[i].handle,
		    (uintmax_t)sigs[i].timeline_value);
		if (type != DRM_NOUVEAU_SYNC_SYNCOBJ &&
		    type != DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync signal unsupported flags idx=%u flags=0x%08x\n",
			    i, sigs[i].flags);
			err = -EINVAL;
			goto out_free_arrays;
		}
		signals[i].syncobj = drm_syncobj_find(file_priv,
		    sigs[i].handle);
		if (signals[i].syncobj == NULL) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync signal missing syncobj idx=%u handle=%u\n",
			    i, sigs[i].handle);
			err = -ENOENT;
			goto out_free_arrays;
		}
		signals[i].type = type;
		signals[i].timeline_value = sigs[i].timeline_value;
		signals[i].fence = dma_fence_get(done_fence);
		if (type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			signals[i].chain = dma_fence_chain_alloc();
			if (signals[i].chain == NULL) {
				err = -ENOMEM;
				goto out_free_arrays;
			}
		}
	}

	*psignals = signals;
	signals = NULL;

out_free_arrays:
	nvkm_drm_exec_signals_put(signals, count);
	kfree(sigs);
	if (err != 0) {
		sc->sync_signal_error_count++;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal prepare failed count=%u err=%d\n",
		    count, err);
	}
	return (err);
}

enum nvkm_drm_job_type {
	NVKM_DRM_JOB_EXEC,
	NVKM_DRM_JOB_VM_BIND,
};

struct nvkm_drm_job_dep {
	struct dma_fence_cb cb;
	struct nvkm_drm_job *job;
	struct dma_fence *fence;
	bool armed;
};

struct nvkm_drm_job {
	TAILQ_ENTRY(nvkm_drm_job) link;
	struct kref refcount;
	enum nvkm_drm_job_type type;
	struct nvkm_softc *sc;
	struct drm_file *file_priv;
	struct nvkm_drm_file *nfile;
	struct dma_fence *done_fence;
	struct dma_fence **wait_fences;
	struct nvkm_drm_job_dep *wait_deps;
	int result;
	uint32_t wait_count;
	uint32_t dep_pending;
	bool completed;
	bool deps_armed;
	bool queued;
	bool running;
	union {
		struct {
			uint32_t channel;
			uint32_t push_count;
			uint32_t sig_count;
			struct drm_nouveau_exec_push *pushes;
			struct nvkm_drm_exec_signal *signals;
		} exec;
		struct {
			uint32_t op_count;
			uint32_t sig_count;
			struct drm_nouveau_vm_bind_op *ops;
			struct drm_gem_object **objects;
			struct nvkm_drm_exec_signal *signals;
		} vm_bind;
	};
};

static int nvkm_drm_exec_submit(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    uint32_t channel_id, struct drm_nouveau_exec_push *pushes,
    uint32_t push_count, struct dma_fence *done_fence,
    struct nvkm_drm_exec_signal *signals, uint32_t sig_count);

static int
nvkm_drm_job_queue_work_locked(struct nvkm_drm_file *nfile)
{
	if (nfile->job_work_queued)
		return (0);
	nfile->job_work_queued = true;
	if (!queue_work(system_wq, &nfile->job_work)) {
		nfile->job_work_queued = false;
		return (-EIO);
	}
	return (0);
}

static void
nvkm_drm_job_wakeup_locked(struct nvkm_drm_file *nfile)
{
	nfile->job_epoch++;
	wakeup(&nfile->job_epoch);
}

static void
nvkm_drm_job_signal(struct nvkm_drm_job *job, int error)
{
	if (job->done_fence != NULL) {
		job->sc->job_done_signal_count++;
		if (error != 0)
			job->sc->job_done_signal_error_count++;
		if (nvkm_drm_fence_flag_signaled(job->done_fence))
			job->sc->job_done_signal_already_signaled_count++;
		if (nvkm_drm_exec_fence_hw_ready(job->done_fence))
			job->sc->job_done_signal_hw_ready_count++;
		if (error != 0)
			nvkm_drm_fence_set_error(job->done_fence, error);
		(void)dma_fence_signal(job->done_fence);
		return;
	}

	switch (job->type) {
	case NVKM_DRM_JOB_EXEC:
		nvkm_drm_exec_signals_signal(job->exec.signals,
		    job->exec.sig_count, error);
		break;
	case NVKM_DRM_JOB_VM_BIND:
		nvkm_drm_exec_signals_signal(job->vm_bind.signals,
		    job->vm_bind.sig_count, error);
		break;
	}
}

static void
nvkm_drm_job_release(struct kref *kref)
{
	struct nvkm_drm_job *job =
	    container_of(kref, struct nvkm_drm_job, refcount);

	if (job == NULL)
		return;
	dma_fence_put(job->done_fence);
	nvkm_drm_wait_fences_put(job->wait_fences, job->wait_count);
	kfree(job->wait_deps);
	switch (job->type) {
	case NVKM_DRM_JOB_EXEC:
		nvkm_drm_exec_signals_put(job->exec.signals,
		    job->exec.sig_count);
		kfree(job->exec.pushes);
		break;
	case NVKM_DRM_JOB_VM_BIND:
		nvkm_drm_exec_signals_put(job->vm_bind.signals,
		    job->vm_bind.sig_count);
		nvkm_drm_vm_bind_objects_put(job->vm_bind.objects,
		    job->vm_bind.op_count);
		kfree(job->vm_bind.ops);
		break;
	}
	kfree(job);
}

static void
nvkm_drm_job_get(struct nvkm_drm_job *job)
{
	kref_get(&job->refcount);
}

static void
nvkm_drm_job_put(struct nvkm_drm_job *job)
{
	if (job != NULL)
		kref_put(&job->refcount, nvkm_drm_job_release);
}

static int
nvkm_drm_job_prepare_deps(struct nvkm_drm_job *job)
{
	if (job->wait_count == 0)
		return (0);
	job->wait_deps = kcalloc(job->wait_count, sizeof(*job->wait_deps),
	    GFP_KERNEL);
	if (job->wait_deps == NULL)
		return (-ENOMEM);
	for (uint32_t i = 0; i < job->wait_count; i++) {
		job->wait_deps[i].job = job;
		job->wait_deps[i].fence = job->wait_fences[i];
	}
	return (0);
}

static void
nvkm_drm_job_dep_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct nvkm_drm_job_dep *dep =
	    container_of(cb, struct nvkm_drm_job_dep, cb);
	struct nvkm_drm_job *job = dep->job;
	struct nvkm_drm_file *nfile = job->nfile;
	struct nvkm_softc *sc = job->sc;
	int err;

	(void)fence;
	sc->sync_job_dep_cb_count++;
	lwkt_gettoken(&nfile->job_token);
	if (dep->armed) {
		dep->armed = false;
		if (job->dep_pending != 0)
			job->dep_pending--;
		if (job->dep_pending == 0 && job->queued &&
		    !nfile->job_closing) {
			err = nvkm_drm_job_queue_work_locked(nfile);
			if (err == 0)
				sc->sync_job_dep_queue_count++;
			else
				sc->sync_job_dep_queue_error_count++;
		}
		nvkm_drm_job_wakeup_locked(nfile);
		wakeup(job);
	}
	lwkt_reltoken(&nfile->job_token);

	nvkm_drm_job_put(job);
}

static int
nvkm_drm_job_arm_deps_locked(struct nvkm_drm_job *job)
{
	struct nvkm_softc *sc = job->sc;
	int err;

	if (job->deps_armed)
		return (0);
	job->deps_armed = true;

	for (uint32_t i = 0; i < job->wait_count; i++) {
		struct nvkm_drm_job_dep *dep = &job->wait_deps[i];
		struct dma_fence *fence = dep->fence;

		if (fence == NULL || dma_fence_is_signaled(fence))
			continue;
		sc->sync_wait_blocking_count++;
		dep->armed = true;
		job->dep_pending++;
		nvkm_drm_job_get(job);
		err = dma_fence_add_callback(fence, &dep->cb,
		    nvkm_drm_job_dep_cb);
		if (err == 0) {
			sc->sync_job_wait_armed_count++;
			continue;
		}

		dep->armed = false;
		if (job->dep_pending != 0)
			job->dep_pending--;
		nvkm_drm_job_put(job);
		if (err == -ENOENT)
			continue;
		sc->sync_wait_error_count++;
		return (err);
	}

	return (0);
}

static void
nvkm_drm_job_disarm_deps_locked(struct nvkm_drm_job *job)
{
	for (uint32_t i = 0; i < job->wait_count; i++) {
		struct nvkm_drm_job_dep *dep = &job->wait_deps[i];

		if (!dep->armed)
			continue;
		if (dma_fence_remove_callback(dep->fence, &dep->cb)) {
			dep->armed = false;
			if (job->dep_pending != 0)
				job->dep_pending--;
			nvkm_drm_job_put(job);
		}
	}
}

static bool
nvkm_drm_job_has_armed_deps_locked(struct nvkm_drm_job *job)
{
	for (uint32_t i = 0; i < job->wait_count; i++) {
		if (job->wait_deps[i].armed)
			return (true);
	}
	return (false);
}

static void
nvkm_drm_job_wait_deps_disarmed(struct nvkm_drm_job *job)
{
	struct nvkm_drm_file *nfile = job->nfile;

	for (;;) {
		lwkt_gettoken(&nfile->job_token);
		if (!nvkm_drm_job_has_armed_deps_locked(job)) {
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		lwkt_reltoken(&nfile->job_token);
		(void)tsleep(job, 0, "nvkjcb", hz / 10);
	}
}

static int
nvkm_drm_job_enqueue(struct nvkm_drm_job *job)
{
	struct nvkm_drm_file *nfile = job->nfile;
	int err = 0;

	lwkt_gettoken(&nfile->job_token);
	if (nfile->job_closing) {
		err = -ENODEV;
	} else {
		TAILQ_INSERT_TAIL(&nfile->job_queue, job, link);
		job->queued = true;
		err = nvkm_drm_job_queue_work_locked(nfile);
		if (err != 0) {
			job->queued = false;
			TAILQ_REMOVE(&nfile->job_queue, job, link);
		}
	}
	lwkt_reltoken(&nfile->job_token);
	return (err);
}

static int
nvkm_drm_job_wait_complete(struct nvkm_drm_job *job)
{
	struct nvkm_drm_file *nfile = job->nfile;
	int result;

	for (;;) {
		lwkt_gettoken(&nfile->job_token);
		if (job->completed) {
			result = job->result;
			lwkt_reltoken(&nfile->job_token);
			return (result);
		}
		if (!job->running && !nfile->job_work_queued &&
		    (!job->queued || job->dep_pending == 0))
			(void)nvkm_drm_job_queue_work_locked(nfile);
		lwkt_reltoken(&nfile->job_token);
		(void)tsleep(job, 0, "nvkjsy", hz / 10);
	}
}

static int
nvkm_drm_job_run(struct nvkm_drm_job *job)
{
	int err;

	switch (job->type) {
	case NVKM_DRM_JOB_EXEC:
		err = nvkm_drm_exec_submit(job->sc, job->file_priv,
		    job->nfile, job->exec.channel, job->exec.pushes,
		    job->exec.push_count, job->done_fence, job->exec.signals,
		    job->exec.sig_count);
		break;
	case NVKM_DRM_JOB_VM_BIND:
		if (job->vm_bind.op_count != 0) {
			err = nvkm_drm_vm_bind_apply(job->sc, job->file_priv,
			    job->nfile, job->vm_bind.ops,
			    job->vm_bind.op_count, job->vm_bind.objects);
		} else {
			err = 0;
		}
		nvkm_drm_job_signal(job, err);
		break;
	}
	return (err);
}

static void
nvkm_drm_job_work(struct work_struct *work)
{
	struct nvkm_drm_file *nfile =
	    container_of(work, struct nvkm_drm_file, job_work);
	struct nvkm_drm_job *job;
	int err;

	for (;;) {
		lwkt_gettoken(&nfile->job_token);
		if (nfile->job_closing) {
			nfile->job_work_queued = false;
			nvkm_drm_job_wakeup_locked(nfile);
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		job = TAILQ_FIRST(&nfile->job_queue);
		if (job == NULL) {
			nfile->job_work_queued = false;
			nvkm_drm_job_wakeup_locked(nfile);
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		err = nvkm_drm_job_arm_deps_locked(job);
		if (err != 0) {
			TAILQ_REMOVE(&nfile->job_queue, job, link);
			job->queued = false;
			job->result = err;
			job->completed = true;
			nvkm_drm_job_disarm_deps_locked(job);
			nvkm_drm_job_wakeup_locked(nfile);
			wakeup(job);
			lwkt_reltoken(&nfile->job_token);

			nvkm_drm_job_signal(job, err);
			nvkm_drm_job_put(job);
			continue;
		}
		if (job->dep_pending != 0) {
			nfile->job_work_queued = false;
			nvkm_drm_job_wakeup_locked(nfile);
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		TAILQ_REMOVE(&nfile->job_queue, job, link);
		job->queued = false;
		job->running = true;
		job->sc->sync_job_ready_count++;
		lwkt_reltoken(&nfile->job_token);

		err = nvkm_drm_job_run(job);
		lwkt_gettoken(&nfile->job_token);
		job->running = false;
		job->result = err;
		job->completed = true;
		nvkm_drm_job_wakeup_locked(nfile);
		wakeup(job);
		lwkt_reltoken(&nfile->job_token);
		nvkm_drm_job_put(job);
	}
}

static void
nvkm_drm_jobs_flush(struct nvkm_drm_file *nfile)
{
	struct nvkm_drm_job *job;

	if (nfile == NULL)
		return;
	for (;;) {
		flush_work(&nfile->job_work);
		lwkt_gettoken(&nfile->job_token);
		if (TAILQ_EMPTY(&nfile->job_queue)) {
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		job = TAILQ_FIRST(&nfile->job_queue);
		if (job != NULL &&
		    (!job->deps_armed || job->dep_pending == 0))
			(void)nvkm_drm_job_queue_work_locked(nfile);
		lwkt_reltoken(&nfile->job_token);
		(void)tsleep(&nfile->job_epoch, 0, "nvkjfl", hz / 10);
	}
}

static void
nvkm_drm_jobs_close(struct nvkm_drm_file *nfile)
{
	struct nvkm_drm_job *job;

	if (nfile == NULL)
		return;

	for (;;) {
		lwkt_gettoken(&nfile->job_token);
		nfile->job_closing = true;
		job = TAILQ_FIRST(&nfile->job_queue);
		if (job == NULL) {
			nvkm_drm_job_wakeup_locked(nfile);
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		TAILQ_REMOVE(&nfile->job_queue, job, link);
		job->queued = false;
		job->result = -ECANCELED;
		job->completed = true;
		nvkm_drm_job_disarm_deps_locked(job);
		nvkm_drm_job_wakeup_locked(nfile);
		wakeup(job);
		lwkt_reltoken(&nfile->job_token);

		nvkm_drm_job_wait_deps_disarmed(job);
		job->sc->sync_job_cancel_count++;
		nvkm_drm_job_signal(job, -ECANCELED);
		nvkm_drm_job_put(job);
	}
	flush_work(&nfile->job_work);
}

static int
nvkm_drm_queue_vm_bind_job(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    const struct drm_nouveau_vm_bind *req,
    struct drm_nouveau_vm_bind_op *ops, bool sync)
{
	struct nvkm_drm_job *job;
	bool published = false;
	bool sync_ref_taken = false;
	int err;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (job == NULL) {
		kfree(ops);
		return (-ENOMEM);
	}
	kref_init(&job->refcount);
	job->type = NVKM_DRM_JOB_VM_BIND;
	job->sc = sc;
	job->file_priv = file_priv;
	job->nfile = nfile;
	job->vm_bind.op_count = req->op_count;
	job->vm_bind.sig_count = req->sig_count;
	job->vm_bind.ops = ops;
	job->done_fence = nvkm_drm_exec_fence_create(sc, ++sc->fence_seqno);
	if (job->done_fence == NULL) {
		err = -ENOMEM;
		goto fail;
	}

	err = nvkm_drm_vm_bind_prepare_objects(file_priv, ops, req->op_count,
	    &job->vm_bind.objects);
	if (err != 0)
		goto fail;

	err = nvkm_drm_collect_wait_syncobjs(sc, file_priv, req->wait_count,
	    req->wait_ptr, &job->wait_fences, &job->wait_count);
	if (err != 0)
		goto fail;
	err = nvkm_drm_job_prepare_deps(job);
	if (err != 0)
		goto fail;

	err = nvkm_drm_prepare_signal_syncobjs(sc, file_priv, req->sig_count,
	    req->sig_ptr, job->done_fence, &job->vm_bind.signals);
	if (err != 0)
		goto fail;

	lockmgr(&nfile->job_submit_lock, LK_EXCLUSIVE);
	err = nvkm_drm_vm_bind_attach_resv_fence(sc, nfile, ops,
	    req->op_count, job->vm_bind.objects, job->done_fence);
	if (err == 0) {
		nvkm_drm_exec_signals_publish(job->vm_bind.signals,
		    job->vm_bind.sig_count);
		published = true;
		if (sync) {
			nvkm_drm_job_get(job);
			sync_ref_taken = true;
		}
		err = nvkm_drm_job_enqueue(job);
	}
	lockmgr(&nfile->job_submit_lock, LK_RELEASE);
	if (err != 0) {
		if (published)
			goto fail_signal;
		goto fail;
	}
	if (sync) {
		err = nvkm_drm_job_wait_complete(job);
		nvkm_drm_job_put(job);
	}
	return (err);

fail_signal:
	nvkm_drm_job_signal(job, err);
	if (sync_ref_taken)
		nvkm_drm_job_put(job);
fail:
	nvkm_drm_job_put(job);
	return (err);
}

static struct nvkm_drm_vm_binding *
nvkm_drm_vm_binding_find_overlap(struct nvkm_drm_file *nfile, uint64_t addr,
    uint64_t size)
{
	struct nvkm_drm_vm_binding *binding;

	LIST_FOREACH(binding, &nfile->vm_bindings, link) {
		if (binding->addr >= addr + size)
			break;
		if (binding->addr + binding->size <= addr)
			continue;
		if (nvkm_drm_vm_ranges_overlap(addr, size, binding->addr,
		    binding->size))
			return (binding);
	}
	return (NULL);
}

static bool
nvkm_drm_gem_object_array_has(struct drm_gem_object **objects,
    uint32_t count, struct drm_gem_object *obj)
{
	for (uint32_t i = 0; i < count; i++) {
		if (objects[i] == obj)
			return (true);
	}
	return (false);
}

static void
nvkm_drm_gem_object_array_put(struct drm_gem_object **objects,
    uint32_t count)
{
	if (objects == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		if (objects[i] != NULL)
			drm_gem_object_put_unlocked(objects[i]);
	}
	kfree(objects);
}

static int
nvkm_drm_gem_object_array_append(struct drm_gem_object ***pobjects,
    uint32_t *pcount, uint32_t *pcapacity, struct drm_gem_object *obj)
{
	struct drm_gem_object **objects = *pobjects;
	uint32_t count = *pcount;
	uint32_t capacity = *pcapacity;

	if (obj == NULL)
		return (0);
	if (nvkm_drm_gem_object_array_has(objects, count, obj))
		return (0);
	if (count == capacity) {
		uint32_t new_capacity = capacity != 0 ? capacity * 2 : 8;
		struct drm_gem_object **new_objects;

		new_objects = krealloc(objects,
		    sizeof(*new_objects) * new_capacity, M_DRM, GFP_KERNEL);
		if (new_objects == NULL)
			return (-ENOMEM);
		objects = new_objects;
		capacity = new_capacity;
	}
	drm_gem_object_get(obj);
	objects[count++] = obj;
	*pobjects = objects;
	*pcount = count;
	*pcapacity = capacity;
	return (0);
}

/*
 * nvkm_drm_vm_bind_snapshot_resv_objects()
 *
 * Ownership:
 *   On success, returns a newly allocated array whose entries each hold one
 *   GEM reference.  The caller owns the array and every entry reference and
 *   must release them with nvkm_drm_gem_object_array_put().  The objects
 *   argument is borrowed; this helper takes independent references for entries
 *   it returns.
 *
 * Lifetime:
 *   The snapshot covers the BOs whose GPUVA bookkeeping can be changed by the
 *   VM_BIND job: explicit MAP objects plus currently active bindings
 *   overlapped by any MAP or UNMAP op.  The references keep those BOs alive
 *   after nfile->vm_token is released.
 *
 * Threading:
 *   Takes nfile->vm_token only while walking the binding list.  It never locks
 *   BO reservation objects while holding vm_token; reservation fences are
 *   attached only after the snapshot owns stable GEM references.
 */
static int
nvkm_drm_vm_bind_snapshot_resv_objects(struct nvkm_drm_file *nfile,
    struct drm_nouveau_vm_bind_op *ops, uint32_t op_count,
    struct drm_gem_object **objects, struct drm_gem_object ***pobjects,
    uint32_t *pobject_count)
{
	*pobjects = NULL;
	*pobject_count = 0;

	for (;;) {
		struct drm_gem_object **resv_objects = NULL;
		struct nvkm_drm_vm_binding *binding;
		uint32_t binding_count = 0;
		uint32_t object_count = 0;
		uint32_t object_capacity;
		uint32_t seen_count = 0;
		bool retry = false;
		int err = 0;

		lwkt_gettoken(&nfile->vm_token);
		LIST_FOREACH(binding, &nfile->vm_bindings, link)
			binding_count++;
		lwkt_reltoken(&nfile->vm_token);

		object_capacity = op_count + binding_count;
		if (object_capacity != 0) {
			resv_objects = kcalloc(object_capacity,
			    sizeof(*resv_objects), GFP_KERNEL);
			if (resv_objects == NULL)
				return (-ENOMEM);
		}

		for (uint32_t i = 0; i < op_count; i++) {
			if (objects != NULL && objects[i] != NULL) {
				err = nvkm_drm_gem_object_array_append(&resv_objects,
				    &object_count, &object_capacity, objects[i]);
				if (err != 0)
					goto fail;
			}
		}

		lwkt_gettoken(&nfile->vm_token);
		LIST_FOREACH(binding, &nfile->vm_bindings, link) {
			if (seen_count == binding_count) {
				retry = true;
				break;
			}
			seen_count++;
			for (uint32_t i = 0; i < op_count; i++) {
				struct drm_nouveau_vm_bind_op *op = &ops[i];

				if (binding->addr >= op->addr + op->range ||
				    binding->addr + binding->size <= op->addr)
					continue;
				if (!nvkm_drm_vm_ranges_overlap(op->addr, op->range,
				    binding->addr, binding->size))
					continue;
				err = nvkm_drm_gem_object_array_append(&resv_objects,
				    &object_count, &object_capacity, binding->obj);
				break;
			}
			if (err != 0)
				break;
		}
		lwkt_reltoken(&nfile->vm_token);
		if (err != 0)
			goto fail;
		if (retry) {
			nvkm_drm_gem_object_array_put(resv_objects, object_count);
			continue;
		}

		*pobjects = resv_objects;
		*pobject_count = object_count;
		return (0);

fail:
		nvkm_drm_gem_object_array_put(resv_objects, object_count);
		return (err);
	}
}

/*
 * nvkm_drm_exec_attach_resv_fence()
 *
 * Ownership:
 *   Borrows fence and nfile. nfile->vm_resv takes its own fence reference
 *   through reservation_object_add_excl_fence(); the caller keeps ownership
 *   of its existing reference.
 *
 * Lifetime:
 *   Must be called after the EXEC done fence exists and before the job can
 *   run. It models nouveau drm_gpuvm_exec reservation attachment at the
 *   per-file GPUVM boundary. The fence is deliberately not attached to every
 *   mapped BO because DragonFly's current reservation_object cannot encode
 *   dma_resv usage classes; publishing a VM-wide EXEC fence through BO
 *   reservation objects would make dma-buf import/export report unrelated
 *   GPUVM work as buffer-content synchronization.
 *
 * Threading:
 *   May sleep while locking vm_resv. It does not hold nfile->vm_token or
 *   sc->gsp_tok, and it never locks BO reservation objects.
 */
static int
nvkm_drm_exec_attach_resv_fence(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct dma_fence *fence)
{
	uint64_t profile_start;

	if (fence == NULL)
		return (-EINVAL);

	if (nvkm_drm_fence_flag_signaled(fence))
		sc->exec_resv_attach_signaled_count++;
	else
		sc->exec_resv_attach_pending_count++;
	profile_start = nvkm_drm_profile_now_us();
	reservation_object_lock(&nfile->vm_resv, NULL);
	reservation_object_add_excl_fence(&nfile->vm_resv, fence);
	reservation_object_unlock(&nfile->vm_resv);
	sc->exec_resv_attach_calls++;
	nvkm_drm_profile_add_us(&sc->exec_profile_attach_resv_us,
	    profile_start);
	return (0);
}

/*
 * nvkm_drm_vm_bind_attach_resv_fence()
 *
 * Ownership:
 *   Borrows ops, objects, nfile, and fence.  Each BO reservation object takes
 *   its own fence reference through nvkm_bo_resv_add_shared_fence().
 *
 * Lifetime:
 *   Must be called after the VM_BIND done fence is visible to out syncobjs and
 *   before the job can run.  This mirrors nouveau BOOKKEEP reservation usage
 *   for VM_BIND: the fence protects GPUVA bookkeeping for the specific BOs the
 *   bind operation maps, unmaps, or replaces.
 *
 * Threading:
 *   May sleep while snapshotting affected VM_BIND objects and while locking BO
 *   reservation objects.  It does not hold nfile->vm_token across reservation
 *   locks.
 */
static int
nvkm_drm_vm_bind_attach_resv_fence(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct drm_nouveau_vm_bind_op *ops,
    uint32_t op_count, struct drm_gem_object **objects,
    struct dma_fence *fence)
{
	struct drm_gem_object **resv_objects;
	uint32_t object_count;
	uint64_t profile_start;
	int err;

	if (fence == NULL)
		return (-EINVAL);

	if (nvkm_drm_fence_flag_signaled(fence))
		sc->vm_bind_resv_attach_signaled_count++;
	else
		sc->vm_bind_resv_attach_pending_count++;
	profile_start = nvkm_drm_profile_now_us();
	err = nvkm_drm_vm_bind_snapshot_resv_objects(nfile, ops, op_count,
	    objects, &resv_objects, &object_count);
	if (err != 0)
		return (err);

	for (uint32_t i = 0; i < object_count; i++) {
		err = nvkm_bo_resv_add_shared_fence(to_nvkm_bo(resv_objects[i]),
		    fence);
		if (err != 0)
			break;
	}
	nvkm_drm_gem_object_array_put(resv_objects, object_count);
	nvkm_drm_profile_add_us(&sc->exec_profile_attach_resv_us,
	    profile_start);
	return (err);
}

static int
nvkm_drm_exec_submit(struct nvkm_softc *sc, struct drm_file *file_priv,
    struct nvkm_drm_file *nfile, uint32_t channel_id,
    struct drm_nouveau_exec_push *pushes, uint32_t push_count,
    struct dma_fence *done_fence, struct nvkm_drm_exec_signal *signals,
    uint32_t sig_count)
{
	struct drm_nouveau_exec req_storage;
	struct drm_nouveau_exec *req = &req_storage;
	struct nvkm_drm_chan *dchan;
	struct nvkm_gsp_chan *chan;
	struct nvkm_drm_exec_pending *pending = NULL;
	struct dma_fence *exec_fence = NULL;
	uint32_t *gpf, *post, *sema;
	uint64_t slot_bar1, post_gva, sema_gva;
	uint32_t put = 0, payload, post_slot, post_offset, sema_offset;
	uint32_t gpf_required = 0;
	uint64_t trace_seq;
	uint32_t trace_first = 0;
	uint32_t trace_count = 0;
	uint64_t profile_start;
	uint64_t profile_push_start = 0;
	int err = 0;
	bool gsp_tok_held = false;
	bool exec_completion_queued = false;
	bool submit_slot_allocated = false;
	uint64_t push_va_lo = ~0ULL;
	uint64_t push_va_hi = 0;

	(void)file_priv;
	memset(&req_storage, 0, sizeof(req_storage));
	req->channel = channel_id;
	req->push_count = push_count;
	req->sig_count = sig_count;

	if (nfile == NULL)
		return (-ENXIO);
	sc->exec_submit_count++;
	dchan = nvkm_drm_channel_find(nfile, req->channel);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC begin channel=%u pushes=%u waits=%u sigs=%u\n",
	    req->channel, req->push_count, req->wait_count, req->sig_count);
	if (dchan == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC missing channel=%u\n", req->channel);
		return (-ENOENT);
	}
	chan = dchan->chan;
	if (chan == NULL || chan->submit_gpf.kva == NULL ||
	    chan->submit_push.kva == NULL || chan->submit_sema.kva == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC channel=%u missing submit buffers chan=%p\n",
		    req->channel, chan);
		return (-ENXIO);
	}
	/*
	 * Truly empty submit (no pushes, no signals): nothing to order or signal
	 * (waits, if any, are handled by the queued job before this helper runs).
	 * NVK uses this as a device-loss error probe after QueueWaitIdle.
	 * Signal-only submits (push_count == 0
	 * but sig_count > 0) deliberately fall through to the normal path below so
	 * their signals are ordered behind the channel's in-flight work via the
	 * completion trailer, matching nouveau (every EXEC, even push.count == 0,
	 * is a scheduler job whose out-fences signal on the ordered done_fence).
	 * CPU-signaling them here would let NVK observe completion early and reuse
	 * a command-pool chunk the GPU is still reading.
	 */
	if (req->push_count == 0 && req->sig_count == 0) {
		sc->exec_signal_only_count++;
		if (chan->faulted)
			return (chan->fault_error != 0 ? chan->fault_error : -EIO);
		return (0);
	}
	if (req->push_count > NVKM_DRM_GPFIFO_ENTRIES - 2) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC too many pushes channel=%u pushes=%u\n",
		    req->channel, req->push_count);
		return (-EINVAL);
	}

	/* push_count may be 0 here for a signal-only submit; pushes stays NULL
	 * and the push loop below runs zero times, leaving only the completion
	 * trailer (which orders the signals behind in-flight channel work). */
	/* Block only against an in-progress page-table mutation. Once the
	 * doorbell is written, VM_BIND no longer waits for this submit; userspace
	 * syncobjs describe the GPU ordering, matching nouveau's VM_BIND UAPI.
	 */
	lwkt_gettoken(&nfile->vm_token);
	lwkt_reltoken(&nfile->vm_token);

	profile_start = nvkm_drm_profile_now_us();
	lwkt_gettoken(&sc->gsp_tok);
	gsp_tok_held = true;
	nvkm_drm_profile_add_us(&sc->exec_profile_token_wait_us,
	    profile_start);
	profile_push_start = nvkm_drm_profile_now_us();
	gpf = (uint32_t *)chan->submit_gpf.kva;
	if (chan->faulted) {
		err = chan->fault_error != 0 ? chan->fault_error : -EIO;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC rejected faulted channel=%u chid=%d err=%d\n",
		    req->channel, chan->chid, err);
		goto out_unlock;
	}
	err = nvkm_drm_submit_slot_alloc(chan, &post_slot);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC no free post slots channel=%u busy=0x%016jx\n",
		    req->channel, (uintmax_t)chan->submit_post_slots_busy);
		goto out_unlock;
	}
	submit_slot_allocated = true;
	post_offset = post_slot * NVKM_DRM_POST_PUSH_DWORDS;
	sema_offset = post_slot;
	post = (uint32_t *)chan->submit_push.kva + post_offset;
	sema = (uint32_t *)chan->submit_sema.kva + sema_offset;
	post_gva = chan->submit_gva_push + (uint64_t)post_offset * 4;
	sema_gva = chan->submit_gva_sema + (uint64_t)sema_offset * 4;
	slot_bar1 = chan->userd_bar2_gva +
	    (uint64_t)((uint32_t)chan->chid % 8u) *
	    NV_USERD_SLOT_SIZE;

	err = nvkm_drm_gpfifo_wait_space(sc, chan, slot_bar1, req->push_count,
	    &put, &gpf_required);
	if (err != 0)
		goto out_unlock;
	payload = (uint32_t)(req->channel << 16) ^ put ^ req->push_count ^
	    0x51a70000u;
	sema[0] = 0;

	for (uint32_t i = 0; i < req->push_count; i++) {
		uint32_t entry0, entry1;

		if ((pushes[i].flags & ~DRM_NOUVEAU_EXEC_PUSH_FLAGS_KNOWN) != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC unknown push flags channel=%u push=%u flags=0x%08x\n",
			    req->channel, i, pushes[i].flags);
			err = -EINVAL;
			goto out_unlock;
		}
		if ((pushes[i].va | pushes[i].va_len) & 3 ||
		    pushes[i].va_len == 0 || pushes[i].va_len >= (1U << 23)) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC invalid push channel=%u idx=%u va=0x%016jx len=0x%08x flags=0x%08x\n",
			    req->channel, i, (uintmax_t)pushes[i].va,
			    pushes[i].va_len, pushes[i].flags);
			err = -EINVAL;
			goto out_unlock;
		}
		if (pushes[i].va < push_va_lo)
			push_va_lo = pushes[i].va;
		if (pushes[i].va + pushes[i].va_len > push_va_hi)
			push_va_hi = pushes[i].va + pushes[i].va_len;
		entry0 = (uint32_t)(pushes[i].va & 0xffffffffu);
		entry1 = (uint32_t)((pushes[i].va >> 32) & 0xffu) |
		    ((pushes[i].va_len / 4) << NVC06F_GP_ENTRY1_LENGTH_SHIFT);
		if ((pushes[i].flags & DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH) != 0)
			entry1 |= NVC06F_GP_ENTRY1_NO_PREFETCH;

		if ((put & (NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)) ==
		    NVKM_DRM_GPFIFO_FETCH_WINDOW - 1) {
			gpf[put * 2 + 0] = 0;
			gpf[put * 2 + 1] = 0;
			put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
		}
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC push channel=%u idx=%u va=0x%016jx len=0x%08x flags=0x%08x gpf[%u]=0x%08x:0x%08x\n",
		    req->channel, i, (uintmax_t)pushes[i].va, pushes[i].va_len,
		    pushes[i].flags, put, entry0, entry1);
		nvkm_drm_dump_large_push(sc, nfile, pushes[i].va,
		    pushes[i].va_len);
		if (nvkm_debug != 0) {
			if (trace_count == 0)
				trace_first = sc->exec_trace_next;
			trace_seq = sc->exec_submit_count;
			nvkm_drm_exec_trace_record(sc, trace_seq, req->channel,
			    chan->chid, post_slot, put, i, req->push_count,
			    &pushes[i]);
			trace_count++;
		}
		gpf[put * 2 + 0] = entry0;
		gpf[put * 2 + 1] = entry1;
		put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	}
	sc->exec_profile_pushes += req->push_count;

	post[0] = NVC36F_PUSH_HDR_SEM_ADDR_TRIPLET;
	post[1] = (uint32_t)(sema_gva & 0xffffffffu);
	post[2] = (uint32_t)((sema_gva >> 32) & 0xffu);
	post[3] = payload;
	post[4] = NVC36F_PUSH_HDR_SEM_EXECUTE;
	post[5] = NVC36F_SEM_EXECUTE_RELEASE |
	    NVC36F_SEM_EXECUTE_RELEASE_WFI;
	post[6] = NVC36F_PUSH_HDR_MEM_OP_A_TO_D;
	post[7] = 0;
	post[8] = 0;
	post[9] = NVC36F_MEM_OP_C_MEMBAR_SYS_MEMBAR;
	post[10] = NVC36F_MEM_OP_D_OPERATION_MEMBAR;
	post[11] = NVC36F_PUSH_HDR_NON_STALL_INTERRUPT;
	post[12] = 0;

	if ((put & (NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)) ==
	    NVKM_DRM_GPFIFO_FETCH_WINDOW - 1) {
		gpf[put * 2 + 0] = 0;
		gpf[put * 2 + 1] = 0;
		put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	}
	gpf[put * 2 + 0] = (uint32_t)(post_gva & 0xffffffffu);
	gpf[put * 2 + 1] = (uint32_t)((post_gva >> 32) & 0xffu) |
	    (NVKM_DRM_POST_PUSH_DWORDS << NVC06F_GP_ENTRY1_LENGTH_SHIFT);
	put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	nvkm_drm_profile_add_us(&sc->exec_profile_push_build_us,
	    profile_push_start);

	if (done_fence != NULL) {
		exec_fence = done_fence;
		if (signals != NULL && req->sig_count != 0)
			sc->exec_signal_fence_count++;
		else
			sc->exec_internal_fence_count++;
	} else {
		exec_fence = nvkm_drm_exec_fence_create(sc, ++sc->fence_seqno);
		if (exec_fence == NULL) {
			err = -ENOMEM;
			goto out_unlock;
		}
		sc->exec_internal_fence_count++;
	}
	pending = nvkm_drm_exec_pending_create(chan, (volatile uint32_t *)sema,
	    payload, post_slot, sc->exec_submit_count, trace_first,
	    trace_count, signals, req->sig_count, exec_fence);
	if (pending == NULL) {
		err = -ENOMEM;
		goto out_unlock;
	}
	nvkm_drm_exec_pending_arm_fences(pending);

	profile_start = nvkm_drm_profile_now_us();
	/*
	 * NVK command BOs and the fixed submit GPFIFO/post/semaphore pages are
	 * coherent sysmem on the x86 desktop targets supported by this driver.
	 * DragonFly pmap_invalidate_cache_range() is for cache-domain changes
	 * and broadcasts WBINVD on CPUs without CPUID_SS, which turns each EXEC
	 * into several global cache flushes. The only ordering needed here is
	 * the Linux nouveau kick sequence below: GP_PUT, wmb/sfence, USERD read
	 * flush, then doorbell.
	 */
	nvkm_drm_profile_add_us(&sc->exec_profile_cache_flush_us,
	    profile_start);
	/* Debug canary: detect command-pool chunk reuse while the GPU is still
	 * reading the previous submit. The scan is diagnostic only and is kept
	 * off the production submit path unless verbose DRM debug is enabled. */
	pending->push_va_lo = push_va_lo;
	pending->push_va_hi = push_va_hi;
	if (nvkm_debug != 0 && push_va_hi > push_va_lo) {
		struct nvkm_drm_exec_pending *op;

		LIST_FOREACH(op, &sc->exec_pending, link) {
			if (op->chan != chan)
				continue;
			if (push_va_lo < op->push_va_hi &&
			    op->push_va_lo < push_va_hi) {
				sc->exec_push_reuse_count++;
				sc->exec_push_reuse_va = push_va_lo;
				sc->exec_push_reuse_prev_payload = op->payload;
				if (sc->exec_push_reuse_count <= 3)
					nvkm_infof(sc->dev,
					    "EXEC push REUSE-IN-FLIGHT: new=[0x%llx,0x%llx) overlaps in-flight=[0x%llx,0x%llx) prev_payload=0x%08x (count=%llu)\n",
					    (unsigned long long)push_va_lo,
					    (unsigned long long)push_va_hi,
					    (unsigned long long)op->push_va_lo,
					    (unsigned long long)op->push_va_hi,
					    op->payload,
					    (unsigned long long)sc->exec_push_reuse_count);
				break;
			}
		}
	}
	/* Keep the file pointer only for fault-time binding diagnostics. The
	 * drm_file owns the channel and cancels pending records before the VMM
	 * is destroyed, so this borrowed pointer stays valid while pending.
	 */
	pending->nfile = nfile;
	LIST_INSERT_HEAD(&sc->exec_pending, pending, link);
	exec_completion_queued = true;
	sc->exec_async_pending_count++;
	profile_start = nvkm_drm_profile_now_us();
	nvkm_gsp_bar1_wr32(sc, slot_bar1 + NV_USERD_GP_PUT, put);
	cpu_sfence();
	(void)nvkm_gsp_bar1_rd32(sc, slot_bar1 + 0);
	nvkm_wr32(sc, NV_USERMODE_DOORBELL, chan->gsp_token);
	nvkm_drm_profile_add_us(&sc->exec_profile_doorbell_us,
	    profile_start);
	chan->gpf_put = put;
	if (chan->gpf_free >= gpf_required)
		chan->gpf_free -= gpf_required;
	else
		chan->gpf_free = 0;
	pending = NULL;
	submit_slot_allocated = false;
	lwkt_reltoken(&sc->gsp_tok);
	gsp_tok_held = false;
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC submitted channel=%u pushes=%u sigs=%u err=%d put=%u sema=0x%08x payload=0x%08x slot=%u\n",
	    req->channel, req->push_count, req->sig_count, err, put, sema[0],
	    payload, post_slot);

out_unlock:
	profile_start = nvkm_drm_profile_now_us();
	if (exec_fence != NULL) {
		if (!exec_completion_queued || err != 0) {
			if (err != 0)
				nvkm_drm_fence_set_error(exec_fence, err);
			(void)dma_fence_signal(exec_fence);
		}
		if (done_fence == NULL)
			dma_fence_put(exec_fence);
	}
	if (submit_slot_allocated)
		nvkm_drm_submit_slot_release(chan, post_slot);
	nvkm_drm_exec_pending_put(sc, pending);
	if (gsp_tok_held)
		lwkt_reltoken(&sc->gsp_tok);
	nvkm_drm_profile_add_us(&sc->exec_profile_cleanup_us, profile_start);
	if (err != 0)
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC return channel=%u err=%d\n",
		    req->channel, err);
	return (err);
}

static int
nvkm_drm_ioctl_exec(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_exec *req = data;
	struct nvkm_drm_chan *dchan;
	struct nvkm_gsp_chan *chan;
	struct nvkm_drm_job *job;
	uint64_t profile_start;
	int err;

	if (nfile == NULL)
		return (-ENXIO);
	dchan = nvkm_drm_channel_find(nfile, req->channel);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC queue channel=%u pushes=%u waits=%u sigs=%u\n",
	    req->channel, req->push_count, req->wait_count, req->sig_count);
	if (dchan == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC missing channel=%u\n", req->channel);
		return (-ENOENT);
	}
	chan = dchan->chan;
	if (chan == NULL || chan->submit_gpf.kva == NULL ||
	    chan->submit_push.kva == NULL || chan->submit_sema.kva == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC channel=%u missing submit buffers chan=%p\n",
		    req->channel, chan);
		return (-ENXIO);
	}
	if (req->push_count > NVKM_DRM_GPFIFO_ENTRIES - 2) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC too many pushes channel=%u pushes=%u\n",
		    req->channel, req->push_count);
		return (-EINVAL);
	}

	if (req->push_count == 0 && req->sig_count == 0 &&
	    req->wait_count == 0) {
		return (nvkm_drm_exec_submit(sc, file_priv, nfile,
		    req->channel, NULL, 0, NULL, NULL, 0));
	}

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (job == NULL)
		return (-ENOMEM);
	kref_init(&job->refcount);
	job->type = NVKM_DRM_JOB_EXEC;
	job->sc = sc;
	job->file_priv = file_priv;
	job->nfile = nfile;
	job->exec.channel = req->channel;
	job->exec.push_count = req->push_count;
	job->exec.sig_count = req->sig_count;
	job->done_fence = nvkm_drm_exec_fence_create(sc, ++sc->fence_seqno);
	if (job->done_fence == NULL) {
		err = -ENOMEM;
		goto fail;
	}

	if (req->push_count != 0) {
		job->exec.pushes = kmalloc_array(req->push_count,
		    sizeof(*job->exec.pushes), GFP_KERNEL);
		if (job->exec.pushes == NULL) {
			err = -ENOMEM;
			goto fail;
		}
		err = copyin((const void *)(uintptr_t)req->push_ptr,
		    job->exec.pushes,
		    sizeof(*job->exec.pushes) * req->push_count);
		if (err != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC push copyin failed channel=%u pushes=%u err=%d\n",
			    req->channel, req->push_count, err);
			err = -EFAULT;
			goto fail;
		}
	}

	profile_start = nvkm_drm_profile_now_us();
	err = nvkm_drm_collect_wait_syncobjs(sc, file_priv, req->wait_count,
	    req->wait_ptr, &job->wait_fences, &job->wait_count);
	nvkm_drm_profile_add_us(&sc->exec_profile_wait_sync_us,
	    profile_start);
	if (err != 0)
		goto fail;
	err = nvkm_drm_job_prepare_deps(job);
	if (err != 0)
		goto fail;

	profile_start = nvkm_drm_profile_now_us();
	err = nvkm_drm_prepare_signal_syncobjs(sc, file_priv, req->sig_count,
	    req->sig_ptr, job->done_fence, &job->exec.signals);
	nvkm_drm_profile_add_us(&sc->exec_profile_prepare_signal_us,
	    profile_start);
	if (err != 0)
		goto fail;

	lockmgr(&nfile->job_submit_lock, LK_EXCLUSIVE);
	err = nvkm_drm_exec_attach_resv_fence(sc, nfile, job->done_fence);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC attach reservation fence failed err=%d\n",
		    err);
	} else {
		nvkm_drm_exec_signals_publish(job->exec.signals,
		    job->exec.sig_count);
		err = nvkm_drm_job_enqueue(job);
	}
	lockmgr(&nfile->job_submit_lock, LK_RELEASE);
	if (err != 0)
		goto fail_signal;
	return (0);

fail_signal:
	nvkm_drm_job_signal(job, err);
fail:
	nvkm_drm_job_put(job);
	return (err);
}

/* ---- ioctl table ---- */
#define DRM_IOCTL_NOUVEAU_GETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GETPARAM, struct drm_nouveau_getparam)
#define DRM_IOCTL_NOUVEAU_VM_INIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_INIT, struct drm_nouveau_vm_init)
/* NVIF is variable-size; use DRM_IOWR with void marker */

static const struct drm_ioctl_desc nvkm_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(NOUVEAU_GETPARAM, nvkm_drm_ioctl_getparam,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_VM_INIT,  nvkm_drm_ioctl_vm_init,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_NVIF,     nvkm_drm_ioctl_nvif,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_CHANNEL_ALLOC, nvkm_drm_ioctl_channel_alloc,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_CHANNEL_FREE, nvkm_drm_ioctl_channel_free,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_NEW, nvkm_drm_ioctl_gem_new,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_INFO, nvkm_drm_ioctl_gem_info,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_CPU_PREP, nvkm_drm_ioctl_gem_cpu_prep,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_CPU_FINI, nvkm_drm_ioctl_gem_cpu_fini,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_VM_BIND, nvkm_drm_ioctl_vm_bind,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_EXEC, nvkm_drm_ioctl_exec,
	    DRM_AUTH | DRM_RENDER_ALLOW),
};
