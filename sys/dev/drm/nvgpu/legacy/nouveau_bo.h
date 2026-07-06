/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal nouveau_bo shim for selected dispnv50 MIT display
 * notifier helpers.
 */
#ifndef _DFLY_NOUVEAU_BO_H_
#define _DFLY_NOUVEAU_BO_H_

#include <nvif/os.h>
#include <nvhw/drf.h>

struct nvkm_softc;

int nvkm_gsp_bar1_map_existing(struct nvkm_softc *sc, uint64_t paddr,
    uint64_t *pgva);
void nvkm_gsp_bar1_unmap_existing(struct nvkm_softc *sc, uint64_t gva);
uint32_t nvkm_gsp_bar1_rd32(struct nvkm_softc *sc, uint64_t bar1_gva);
void nvkm_gsp_bar1_wr32(struct nvkm_softc *sc, uint64_t bar1_gva,
    uint32_t val);
void nvkm_gsp_bar1_flush(struct nvkm_softc *sc);

struct nouveau_bo {
	u64 offset;
	/*
	 * Optional fixed BAR1 mapping for IRQ-safe reads.
	 *
	 * Ownership: the creator owns bar1_gva/bar1_size and must unmap them.
	 * Lifetime: readers may use the fixed mapping only while the creator keeps
	 * the underlying VRAM allocation and BAR1 mapping alive.
	 * Threading: read/write helpers perform plain BAR1 MMIO when this range is
	 * present; the fallback transient mapping path is process-context only.
	 */
	u64 bar1_gva;
	u64 bar1_size;
	struct nvkm_softc *sc;
};

static inline u32
nouveau_bo_rd32(struct nouveau_bo *bo, unsigned int index)
{
	uint64_t addr;
	uint64_t byte;
	uint64_t page;
	uint64_t page_off;
	uint64_t gva;
	u32 data = 0;

	if (bo == NULL || bo->sc == NULL)
		return 0;

	byte = (uint64_t)index * sizeof(u32);
	if (bo->bar1_size != 0 && byte + sizeof(u32) <= bo->bar1_size)
		return nvkm_gsp_bar1_rd32(bo->sc, bo->bar1_gva + byte);

	addr = bo->offset + byte;
	page = addr & ~(uint64_t)(PAGE_SIZE - 1);
	page_off = addr - page;
	if (nvkm_gsp_bar1_map_existing(bo->sc, page, &gva) == 0) {
		data = nvkm_gsp_bar1_rd32(bo->sc, gva + page_off);
		nvkm_gsp_bar1_unmap_existing(bo->sc, gva);
	}
	return data;
}

static inline void
nouveau_bo_wr32(struct nouveau_bo *bo, unsigned int index, u32 data)
{
	uint64_t addr;
	uint64_t byte;
	uint64_t page;
	uint64_t page_off;
	uint64_t gva;

	if (bo == NULL || bo->sc == NULL)
		return;

	byte = (uint64_t)index * sizeof(u32);
	if (bo->bar1_size != 0 && byte + sizeof(u32) <= bo->bar1_size) {
		nvkm_gsp_bar1_wr32(bo->sc, bo->bar1_gva + byte, data);
		nvkm_gsp_bar1_flush(bo->sc);
		return;
	}

	addr = bo->offset + byte;
	page = addr & ~(uint64_t)(PAGE_SIZE - 1);
	page_off = addr - page;
	if (nvkm_gsp_bar1_map_existing(bo->sc, page, &gva) == 0) {
		nvkm_gsp_bar1_wr32(bo->sc, gva + page_off, data);
		nvkm_gsp_bar1_unmap_existing(bo->sc, gva);
		nvkm_gsp_bar1_flush(bo->sc);
	}
}

#define NVBO_WR32_(b, o, dr, f) nouveau_bo_wr32((b), (o) / 4 + (dr), (f))
#define NVBO_RD32_(b, o, dr) nouveau_bo_rd32((b), (o) / 4 + (dr))
#define NVBO_TD32(A...) DRF_TD(NVBO_RD32_, ##A)
#define NVBO_WR32(A...) DRF_WR(NVBO_WR32_, ##A)

#endif
