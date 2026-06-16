/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * nvkm BO layer — GART/VRAM BO backing.
 *
 * GEM BOs are TTM objects placed in TTM_PL_TT or TTM_PL_VRAM. VM_BIND maps
 * TT pages or VRAM physical addresses into the GPU VMM; CPU mmap enters the
 * DragonFly TTM pager.
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
#include <machine/vmparam.h>

#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/dma-fence.h>
#include <linux/ktime.h>
#include <linux/reservation.h>
#include <linux/sched.h>
#include <drm/drmP.h>
#include <drm/drm_gem.h>
#include <drm/drm_vma_manager.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>
#include <uapi/drm/drm_mode.h>

#include "nvkm_priv.h"
#include "nvkm_bo.h"
#include "nvkm_gsp_rm.h"
#include "nvkm_ttm.h"

static uint64_t
nvkm_bo_now_us(void)
{
	return ((uint64_t)ktime_to_us(ktime_get()));
}

static void
nvkm_bo_add_us(uint64_t *total, uint64_t start_us)
{
	uint64_t end_us = nvkm_bo_now_us();

	if (end_us >= start_us)
		*total += end_us - start_us;
}

static uint32_t
nvkm_bo_size_bucket(uint64_t size)
{
	static const uint64_t limits[NVKM_BO_SIZE_BUCKET_COUNT - 1] = {
		4ULL * 1024ULL,
		16ULL * 1024ULL,
		64ULL * 1024ULL,
		256ULL * 1024ULL,
		1024ULL * 1024ULL,
		4ULL * 1024ULL * 1024ULL,
	};

	for (uint32_t bucket = 0; bucket < NVKM_BO_SIZE_BUCKET_COUNT - 1;
	     bucket++) {
		if (size <= limits[bucket])
			return (bucket);
	}
	return (NVKM_BO_SIZE_BUCKET_COUNT - 1);
}

static void
nvkm_bo_record_gem_new_size(struct nvkm_softc *sc, const struct nvkm_bo *bo,
    bool mappable_req)
{
	uint32_t bucket = nvkm_bo_size_bucket(bo->base.size);

	sc->bo_gem_new_size_bucket[bucket]++;
	if (bo->domain & NOUVEAU_GEM_DOMAIN_GART)
		sc->bo_gem_new_gart_size_bucket[bucket]++;
	if (mappable_req)
		sc->bo_gem_new_mappable_size_bucket[bucket]++;
}

static void
nvkm_bo_record_gem_new_request(struct nvkm_softc *sc,
    const struct drm_nouveau_gem_info *info)
{
	uint32_t placement;

	placement = info->domain &
	    (NOUVEAU_GEM_DOMAIN_CPU | NOUVEAU_GEM_DOMAIN_VRAM |
	    NOUVEAU_GEM_DOMAIN_GART);
	if ((info->domain & NOUVEAU_GEM_DOMAIN_CPU) != 0)
		sc->bo_gem_new_req_cpu_count++;
	if ((info->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0)
		sc->bo_gem_new_req_vram_count++;
	if ((info->domain & NOUVEAU_GEM_DOMAIN_GART) != 0)
		sc->bo_gem_new_req_gart_count++;
	if ((info->domain & (NOUVEAU_GEM_DOMAIN_VRAM |
	    NOUVEAU_GEM_DOMAIN_GART)) ==
	    (NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_GART))
		sc->bo_gem_new_req_vram_gart_count++;
	if (placement == 0)
		sc->bo_gem_new_req_no_domain_count++;
	if ((info->domain & NOUVEAU_GEM_DOMAIN_COHERENT) != 0)
		sc->bo_gem_new_req_coherent_count++;
	if ((info->domain & NOUVEAU_GEM_DOMAIN_NO_SHARE) != 0)
		sc->bo_gem_new_req_no_share_count++;
	if (info->tile_mode != 0 || info->tile_flags != 0)
		sc->bo_gem_new_req_tiled_count++;
}

static void
nvkm_bo_record_gem_new_trace(struct nvkm_softc *sc, uint64_t req_size,
    uint32_t req_domain, const struct nvkm_bo *bo, uint32_t handle,
    uint64_t map_handle, bool mappable_req)
{
	struct nvkm_bo_gem_new_trace *trace;
	struct proc *proc;
	uint64_t seq;

	if (nvkm_debug == 0)
		return;

	seq = ++sc->bo_gem_new_trace_seq;
	trace = &sc->bo_gem_new_trace[(seq - 1) %
	    NVKM_BO_GEM_NEW_TRACE_COUNT];
	trace->seq = seq;
	trace->req_size = req_size;
	trace->size = bo->base.size;
	trace->pid = 0;
	trace->handle = handle;
	trace->req_domain = req_domain;
	trace->domain = bo->domain;
	trace->map_handle = map_handle;
	trace->tile_mode = bo->tile_mode;
	trace->tile_flags = bo->tile_flags;
	trace->mappable_req = mappable_req ? 1 : 0;
	trace->cpu_mappable = nvkm_bo_cpu_mappable(bo) ? 1 : 0;
	trace->comm[0] = '\0';

	proc = curproc;
	if (proc != NULL) {
		trace->pid = (uint32_t)proc->p_pid;
		strlcpy(trace->comm, proc->p_comm, sizeof(trace->comm));
	}
}

static bool
nvkm_bo_ttm_sysmem(const struct nvkm_bo *bo)
{
	if (!bo->ttm_backed || bo->tbo.ttm == NULL)
		return (false);
	return (bo->tbo.mem.mem_type == TTM_PL_TT ||
	    bo->tbo.mem.mem_type == TTM_PL_SYSTEM);
}

static bool
nvkm_bo_ttm_vram(const struct nvkm_bo *bo)
{
	return (bo->ttm_backed && bo->tbo.mem.mem_type == TTM_PL_VRAM);
}

static void
nvkm_bo_refresh_ttm_domain(struct nvkm_bo *bo, uint32_t req_domain)
{
	uint32_t flags = req_domain & ~(NOUVEAU_GEM_DOMAIN_CPU |
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

static uint64_t
nvkm_bo_mmap_handle(struct nvkm_bo *bo)
{
	if (bo->ttm_backed) {
		if (!nvkm_bo_cpu_mappable(bo))
			return (0);
		return (drm_vma_node_offset_addr(&bo->tbo.vma_node));
	}
	if (!nvkm_bo_cpu_mappable(bo))
		return (0);
	return (DRM_GEM_MAPPING_KEY |
	    DRM_GEM_MAPPING_OFF(bo->base.map_list.key));
}

bool
nvkm_bo_has_sysmem(const struct nvkm_bo *bo)
{
	return (nvkm_bo_ttm_sysmem(bo) || bo->kva != NULL ||
	    bo->pages != NULL);
}

bool
nvkm_bo_cpu_mappable(const struct nvkm_bo *bo)
{
	if (nvkm_bo_has_sysmem(bo))
		return (true);
	if (nvkm_bo_ttm_vram(bo)) {
		struct nvkm_softc *sc;

		if (bo->base.dev == NULL)
			return (false);
		sc = bo->base.dev->dev_private;
		return (sc->bar1.ready && bo->vram_alloc != NULL);
	}
	if (bo->base.dev != NULL && bo->base.dev->drm_ttm_bdev != NULL)
		return (false);
	return (bo->bar1_mappable);
}

static vm_page_t
nvkm_bo_page_at(const struct nvkm_bo *bo, uint64_t offset)
{
	uint32_t page_index;
	vm_paddr_t pa;

	if (offset >= bo->base.size)
		return (NULL);

	if (nvkm_bo_ttm_sysmem(bo)) {
		page_index = OFF_TO_IDX(offset);
		if (page_index >= bo->tbo.num_pages ||
		    bo->tbo.ttm->pages == NULL)
			return (NULL);
		return ((vm_page_t)bo->tbo.ttm->pages[page_index]);
	}

	if (bo->pages != NULL) {
		page_index = OFF_TO_IDX(offset);
		if (page_index >= bo->page_count)
			return (NULL);
		return (bo->pages[page_index]);
	}

	if (bo->kva != NULL) {
		pa = vtophys((uint8_t *)bo->kva + trunc_page(offset));
		return (PHYS_TO_VM_PAGE(pa));
	}

	return (NULL);
}

int
nvkm_bo_ensure_ttm_populated(struct nvkm_bo *bo)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false,
	};
	int err;

	if (!bo->ttm_backed)
		return (0);
	if (!nvkm_bo_ttm_sysmem(bo) || bo->tbo.ttm == NULL)
		return (ENXIO);
	if (bo->tbo.ttm->state != tt_unpopulated)
		return (0);

	err = ttm_bo_reserve(&bo->tbo, true, false, NULL);
	if (err != 0)
		return (err < 0 ? -err : err);

	if (bo->tbo.ttm == NULL) {
		err = ENXIO;
	} else {
		err = ttm_tt_populate(bo->tbo.ttm, &ctx);
		if (err < 0)
			err = -err;
	}
	ttm_bo_unreserve(&bo->tbo);
	return (err);
}

int
nvkm_bo_paddr_at(const struct nvkm_bo *bo, uint64_t offset, vm_paddr_t *paddr)
{
	vm_page_t page;

	if (offset >= bo->base.size)
		return (EINVAL);

	if (nvkm_bo_ttm_sysmem(bo)) {
		page = nvkm_bo_page_at(bo, offset);
		if (page == NULL)
			return (ENXIO);
		*paddr = VM_PAGE_TO_PHYS(page) + (offset & PAGE_MASK);
		return (0);
	}

	if (bo->pages != NULL) {
		page = nvkm_bo_page_at(bo, offset);
		if (page == NULL)
			return (ENXIO);
		*paddr = VM_PAGE_TO_PHYS(page) + (offset & PAGE_MASK);
		return (0);
	}

	if (bo->kva != NULL) {
		*paddr = vtophys((uint8_t *)bo->kva + offset);
		return (0);
	}

	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0) {
		*paddr = bo->paddr + offset;
		return (0);
	}

	return (ENXIO);
}

int
nvkm_bo_read32(struct nvkm_bo *bo, uint64_t offset, uint32_t *value)
{
	uint32_t result = 0;
	int err;

	if (offset > bo->base.size || sizeof(*value) > bo->base.size - offset)
		return (EINVAL);

	err = nvkm_bo_ensure_ttm_populated(bo);
	if (err != 0)
		return (err);

	if (bo->kva != NULL) {
		memcpy(value, (uint8_t *)bo->kva + offset, sizeof(*value));
		return (0);
	}

	if (!nvkm_bo_has_sysmem(bo))
		return (ENXIO);

	for (uint32_t i = 0; i < sizeof(*value); i++) {
		vm_paddr_t pa;
		int err;

		err = nvkm_bo_paddr_at(bo, offset + i, &pa);
		if (err != 0)
			return (err);
		result |= (uint32_t)(*(volatile uint8_t *)PHYS_TO_DMAP(pa))
		    << (i * 8);
	}
	*value = result;
	return (0);
}

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

#define NVKM_BO_ACCOUNT_NONE	0
#define NVKM_BO_ACCOUNT_SYSMEM	1
#define NVKM_BO_ACCOUNT_VRAM	2

static uint8_t
nvkm_bo_account_kind(const struct nvkm_bo *bo)
{
	if (nvkm_bo_has_sysmem(bo))
		return (NVKM_BO_ACCOUNT_SYSMEM);
	if (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM)
		return (NVKM_BO_ACCOUNT_VRAM);
	return (NVKM_BO_ACCOUNT_NONE);
}

static void
nvkm_bo_account_alloc(struct nvkm_softc *sc, struct nvkm_bo *bo)
{
	uint8_t kind;
	uint64_t size = bo->base.size;

	if (bo->accounted)
		return;
	kind = nvkm_bo_account_kind(bo);
	if (kind == NVKM_BO_ACCOUNT_NONE)
		return;
	bo->accounted = true;
	bo->account_kind = kind;

	if (kind == NVKM_BO_ACCOUNT_SYSMEM) {
		sc->bo_sysmem_active_count++;
		sc->bo_sysmem_active_bytes += size;
		if (sc->bo_sysmem_active_bytes > sc->bo_sysmem_high_bytes)
			sc->bo_sysmem_high_bytes = sc->bo_sysmem_active_bytes;
		return;
	}
	if (kind == NVKM_BO_ACCOUNT_VRAM) {
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

	if (!bo->accounted)
		return;

	if (bo->account_kind == NVKM_BO_ACCOUNT_SYSMEM) {
		if (sc->bo_sysmem_active_count != 0)
			sc->bo_sysmem_active_count--;
		if (sc->bo_sysmem_active_bytes >= size)
			sc->bo_sysmem_active_bytes -= size;
		else
			sc->bo_sysmem_active_bytes = 0;
	} else if (bo->account_kind == NVKM_BO_ACCOUNT_VRAM) {
		if (sc->bo_vram_active_count != 0)
			sc->bo_vram_active_count--;
		if (sc->bo_vram_active_bytes >= size)
			sc->bo_vram_active_bytes -= size;
		else
			sc->bo_vram_active_bytes = 0;
	}
	bo->accounted = false;
	bo->account_kind = NVKM_BO_ACCOUNT_NONE;
}

static int nvkm_bo_bar1_map(struct nvkm_softc *sc, struct nvkm_bo *bo);
static void nvkm_bo_ttm_destroy(struct ttm_buffer_object *tbo);

struct reservation_object *
nvkm_bo_resv(struct nvkm_bo *bo)
{
	/*
	 * A no_share BO is never exported, so EXEC never publishes a content
	 * fence on its own resv -- the VM-wide EXEC completion lives on the
	 * file's vm_resv. Alias to it so CPU_PREP and free wait the GPU work
	 * that may still be reading this BO. SHARED BOs keep their own resv,
	 * which the DRM/dma-buf framework exposes for cross-process sync.
	 */
	if (bo->no_share && bo->vm_resv != NULL)
		return (bo->vm_resv);
	if (bo->ttm_backed)
		return (bo->tbo.resv);
	return (&bo->resv);
}

static int
nvkm_bo_ttm_pin_record(struct nvkm_bo *bo, uint32_t *record_count)
{
	struct ttm_buffer_object *tbo;
	int err;

	if (!bo->ttm_backed)
		return (0);

	tbo = &bo->tbo;
	err = ttm_bo_reserve(tbo, false, false, NULL);
	if (err != 0)
		return (err < 0 ? -err : err);

	if (*record_count == UINT32_MAX || bo->ttm_pin_count == UINT32_MAX) {
		err = EOVERFLOW;
		goto out_unreserve;
	}

	if (bo->ttm_pin_count == 0)
		tbo->mem.placement |= TTM_PL_FLAG_NO_EVICT;
	bo->ttm_pin_count++;
	(*record_count)++;
	err = 0;

out_unreserve:
	ttm_bo_unreserve(tbo);
	return (err);
}

static int
nvkm_bo_ttm_unpin_record(struct nvkm_bo *bo, uint32_t *record_count)
{
	struct ttm_buffer_object *tbo;
	int err;

	if (!bo->ttm_backed)
		return (0);

	tbo = &bo->tbo;
	err = ttm_bo_reserve(tbo, false, false, NULL);
	if (err != 0)
		return (err < 0 ? -err : err);

	if (*record_count == 0 || bo->ttm_pin_count == 0) {
		err = EINVAL;
		goto out_unreserve;
	}

	(*record_count)--;
	bo->ttm_pin_count--;
	if (bo->ttm_pin_count == 0 &&
	    tbo->mem.mem_type != TTM_PL_VRAM)
		tbo->mem.placement &= ~TTM_PL_FLAG_NO_EVICT;
	err = 0;

out_unreserve:
	ttm_bo_unreserve(tbo);
	return (err);
}

int
nvkm_bo_vm_bind_pin(struct nvkm_bo *bo)
{
	return (nvkm_bo_ttm_pin_record(bo, &bo->vm_bind_pin_count));
}

int
nvkm_bo_vm_bind_unpin(struct nvkm_bo *bo)
{
	return (nvkm_bo_ttm_unpin_record(bo, &bo->vm_bind_pin_count));
}

int
nvkm_bo_scanout_pin(struct nvkm_bo *bo)
{
	return (nvkm_bo_ttm_pin_record(bo, &bo->scanout_pin_count));
}

int
nvkm_bo_scanout_unpin(struct nvkm_bo *bo)
{
	return (nvkm_bo_ttm_unpin_record(bo, &bo->scanout_pin_count));
}

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

	if (nvkm_bo_has_sysmem(bo)) {
		m = nvkm_bo_page_at(bo, offset);
	} else {
		sc->bo_bar1_fault_count++;
		if (bo->bar1_mappable && bo->bar1_gva == 0) {
			sc->bo_bar1_fault_map_count++;
			if (nvkm_bo_bar1_map(sc, bo) != 0) {
				sc->bo_bar1_fault_error_count++;
				return (VM_PAGER_ERROR);
			}
		}
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
	if (err != 0) {
		sc->bo_bar1_map_error_count++;
		return (err);
	}
	bo->bar1_size = bo->base.size;
	sc->bo_bar1_map_count++;
	return (0);
}

static void
nvkm_bo_bar1_unmap(struct nvkm_softc *sc, struct nvkm_bo *bo)
{
	if (bo->bar1_gva == 0)
		return;
	nvkm_gsp_bar1_unmap_existing_range(sc, bo->bar1_gva, bo->bar1_size);
	sc->bo_bar1_unmap_count++;
	bo->bar1_gva = 0;
	bo->bar1_size = 0;
}

static void
nvkm_bo_free_pages(struct nvkm_bo *bo)
{
	if (bo->pages == NULL)
		return;

	for (uint32_t i = 0; i < bo->page_count; i++) {
		if (bo->pages[i] != NULL)
			vm_page_freezwq(bo->pages[i]);
	}
	kfree(bo->pages);
	bo->pages = NULL;
	bo->page_count = 0;
}

static int
nvkm_bo_init_ttm(struct nvkm_softc *sc, struct nvkm_bo *bo, uint64_t size,
    uint32_t domain)
{
	struct ttm_place places[3];
	struct ttm_placement placement = {
		.placement = places,
		.busy_placement = places,
	};
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false,
	};
	struct ttm_bo_device *bdev;
	uint32_t placement_domain;
	unsigned int n = 0;
	size_t acc_size;
	int err;

	bdev = nvkm_ttm_bo_device(sc);
	if (bdev == NULL)
		return (-ENODEV);

	placement_domain = domain & (NOUVEAU_GEM_DOMAIN_CPU |
	    NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_GART);
	if (placement_domain == 0)
		placement_domain = NOUVEAU_GEM_DOMAIN_GART;
	if ((placement_domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0) {
		places[n].fpfn = 0;
		places[n].lpfn = 0;
		places[n].flags = TTM_PL_FLAG_VRAM | TTM_PL_FLAG_WC |
		    TTM_PL_FLAG_NO_EVICT;
		n++;
	}
	if ((placement_domain & NOUVEAU_GEM_DOMAIN_GART) != 0) {
		places[n].fpfn = 0;
		places[n].lpfn = 0;
		places[n].flags = TTM_PL_FLAG_TT | TTM_PL_FLAG_CACHED;
		n++;
	}
	if ((placement_domain & NOUVEAU_GEM_DOMAIN_CPU) != 0) {
		places[n].fpfn = 0;
		places[n].lpfn = 0;
		places[n].flags = TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED;
		n++;
	}
	placement.num_placement = n;
	placement.num_busy_placement = n;

	bo->ttm_backed = true;
	bo->domain = domain;
	acc_size = ttm_bo_dma_acc_size(bdev, size, sizeof(*bo));
	err = ttm_bo_init_reserved(bdev, &bo->tbo, size, ttm_bo_type_device,
	    &placement, 1, &ctx, acc_size, NULL, NULL, nvkm_bo_ttm_destroy);
	if (err == 0) {
		nvkm_bo_refresh_ttm_domain(bo, domain);
		ttm_bo_unreserve(&bo->tbo);
	}
	return (err);
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
	int err;
	bool wants_vram;
	bool can_fallback_gart;

	size = roundup(size, PAGE_SIZE);
	if (size == 0)
		return (NULL);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (bo == NULL)
		return (NULL);
	reservation_object_init(&bo->resv);
	drm_gem_private_object_init(ddev, &bo->base, size);
	bo->tile_mode = tile_mode;
	bo->tile_flags = tile_flags;
	bo->no_share = (domain & NOUVEAU_GEM_DOMAIN_NO_SHARE) != 0;

	if (nvkm_ttm_bo_device(sc) != NULL) {
		err = nvkm_bo_init_ttm(sc, bo, size, domain);
		if (err != 0) {
			nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_SYSMEM,
			    size, domain, err);
			return (NULL);
		}
		nvkm_bo_account_alloc(sc, bo);
		return (bo);
	}

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
				drm_gem_object_release(&bo->base);
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
		if (nvkm_ttm_bo_device(sc) == NULL) {
			nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_SYSMEM,
			    size, domain, -ENODEV);
			drm_gem_object_release(&bo->base);
			reservation_object_fini(&bo->resv);
			kfree(bo);
			return (NULL);
		}
		err = nvkm_bo_init_ttm(sc, bo, size, domain);
		if (err != 0) {
			nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_SYSMEM,
			    size, domain, err);
			return (NULL);
		}
	}

	nvkm_bo_account_alloc(sc, bo);
	return (bo);
}

static void
nvkm_bo_ttm_destroy(struct ttm_buffer_object *tbo)
{
	struct nvkm_bo *bo = container_of(tbo, struct nvkm_bo, tbo);
	struct nvkm_softc *sc = bo->base.dev->dev_private;

	nvkm_bo_account_free(sc, bo);
	reservation_object_fini(&bo->resv);
	drm_gem_object_release(&bo->base);
	kfree(bo);
}

void
nvkm_bo_gem_free(struct drm_gem_object *obj)
{
	struct nvkm_bo *bo = to_nvkm_bo(obj);
	struct nvkm_softc *sc = obj->dev->dev_private;

	nvkm_debugf(sc->dev,
	    "nvkm_bo: GEM_FREE obj=%p domain=0x%x size=0x%llx paddr=0x%llx cpu_map=%u\n",
	    obj, bo->domain, (unsigned long long)obj->size,
	    (unsigned long long)bo->paddr, nvkm_bo_has_sysmem(bo));

	sc->bo_gem_free_count++;
	if (bo->ttm_backed) {
		/*
		 * no_share BOs alias their fence resv to the file's vm_resv, but
		 * TTM's own delayed destroy only waits the BO's tbo.resv (which
		 * carries no EXEC/VM_BIND fence). Wait the VM's GPU completion
		 * here so TTM does not reclaim pages a still-in-flight job reads.
		 */
		if (bo->no_share && bo->vm_resv != NULL)
			(void)nvkm_bo_resv_wait(bo, false, true, false);
		ttm_bo_put(&bo->tbo);
		return;
	}

	nvkm_bo_account_free(sc, bo);
	(void)nvkm_bo_resv_wait(bo, false, true, false);
	nvkm_bo_bar1_unmap(sc, bo);
	if (bo->kva != NULL) {
		kmem_free(kernel_map, (vm_offset_t)bo->kva, obj->size);
		bo->kva = NULL;
	}
	nvkm_bo_free_pages(bo);
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
	struct reservation_object *resv = nvkm_bo_resv(bo);

	reservation_object_lock(resv, NULL);
	reservation_object_add_excl_fence(resv, fence);
	reservation_object_unlock(resv);
}

int
nvkm_bo_resv_add_shared_fence(struct nvkm_bo *bo, struct dma_fence *fence)
{
	struct reservation_object *resv = nvkm_bo_resv(bo);
	int err;

	reservation_object_lock(resv, NULL);
	err = reservation_object_reserve_shared(resv);
	if (err == 0)
		reservation_object_add_shared_fence(resv, fence);
	reservation_object_unlock(resv);
	return (err < 0 ? -err : err);
}

int
nvkm_bo_resv_wait(struct nvkm_bo *bo, bool intr, bool write, bool nowait)
{
	struct nvkm_softc *sc = bo->base.dev->dev_private;
	struct reservation_object *resv = nvkm_bo_resv(bo);
	long ret;

	sc->bo_resv_wait_count++;

	/*
	 * write access tests/waits read+write fences (wait_all=true); read
	 * access only the exclusive (write) fence (wait_all=false).
	 *
	 * NOWAIT must be strictly non-blocking, but the DragonFly reservation
	 * helper rewrites a 0 timeout to 1 tick (reservation_object_wait_timeout
	 * _rcu: "timeout ? timeout : 1"), so wait_timeout_rcu(0) would still sleep
	 * up to a tick. Use the non-sleeping signaled test instead and map a
	 * pending fence straight to -EBUSY, matching nouveau's timeout=0 path.
	 */
	if (nowait) {
		if (reservation_object_test_signaled_rcu(resv, write))
			return (0);
		sc->bo_resv_wait_error_count++;
		return (-EBUSY);
	}

	ret = reservation_object_wait_timeout_rcu(resv, write, intr,
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

	sc->bo_dumb_create_count++;
	pitch = roundup2((uint64_t)args->width * howmany(args->bpp, 8), 64);
	size = roundup(pitch * args->height, PAGE_SIZE);
	if (pitch > UINT32_MAX || size == 0)
		return (-EINVAL);

	domain = sc->bar1.ready ? NOUVEAU_GEM_DOMAIN_VRAM :
	    NOUVEAU_GEM_DOMAIN_GART;
	if (domain == NOUVEAU_GEM_DOMAIN_VRAM)
		sc->bo_dumb_create_vram_count++;
	else
		sc->bo_dumb_create_gart_count++;
	bo = nvkm_bo_create(ddev, size, domain, 0, 0);
	if (bo == NULL)
		return (-ENOMEM);
	if (domain == NOUVEAU_GEM_DOMAIN_VRAM && !bo->ttm_backed) {
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

	if (bo->ttm_backed || !nvkm_bo_cpu_mappable(bo))
		err = 0;
	else
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
	struct nvkm_bo *bo;
	int err;

	(void)ddev;
	obj = drm_gem_object_lookup(file_priv, handle);
	if (obj == NULL)
		return (-ENOENT);
	if (obj->import_attach) {
		drm_gem_object_put_unlocked(obj);
		return (-EINVAL);
	}

	bo = to_nvkm_bo(obj);
	if (!nvkm_bo_cpu_mappable(bo)) {
		err = -ENXIO;
	} else if (bo->ttm_backed) {
		*offset = nvkm_bo_mmap_handle(bo);
		err = 0;
	} else {
		err = drm_gem_create_mmap_offset(obj);
		if (err == 0)
			*offset = nvkm_bo_mmap_handle(bo);
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
	uint32_t req_domain = req->info.domain;
	uint32_t create_domain;
	uint32_t handle = 0;
	uint64_t req_size = req->info.size;
	bool mappable_req;
	int err;

	mappable_req = (req_domain & NOUVEAU_GEM_DOMAIN_MAPPABLE) != 0;
	create_domain = req_domain;
	if (mappable_req && (req_domain & NOUVEAU_GEM_DOMAIN_GART) != 0)
		create_domain = (req_domain & ~NOUVEAU_GEM_DOMAIN_VRAM) |
		    NOUVEAU_GEM_DOMAIN_GART;
	sc->bo_gem_new_count++;
	nvkm_bo_record_gem_new_request(sc, &req->info);
	if (mappable_req)
		sc->bo_gem_new_mappable_req_count++;
	bo = nvkm_bo_create(ddev, req->info.size, create_domain,
	    req->info.tile_mode, req->info.tile_flags);
	if (bo == NULL)
		return (-ENOMEM);
	if (mappable_req && (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0 &&
	    !nvkm_bo_cpu_mappable(bo)) {
		if ((req_domain & NOUVEAU_GEM_DOMAIN_GART) != 0) {
			uint32_t gart_domain;

			gart_domain = (req_domain &
			    ~NOUVEAU_GEM_DOMAIN_VRAM) |
			    NOUVEAU_GEM_DOMAIN_GART;
			nvkm_debugf(sc->dev,
			    "nvkm_bo: GEM_NEW mappable VRAM fallback to GART req_domain=0x%x size=0x%llx\n",
			    req_domain,
			    (unsigned long long)bo->base.size);
			drm_gem_object_put_unlocked(&bo->base);
			bo = nvkm_bo_create(ddev, req->info.size,
			    gart_domain, req->info.tile_mode,
			    req->info.tile_flags);
			if (bo == NULL)
				return (-ENOMEM);
		} else if (sc->bar1.ready && ddev->drm_ttm_bdev == NULL) {
			bo->bar1_mappable = true;
		} else {
			nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_MMAP,
			    bo->base.size, req_domain, ENXIO);
			drm_gem_object_put_unlocked(&bo->base);
			return (ENXIO);
		}
	}

	/* no_share BOs alias their fence-wait resv to this file's vm_resv so
	 * CPU_PREP/free wait the VM's EXEC completion (see nvkm_bo_resv). */
	if (bo->no_share)
		bo->vm_resv = nvkm_drm_file_vm_resv(file_priv);

	err = drm_gem_handle_create(file_priv, &bo->base, &handle);
	if (err != 0)
		nvkm_bo_record_alloc_fail(sc, NVKM_BO_ALLOC_FAIL_HANDLE,
		    bo->base.size, req_domain, err);
	/* drop our local reference; the handle holds one now. */
	drm_gem_object_put_unlocked(&bo->base);
	if (err != 0)
		return (err);

	nvkm_debugf(sc->dev,
	    "nvkm_bo: GEM_NEW handle=%u obj=%p req_domain=0x%x domain=0x%x size=0x%llx paddr=0x%llx cpu_map=%u\n",
	    handle, &bo->base, req_domain, bo->domain,
	    (unsigned long long)bo->base.size,
	    (unsigned long long)bo->paddr, nvkm_bo_cpu_mappable(bo));

	if (nvkm_bo_cpu_mappable(bo) && !bo->ttm_backed) {
		err = drm_gem_create_mmap_offset(&bo->base);
		if (err != 0) {
			nvkm_bo_record_alloc_fail(sc,
			    NVKM_BO_ALLOC_FAIL_MMAP, bo->base.size,
			    req_domain, err);
			drm_gem_handle_delete(file_priv, handle);
			return (err);
		}
	}

	req->info.handle = handle;
	req->info.domain = bo->domain;
	req->info.size = bo->base.size;
	req->info.offset = 0;	/* GPU VA — set by VM_BIND later */
	req->info.map_handle = nvkm_bo_mmap_handle(bo);
	nvkm_bo_record_gem_new_trace(sc, req_size, req_domain, bo, handle,
	    req->info.map_handle, mappable_req);
	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0)
		sc->bo_gem_new_vram_count++;
	else if ((bo->domain & NOUVEAU_GEM_DOMAIN_GART) != 0)
		sc->bo_gem_new_gart_count++;
	if (mappable_req) {
		if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0)
			sc->bo_gem_new_mappable_vram_count++;
		else if ((bo->domain & NOUVEAU_GEM_DOMAIN_GART) != 0)
			sc->bo_gem_new_mappable_gart_count++;
	}
	if (req->info.map_handle != 0)
		sc->bo_gem_new_map_handle_count++;
	nvkm_bo_record_gem_new_size(sc, bo, mappable_req);
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
	req->map_handle = nvkm_bo_mmap_handle(bo);
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
	err = nvkm_bo_resv_wait(bo, true,
	    (req->flags & NOUVEAU_GEM_CPU_PREP_WRITE) != 0,
	    (req->flags & NOUVEAU_GEM_CPU_PREP_NOWAIT) != 0);
	sc->cpu_prep_wait_count++;
	if (err != 0)
		sc->cpu_prep_wait_error_count++;
	nvkm_debugf(sc->dev,
	    "nvkm_bo: CPU_PREP handle=%u obj=%p domain=0x%x size=0x%llx paddr=0x%llx cpu_map=%u flags=0x%x wait_err=%d\n",
	    req->handle, obj, bo->domain, (unsigned long long)obj->size,
	    (unsigned long long)bo->paddr, nvkm_bo_has_sysmem(bo),
	    req->flags, err);
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
	sc->cpu_fini_count++;
	/*
	 * Discrete x86 sysmem BOs are WB-coherent (the GPU snoops), so the
	 * device sees CPU writes without an explicit cache flush. A store
	 * barrier is enough to order those writes ahead of the subsequent GPU
	 * access. This mirrors the EXEC submit path, which already dropped the
	 * per-page pmap_invalidate_cache_range (it degrades to an all-CPU
	 * WBINVD on this hardware). See README 288ec48262.
	 */
	cpu_sfence();
	nvkm_debugf(sc->dev,
	    "nvkm_bo: CPU_FINI handle=%u obj=%p domain=0x%x size=0x%llx paddr=0x%llx cpu_map=%u flushed=%u\n",
	    req->handle, obj, bo->domain, (unsigned long long)obj->size,
	    (unsigned long long)bo->paddr, nvkm_bo_has_sysmem(bo),
	    nvkm_bo_has_sysmem(bo));
	drm_gem_object_put_unlocked(obj);
	return (0);
}
