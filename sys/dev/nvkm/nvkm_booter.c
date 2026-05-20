/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Booter ucode container parser.
 *
 * NVIDIA's GSP-boot blobs (booter_load-570.144.bin and friends) wrap
 * the actual code and data inside three nested headers, all little
 * endian:
 *
 *   Offset 0:                 nvkm_booter_bin_hdr
 *      |
 *      bin_hdr.header_offset -> nvkm_booter_hs_header_v2
 *                                  |
 *                                  hs.header_offset
 *                                       -> nvkm_booter_hs_load_header_v2
 *
 *      bin_hdr.data_offset   -> raw code / data section,
 *                               described by the load header above.
 *
 * The load header tells us:
 *   * os_code  -- non-secure ("nmem") portion
 *   * os_data  -- DMEM data
 *   * app[0]   -- secure ("imem") portion, where HS code lives
 *
 * Structure layouts and field semantics are NVIDIA's, taken from
 * linux/drivers/gpu/drm/nouveau/include/nvfw/{fw,hs}.h (MIT).
 */

#include "nvkm_priv.h"

#include <sys/firmware.h>

struct nvkm_booter_bin_hdr {
	uint32_t bin_magic;	/* 0x10de */
	uint32_t bin_ver;
	uint32_t bin_size;
	uint32_t header_offset;	/* -> hs_header */
	uint32_t data_offset;
	uint32_t data_size;
} __packed;

struct nvkm_booter_hs_header_v2 {
	uint32_t sig_prod_offset;
	uint32_t sig_prod_size;
	uint32_t patch_loc;
	uint32_t patch_sig;
	uint32_t meta_data_offset;
	uint32_t meta_data_size;
	uint32_t num_sig;
	uint32_t header_offset;	/* -> hs_load_header */
	uint32_t header_size;
} __packed;

struct nvkm_booter_hs_load_header_v2 {
	uint32_t os_code_offset;	/* non-secure code in data section */
	uint32_t os_code_size;
	uint32_t os_data_offset;	/* dmem data section */
	uint32_t os_data_size;
	uint32_t num_apps;
	struct {
		uint32_t offset;	/* secure code section */
		uint32_t size;
		uint32_t data_offset;
		uint32_t data_size;
	} app[];
} __packed;

#define NVKM_BOOTER_BIN_MAGIC	0x000010deu

int
nvkm_booter_parse(struct nvkm_softc *sc, const struct firmware *fw,
    struct nvkm_booter_info *info)
{
	const uint8_t *base = fw->data;
	const struct nvkm_booter_bin_hdr *bh;
	const struct nvkm_booter_hs_header_v2 *hs;
	const struct nvkm_booter_hs_load_header_v2 *lh;
	uint32_t patch_loc, patch_sig, num_sig;

	if (fw->datasize < sizeof(*bh))
		return (EIO);
	bh = (const struct nvkm_booter_bin_hdr *)base;
	if (bh->bin_magic != NVKM_BOOTER_BIN_MAGIC) {
		device_printf(sc->dev,
		    "booter: bad bin magic 0x%08x\n", bh->bin_magic);
		return (EIO);
	}
	/*
	 * bin_size is informational and may exceed the actual file size
	 * (linux-firmware seems to strip some trailing material). Validate
	 * only the offsets that we actually index into.
	 */
	if (bh->header_offset >= fw->datasize ||
	    bh->data_offset   >= fw->datasize ||
	    bh->data_offset + bh->data_size > fw->datasize) {
		device_printf(sc->dev,
		    "booter: bin_hdr offsets out of range "
		    "(hdr=0x%x data=0x%x+%u file=%zu)\n",
		    bh->header_offset, bh->data_offset, bh->data_size,
		    fw->datasize);
		return (EIO);
	}

	hs = (const struct nvkm_booter_hs_header_v2 *)
	    (base + bh->header_offset);
	if (bh->header_offset + sizeof(*hs) > fw->datasize)
		return (EIO);
	if (hs->header_offset + sizeof(*lh) > fw->datasize)
		return (EIO);

	lh = (const struct nvkm_booter_hs_load_header_v2 *)
	    (base + hs->header_offset);
	if (lh->num_apps < 1) {
		device_printf(sc->dev,
		    "booter: no apps in load header (num_apps=%u)\n",
		    lh->num_apps);
		return (EIO);
	}

	/* Patch info: each is a u32 at an absolute offset within the blob. */
	if (hs->patch_loc + 4 > fw->datasize ||
	    hs->patch_sig + 4 > fw->datasize ||
	    hs->num_sig   + 4 > fw->datasize)
		return (EIO);
	patch_loc = *(const uint32_t *)(base + hs->patch_loc);
	patch_sig = *(const uint32_t *)(base + hs->patch_sig);
	num_sig   = *(const uint32_t *)(base + hs->num_sig);

	info->blob	  = base;
	info->blob_size	  = fw->datasize;
	info->data_offset = bh->data_offset;
	info->data_size	  = bh->data_size;

	info->nmem_offset = lh->os_code_offset;	/* relative to blob */
	info->nmem_size   = lh->os_code_size;
	info->imem_offset = lh->app[0].offset;
	info->imem_size   = lh->app[0].size;
	info->dmem_offset = lh->os_data_offset;
	info->dmem_size   = lh->os_data_size;
	info->boot_addr   = lh->os_code_offset;	/* per nouveau tu102 */

	info->sig_prod_offset = hs->sig_prod_offset;
	info->sig_prod_size   = hs->sig_prod_size;
	info->patch_loc	      = patch_loc;
	info->patch_sig	      = patch_sig;
	info->num_sig	      = num_sig;

	device_printf(sc->dev,
	    "booter: bin magic=0x%x ver=%u size=%u, hs@0x%x ld@0x%x data@0x%x+%u\n",
	    bh->bin_magic, bh->bin_ver, bh->bin_size,
	    bh->header_offset, hs->header_offset,
	    bh->data_offset, bh->data_size);
	device_printf(sc->dev,
	    "booter: nmem(off=0x%x sz=%u) imem(off=0x%x sz=%u) "
	    "dmem(off=0x%x sz=%u) boot_addr=0x%x apps=%u\n",
	    info->nmem_offset, info->nmem_size,
	    info->imem_offset, info->imem_size,
	    info->dmem_offset, info->dmem_size,
	    info->boot_addr, lh->num_apps);
	device_printf(sc->dev,
	    "booter: sig_prod off=0x%x sz=%u, patch loc/sig/num = %u/%u/%u\n",
	    info->sig_prod_offset, info->sig_prod_size,
	    info->patch_loc, info->patch_sig, info->num_sig);

	return (0);
}
