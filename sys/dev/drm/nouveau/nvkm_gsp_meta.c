/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP firmware WPR2 metadata (GspFwWprMeta) -- the handoff structure
 * that the booter on SEC2 reads from sysmem (via MAILBOX0/1) to find
 * the GSP-RM image, bootloader, FRTS layout etc.
 *
 * Layout reference (256 bytes total):
 *   open-rm 570.144 src/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h
 *   linux nouveau   include/nvfw/gsp.h
 *
 *   ---------------------------- <- fbSize (1 MiB aligned)
 *   | VGA WORKSPACE            |
 *   ---------------------------- <- vbiosReservedOffset
 *   | (alignment gap)          |
 *   ---------------------------- <- gspFwWprEnd + frtsSize + pmuReservedSize
 *   | PMU reservation          |
 *   ---------------------------- <- gspFwWprEnd + frtsSize
 *   | FRTS data                |
 *   ---------------------------- <- frtsOffset
 *   | BOOT BIN (SK + BL)       |
 *   ---------------------------- <- bootBinOffset
 *   | GSP FW ELF               |
 *   ---------------------------- <- gspFwOffset
 *   | GSP FW heap (WPR)        |
 *   ---------------------------- <- gspFwHeapOffset
 *   | (struct GspFwWprMeta)    |
 *   ---------------------------- <- gspFwWprStart (128 KiB aligned)
 *   | GSP FW heap (non-WPR)    |
 *   ---------------------------- <- nonWprHeapOffset, gspFwRsvdStart
 */

#include "nvkm_priv.h"

static MALLOC_DEFINE(M_NVKM_GSP_META, "nvkm_gsp_meta", "nvkm GSP wpr meta");

_Static_assert(sizeof(struct nvkm_gsp_wpr_meta) == NVKM_GSP_FW_WPR_META_SIZE,
    "GspFwWprMeta must be exactly 256 bytes");

int
nvkm_gsp_meta_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_wpr_meta *m;
	int error;

	if (sc->wpr_meta.kva != NULL)
		return (0);

	error = nvkm_dmamem_alloc(sc, NVKM_GSP_FW_WPR_META_SIZE, 4096,
	    &sc->wpr_meta);
	if (error != 0) {
		device_printf(sc->dev,
		    "gsp_meta: alloc failed (%d)\n", error);
		return (error);
	}

	m = sc->wpr_meta.kva;
	memset(m, 0, NVKM_GSP_FW_WPR_META_SIZE);
	m->magic = NVKM_GSP_FW_WPR_META_MAGIC;
	m->revision = NVKM_GSP_FW_WPR_META_REVISION;
	/*
	 * Everything else left zero. Booter will validate magic/revision
	 * first and report a distinct error code if other fields are bad.
	 * We fill the rest as Phase 0.2.5 progresses (image staging, boot
	 * bin, FRTS layout etc.).
	 */

	device_printf(sc->dev,
	    "gsp_meta: allocated 256 B @ kva=%p paddr=0x%llx "
	    "(magic=0x%llx revision=%llu)\n",
	    m, (unsigned long long)sc->wpr_meta.paddr,
	    (unsigned long long)m->magic, (unsigned long long)m->revision);
	return (0);
}

void
nvkm_gsp_meta_fini(struct nvkm_softc *sc)
{
	if (sc->wpr_meta.kva != NULL)
		nvkm_dmamem_free(sc, &sc->wpr_meta);
}
