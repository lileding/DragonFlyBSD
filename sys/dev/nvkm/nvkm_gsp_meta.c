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

#define NVKM_GSP_FW_WPR_META_MAGIC    0xdc3aae21371a60b3ULL
#define NVKM_GSP_FW_WPR_META_REVISION 1ULL
#define NVKM_GSP_FW_WPR_META_SIZE     256

/*
 * Match the open-rm layout byte-for-byte. Field names follow nouveau's
 * lower_camel_case for easier diffing against their tu102 implementation.
 */
struct nvkm_gsp_wpr_meta {
	uint64_t magic;
	uint64_t revision;

	/* sysmem inputs */
	uint64_t sysmemAddrOfRadix3Elf;
	uint64_t sizeOfRadix3Elf;
	uint64_t sysmemAddrOfBootloader;
	uint64_t sizeOfBootloader;
	uint64_t bootloaderCodeOffset;
	uint64_t bootloaderDataOffset;
	uint64_t bootloaderManifestOffset;
	/* union { signature[2]; freeListWprOffset+pad } -- first variant */
	uint64_t sysmemAddrOfSignature;
	uint64_t sizeOfSignature;

	/* FB layout */
	uint64_t gspFwRsvdStart;
	uint64_t nonWprHeapOffset;
	uint64_t nonWprHeapSize;
	uint64_t gspFwWprStart;
	uint64_t gspFwHeapOffset;
	uint64_t gspFwHeapSize;
	uint64_t gspFwOffset;
	uint64_t bootBinOffset;
	uint64_t frtsOffset;
	uint64_t frtsSize;
	uint64_t gspFwWprEnd;
	uint64_t fbSize;

	uint64_t vgaWorkspaceOffset;
	uint64_t vgaWorkspaceSize;
	uint64_t bootCount;

	/* union { partitionRpc + elf fields | crashReport variant } */
	uint64_t partitionRpcAddr;
	uint16_t partitionRpcRequestOffset;
	uint16_t partitionRpcReplyOffset;
	uint32_t elfCodeOffset;
	uint32_t elfDataOffset;
	uint32_t elfCodeSize;
	uint32_t elfDataSize;
	uint32_t lsUcodeVersion;

	uint8_t  gspFwHeapVfPartitionCount;
	uint8_t  flags;
	uint8_t  padding[2];
	uint32_t pmuReservedSize;

	uint64_t verified;  /* 0 -> unverified, 0xa0... -> verified */
} __packed;

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
