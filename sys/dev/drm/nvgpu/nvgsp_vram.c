/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP VRAM allocation boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_vram.h"
#include "nvgsp_priv.h"

static MALLOC_DEFINE(M_NVGSP_VRAM, "nvgsp_vram", "nvgsp VRAM allocation metadata");

static struct nvgsp_vram_alloc *
nvgsp_vram_record_alloc(uint64_t size, uint64_t align, enum nvgsp_vram_kind kind, void *owner)
{
	struct nvgsp_vram_alloc *alloc;

	alloc = kmalloc(sizeof(*alloc), M_NVGSP_VRAM, M_WAITOK | M_ZERO);
	alloc->size = size;
	alloc->align = align;
	alloc->kind = kind;
	alloc->owner = owner;
	return (alloc);
}

int
nvgsp_vram_init(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	uint64_t base, size;

	if (gsp == NULL)
		return (ENXIO);
	base = gsp->fb_usable_base;
	size = gsp->fb_usable_size;
	if (size == 0)
		return (ENXIO);
	if (size > (64ULL << 20))
		size -= (64ULL << 20);

	gsp->vram_bump_base = base;
	gsp->vram_bump_limit = base + size;
	drm_mm_init(&gsp->vram_mm, base, size);
	lockinit(&gsp->vram_lock, "nvgspvram", 0, 0);
	TAILQ_INIT(&gsp->vram_allocs);
	memset(gsp->vram_alloc_bytes, 0, sizeof(gsp->vram_alloc_bytes));
	memset(gsp->vram_alloc_count, 0, sizeof(gsp->vram_alloc_count));
	lwkt_token_init(&gsp->chid_tok, "nvgsp-chid");
	memset(gsp->chid_used, 0, sizeof(gsp->chid_used));
	gsp->chid_used[0] = 0x3ULL;
	nvgpu_log(NVGPU_LOG_DEBUG, "vram window 0x%llx..0x%llx\n",
	    (unsigned long long)gsp->vram_bump_base,
	    (unsigned long long)gsp->vram_bump_limit);
	return (0);
}

void
nvgsp_vram_fini(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	struct nvgsp_vram_alloc *alloc;

	if (gsp == NULL || gsp->vram_bump_limit == 0)
		return;
	lockmgr(&gsp->vram_lock, LK_EXCLUSIVE);
	while ((alloc = TAILQ_FIRST(&gsp->vram_allocs)) != NULL) {
		TAILQ_REMOVE(&gsp->vram_allocs, alloc, link);
		if (drm_mm_node_allocated(&alloc->node))
			drm_mm_remove_node(&alloc->node);
		kfree(alloc, M_NVGSP_VRAM);
	}
	lockmgr(&gsp->vram_lock, LK_RELEASE);
	drm_mm_takedown(&gsp->vram_mm);
	lockuninit(&gsp->vram_lock);
	gsp->vram_bump_base = 0;
	gsp->vram_bump_limit = 0;
}

uint64_t
nvgsp_vram_alloc_kind(struct nvgsp_state *gsp, uint64_t size, uint64_t align,
    enum nvgsp_vram_kind kind, void *owner)
{
	struct nvgsp_vram_alloc *alloc;
	int error;

	if (align == 0)
		align = 0x1000;
	size = NVGSP_ALIGN_UP(size, align);
	alloc = nvgsp_vram_record_alloc(size, align, kind, owner);

	lockmgr(&gsp->vram_lock, LK_EXCLUSIVE);
	error = drm_mm_insert_node_in_range(&gsp->vram_mm, &alloc->node, size,
	    align, 0, gsp->vram_bump_base, gsp->vram_bump_limit, DRM_MM_INSERT_HIGH);
	if (error != 0) {
		lockmgr(&gsp->vram_lock, LK_RELEASE);
		kfree(alloc, M_NVGSP_VRAM);
		return (0);
	}
	alloc->paddr = alloc->node.start;
	TAILQ_INSERT_TAIL(&gsp->vram_allocs, alloc, link);
	gsp->vram_alloc_bytes[kind] += alloc->size;
	gsp->vram_alloc_count[kind]++;
	lockmgr(&gsp->vram_lock, LK_RELEASE);
	return (alloc->paddr);
}

void
nvgsp_vram_free_kind(struct nvgsp_state *gsp, uint64_t paddr,
    enum nvgsp_vram_kind kind, void *owner)
{
	struct nvgsp_vram_alloc *alloc;

	if (gsp == NULL || paddr == 0)
		return;
	lockmgr(&gsp->vram_lock, LK_EXCLUSIVE);
	TAILQ_FOREACH(alloc, &gsp->vram_allocs, link) {
		if (alloc->paddr == paddr && alloc->kind == kind && alloc->owner == owner)
			break;
	}
	if (alloc != NULL) {
		TAILQ_REMOVE(&gsp->vram_allocs, alloc, link);
		gsp->vram_alloc_bytes[kind] -= alloc->size;
		gsp->vram_alloc_count[kind]--;
		if (drm_mm_node_allocated(&alloc->node))
			drm_mm_remove_node(&alloc->node);
	}
	lockmgr(&gsp->vram_lock, LK_RELEASE);
	if (alloc != NULL)
		kfree(alloc, M_NVGSP_VRAM);
}

uint32_t
nvgsp_vram_free_owner(struct nvgsp_state *gsp, void *owner)
{
	struct nvgsp_vram_alloc *alloc, *next;
	uint32_t count = 0;

	if (gsp == NULL || owner == NULL)
		return (0);
	lockmgr(&gsp->vram_lock, LK_EXCLUSIVE);
	TAILQ_FOREACH_MUTABLE(alloc, &gsp->vram_allocs, link, next) {
		if (alloc->owner != owner)
			continue;
		TAILQ_REMOVE(&gsp->vram_allocs, alloc, link);
		gsp->vram_alloc_bytes[alloc->kind] -= alloc->size;
		gsp->vram_alloc_count[alloc->kind]--;
		if (drm_mm_node_allocated(&alloc->node))
			drm_mm_remove_node(&alloc->node);
		kfree(alloc, M_NVGSP_VRAM);
		count++;
	}
	lockmgr(&gsp->vram_lock, LK_RELEASE);
	return (count);
}

int
nvgsp_vram_alloc_channel_inst(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	struct nvgsp_channel *chan = gsp != NULL ? gsp->bootstrap_channel : NULL;

	if (chan == NULL)
		return (ENXIO);
	chan->inst_vram = nvgsp_vram_alloc_kind(gsp, 0x1000, 0x1000,
	    NVGSP_VRAM_CHANNEL_INST, chan);
	return (chan->inst_vram != 0 ? 0 : ENOMEM);
}

int
nvgsp_vram_alloc_userd(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	struct nvgsp_channel *chan = gsp != NULL ? gsp->bootstrap_channel : NULL;
	uint32_t userd_page;

	if (chan == NULL || chan->chid < 0)
		return (ENXIO);
	userd_page = (uint32_t)chan->chid / 8u;
	chan->userd_vram = nvgsp_vram_alloc_kind(gsp,
	    (uint64_t)(userd_page + 1u) * 0x1000u, 0x1000,
	    NVGSP_VRAM_CHANNEL_USERD, chan);
	return (chan->userd_vram != 0 ? 0 : ENOMEM);
}

int
nvgsp_vram_alloc_golden(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	struct nvgsp_channel *chan = gsp != NULL ? gsp->golden_channel : NULL;

	if (chan == NULL)
		return (ENXIO);
	chan->inst_vram = nvgsp_vram_alloc_kind(gsp, 0x12000, 0x1000,
	    NVGSP_VRAM_CHANNEL_GOLDEN, chan);
	if (chan->inst_vram == 0)
		return (ENOMEM);
	chan->userd_vram = chan->inst_vram + 0x1000;
	chan->mthdbuf_paddr = chan->inst_vram + 0x2000;
	return (0);
}
