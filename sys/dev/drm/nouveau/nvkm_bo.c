/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * nvkm BO layer — minimal GART/VRAM BO backing.
 *
 * GART BOs are page-aligned kernel virtual allocations whose individual
 * pages are mapped into the GPU VMM. VRAM BOs have only a GPU physical
 * address and are not CPU-mappable yet.
 *
 * No TTM or eviction yet. VRAM BOs use the current GSP-RM bump allocator
 * and are GPU-only for now.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/rman.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_param.h>
#include <vm/vm_page.h>
#include <vm/vm_object.h>
#include <vm/vm_pager.h>
#include <machine/pmap.h>

#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/dma-fence.h>
#include <linux/reservation.h>
#include <linux/sched.h>
#include <drm/drmP.h>
#include <drm/drm_gem.h>
#include <drm/drm_vma_manager.h>
#include <uapi/drm/drm_mode.h>

#include "nvkm_priv.h"
#include "nvkm_bo.h"
#include "nvkm_gsp_rm.h"

static MALLOC_DEFINE(M_NVKM_BO, "nvkm_bo", "nvkm GEM buffer object pages");

static const char *
nvkm_bo_alloc_fail_path_name(enum nvkm_bo_alloc_fail_path path)
{
	switch (path) {
	case NVKM_BO_ALLOC_FAIL_NONE:
		return ("none");
	case NVKM_BO_ALLOC_FAIL_VRAM:
		return ("vram");
	case NVKM_BO_ALLOC_FAIL_SYSMEM:
		return ("sysmem");
	case NVKM_BO_ALLOC_FAIL_HANDLE:
		return ("handle");
	case NVKM_BO_ALLOC_FAIL_MMAP:
		return ("mmap");
	default:
		return ("unknown");
	}
}

static void
nvkm_bo_record_alloc_fail(struct nvkm_softc *sc,
    enum nvkm_bo_alloc_fail_path path, uint64_t size, uint32_t domain,
    int error)
{
	sc->bo_alloc_fail_count++;
	sc->bo_alloc_fail_path = path;
	sc->bo_alloc_fail_size = size;
	sc->bo_alloc_fail_domain = domain;
	sc->bo_alloc_fail_error = error;

	device_printf(sc->dev,
	    "nvkm_bo: GEM_NEW failed path=%s error=%d req_domain=0x%x size=0x%llx\n",
	    nvkm_bo_alloc_fail_path_name(path), error, domain,
	    (unsigned long long)size);
}

static void
nvkm_bo_account_alloc(struct nvkm_softc *sc, struct nvkm_bo *bo)
{
	uint64_t size = bo->base.size;

	if (bo->kva != NULL) {
		sc->bo_sysmem_active_count++;
		sc->bo_sysmem_active_bytes += size;
		if (sc->bo_sysmem_active_bytes > sc->bo_sysmem_high_bytes)
			sc->bo_sysmem_high_bytes = sc->bo_sysmem_active_bytes;
		return;
	}
	if (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) {
		sc->bo_vram_active_count++;
		sc->bo_vram_active_bytes += size;
		if (sc->bo_vram_active_bytes > sc->bo_vram_high_bytes)
			sc->bo_vram_high_bytes = sc->bo_vram_active_bytes;
	}
}

static void
nvkm_bo_account_free(struct nvkm_softc *sc, struct nvkm_bo *bo)
{
	uint64_t size = bo->base.size;

	if (bo->kva != NULL) {
		if (sc->bo_sysmem_active_count != 0)
			sc->bo_sysmem_active_count--;
		if (sc->bo_sysmem_active_bytes >= size)
			sc->bo_sysmem_active_bytes -= size;
		else
			sc->bo_sysmem_active_bytes = 0;
		return;
	}
	if (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) {
		if (sc->bo_vram_active_count != 0)
			sc->bo_vram_active_count--;
		if (sc->bo_vram_active_bytes >= size)
			sc->bo_vram_active_bytes -= size;
		else
			sc->bo_vram_active_bytes = 0;
	}
}

static bool
nvkm_bo_cpu_mappable(const struct nvkm_bo *bo)
{
	return (bo->kva != NULL || bo->bar1_mappable);
}

static int nvkm_bo_bar1_map(struct nvkm_softc *sc, struct nvkm_bo *bo);

/* ============================================================
 * cdev pager — backs userspace mmap with our contig pages.
 * ============================================================ */

static int
nvkm_gem_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
	*color = 0;
	return (0);
}

static void
nvkm_gem_pager_dtor(void *handle)
{
	struct drm_gem_object *obj = handle;

	if (obj != NULL)
		drm_gem_object_unreference_unlocked(obj);
}

static int
nvkm_gem_pager_fault(vm_object_t vm_obj, vm_ooffset_t offset, int prot,
    vm_page_t *mres)
{
	struct drm_gem_object *obj = vm_obj->handle;
	struct nvkm_bo *bo = to_nvkm_bo(obj);
	struct nvkm_softc *sc = obj->dev->dev_private;
	vm_page_t m;
	vm_paddr_t pa;

	if (offset < 0 || (vm_ooffset_t)offset >= obj->size)
		return (VM_PAGER_ERROR);

	/* OBJT_MGTDEVICE: *mres is NULL on entry and we return the
	 * backing page directly without inserting it into vm_obj. */
	KKASSERT(*mres == NULL);

	if (bo->kva != NULL) {
		pa = vtophys((uint8_t *)bo->kva + offset);
		m = PHYS_TO_VM_PAGE(pa);
	} else {
		if (bo->bar1_mappable && bo->bar1_gva == 0 &&
		    nvkm_bo_bar1_map(sc, bo) != 0)
			return (VM_PAGER_ERROR);
		if (bo->bar1_gva != 0 &&
		    bo->bar1_size >= (uint64_t)offset + PAGE_SIZE &&
		    sc->bar_res[1] != NULL) {
			pa = rman_get_start(sc->bar_res[1]) +
			    bo->bar1_gva + offset;
			m = vm_phys_fictitious_to_vm_page(pa);
		} else {
			m = NULL;
		}
	}
	if (m == NULL)
		return (VM_PAGER_ERROR);

	if (vm_page_busy_try(m, FALSE))
		return (VM_PAGER_ERROR);

	m->valid = VM_PAGE_BITS_ALL;
	*mres = m;
	return (VM_PAGER_OK);
}

struct cdev_pager_ops nvkm_gem_pager_ops = {
	.cdev_pg_ctor	= nvkm_gem_pager_ctor,
	.cdev_pg_dtor	= nvkm_gem_pager_dtor,
	.cdev_pg_fault	= nvkm_gem_pager_fault,
};

static int
nvkm_bo_bar1_map(struct nvkm_softc *sc, struct nvkm_bo *bo)
{
	int err;

	if (bo->bar1_gva != 0)
		return (0);
	if (bo->vram_alloc == NULL || bo->paddr == 0)
		return (EINVAL);

	err = nvkm_gsp_bar1_map_existing_range(sc, bo->paddr, bo->base.size,
	    &bo->bar1_gva);
	if (err != 0)
		return (err);
	bo->bar1_size = bo->base.size;
	return (0);
}

static void
nvkm_bo_bar1_unmap(struct nvkm_softc *sc, struct nvkm_bo *bo)
{
	if (bo->bar1_gva == 0)
		return;
	nvkm_gsp_bar1_unmap_existing_range(sc, bo->bar1_gva, bo->bar1_size);
	bo->bar1_gva = 0;
	bo->bar1_size = 0;
}

/* ============================================================
 * BO alloc / free
 * ============================================================ */

static struct nvkm_bo *
nvkm_bo_create(struct drm_device *ddev, uint64_t size, uint32_t domain,
    uint32_t tile_mode, uint32_t tile_flags)
{
	struct nvkm_softc *sc = ddev->dev_private;
	struct nvkm_bo *bo;
	void *kva;
	bool wants_vram;
	bool can_fallback_gart;

	size = roundup(size, PAGE_SIZE);
	if (size == 0)
		return (NULL);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (bo == NULL)
		return (NULL);
	reservation_object_init(&bo->resv);

	wants_vram = (domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0;
	can_fallback_gart = (domain & NOUVEAU_GEM_DOMAIN_GART) != 0 ||
	    !wants_vram;

	if (wants_vram) {
		/* GEM VRAM BO ownership:
		 *
		 * - owner: this struct nvkm_bo / drm_gem_object lifetime.
		 * - borrows: VM_BIND GPU-VA mappings, held through GEM refs.
		 * - release: nvkm_bo_gem_free(), but only after VRAM
		 *   allocation metadata can prove this is GEM-owned backing
		 *   from a reclaimable GEM arena.
		 */
		bo->vram_alloc = nvkm_gsp_vram_alloc_ref(sc, size, PAGE_SIZE,
		    NVKM_VRAM_GEM, bo);
		if (bo->vram_alloc == NULL) {
			if (!can_fallback_gart) {
				nvkm_bo_record_alloc_fail(sc,
				    NVKM_BO_ALLOC_FAIL_VRAM, size, domain,
				    -ENOMEM);
				reservation_object_fini(&bo->resv);
				kfree(bo);
				return (NULL);
			}
			nvkm_debugf(sc->dev,
			    "nvkm_bo: GEM_NEW VRAM fallback to GART req_domain=0x%x size=0x%llx\n",
			    domain, (unsigned long long)size);
		}
	}
	if (bo->vram_alloc != NULL) {
		bo->paddr = bo->vram_alloc->paddr;
		bo->domain = NOUVEAU_GEM_DOMAIN_VRAM;
	} else {
		kva = (void *)kmem_alloc(kernel_map, size, VM_SUBSYS_DRM_GEM);
		if (kva == NULL) {
			nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_SYSMEM,
			    size, domain, -ENOMEM);
			reservation_object_fini(&bo->resv);
			kfree(bo);
			return (NULL);
		}
		bo->kva = kva;
		bo->paddr = vtophys(kva);
		bo->domain = (domain ? domain : NOUVEAU_GEM_DOMAIN_GART) &
		    ~NOUVEAU_GEM_DOMAIN_VRAM;
		bo->domain |= NOUVEAU_GEM_DOMAIN_GART;
	}
	bo->tile_mode = tile_mode;
	bo->tile_flags = tile_flags;

	drm_gem_private_object_init(ddev, &bo->base, size);
	nvkm_bo_account_alloc(sc, bo);
	return (bo);
}

void
nvkm_bo_gem_free(struct drm_gem_object *obj)
{
	struct nvkm_bo *bo = to_nvkm_bo(obj);
	struct nvkm_softc *sc = obj->dev->dev_private;

	nvkm_debugf(sc->dev,
	    "nvkm_bo: GEM_FREE obj=%p domain=0x%x size=0x%llx paddr=0x%llx cpu_map=%u\n",
	    obj, bo->domain, (unsigned long long)obj->size,
	    (unsigned long long)bo->paddr, bo->kva != NULL);

	sc->bo_gem_free_count++;
	nvkm_bo_account_free(sc, bo);

	(void)nvkm_bo_resv_wait(bo, false);
	nvkm_bo_bar1_unmap(sc, bo);
	if (bo->kva != NULL) {
		kmem_free(kernel_map, (vm_offset_t)bo->kva, obj->size);
		bo->kva = NULL;
	}
	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) &&
	    bo->vram_alloc != NULL)
		nvkm_gsp_vram_free_gem(sc, bo->vram_alloc, bo);

	reservation_object_fini(&bo->resv);
	drm_gem_object_release(obj);
	kfree(bo);
}

void
nvkm_bo_resv_add_excl_fence(struct nvkm_bo *bo, struct dma_fence *fence)
{
	reservation_object_lock(&bo->resv, NULL);
	reservation_object_add_excl_fence(&bo->resv, fence);
	reservation_object_unlock(&bo->resv);
}

int
nvkm_bo_resv_wait(struct nvkm_bo *bo, bool intr)
{
	long ret;

	struct nvkm_softc *sc = bo->base.dev->dev_private;

	sc->bo_resv_wait_count++;
	ret = reservation_object_wait_timeout_rcu(&bo->resv, true, intr,
	    MAX_SCHEDULE_TIMEOUT);
	if (ret < 0) {
		sc->bo_resv_wait_error_count++;
		return ((int)ret);
	}
	if (ret == 0) {
		sc->bo_resv_wait_error_count++;
		return (-ETIME);
	}
	return (0);
}

int
nvkm_bo_dumb_create(struct drm_file *file_priv, struct drm_device *ddev,
    struct drm_mode_create_dumb *args)
{
	struct nvkm_softc *sc = ddev->dev_private;
	struct nvkm_bo *bo;
	uint64_t pitch;
	uint64_t size;
	uint32_t domain;
	uint32_t handle;
	int err;

	if (args->flags != 0)
		return (-EINVAL);

	pitch = roundup2((uint64_t)args->width * howmany(args->bpp, 8), 64);
	size = roundup(pitch * args->height, PAGE_SIZE);
	if (pitch > UINT32_MAX || size == 0)
		return (-EINVAL);

	domain = sc->bar1.ready ? NOUVEAU_GEM_DOMAIN_VRAM :
	    NOUVEAU_GEM_DOMAIN_GART;
	bo = nvkm_bo_create(ddev, size, domain, 0, 0);
	if (bo == NULL)
		return (-ENOMEM);
	if (domain == NOUVEAU_GEM_DOMAIN_VRAM) {
		err = nvkm_bo_bar1_map(sc, bo);
		if (err != 0) {
			nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_MMAP,
			    bo->base.size, domain, err);
			drm_gem_object_put_unlocked(&bo->base);
			return (err);
		}
	}

	err = drm_gem_handle_create(file_priv, &bo->base, &handle);
	if (err != 0) {
		nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_HANDLE,
		    bo->base.size, domain, err);
		drm_gem_object_put_unlocked(&bo->base);
		return (err);
	}

	err = drm_gem_create_mmap_offset(&bo->base);
	drm_gem_object_put_unlocked(&bo->base);
	if (err != 0) {
		nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_MMAP,
		    bo->base.size, domain, err);
		drm_gem_handle_delete(file_priv, handle);
		return (err);
	}

	args->handle = handle;
	args->pitch = (uint32_t)pitch;
	args->size = bo->base.size;
	nvkm_debugf(sc->dev,
	    "nvkm_bo: DUMB_CREATE handle=%u %ux%u bpp=%u pitch=%u "
	    "size=0x%llx domain=0x%x paddr=0x%llx bar1=0x%llx\n",
	    handle, args->width, args->height, args->bpp, args->pitch,
	    (unsigned long long)args->size, bo->domain,
	    (unsigned long long)bo->paddr, (unsigned long long)bo->bar1_gva);
	return (0);
}

int
nvkm_bo_dumb_map_offset(struct drm_file *file_priv, struct drm_device *ddev,
    uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *obj;
	int err;

	(void)ddev;
	obj = drm_gem_object_lookup(file_priv, handle);
	if (obj == NULL)
		return (-ENOENT);
	if (obj->import_attach) {
		drm_gem_object_put_unlocked(obj);
		return (-EINVAL);
	}

	err = drm_gem_create_mmap_offset(obj);
	if (err == 0) {
		*offset = DRM_GEM_MAPPING_KEY |
		    DRM_GEM_MAPPING_OFF(obj->map_list.key);
	}
	drm_gem_object_put_unlocked(obj);
	return (err);
}

int
nvkm_bo_dumb_destroy(struct drm_file *file_priv, struct drm_device *ddev,
    uint32_t handle)
{
	return (drm_gem_dumb_destroy(file_priv, ddev, handle));
}

/* ============================================================
 * ioctl handlers
 * ============================================================ */

int
nvkm_drm_ioctl_gem_new(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = ddev->dev_private;
	struct drm_nouveau_gem_new *req = data;
	struct nvkm_bo *bo;
	uint32_t handle = 0;
	int err;

	sc->bo_gem_new_count++;
	bo = nvkm_bo_create(ddev, req->info.size, req->info.domain,
	    req->info.tile_mode, req->info.tile_flags);
	if (bo == NULL)
		return (-ENOMEM);
	if ((req->info.domain & NOUVEAU_GEM_DOMAIN_MAPPABLE) != 0 &&
	    bo->vram_alloc != NULL) {
		if (sc->bar1.ready) {
			bo->bar1_mappable = true;
		} else if ((req->info.domain & NOUVEAU_GEM_DOMAIN_GART) != 0) {
			uint32_t gart_domain;

			gart_domain = (req->info.domain &
			    ~NOUVEAU_GEM_DOMAIN_VRAM) |
			    NOUVEAU_GEM_DOMAIN_GART;
			nvkm_debugf(sc->dev,
			    "nvkm_bo: GEM_NEW mappable VRAM fallback to GART req_domain=0x%x size=0x%llx\n",
			    req->info.domain,
			    (unsigned long long)bo->base.size);
			drm_gem_object_put_unlocked(&bo->base);
			bo = nvkm_bo_create(ddev, req->info.size,
			    gart_domain, req->info.tile_mode,
			    req->info.tile_flags);
			if (bo == NULL)
				return (-ENOMEM);
		} else {
			nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_MMAP,
			    bo->base.size, req->info.domain, ENXIO);
			drm_gem_object_put_unlocked(&bo->base);
			return (ENXIO);
		}
	}

	err = drm_gem_handle_create(file_priv, &bo->base, &handle);
	if (err != 0)
		nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_HANDLE,
		    bo->base.size, req->info.domain, err);
	/* drop our local reference; the handle holds one now. */
	drm_gem_object_put_unlocked(&bo->base);
	if (err != 0)
		return (err);

	nvkm_debugf(sc->dev,
	    "nvkm_bo: GEM_NEW handle=%u obj=%p req_domain=0x%x domain=0x%x size=0x%llx paddr=0x%llx cpu_map=%u\n",
	    handle, &bo->base, req->info.domain, bo->domain,
	    (unsigned long long)bo->base.size,
	    (unsigned long long)bo->paddr, nvkm_bo_cpu_mappable(bo));

	if (nvkm_bo_cpu_mappable(bo)) {
		err = drm_gem_create_mmap_offset(&bo->base);
		if (err != 0) {
			nvkm_bo_record_alloc_fail(sc,
			    NVKM_BO_ALLOC_FAIL_MMAP, bo->base.size,
			    req->info.domain, err);
			drm_gem_handle_delete(file_priv, handle);
			return (err);
		}
	}

	req->info.handle = handle;
	req->info.domain = bo->domain;
	req->info.size = bo->base.size;
	req->info.offset = 0;	/* GPU VA — set by VM_BIND later */
	req->info.map_handle = nvkm_bo_cpu_mappable(bo) ?
	    (DRM_GEM_MAPPING_KEY |
	    DRM_GEM_MAPPING_OFF(bo->base.map_list.key)) : 0;
	req->info.tile_mode = bo->tile_mode;
	req->info.tile_flags = bo->tile_flags;
	return (0);
}

int
nvkm_drm_ioctl_gem_info(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct drm_nouveau_gem_info *req = data;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;

	obj = drm_gem_object_lookup(file_priv, req->handle);
	if (obj == NULL)
		return (-ENOENT);
	bo = to_nvkm_bo(obj);

	req->domain = bo->domain;
	req->size = obj->size;
	req->offset = 0;
	req->map_handle = nvkm_bo_cpu_mappable(bo) ?
	    (DRM_GEM_MAPPING_KEY |
	    DRM_GEM_MAPPING_OFF(obj->map_list.key)) : 0;
	req->tile_mode = bo->tile_mode;
	req->tile_flags = bo->tile_flags;

	drm_gem_object_put_unlocked(obj);
	return (0);
}

int
nvkm_drm_ioctl_gem_cpu_prep(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = ddev->dev_private;
	struct drm_nouveau_gem_cpu_prep *req = data;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;
	int err;

	obj = drm_gem_object_lookup(file_priv, req->handle);
	if (obj == NULL)
		return (-ENOENT);
	bo = to_nvkm_bo(obj);
	err = nvkm_bo_resv_wait(bo, true);
	sc->cpu_prep_wait_count++;
	if (err != 0)
		sc->cpu_prep_wait_error_count++;
	nvkm_debugf(sc->dev,
	    "nvkm_bo: CPU_PREP handle=%u obj=%p domain=0x%x size=0x%llx paddr=0x%llx cpu_map=%u flags=0x%x wait_err=%d\n",
	    req->handle, obj, bo->domain, (unsigned long long)obj->size,
	    (unsigned long long)bo->paddr, bo->kva != NULL, req->flags, err);
	drm_gem_object_put_unlocked(obj);
	return (err);
}

int
nvkm_drm_ioctl_gem_cpu_fini(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = ddev->dev_private;
	struct drm_nouveau_gem_cpu_fini *req = data;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;

	obj = drm_gem_object_lookup(file_priv, req->handle);
	if (obj == NULL)
		return (-ENOENT);
	bo = to_nvkm_bo(obj);
	if (bo->kva != NULL)
		pmap_invalidate_cache_range((vm_offset_t)bo->kva,
		    (vm_offset_t)bo->kva + obj->size);
	nvkm_debugf(sc->dev,
	    "nvkm_bo: CPU_FINI handle=%u obj=%p domain=0x%x size=0x%llx paddr=0x%llx cpu_map=%u flushed=%u\n",
	    req->handle, obj, bo->domain, (unsigned long long)obj->size,
	    (unsigned long long)bo->paddr, bo->kva != NULL, bo->kva != NULL);
	drm_gem_object_put_unlocked(obj);
	return (0);
}
