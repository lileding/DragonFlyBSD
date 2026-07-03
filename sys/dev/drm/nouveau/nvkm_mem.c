/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DMA-coherent memory helpers.
 *
 * GSP boot needs system-memory buffers that are simultaneously visible
 * to the host CPU (for staging firmware and inspecting results) and to
 * the GPU (for ucode DMA loads, WPR descriptors, msgq rings).
 *
 * DFly's bus_dmamem_coherent_any() returns the kernel virtual address,
 * a fresh bus_dma_tag/map pair, and the bus (physical) address in one
 * call, which is exactly the bundle we want per allocation.
 */

#include "nvkm_priv.h"

int
nvkm_dmamem_alloc(struct nvkm_softc *sc, bus_size_t size,
    bus_size_t alignment, struct nvkm_dmamem *out)
{
	void *kva;
	bus_dma_tag_t tag = NULL;
	bus_dmamap_t  map = NULL;
	bus_addr_t paddr = 0;

	kva = bus_dmamem_coherent_any(bus_get_dma_tag(sc->dev),
	    alignment,
	    size,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO,
	    &tag, &map, &paddr);
	if (kva == NULL)
		return (ENOMEM);

	out->kva = kva;
	out->paddr = paddr;
	out->size = size;
	out->tag = tag;
	out->map = map;
	return (0);
}

void
nvkm_dmamem_free(struct nvkm_softc *sc __unused, struct nvkm_dmamem *mem)
{
	if (mem->kva == NULL)
		return;
	bus_dmamap_unload(mem->tag, mem->map);
	bus_dmamem_free(mem->tag, mem->kva, mem->map);
	bus_dma_tag_destroy(mem->tag);
	mem->kva = NULL;
	mem->paddr = 0;
	mem->size = 0;
	mem->tag = NULL;
	mem->map = NULL;
}
