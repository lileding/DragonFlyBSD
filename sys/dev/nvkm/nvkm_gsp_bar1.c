/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR1 API -- thin wrapper around BAR2.
 *
 * On TU102 r570, GSP does NOT initialize BAR1 inst block (we verified
 * inst[0x200] = 0 -- walker has no PDB to walk). All host->VRAM access
 * routes through BAR2 instead, which is L2-coherent and properly set up.
 *
 * The struct field `bar1_gva` retained for API compat actually holds a
 * BAR2 GVA. The host writes data through BAR2 -> walker -> L2 -> VRAM;
 * PBDMA later reads same VRAM via channel vmm walker, also L2-coherent.
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"

int
nvkm_gsp_bar1_init(struct nvkm_softc *sc)
{
	if (!sc->bar2.ready) {
		device_printf(sc->dev,
		    "bar1: BAR2 not ready (required, BAR1 itself is unused)\n");
		return (ENXIO);
	}
	sc->bar1.ready = true;
	device_printf(sc->dev,
	    "bar1: routing all host VRAM access via BAR2 (BAR1 not init by GSP)\n");
	return (0);
}

void
nvkm_gsp_bar1_fini(struct nvkm_softc *sc)
{
	sc->bar1.ready = false;
}

/* Map a VRAM page at a specific GVA. Forward to BAR2. */
int
nvkm_gsp_bar1_map_vram(struct nvkm_softc *sc, uint64_t bar1_gva,
    uint64_t vram_paddr)
{
	int err = nvkm_gsp_bar2_map_vram(sc, bar1_gva, vram_paddr);
	if (err == 0)
		nvkm_gsp_bar2_flush(sc);
	return (err);
}

/* Write helpers route through BAR2 (PCIe BAR3). */
void
nvkm_gsp_bar1_wr32(struct nvkm_softc *sc, uint64_t gva, uint32_t val)
{
	nvkm_gsp_bar2_wr32(sc, gva, val);
}

uint32_t
nvkm_gsp_bar1_rd32(struct nvkm_softc *sc, uint64_t gva)
{
	return (nvkm_gsp_bar2_rd32(sc, gva));
}

void
nvkm_gsp_bar1_wr64(struct nvkm_softc *sc, uint64_t gva, uint64_t val)
{
	nvkm_gsp_bar2_wr64(sc, gva, val);
}

uint64_t
nvkm_gsp_bar1_rd64(struct nvkm_softc *sc, uint64_t gva)
{
	return (nvkm_gsp_bar2_rd64(sc, gva));
}

/* Allocate a VRAM page + map into BAR2 at next free GVA. */
int
nvkm_gsp_bar1_alloc_page(struct nvkm_softc *sc, struct nvkm_bar1_page *page)
{
	uint64_t paddr, gva;
	int err;

	if (!sc->bar1.ready)
		return (ENXIO);

	paddr = nvkm_gsp_vram_alloc(sc, NVKM_GMMU_PT_PAGE_SIZE,
	    NVKM_GMMU_PT_PAGE_SIZE);
	if (paddr == 0)
		return (ENOMEM);

	gva = sc->bar2.next_gva;
	sc->bar2.next_gva += NVKM_GMMU_PT_PAGE_SIZE;

	err = nvkm_gsp_bar2_map_vram(sc, gva, paddr);
	if (err != 0)
		return (err);
	nvkm_gsp_bar2_flush(sc);

	page->vram_paddr = paddr;
	page->bar1_gva   = gva;   /* field name is legacy; this is a BAR2 GVA */
	return (0);
}

void
nvkm_gsp_bar1_free_page(struct nvkm_softc *sc __unused,
    struct nvkm_bar1_page *page)
{
	page->vram_paddr = 0;
	page->bar1_gva   = 0;
}

void
nvkm_gsp_bar1_dump_pt(struct nvkm_softc *sc __unused,
    uint64_t target_paddr __unused, uint32_t target_off __unused)
{
}
