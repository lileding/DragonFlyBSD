/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * TTM device glue for nvkm.
 */

#include "nvkm_priv.h"
#include "nvkm_bo.h"
#include "nvkm_gsp_rm.h"
#include "nvkm_ttm.h"

#include <sys/rman.h>
#include <asm/page.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <drm/ttm/ttm_bo_api.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_memory.h>
#include <drm/ttm/ttm_page_alloc.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>

#define DRM_FILE_PAGE_OFFSET (0x100000000ULL >> PAGE_SHIFT)

struct nvkm_ttm {
	struct nvkm_softc *sc;
	struct ttm_bo_device bdev;
	struct ttm_bo_global_ref bo_global_ref;
	struct drm_global_reference mem_global_ref;
	bool mem_global_referenced;
	bool bdev_initialized;
	bool tt_initialized;
	bool vram_initialized;
};

struct nvkm_ttm_tt {
	struct ttm_dma_tt ttm;
};

static struct nvkm_ttm *
nvkm_ttm_from_bdev(struct ttm_bo_device *bdev)
{
	return (container_of(bdev, struct nvkm_ttm, bdev));
}

static int
nvkm_ttm_mem_global_init(struct drm_global_reference *ref)
{
	return (ttm_mem_global_init(ref->object));
}

static void
nvkm_ttm_mem_global_release(struct drm_global_reference *ref)
{
	ttm_mem_global_release(ref->object);
}

static int
nvkm_ttm_global_init(struct nvkm_ttm *ttm)
{
	struct drm_global_reference *global_ref;
	int err;

	ttm->mem_global_referenced = false;
	global_ref = &ttm->mem_global_ref;
	global_ref->global_type = DRM_GLOBAL_TTM_MEM;
	global_ref->size = sizeof(struct ttm_mem_global);
	global_ref->init = nvkm_ttm_mem_global_init;
	global_ref->release = nvkm_ttm_mem_global_release;
	err = drm_global_item_ref(global_ref);
	if (err != 0)
		return (err);

	ttm->bo_global_ref.mem_glob = ttm->mem_global_ref.object;
	global_ref = &ttm->bo_global_ref.ref;
	global_ref->global_type = DRM_GLOBAL_TTM_BO;
	global_ref->size = sizeof(struct ttm_bo_global);
	global_ref->init = ttm_bo_global_init;
	global_ref->release = ttm_bo_global_release;
	err = drm_global_item_ref(global_ref);
	if (err != 0) {
		drm_global_item_unref(&ttm->mem_global_ref);
		return (err);
	}

	ttm->mem_global_referenced = true;
	return (0);
}

static void
nvkm_ttm_global_fini(struct nvkm_ttm *ttm)
{
	if (!ttm->mem_global_referenced)
		return;
	drm_global_item_unref(&ttm->bo_global_ref.ref);
	drm_global_item_unref(&ttm->mem_global_ref);
	ttm->mem_global_referenced = false;
}

static int
nvkm_ttm_tt_bind(struct ttm_tt *ttm, struct ttm_mem_reg *mem)
{
	(void)ttm;
	(void)mem;
	return (0);
}

static int
nvkm_ttm_tt_unbind(struct ttm_tt *ttm)
{
	(void)ttm;
	return (0);
}

static void
nvkm_ttm_tt_destroy(struct ttm_tt *ttm)
{
	struct nvkm_ttm_tt *ntt = (void *)ttm;

	ttm_dma_tt_fini(&ntt->ttm);
	kfree(ntt);
}

static struct ttm_backend_func nvkm_ttm_backend_func = {
	.bind = nvkm_ttm_tt_bind,
	.unbind = nvkm_ttm_tt_unbind,
	.destroy = nvkm_ttm_tt_destroy,
};

static struct ttm_tt *
nvkm_ttm_tt_create(struct ttm_buffer_object *bo, uint32_t page_flags)
{
	struct nvkm_ttm_tt *ntt;

	ntt = kzalloc(sizeof(*ntt), GFP_KERNEL);
	if (ntt == NULL)
		return (NULL);
	ntt->ttm.ttm.func = &nvkm_ttm_backend_func;
	if (ttm_dma_tt_init(&ntt->ttm, bo, page_flags) != 0) {
		kfree(ntt);
		return (NULL);
	}
	return (&ntt->ttm.ttm);
}

static void
nvkm_ttm_sync_bo_domain(struct nvkm_bo *bo)
{
	uint32_t flags = bo->domain & ~(NOUVEAU_GEM_DOMAIN_CPU |
	    NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_GART);

	if (!bo->ttm_backed)
		return;

	switch (bo->tbo.mem.mem_type) {
	case TTM_PL_VRAM:
		bo->domain = flags | NOUVEAU_GEM_DOMAIN_VRAM;
		if (bo->vram_alloc != NULL)
			bo->paddr = bo->vram_alloc->paddr;
		break;
	case TTM_PL_TT:
		bo->domain = flags | NOUVEAU_GEM_DOMAIN_GART;
		bo->paddr = 0;
		break;
	case TTM_PL_SYSTEM:
	default:
		bo->domain = flags | NOUVEAU_GEM_DOMAIN_CPU;
		bo->paddr = 0;
		break;
	}
}

static int
nvkm_ttm_vram_man_init(struct ttm_mem_type_manager *man, unsigned long p_size)
{
	(void)p_size;

	man->priv = nvkm_ttm_from_bdev(man->bdev);
	return (0);
}

static int
nvkm_ttm_vram_man_takedown(struct ttm_mem_type_manager *man)
{
	man->priv = NULL;
	return (0);
}

static int
nvkm_ttm_vram_man_get_node(struct ttm_mem_type_manager *man,
    struct ttm_buffer_object *tbo, const struct ttm_place *place,
    struct ttm_mem_reg *mem)
{
	struct nvkm_ttm *ttm = man->priv;
	struct nvkm_softc *sc = ttm->sc;
	struct nvkm_bo *bo = container_of(tbo, struct nvkm_bo, tbo);
	struct nvkm_vram_alloc *alloc;
	uint64_t align;
	uint64_t base;
	uint64_t start;
	uint64_t lpfn;
	uint64_t size;

	mem->mm_node = NULL;
	size = (uint64_t)mem->num_pages << PAGE_SHIFT;
	align = mem->page_alignment != 0 ?
	    (uint64_t)mem->page_alignment << PAGE_SHIFT : PAGE_SIZE;
	base = sc->fb_usable_base;
	lpfn = place->lpfn != 0 ? place->lpfn : man->size;

	alloc = nvkm_gsp_vram_alloc_ref(sc, size, align, NVKM_VRAM_GEM, bo);
	if (alloc == NULL)
		return (0);
	if (alloc->paddr < base ||
	    ((alloc->paddr - base) & (PAGE_SIZE - 1)) != 0) {
		nvkm_gsp_vram_free_gem(sc, alloc, bo);
		return (0);
	}

	start = (alloc->paddr - base) >> PAGE_SHIFT;
	if (start < place->fpfn || start + mem->num_pages > lpfn) {
		nvkm_gsp_vram_free_gem(sc, alloc, bo);
		return (0);
	}

	mem->mm_node = &alloc->node;
	mem->start = start;
	bo->vram_alloc = alloc;
	bo->paddr = alloc->paddr;
	return (0);
}

static void
nvkm_ttm_vram_man_put_node(struct ttm_mem_type_manager *man,
    struct ttm_mem_reg *mem)
{
	struct nvkm_ttm *ttm = man->priv;
	struct nvkm_vram_alloc *alloc;
	struct nvkm_bo *bo;

	if (mem->mm_node == NULL)
		return;

	alloc = container_of(mem->mm_node, struct nvkm_vram_alloc, node);
	bo = alloc->owner;
	if (bo != NULL && bo->vram_alloc == alloc) {
		bo->vram_alloc = NULL;
		if (bo->paddr == alloc->paddr)
			bo->paddr = 0;
	}
	nvkm_gsp_vram_free_gem(ttm->sc, alloc, bo);
	mem->mm_node = NULL;
}

static void
nvkm_ttm_vram_man_debug(struct ttm_mem_type_manager *man,
    struct drm_printer *printer)
{
	drm_printf(printer, "nvkm vram manager: size=%llu pages\n",
	    (unsigned long long)man->size);
}

static const struct ttm_mem_type_manager_func nvkm_ttm_vram_manager_func = {
	.init = nvkm_ttm_vram_man_init,
	.takedown = nvkm_ttm_vram_man_takedown,
	.get_node = nvkm_ttm_vram_man_get_node,
	.put_node = nvkm_ttm_vram_man_put_node,
	.debug = nvkm_ttm_vram_man_debug,
};

static int
nvkm_ttm_tt_populate(struct ttm_tt *ttm, struct ttm_operation_ctx *ctx)
{
	struct nvkm_ttm *nvttm = nvkm_ttm_from_bdev(ttm->bdev);
	struct pci_dev *pdev = nvttm->sc->drm_pdev;
	struct ttm_dma_tt *dma = (void *)ttm;
	int err;

	if (ttm->state != tt_unpopulated)
		return (0);

	err = ttm_pool_populate(ttm, ctx);
	if (err != 0)
		return (err);

	for (unsigned long i = 0; i < ttm->num_pages; i++)
		dma->dma_address[i] = pci_map_page(pdev, ttm->pages[i], 0,
		    PAGE_SIZE, PCI_DMA_BIDIRECTIONAL);
	return (0);
}

static void
nvkm_ttm_tt_unpopulate(struct ttm_tt *ttm)
{
	struct nvkm_ttm *nvttm = nvkm_ttm_from_bdev(ttm->bdev);
	struct pci_dev *pdev = nvttm->sc->drm_pdev;
	struct ttm_dma_tt *dma = (void *)ttm;

	for (unsigned long i = 0; i < ttm->num_pages; i++) {
		if (dma->dma_address[i] != 0) {
			pci_unmap_page(pdev, dma->dma_address[i], PAGE_SIZE,
			    PCI_DMA_BIDIRECTIONAL);
		}
		dma->dma_address[i] = 0;
	}
	ttm_pool_unpopulate(ttm);
}

static int
nvkm_ttm_invalidate_caches(struct ttm_bo_device *bdev, uint32_t flags)
{
	(void)bdev;
	(void)flags;
	return (0);
}

static int
nvkm_ttm_init_mem_type(struct ttm_bo_device *bdev, uint32_t type,
    struct ttm_mem_type_manager *man)
{
	struct nvkm_ttm *ttm = nvkm_ttm_from_bdev(bdev);
	struct nvkm_softc *sc = ttm->sc;

	switch (type) {
	case TTM_PL_SYSTEM:
		man->flags = TTM_MEMTYPE_FLAG_MAPPABLE;
		man->available_caching = TTM_PL_MASK_CACHING;
		man->default_caching = TTM_PL_FLAG_CACHED;
		break;
	case TTM_PL_TT:
		man->func = &ttm_bo_manager_func;
		man->gpu_offset = 0;
		man->available_caching = TTM_PL_MASK_CACHING;
		man->default_caching = TTM_PL_FLAG_CACHED;
		man->flags = TTM_MEMTYPE_FLAG_MAPPABLE | TTM_MEMTYPE_FLAG_CMA;
		break;
	case TTM_PL_VRAM:
		man->func = &nvkm_ttm_vram_manager_func;
		man->gpu_offset = sc->fb_usable_base;
		man->flags = TTM_MEMTYPE_FLAG_FIXED |
		    TTM_MEMTYPE_FLAG_MAPPABLE;
		man->available_caching = TTM_PL_FLAG_UNCACHED | TTM_PL_FLAG_WC;
		man->default_caching = TTM_PL_FLAG_WC;
		break;
	default:
		device_printf(sc->dev,
		    "nvkm_ttm: unsupported memory type %u\n", type);
		return (-EINVAL);
	}
	return (0);
}

static void
nvkm_ttm_move_null(struct ttm_buffer_object *tbo, struct ttm_mem_reg *new_mem)
{
	struct ttm_mem_reg old_mem = tbo->mem;

	tbo->mem = *new_mem;
	new_mem->mm_node = NULL;
	ttm_bo_mem_put(tbo, &old_mem);
}

static int
nvkm_ttm_bo_move(struct ttm_buffer_object *tbo, bool evict,
    struct ttm_operation_ctx *ctx, struct ttm_mem_reg *new_mem)
{
	struct nvkm_bo *bo = container_of(tbo, struct nvkm_bo, tbo);
	struct ttm_mem_reg *old_mem = &tbo->mem;
	int err;

	(void)evict;
	if (old_mem->mem_type == TTM_PL_SYSTEM && tbo->ttm == NULL) {
		nvkm_ttm_move_null(tbo, new_mem);
		nvkm_ttm_sync_bo_domain(bo);
		return (0);
	}
	if ((old_mem->mem_type == TTM_PL_TT &&
	     new_mem->mem_type == TTM_PL_SYSTEM) ||
	    (old_mem->mem_type == TTM_PL_SYSTEM &&
	     new_mem->mem_type == TTM_PL_TT)) {
		err = ttm_bo_move_ttm(tbo, ctx, new_mem);
		if (err == 0)
			nvkm_ttm_sync_bo_domain(bo);
		return (err);
	}

	err = ttm_bo_move_memcpy(tbo, ctx, new_mem);
	if (err == 0)
		nvkm_ttm_sync_bo_domain(bo);
	return (err);
}

static void
nvkm_ttm_evict_flags(struct ttm_buffer_object *bo,
    struct ttm_placement *placement)
{
	static const struct ttm_place system = {
		.fpfn = 0,
		.lpfn = 0,
		.flags = TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED,
	};

	(void)bo;
	placement->placement = &system;
	placement->busy_placement = &system;
	placement->num_placement = 1;
	placement->num_busy_placement = 1;
}

static int
nvkm_ttm_verify_access(struct ttm_buffer_object *bo, struct file *filp)
{
	(void)bo;
	(void)filp;
	return (0);
}

static int
nvkm_ttm_io_mem_reserve(struct ttm_bo_device *bdev,
    struct ttm_mem_reg *mem)
{
	struct nvkm_ttm *ttm = nvkm_ttm_from_bdev(bdev);
	struct nvkm_softc *sc = ttm->sc;
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];
	struct nvkm_vram_alloc *alloc;
	int err;

	mem->bus.addr = NULL;
	mem->bus.offset = 0;
	mem->bus.size = mem->num_pages << PAGE_SHIFT;
	mem->bus.base = 0;
	mem->bus.is_iomem = false;
	if ((man->flags & TTM_MEMTYPE_FLAG_MAPPABLE) == 0)
		return (-EINVAL);

	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:
	case TTM_PL_TT:
		return (0);
	case TTM_PL_VRAM:
		if (mem->mm_node == NULL || sc->bar_res[1] == NULL)
			return (-EINVAL);
		alloc = container_of(mem->mm_node, struct nvkm_vram_alloc, node);
		if (alloc->bar1_gva == 0) {
			err = nvkm_gsp_bar1_map_existing_range(sc, alloc->paddr,
			    mem->bus.size, &alloc->bar1_gva);
			if (err != 0)
				return (err < 0 ? err : -err);
			alloc->bar1_size = mem->bus.size;
		}
		mem->bus.offset = alloc->bar1_gva;
		mem->bus.base = rman_get_start(sc->bar_res[1]);
		mem->bus.is_iomem = true;
		return (0);
	default:
		return (-EINVAL);
	}
}

static void
nvkm_ttm_io_mem_free(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem)
{
	(void)bdev;
	(void)mem;
}

static struct ttm_bo_driver nvkm_ttm_bo_driver = {
	.ttm_tt_create = nvkm_ttm_tt_create,
	.ttm_tt_populate = nvkm_ttm_tt_populate,
	.ttm_tt_unpopulate = nvkm_ttm_tt_unpopulate,
	.invalidate_caches = nvkm_ttm_invalidate_caches,
	.init_mem_type = nvkm_ttm_init_mem_type,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.evict_flags = nvkm_ttm_evict_flags,
	.move = nvkm_ttm_bo_move,
	.verify_access = nvkm_ttm_verify_access,
	.io_mem_reserve = nvkm_ttm_io_mem_reserve,
	.io_mem_free = nvkm_ttm_io_mem_free,
};

int
nvkm_ttm_init(struct nvkm_softc *sc, struct drm_device *ddev)
{
	struct nvkm_ttm *ttm;
	uint64_t tt_size;
	uint64_t vram_size;
	int err;

	ttm = kzalloc(sizeof(*ttm), GFP_KERNEL);
	if (ttm == NULL)
		return (ENOMEM);
	ttm->sc = sc;

	err = nvkm_ttm_global_init(ttm);
	if (err != 0)
		goto fail;

	err = ttm_bo_device_init(&ttm->bdev,
	    ttm->bo_global_ref.ref.object, &nvkm_ttm_bo_driver, NULL,
	    DRM_FILE_PAGE_OFFSET, false);
	if (err != 0)
		goto fail_global;
	ttm->bdev_initialized = true;
	ttm->bdev.no_retry = true;

	vram_size = sc->fb_usable_size;
	if (vram_size != 0) {
		err = ttm_bo_init_mm(&ttm->bdev, TTM_PL_VRAM,
		    vram_size >> PAGE_SHIFT);
		if (err != 0)
			goto fail_bdev;
		ttm->vram_initialized = true;
	}

	tt_size = sc->fb_usable_size;
	if (tt_size == 0)
		tt_size = 256ULL << 20;
	err = ttm_bo_init_mm(&ttm->bdev, TTM_PL_TT, tt_size >> PAGE_SHIFT);
	if (err != 0)
		goto fail_vram;
	ttm->tt_initialized = true;

	sc->ttm = ttm;
	ddev->drm_ttm_bdev = &ttm->bdev;
	device_printf(sc->dev,
	    "nvkm_ttm: VRAM manager size=0x%llx TT manager size=0x%llx\n",
	    (unsigned long long)vram_size, (unsigned long long)tt_size);
	return (0);

fail_vram:
	if (ttm->vram_initialized)
		ttm_bo_clean_mm(&ttm->bdev, TTM_PL_VRAM);
fail_bdev:
	(void)ttm_bo_device_release(&ttm->bdev);
fail_global:
	nvkm_ttm_global_fini(ttm);
fail:
	kfree(ttm);
	return (err < 0 ? -err : err);
}

void
nvkm_ttm_fini(struct nvkm_softc *sc)
{
	struct nvkm_ttm *ttm = sc->ttm;

	if (ttm == NULL)
		return;
	if (sc->drm_dev != NULL && sc->drm_dev->drm_ttm_bdev == &ttm->bdev)
		sc->drm_dev->drm_ttm_bdev = NULL;
	if (ttm->tt_initialized)
		ttm_bo_clean_mm(&ttm->bdev, TTM_PL_TT);
	if (ttm->vram_initialized)
		ttm_bo_clean_mm(&ttm->bdev, TTM_PL_VRAM);
	if (ttm->bdev_initialized)
		(void)ttm_bo_device_release(&ttm->bdev);
	nvkm_ttm_global_fini(ttm);
	sc->ttm = NULL;
	kfree(ttm);
}

struct ttm_bo_device *
nvkm_ttm_bo_device(struct nvkm_softc *sc)
{
	if (sc == NULL || sc->ttm == NULL)
		return (NULL);
	return (&sc->ttm->bdev);
}
