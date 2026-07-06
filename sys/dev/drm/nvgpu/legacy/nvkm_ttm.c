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
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <drm/ttm/ttm_bo_api.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_memory.h>
#include <drm/ttm/ttm_page_alloc.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>

#define DRM_FILE_PAGE_OFFSET (0x100000000ULL >> PAGE_SHIFT)
#define NVKM_TTM_MOVE_BAR1_CHUNK_SIZE (8ULL << 20)

struct nvkm_ttm {
	struct nvkm_softc *sc;
	struct ttm_bo_device bdev;
	struct ttm_bo_global_ref bo_global_ref;
	struct drm_global_reference mem_global_ref;
	struct lwkt_token last_bound_move_token;
	struct ttm_buffer_object *last_bound_move_bo;
	bool mem_global_referenced;
	bool bdev_initialized;
	bool tt_initialized;
	bool vram_initialized;
};

struct nvkm_ttm_tt {
	struct ttm_dma_tt ttm;
	struct nvkm_dmamem contig;
	bus_size_t contig_alignment;
	unsigned long contig_accounted_pages;
	bool prefer_contig;
	bool contig_populated;
};

struct nvkm_ttm_lru_sample {
	uint64_t count;
	uint64_t no_evict_count;
	uint64_t live_count;
	uint64_t reserve_ok_count;
	uint64_t reserve_busy_count;
};

static bool nvkm_ttm_tt_unpopulate_contig(struct nvkm_ttm *nvttm,
    struct nvkm_ttm_tt *ntt);

static struct nvkm_ttm *
nvkm_ttm_from_bdev(struct ttm_bo_device *bdev)
{
	return (container_of(bdev, struct nvkm_ttm, bdev));
}

/*
 * nvkm_ttm_last_bound_move_set()
 *
 * Ownership:
 *   Takes one TTM BO reference for the debug slot and releases the previous
 *   slot reference after publishing the replacement.  The caller keeps owning
 *   its reservation and any placement transition in progress.
 *
 * Lifetime:
 *   The debug slot keeps the BO alive only so a later test sysctl can call
 *   DragonFly TTM validation on the same object.  It does not pin placement,
 *   bypass TTM LRU membership, or extend GPUVA mapping ownership.
 *
 * Threading:
 *   The slot pointer is serialized by last_bound_move_token.  ttm_bo_put() is
 *   intentionally run after dropping the token because destruction may sleep.
 */
static void
nvkm_ttm_last_bound_move_set(struct nvkm_ttm *ttm,
    struct ttm_buffer_object *tbo)
{
	struct ttm_buffer_object *old;

	ttm_bo_get(tbo);
	lwkt_gettoken(&ttm->last_bound_move_token);
	old = ttm->last_bound_move_bo;
	ttm->last_bound_move_bo = tbo;
	lwkt_reltoken(&ttm->last_bound_move_token);
	if (old != NULL)
		ttm_bo_put(old);
	ttm->sc->ttm_last_bound_move_capture_count++;
}

static struct ttm_buffer_object *
nvkm_ttm_last_bound_move_get(struct nvkm_ttm *ttm)
{
	struct ttm_buffer_object *tbo;

	lwkt_gettoken(&ttm->last_bound_move_token);
	tbo = ttm->last_bound_move_bo;
	if (tbo != NULL)
		ttm_bo_get(tbo);
	lwkt_reltoken(&ttm->last_bound_move_token);
	return (tbo);
}

static void
nvkm_ttm_last_bound_move_clear(struct nvkm_ttm *ttm)
{
	struct ttm_buffer_object *old;

	lwkt_gettoken(&ttm->last_bound_move_token);
	old = ttm->last_bound_move_bo;
	ttm->last_bound_move_bo = NULL;
	lwkt_reltoken(&ttm->last_bound_move_token);
	if (old != NULL) {
		ttm->sc->ttm_last_bound_move_clear_count++;
		ttm_bo_put(old);
	}
}

/*
 * nvkm_ttm_last_bound_move_clear_if()
 *
 * Ownership:
 *   Borrows ttm and tbo.  If the debug slot currently owns a reference to the
 *   same TTM BO, this helper consumes only that slot reference.
 *
 * Lifetime:
 *   Used after a normal TTM validate path has already handled the same object,
 *   so the debug slot must not keep the BO alive after ordinary userspace
 *   closes its GEM handle.
 *
 * Threading:
 *   Serializes slot pointer mutation with last_bound_move_token.  ttm_bo_put()
 *   runs after the token is released because BO destruction may sleep.
 */
static void
nvkm_ttm_last_bound_move_clear_if(struct nvkm_ttm *ttm,
    struct ttm_buffer_object *tbo)
{
	struct ttm_buffer_object *old = NULL;

	lwkt_gettoken(&ttm->last_bound_move_token);
	if (ttm->last_bound_move_bo == tbo) {
		old = ttm->last_bound_move_bo;
		ttm->last_bound_move_bo = NULL;
	}
	lwkt_reltoken(&ttm->last_bound_move_token);
	if (old != NULL) {
		ttm->sc->ttm_last_bound_move_clear_count++;
		ttm_bo_put(old);
	}
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
	struct nvkm_ttm *nvttm = nvkm_ttm_from_bdev(ttm->bdev);
	struct nvkm_ttm_tt *ntt = (void *)ttm;

	(void)nvkm_ttm_tt_unpopulate_contig(nvttm, ntt);
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
	struct nvkm_bo *nbo = container_of(bo, struct nvkm_bo, tbo);
	struct nvkm_ttm_tt *ntt;
	uint64_t alignment;

	ntt = kzalloc(sizeof(*ntt), GFP_KERNEL);
	if (ntt == NULL)
		return (NULL);
	ntt->ttm.ttm.func = &nvkm_ttm_backend_func;
	if (ttm_dma_tt_init(&ntt->ttm, bo, page_flags) != 0) {
		kfree(ntt);
		return (NULL);
	}
	alignment = (uint64_t)bo->mem.page_alignment << PAGE_SHIFT;
	if ((nbo->domain & NOUVEAU_GEM_DOMAIN_GART) != 0 &&
	    alignment >= NVKM_GMMU_LPT_PAGE_SIZE &&
	    bo->num_pages >= (NVKM_GMMU_LPT_PAGE_SIZE >> PAGE_SHIFT)) {
		ntt->prefer_contig = true;
		ntt->contig_alignment = (bus_size_t)alignment;
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
	if (size >= NVKM_GMMU_LPT_PAGE_SIZE &&
	    align < NVKM_GMMU_LPT_PAGE_SIZE)
		align = NVKM_GMMU_LPT_PAGE_SIZE;
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

/*
 * nvkm_ttm_tt_populate_contig()
 *
 * Ownership:
 *   ntt owns the nvkm_dmamem allocation after success.  The helper borrows the
 *   TTM page and DMA-address arrays and fills them with aliases of that single
 *   contiguous DMA allocation; it does not transfer dmamem ownership to TTM.
 *
 * Lifetime:
 *   The contiguous allocation remains valid until
 *   nvkm_ttm_tt_unpopulate_contig().  Each filled page is also charged to TTM
 *   mem_global so BO accounting stays visible to the common TTM limits.
 *
 * Threading:
 *   Called from the TTM populate path while the BO is reserved.  It may sleep
 *   in bus_dma allocation and TTM memory accounting, but it does not touch GPU
 *   page tables or VM_BIND state.
 */
static int
nvkm_ttm_tt_populate_contig(struct nvkm_ttm *nvttm, struct nvkm_ttm_tt *ntt,
    struct ttm_operation_ctx *ctx)
{
	struct ttm_tt *ttm = &ntt->ttm.ttm;
	struct ttm_mem_global *mem_glob = ttm->bdev->glob->mem_glob;
	bus_size_t size = (bus_size_t)ttm->num_pages << PAGE_SHIFT;
	int err;

	if (!ntt->prefer_contig || ntt->contig_populated ||
	    ttm->num_pages > (ULONG_MAX >> PAGE_SHIFT))
		return (EINVAL);

	err = nvkm_dmamem_alloc(nvttm->sc, size, ntt->contig_alignment,
	    &ntt->contig);
	if (err != 0)
		return (err);

	for (unsigned long i = 0; i < ttm->num_pages; i++) {
		struct page *page;
		vm_paddr_t paddr = (vm_paddr_t)ntt->contig.paddr +
		    ((vm_paddr_t)i << PAGE_SHIFT);

		page = (struct page *)PHYS_TO_VM_PAGE(paddr);
		err = ttm_mem_global_alloc_page(mem_glob, page, PAGE_SIZE,
		    ctx);
		if (err != 0) {
			for (unsigned long j = 0; j < i; j++) {
				if (ttm->pages[j] != NULL) {
					ttm_mem_global_free_page(mem_glob,
					    ttm->pages[j], PAGE_SIZE);
				}
				ttm->pages[j] = NULL;
				ntt->ttm.dma_address[j] = 0;
			}
			nvkm_dmamem_free(nvttm->sc, &ntt->contig);
			ntt->contig_accounted_pages = 0;
			return (err);
		}
		ttm->pages[i] = page;
		ntt->ttm.dma_address[i] = (dma_addr_t)paddr;
		ntt->contig_accounted_pages++;
	}

	ttm->state = tt_unbound;
	ntt->contig_populated = true;
	return (0);
}

/*
 * nvkm_ttm_tt_unpopulate_contig()
 *
 * Ownership:
 *   Consumes ntt's owned contiguous DMA allocation, clears the borrowed TTM
 *   page/DMA arrays, and returns TTM mem_global charges.
 *
 * Lifetime:
 *   After return, no TTM page or DMA-address slot references the old
 *   allocation and the TT is back in tt_unpopulated state.
 *
 * Threading:
 *   Called from the TTM unpopulate/destroy path while the BO is no longer
 *   bound.  It does not acquire GPU VM tokens and does not touch hardware PTEs.
 */
static bool
nvkm_ttm_tt_unpopulate_contig(struct nvkm_ttm *nvttm,
    struct nvkm_ttm_tt *ntt)
{
	struct ttm_tt *ttm = &ntt->ttm.ttm;
	struct ttm_mem_global *mem_glob = ttm->bdev->glob->mem_glob;

	if (!ntt->contig_populated)
		return (false);

	for (unsigned long i = 0; i < ntt->contig_accounted_pages; i++) {
		if (ttm->pages[i] != NULL)
			ttm_mem_global_free_page(mem_glob, ttm->pages[i],
			    PAGE_SIZE);
		ttm->pages[i] = NULL;
		ntt->ttm.dma_address[i] = 0;
	}
	ntt->contig_accounted_pages = 0;
	nvkm_dmamem_free(nvttm->sc, &ntt->contig);
	ntt->contig_populated = false;
	ttm->state = tt_unpopulated;
	return (true);
}

static int
nvkm_ttm_tt_populate(struct ttm_tt *ttm, struct ttm_operation_ctx *ctx)
{
	struct nvkm_ttm *nvttm = nvkm_ttm_from_bdev(ttm->bdev);
	struct pci_dev *pdev = nvttm->sc->drm_pdev;
	struct nvkm_ttm_tt *ntt = (void *)ttm;
	struct ttm_dma_tt *dma = (void *)ttm;
	int err;

	if (ttm->state != tt_unpopulated)
		return (0);

	if (ntt->prefer_contig) {
		err = nvkm_ttm_tt_populate_contig(nvttm, ntt, ctx);
		if (err == 0)
			return (0);
	}

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
	struct nvkm_ttm_tt *ntt = (void *)ttm;
	struct ttm_dma_tt *dma = (void *)ttm;

	if (nvkm_ttm_tt_unpopulate_contig(nvttm, ntt))
		return;

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
		/*
		 * Ownership:
		 *   TTM owns the io-reserve lifetime for CPU access to VRAM.
		 *   nvkm only lends a BAR1 GVA while TTM keeps the reservation.
		 *
		 * Lifetime:
		 *   Dynamic BAR1 mappings are released by nvkm_ttm_io_mem_free().
		 *   Keeping the default fastpath would skip io_mem_free entirely
		 *   and turn temporary TTM move mappings into long-lived BAR1
		 *   consumers.
		 *
		 * Threading:
		 *   TTM serializes reserve/free through the memory manager's
		 *   io_reserve_mutex when fastpath is disabled.  The LRU lets TTM
		 *   evict stale CPU mappings and retry when BAR1 is exhausted.
		 */
		man->io_reserve_fastpath = false;
		man->use_io_reserve_lru = true;
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
nvkm_ttm_move_chunk_copy_page(struct ttm_tt *ttm, void *iomap,
    unsigned long page_index, unsigned long chunk_page, bool vram_to_sysmem)
{
	struct page *page;
	void *kva;
	void *io_page;

	if (ttm == NULL || ttm->pages == NULL || page_index >= ttm->num_pages)
		return (-ENXIO);
	page = ttm->pages[page_index];
	if (page == NULL)
		return (-ENOMEM);

	io_page = (uint8_t *)iomap + (chunk_page << PAGE_SHIFT);
	kva = kmap(page);
	if (kva == NULL)
		return (-ENOMEM);
	if (vram_to_sysmem)
		memcpy_fromio(kva, io_page, PAGE_SIZE);
	else
		memcpy_toio(io_page, kva, PAGE_SIZE);
	kunmap(page);
	return (0);
}

/*
 * nvkm_ttm_move_chunked_bar1()
 *
 * Ownership:
 *   Borrows the TTM BO reservation, old/new TTM placements, and the caller's
 *   GPUVA snapshot.  TTM still owns placement selection, TT allocation, and the
 *   final BO memory record.  nvkm only borrows short-lived BAR1 windows for
 *   copying VRAM pages.
 *
 * Lifetime:
 *   The old placement remains published until the copy and any required GPUVA
 *   rebind have both succeeded.  That keeps every failure before the
 *   tbo->mem/new_mem ownership transfer recoverable by common TTM.  On
 *   success this helper consumes new_mem's mm_node exactly like
 *   ttm_bo_move_memcpy().
 *
 * Threading:
 *   Called from the TTM move callback while the BO is reserved and after common
 *   TTM has waited or serialized against conflicting BO users.  BAR1 mappings
 *   are private to this call and are released before returning.
 */
static int
nvkm_ttm_move_chunked_bar1(struct ttm_buffer_object *tbo,
    struct ttm_operation_ctx *ctx, struct ttm_mem_reg *new_mem,
    const struct nvkm_drm_bo_vm_snapshot *snapshot,
    struct dma_fence *move_fence, bool *rebound)
{
	struct nvkm_bo *bo = container_of(tbo, struct nvkm_bo, tbo);
	struct nvkm_ttm *ttm = nvkm_ttm_from_bdev(tbo->bdev);
	struct nvkm_softc *sc = ttm->sc;
	struct ttm_mem_reg old_copy = tbo->mem;
	struct nvkm_vram_alloc *vram_alloc;
	bool vram_to_sysmem;
	uint64_t size;
	uint64_t done;
	int err;

	*rebound = false;
	if (old_copy.mem_type == TTM_PL_VRAM &&
	    new_mem->mem_type == TTM_PL_SYSTEM) {
		vram_to_sysmem = true;
		if (old_copy.mm_node == NULL || tbo->ttm == NULL)
			return (-ENXIO);
		vram_alloc = container_of(old_copy.mm_node,
		    struct nvkm_vram_alloc, node);
	} else if (old_copy.mem_type == TTM_PL_SYSTEM &&
	    new_mem->mem_type == TTM_PL_VRAM) {
		vram_to_sysmem = false;
		if (new_mem->mm_node == NULL || tbo->ttm == NULL)
			return (-ENXIO);
		vram_alloc = container_of(new_mem->mm_node,
		    struct nvkm_vram_alloc, node);
	} else {
		return (-EOPNOTSUPP);
	}

	sc->ttm_move_bo_wait_count++;
	sc->ttm_move_bo_wait_last_interruptible =
	    ctx->interruptible ? 1 : 0;
	sc->ttm_move_bo_wait_last_no_wait =
	    ctx->no_wait_gpu ? 1 : 0;
	sc->ttm_move_bo_wait_last_no_share = bo->no_share ? 1 : 0;
	sc->ttm_move_bo_wait_last_resv_is_ttm =
	    (tbo->resv == &tbo->ttm_resv) ? 1 : 0;
	err = ttm_bo_wait(tbo, ctx->interruptible, ctx->no_wait_gpu);
	if (err != 0) {
		sc->ttm_move_bo_wait_error_count++;
		sc->ttm_move_bo_wait_last_error = err;
		return (err);
	}
	sc->ttm_move_bo_wait_last_error = 0;

	err = ttm_tt_populate(tbo->ttm, ctx);
	if (err != 0)
		return (err);

	size = (uint64_t)new_mem->num_pages << PAGE_SHIFT;
	if (size > bo->base.size)
		size = bo->base.size;
	for (done = 0; done < size;) {
		uint64_t chunk = MIN(size - done, NVKM_TTM_MOVE_BAR1_CHUNK_SIZE);
		uint64_t gva = 0;
		void *iomap;

		chunk = round_page(chunk);
		if (done + chunk > size)
			chunk = size - done;
		err = nvkm_gsp_bar1_map_existing_range(sc,
		    vram_alloc->paddr + done, chunk, &gva);
		if (err != 0)
			return (err < 0 ? err : -err);
		iomap = ioremap_wc(rman_get_start(sc->bar_res[1]) + gva,
		    chunk);
		if (iomap == NULL) {
			nvkm_gsp_bar1_unmap_existing_range(sc, gva, chunk);
			return (-ENOMEM);
		}

		for (uint64_t off = 0; off < chunk; off += PAGE_SIZE) {
			unsigned long page_index =
			    (unsigned long)((done + off) >> PAGE_SHIFT);
			unsigned long chunk_page =
			    (unsigned long)(off >> PAGE_SHIFT);

			err = nvkm_ttm_move_chunk_copy_page(tbo->ttm, iomap,
			    page_index, chunk_page, vram_to_sysmem);
			if (err != 0)
				break;
		}
		mb();
		iounmap(iomap);
		nvkm_gsp_bar1_unmap_existing_range(sc, gva, chunk);
		if (err != 0)
			return (err);
		done += chunk;
	}

	if (snapshot != NULL && snapshot->count != 0) {
		err = nvkm_drm_bo_vm_snapshot_rebind(bo, snapshot, new_mem,
		    false);
		if (err != 0)
			return (err);
		(void)nvkm_drm_bo_publish_ttm_move_fence(bo, move_fence);
		*rebound = true;
	}

	tbo->mem = *new_mem;
	new_mem->mm_node = NULL;
	if (vram_to_sysmem) {
		bo->vram_alloc = NULL;
		if (bo->paddr == vram_alloc->paddr)
			bo->paddr = 0;
	} else {
		bo->vram_alloc = vram_alloc;
		bo->paddr = vram_alloc->paddr;
		ttm_tt_destroy(tbo->ttm);
		tbo->ttm = NULL;
	}
	ttm_bo_mem_put(tbo, &old_copy);
	return (0);
}

static int
nvkm_ttm_bo_move(struct ttm_buffer_object *tbo, bool evict,
    struct ttm_operation_ctx *ctx, struct ttm_mem_reg *new_mem)
{
	struct nvkm_bo *bo = container_of(tbo, struct nvkm_bo, tbo);
	struct nvkm_ttm *ttm = nvkm_ttm_from_bdev(tbo->bdev);
	struct nvkm_softc *sc = ttm->sc;
	struct ttm_mem_reg *old_mem = &tbo->mem;
	struct nvkm_drm_bo_vm_snapshot snapshot;
	struct dma_fence *move_fence = NULL;
	uint32_t live_mappings;
	bool chunked_vram_move;
	bool rebound = false;
	int err;

	(void)evict;
	nvkm_drm_bo_vm_snapshot_init(&snapshot);
	err = nvkm_drm_bo_vm_snapshot_collect(bo, &snapshot);
	if (err != 0) {
		device_printf(sc->dev,
		    "nvkm_ttm: failed to snapshot BO GPUVA mappings obj=%p err=%d old_type=%u new_type=%u\n",
		    &bo->base, err, old_mem->mem_type, new_mem->mem_type);
		nvkm_drm_bo_vm_snapshot_fini(&snapshot);
		return (err);
	}
	live_mappings = snapshot.count;
	chunked_vram_move = (old_mem->mem_type == TTM_PL_VRAM &&
	    new_mem->mem_type == TTM_PL_SYSTEM) ||
	    (old_mem->mem_type == TTM_PL_SYSTEM &&
	    new_mem->mem_type == TTM_PL_VRAM);
	if (live_mappings != 0) {
		sc->ttm_move_bound_attempt_count++;
		if (sc->ttm_bound_rebind_test_enable == 0) {
			sc->ttm_move_bound_reject_count++;
			device_printf(sc->dev,
			    "nvkm_ttm: rejecting bound BO move obj=%p live_gpuva=%u old_type=%u new_type=%u\n",
			    &bo->base, live_mappings, old_mem->mem_type,
			    new_mem->mem_type);
			nvkm_drm_bo_vm_snapshot_fini(&snapshot);
			return (-EBUSY);
		}
		if (!chunked_vram_move) {
			sc->ttm_move_bound_reject_count++;
			device_printf(sc->dev,
			    "nvkm_ttm: rejecting bound BO move without no-fail rebind obj=%p live_gpuva=%u old_type=%u new_type=%u\n",
			    &bo->base, live_mappings, old_mem->mem_type,
			    new_mem->mem_type);
			nvkm_drm_bo_vm_snapshot_fini(&snapshot);
			return (-EBUSY);
		}
		err = nvkm_drm_bo_create_ttm_move_fence(bo, &move_fence);
		if (err != 0) {
			nvkm_drm_bo_vm_snapshot_fini(&snapshot);
			return (err);
		}
		err = nvkm_drm_bo_vm_snapshot_wait_exec_resv(sc, &snapshot,
		    ctx->interruptible, ctx->no_wait_gpu);
		if (err != 0) {
			sc->ttm_move_bound_reject_count++;
			dma_fence_put(move_fence);
			nvkm_drm_bo_vm_snapshot_fini(&snapshot);
			return (err);
		}
	}

	if (old_mem->mem_type == TTM_PL_SYSTEM && tbo->ttm == NULL) {
		nvkm_ttm_move_null(tbo, new_mem);
		err = 0;
	} else if ((old_mem->mem_type == TTM_PL_TT &&
	    new_mem->mem_type == TTM_PL_SYSTEM) ||
	    (old_mem->mem_type == TTM_PL_SYSTEM &&
	    new_mem->mem_type == TTM_PL_TT)) {
		err = ttm_bo_move_ttm(tbo, ctx, new_mem);
	} else if ((old_mem->mem_type == TTM_PL_VRAM &&
	    new_mem->mem_type == TTM_PL_SYSTEM) ||
	    (old_mem->mem_type == TTM_PL_SYSTEM &&
	    new_mem->mem_type == TTM_PL_VRAM)) {
		err = nvkm_ttm_move_chunked_bar1(tbo, ctx, new_mem,
		    live_mappings != 0 ? &snapshot : NULL, move_fence,
		    &rebound);
	} else {
		err = ttm_bo_move_memcpy(tbo, ctx, new_mem);
	}

	if (err == 0) {
		nvkm_ttm_sync_bo_domain(bo);
		if (live_mappings != 0 && !rebound) {
			/*
			 * TTM owns target population and publishes tbo->mem
			 * during the native move.  Rebind only after that point
			 * so SYSTEM/TT DMA addresses are valid snapshots.
			 */
			err = nvkm_drm_bo_vm_snapshot_rebind(bo, &snapshot,
			    &tbo->mem, false);
			if (err != 0) {
				sc->ttm_move_bound_reject_count++;
				device_printf(sc->dev,
				    "nvkm_ttm: failed post-move bound BO rebind obj=%p live_gpuva=%u err=%d new_type=%u\n",
				    &bo->base, live_mappings, err,
				    tbo->mem.mem_type);
			} else {
				(void)nvkm_drm_bo_publish_ttm_move_fence(bo,
				    move_fence);
			}
		}
		if (live_mappings != 0 && err == 0)
			nvkm_ttm_last_bound_move_set(ttm, tbo);
	}
	dma_fence_put(move_fence);
	nvkm_drm_bo_vm_snapshot_fini(&snapshot);
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
	uint32_t pages;
	int err;

	mem->bus.addr = NULL;
	mem->bus.offset = 0;
	mem->bus.size = mem->num_pages << PAGE_SHIFT;
	mem->bus.base = 0;
	mem->bus.is_iomem = false;
	sc->ttm_io_reserve_count++;
	sc->ttm_io_reserve_last_size = mem->bus.size;
	sc->ttm_io_reserve_last_error = 0;
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
		pages = (uint32_t)mem->num_pages;
		if (pages == 0 || (uint64_t)pages != mem->num_pages)
			return (-EINVAL);
		if (alloc->bar1_page_gva == NULL) {
			/*
			 * Ownership:
			 *   TTM owns the CPU mapping lifetime through io_mem_reserve/free.
			 *   The nvkm_vram_alloc owns the scatter BAR1 GVA table while that
			 *   lifetime is active; each entry is a stable loan for one user
			 *   PTE returned by nvkm_ttm_io_mem_pfn().
			 *
			 * Lifetime:
			 *   The table survives until TTM drops the io reserve or the VRAM
			 *   allocation is freed.  It deliberately does not require a
			 *   contiguous BAR1 range, because user mmap PTEs only need stable
			 *   per-page PFNs, not adjacent BAR1 addresses.
			 *
			 * Threading:
			 *   The TTM io_reserve_mutex serializes reserve/free and fault-time
			 *   PFN lookup for this memory manager.  The table is not touched
			 *   from interrupt context.
			 */
			alloc->bar1_page_gva = kzalloc(
			    (size_t)pages * sizeof(*alloc->bar1_page_gva),
			    GFP_KERNEL);
			if (alloc->bar1_page_gva == NULL) {
				sc->ttm_io_reserve_error_count++;
				sc->ttm_io_reserve_last_error = -ENOMEM;
				return (-ENOMEM);
			}
			alloc->bar1_page_count = pages;
			err = nvkm_gsp_bar1_map_existing_scatter(sc,
			    alloc->paddr, mem->bus.size, alloc->bar1_page_gva,
			    alloc->bar1_page_count);
			if (err != 0) {
				err = err < 0 ? err : -err;
				kfree(alloc->bar1_page_gva);
				alloc->bar1_page_gva = NULL;
				alloc->bar1_page_count = 0;
				sc->ttm_io_reserve_error_count++;
				sc->ttm_io_reserve_last_error = err;
				if (err == -ENOSPC) {
					sc->ttm_io_reserve_bar1_retry_count++;
					return (-EAGAIN);
				}
				return (err);
			}
			alloc->bar1_size = mem->bus.size;
		} else if (alloc->bar1_page_count != pages ||
		    alloc->bar1_size != mem->bus.size) {
			sc->ttm_io_reserve_error_count++;
			sc->ttm_io_reserve_last_error = -EINVAL;
			return (-EINVAL);
		}
		mem->bus.offset = alloc->bar1_page_gva[0];
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
	struct nvkm_ttm *ttm = nvkm_ttm_from_bdev(bdev);
	struct nvkm_softc *sc = ttm->sc;
	struct nvkm_vram_alloc *alloc;

	sc->ttm_io_free_count++;
	if (mem->mem_type != TTM_PL_VRAM || mem->mm_node == NULL)
		return;

	alloc = container_of(mem->mm_node, struct nvkm_vram_alloc, node);
	if (alloc->bar1_page_gva != NULL) {
		nvkm_gsp_bar1_unmap_existing_scatter(sc, alloc->bar1_page_gva,
		    alloc->bar1_page_count);
		kfree(alloc->bar1_page_gva);
		alloc->bar1_page_gva = NULL;
		alloc->bar1_page_count = 0;
		alloc->bar1_size = 0;
		sc->ttm_io_free_bar1_count++;
		return;
	}
	if (alloc->bar1_gva == 0)
		return;

	/*
	 * Ownership:
	 *   The nvkm_vram_alloc owns the physical VRAM allocation.  This
	 *   callback only consumes the temporary BAR1 GVA loan created by
	 *   nvkm_ttm_io_mem_reserve(); it does not release VRAM.
	 *
	 * Lifetime:
	 *   TTM calls this when the io-reserve count reaches zero or when the
	 *   io-reserve LRU drops a stale CPU mapping.  After return, mem->bus no
	 *   longer names a valid BAR1 aperture for this allocation.
	 *
	 * Threading:
	 *   Called under the TTM memory manager's io_reserve_mutex because the
	 *   VRAM manager disables io_reserve_fastpath.
	 */
	nvkm_gsp_bar1_unmap_existing_range(sc, alloc->bar1_gva,
	    alloc->bar1_size);
	alloc->bar1_gva = 0;
	alloc->bar1_size = 0;
	sc->ttm_io_free_bar1_count++;
}

/*
 * nvkm_ttm_io_mem_pfn()
 *
 * Ownership:
 *   Borrows the reserved TTM BO and the active nvkm_vram_alloc scatter BAR1
 *   table.  It returns a PFN snapshot and does not acquire a new BAR1 loan.
 *
 * Lifetime:
 *   The returned PFN is valid only while TTM keeps the io_mem_reserve lifetime
 *   active and the user VM mapping owns the corresponding PTE.
 *
 * Threading:
 *   Called from the TTM fault path while the BO is reserved and the VRAM memory
 *   manager io_reserve_mutex is held.  It must not sleep or mutate the table.
 */
static unsigned long
nvkm_ttm_io_mem_pfn(struct ttm_buffer_object *tbo, unsigned long page_offset)
{
	struct nvkm_bo *bo = container_of(tbo, struct nvkm_bo, tbo);
	struct nvkm_softc *sc = bo->base.dev->dev_private;
	struct nvkm_vram_alloc *alloc;
	uint64_t gva;

	if (tbo->mem.mem_type != TTM_PL_VRAM || tbo->mem.mm_node == NULL ||
	    sc->bar_res[1] == NULL)
		return (0);

	alloc = container_of(tbo->mem.mm_node, struct nvkm_vram_alloc, node);
	if (alloc->bar1_page_gva == NULL ||
	    page_offset >= alloc->bar1_page_count)
		return (0);

	gva = alloc->bar1_page_gva[page_offset];
	if (gva == 0)
		return (0);
	return ((unsigned long)((rman_get_start(sc->bar_res[1]) + gva) >>
	    PAGE_SHIFT));
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
	.io_mem_pfn = nvkm_ttm_io_mem_pfn,
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
	lwkt_token_init(&ttm->last_bound_move_token, "nvkm-ttm-last");

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
	nvkm_ttm_last_bound_move_clear(ttm);
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

/*
 * nvkm_ttm_sample_vram_lru()
 *
 * Ownership:
 *   Borrows sc and the DragonFly TTM VRAM manager.  It does not take BO
 *   references, does not remove BOs from LRU, and does not keep any pointer
 *   after returning.
 *
 * Lifetime:
 *   The returned scalar snapshot is valid only as a diagnostic sample.  BOs may
 *   enter or leave the LRU immediately after glob->lru_lock is released.
 *
 * Threading:
 *   Takes TTM's global LRU lock and briefly try-locks each BO reservation to
 *   mirror the selection boundary used by ttm_mem_evict_first().  A successful
 *   try-lock is released immediately.  This helper must never call into VM/GSP
 *   code or sleep while holding the LRU lock.
 */
static void
nvkm_ttm_sample_vram_lru(struct nvkm_softc *sc,
    struct nvkm_ttm_lru_sample *sample)
{
	struct ttm_bo_device *bdev;
	struct ttm_bo_global *glob;
	struct ttm_mem_type_manager *man;
	struct ttm_buffer_object *tbo;

	memset(sample, 0, sizeof(*sample));
	bdev = nvkm_ttm_bo_device(sc);
	if (bdev == NULL)
		return;
	man = &bdev->man[TTM_PL_VRAM];
	if (!man->has_type)
		return;
	glob = bdev->glob;

	lockmgr(&glob->lru_lock, LK_EXCLUSIVE);
	for (unsigned int i = 0; i < TTM_MAX_BO_PRIORITY; i++) {
		list_for_each_entry(tbo, &man->lru[i], lru) {
			struct nvkm_bo *bo;

			bo = container_of(tbo, struct nvkm_bo, tbo);
			sample->count++;
			if ((tbo->mem.placement & TTM_PL_FLAG_NO_EVICT) != 0)
				sample->no_evict_count++;
			/*
			 * Advisory only: do not take bo->vm_mapping_token while
			 * holding TTM's LRU lock.  The move callback performs the
			 * authoritative refcounted snapshot after TTM selects a BO.
			 */
			if (bo->vm_mapping_count != 0)
				sample->live_count++;
			if (reservation_object_trylock(tbo->resv)) {
				sample->reserve_ok_count++;
				reservation_object_unlock(tbo->resv);
			} else {
				sample->reserve_busy_count++;
			}
		}
	}
	lockmgr(&glob->lru_lock, LK_RELEASE);
}

static void
nvkm_ttm_store_evict_lru_before(struct nvkm_softc *sc,
    const struct nvkm_ttm_lru_sample *sample)
{
	sc->ttm_evict_lru_before_count = sample->count;
	sc->ttm_evict_lru_before_no_evict_count = sample->no_evict_count;
	sc->ttm_evict_lru_before_live_count = sample->live_count;
	sc->ttm_evict_lru_before_reserve_ok_count = sample->reserve_ok_count;
	sc->ttm_evict_lru_before_reserve_busy_count = sample->reserve_busy_count;
}

static void
nvkm_ttm_store_evict_lru_after(struct nvkm_softc *sc,
    const struct nvkm_ttm_lru_sample *sample)
{
	sc->ttm_evict_lru_after_count = sample->count;
	sc->ttm_evict_lru_after_no_evict_count = sample->no_evict_count;
	sc->ttm_evict_lru_after_live_count = sample->live_count;
	sc->ttm_evict_lru_after_reserve_ok_count = sample->reserve_ok_count;
	sc->ttm_evict_lru_after_reserve_busy_count = sample->reserve_busy_count;
}

/*
 * nvkm_ttm_evict_vram()
 *
 * Ownership:
 *   Borrows the nvkm TTM device from sc.  No BO reference, reservation, or
 *   placement ownership is acquired directly by nvkm; all object selection and
 *   movement is delegated to DragonFly DRM/TTM.
 *
 * Lifetime:
 *   The TTM device must be initialized for the whole call.  TTM owns the LRU
 *   walk, reservation handling, evict_flags callback, and move callback
 *   sequencing.
 *
 * Threading:
 *   May sleep inside TTM while reserving and moving BOs.  Callers must not hold
 *   nvkm VM, GSP, BO reverse-map, or display locks.
 */
int
nvkm_ttm_evict_vram(struct nvkm_softc *sc)
{
	struct ttm_bo_device *bdev;
	struct nvkm_ttm_lru_sample sample;
	int err;

	bdev = nvkm_ttm_bo_device(sc);
	if (bdev == NULL)
		return (-ENODEV);
	nvkm_ttm_sample_vram_lru(sc, &sample);
	nvkm_ttm_store_evict_lru_before(sc, &sample);
	sc->ttm_evict_lru_sample_count++;
	err = ttm_bo_evict_mm(bdev, TTM_PL_VRAM);
	nvkm_ttm_sample_vram_lru(sc, &sample);
	nvkm_ttm_store_evict_lru_after(sc, &sample);
	sc->ttm_evict_lru_last_error = err;
	sc->ttm_evict_lru_sample_count++;
	return (err);
}

/*
 * nvkm_ttm_validate_bo_preferred()
 *
 * Ownership:
 *   Borrows bo and asks DragonFly TTM to validate it back to the immutable
 *   placement preference captured at GEM creation.  TTM owns reservation,
 *   placement selection, eviction, and the move callback.  nvkm never mutates
 *   tbo->mem directly from this helper.
 *
 * Lifetime:
 *   bo must stay alive for the whole call.  On success, TTM has published the
 *   final placement and any move callback has already rebound live GPUVAs.
 *
 * Threading:
 *   May sleep while reserving or validating the BO.  Callers must not hold a
 *   drm_file VM token, bo->vm_mapping_token, gsp_tok, KMS locks, or TTM LRU
 *   locks; a TTM move callback can re-enter VM/GSP rebind code.
 */
int
nvkm_ttm_validate_bo_preferred(struct nvkm_bo *bo)
{
	static const struct ttm_place vram_place = {
		.fpfn = 0,
		.lpfn = 0,
		.flags = TTM_PL_FLAG_VRAM | TTM_PL_FLAG_WC,
	};
	struct ttm_placement placement = {
		.num_placement = 1,
		.placement = &vram_place,
		.num_busy_placement = 1,
		.busy_placement = &vram_place,
	};
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false,
	};
	struct ttm_buffer_object *tbo;
	int err;

	if (bo == NULL || !bo->ttm_backed)
		return (0);
	if (!nvkm_bo_ttm_prefers_vram(bo))
		return (0);

	tbo = &bo->tbo;
	err = ttm_bo_reserve(tbo, false, false, NULL);
	if (err != 0)
		return (err);
	err = ttm_bo_validate(tbo, &placement, &ctx);
	if (err == 0)
		nvkm_ttm_sync_bo_domain(bo);
	ttm_bo_unreserve(tbo);
	nvkm_ttm_last_bound_move_clear_if(nvkm_ttm_from_bdev(tbo->bdev), tbo);
	return (err);
}

/*
 * nvkm_ttm_validate_last_bound_to_vram()
 *
 * Ownership:
 *   Borrows sc, takes a temporary TTM BO reference from the debug slot, and
 *   lets DragonFly TTM own reservation, placement selection, eviction, and the
 *   move callback.  nvkm does not modify tbo->mem directly.
 *
 * Lifetime:
 *   The debug slot must have been populated by a successful live-bound TTM
 *   move.  This helper keeps that BO alive only for the duration of
 *   ttm_bo_validate(); the BO remains owned by GEM/TTM afterward.
 *
 * Threading:
 *   May sleep while reserving or validating the BO.  Callers must not hold
 *   nvkm VM, GSP, BO reverse-map, KMS, or TTM LRU locks.
 */
int
nvkm_ttm_validate_last_bound_to_vram(struct nvkm_softc *sc)
{
	struct nvkm_ttm *ttm;
	struct ttm_buffer_object *tbo;
	struct nvkm_bo *bo;
	int err;

	if (sc == NULL || sc->ttm == NULL)
		return (-ENODEV);

	ttm = sc->ttm;
	tbo = nvkm_ttm_last_bound_move_get(ttm);
	if (tbo == NULL) {
		sc->ttm_validate_vram_test_empty_count++;
		return (-ENOENT);
	}

	bo = container_of(tbo, struct nvkm_bo, tbo);
	err = nvkm_ttm_validate_bo_preferred(bo);

	nvkm_ttm_last_bound_move_clear(ttm);
	ttm_bo_put(tbo);
	return (err);
}

/*
 * nvkm_ttm_clear_last_bound_move()
 *
 * Ownership:
 *   Borrows sc and releases nvkm's debug-only TTM BO reference if one is
 *   currently stored.  GEM/TTM remain the owners of the BO and its placement.
 *
 * Lifetime:
 *   Intended as a cleanup escape hatch for tests that trigger a live-bound
 *   move but do not subsequently run validate.  It must not be required by
 *   normal userspace because the debug slot is populated only from test-gated
 *   live-bound TTM moves.
 *
 * Threading:
 *   May sleep if dropping the last debug reference lets TTM destroy the BO.
 *   Callers must not hold nvkm VM, GSP, BO reverse-map, KMS, or TTM LRU locks.
 */
int
nvkm_ttm_clear_last_bound_move(struct nvkm_softc *sc)
{
	if (sc == NULL || sc->ttm == NULL)
		return (-ENODEV);
	nvkm_ttm_last_bound_move_clear(sc->ttm);
	return (0);
}
