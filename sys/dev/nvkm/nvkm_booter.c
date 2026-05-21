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
#include "nvkm_falcon.h"

#include <sys/firmware.h>
#include <sys/libkern.h>		/* memcpy */

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

/*
 * Bootloader DMEM descriptor v2. The booter's nmem (non-secure)
 * bootloader stub reads this at DMEM offset 0 to know where the rest of
 * the image lives in system DMA memory and which DMA index to use for
 * fetching it. Layout matches NVIDIA's flcn_bl_dmem_desc_v2.
 */
struct nvkm_bl_dmem_desc_v2 {
	uint32_t	reserved[4];
	uint32_t	signature[4];
	uint32_t	ctx_dma;
	uint64_t	code_dma_base;
	uint32_t	non_sec_code_off;
	uint32_t	non_sec_code_size;
	uint32_t	sec_code_off;
	uint32_t	sec_code_size;
	uint32_t	code_entry_point;
	uint64_t	data_dma_base;
	uint32_t	data_size;
	uint32_t	argc;
	uint32_t	argv;
} __packed;

/* FALCON_DMAIDX values, see linux/nvkm/engine/falcon.h. */
#define NVKM_FLCN_DMAIDX_PHYS_SYS_NCOH	4

/* FBIF TRANSCFG[ctx_dma]: tells Falcon how to interpret this DMA index.
 * Value 0x5 == TARGET=NONCOHERENT_SYSMEM | MEMTYPE=PHYSICAL. */
#define NVKM_FBIF_TRANSCFG(i)		(0x600u + (i) * 4u)
#define NVKM_FBIF_TRANSCFG_NCOH_PHYS	0x00000005u

int
nvkm_booter_load_and_start(struct nvkm_softc *sc)
{
	const struct nvkm_booter_info *bi = &sc->booter;
	struct nvkm_falcon *sec2 = sc->sec2;
	struct nvkm_bl_dmem_desc_v2 desc;
	uint32_t mb0, mb1, cpuctl, dmactl;
	uint32_t imem_top_off;
	int error;

	if (bi->blob == NULL || sec2 == NULL) {
		device_printf(sc->dev,
		    "booter: cannot start (booter info or sec2 missing)\n");
		return (ENXIO);
	}

	/* 1. Allocate DMA-coherent buffer of bi->data_size and copy the
	 * data section into it. The GPU will DMA from here. */
	error = nvkm_dmamem_alloc(sc, bi->data_size, 4096, &sc->booter_dma);
	if (error != 0) {
		device_printf(sc->dev, "booter: dma alloc failed (%d)\n", error);
		return (error);
	}
	memcpy(sc->booter_dma.kva, bi->blob + bi->data_offset, bi->data_size);
	device_printf(sc->dev,
	    "booter: staged %u bytes to kva=%p paddr=%#jx\n",
	    bi->data_size, sc->booter_dma.kva,
	    (uintmax_t)sc->booter_dma.paddr);

	/*
	 * 1b. Patch the HS signature into the staged blob the way
	 * nouveau nvkm_falcon_fw_patch does. The blob carries
	 * num_sig signatures at blob[sig_prod_offset], each
	 * sig_prod_size / num_sig bytes. The HS auth hardware looks
	 * for the signature at blob[patch_loc]. With num_sig = 1 we
	 * just copy that single signature from sig_prod_offset to
	 * patch_loc inside the staging buffer (offsets are relative
	 * to the start of the blob; subtract data_offset to land in
	 * the kva-mapped staging copy).
	 */
	if (bi->num_sig > 0 && bi->sig_prod_size > 0 &&
	    bi->patch_loc >= bi->data_offset) {
		uint32_t sig_size = bi->sig_prod_size / bi->num_sig;
		uint32_t src_off  = bi->sig_prod_offset +
		    bi->patch_sig * sig_size;
		uint32_t dst_in_kva = bi->patch_loc - bi->data_offset;

		if (src_off + sig_size <= bi->blob_size &&
		    dst_in_kva + sig_size <= bi->data_size) {
			memcpy((uint8_t *)sc->booter_dma.kva + dst_in_kva,
			    bi->blob + src_off, sig_size);
			device_printf(sc->dev,
			    "booter: patched %u-byte sig idx=%u from blob+0x%x to dmem(blob+0x%x)\n",
			    sig_size, bi->patch_sig, src_off, bi->patch_loc);
		} else {
			device_printf(sc->dev,
			    "booter: sig patch OOB (src=0x%x+%u dst=0x%x+%u)\n",
			    src_off, sig_size, bi->patch_loc, sig_size);
		}
	}

	/* 2. Build the bootloader DMEM descriptor v2. */
	memset(&desc, 0, sizeof(desc));
	desc.ctx_dma          = NVKM_FLCN_DMAIDX_PHYS_SYS_NCOH;
	desc.code_dma_base    = sc->booter_dma.paddr;
	desc.non_sec_code_off = bi->nmem_offset;
	desc.non_sec_code_size = bi->nmem_size;
	desc.sec_code_off     = bi->imem_offset;
	desc.sec_code_size    = bi->imem_size;
	desc.code_entry_point = 0;
	desc.data_dma_base    = sc->booter_dma.paddr + bi->dmem_offset;
	desc.data_size        = bi->dmem_size;
	desc.argc             = 0;
	desc.argv             = 0;

	/*
	 * 3a. Reset SEC2 to a known state (matches the per-run reset
	 * open-rm does before each HS Falcon execution).
	 */
	{
		int rerr = nvkm_falcon_reset_eng(sec2);
		if (rerr != 0)
			device_printf(sc->dev,
			    "booter: SEC2 reset_eng returned %d (continuing)\n",
			    rerr);
	}

	/*
	 * 3b. Disable context requirement: sets FBIF_CTL.ALLOW_PHYS_NO_CTX
	 * AND clears DMACTL.REQUIRE_CTX. Without ALLOW_PHYS_NO_CTX the
	 * booter's own nmem stub cannot DMA the rest of the ucode from
	 * sysmem and HS auth fails silently.
	 */
	nvkm_falcon_disable_ctx_req(sec2);

	/* 3c. Program FBIF TRANSCFG for the DMA index the booter uses. */
	nvkm_falcon_mask(sec2, NVKM_FBIF_TRANSCFG(desc.ctx_dma),
	    0x00000007u, NVKM_FBIF_TRANSCFG_NCOH_PHYS);

	/* 5. PIO-write descriptor to DMEM offset 0. */
	error = nvkm_falcon_load_dmem(sec2, &desc, 0, sizeof(desc), 0);
	if (error != 0) {
		device_printf(sc->dev,
		    "booter: load_dmem(desc) failed (%d)\n", error);
		goto out_free;
	}

	/*
	 * 6. PIO-write the non-secure bootloader stub (nmem) into the TOP
	 * of IMEM with tag = boot_addr / IMEM_BLKSIZE. Falcon's tagged
	 * IMEM means BOOTVEC=boot_addr will fetch from this block.
	 */
	imem_top_off = 65536u - NVKM_FLCN_IMEM_BLKSIZE;	/* 0xff00 */
	error = nvkm_falcon_load_imem(sec2,
	    bi->blob + bi->data_offset + bi->nmem_offset,
	    imem_top_off,
	    bi->nmem_size,
	    bi->boot_addr / NVKM_FLCN_IMEM_BLKSIZE,
	    0, false);
	if (error != 0) {
		device_printf(sc->dev,
		    "booter: load_imem(bootloader) failed (%d)\n", error);
		goto out_free;
	}

	/*
	 * 7. Set BOOTVEC and mailbox inputs. The booter expects the
	 * sysmem physical address of the GspFwWprMeta struct in
	 * MAILBOX0 (low 32) / MAILBOX1 (high 32). Without this the
	 * booter halts with mb0 = 0x31 ("no wpr_meta provided").
	 */
	nvkm_falcon_set_bootvec(sec2, bi->boot_addr);
	if (sc->wpr_meta.kva != NULL) {
		uint64_t mp = sc->wpr_meta.paddr;
		nvkm_falcon_wr32(sec2, NVKM_FLCN_MAILBOX0,
		    (uint32_t)(mp & 0xffffffffu));
		nvkm_falcon_wr32(sec2, NVKM_FLCN_MAILBOX1,
		    (uint32_t)(mp >> 32));
		device_printf(sc->dev,
		    "booter: passing wpr_meta sysmem=0x%llx (mb0=0x%08x mb1=0x%08x)\n",
		    (unsigned long long)mp,
		    (uint32_t)(mp & 0xffffffffu),
		    (uint32_t)(mp >> 32));
	} else {
		nvkm_falcon_wr32(sec2, NVKM_FLCN_MAILBOX0, 0);
		nvkm_falcon_wr32(sec2, NVKM_FLCN_MAILBOX1, 0);
	}

	device_printf(sc->dev,
	    "booter: starting SEC2 (bootvec=0x%x, ctx_dma=%u, dma_base=%#jx)\n",
	    bi->boot_addr, desc.ctx_dma, (uintmax_t)desc.code_dma_base);

	/* 8. Start and wait. */
	nvkm_falcon_start(sec2);
	error = nvkm_falcon_wait_for_halt(sec2, 2000000);	/* 2 s */

	mb0    = nvkm_falcon_rd32(sec2, NVKM_FLCN_MAILBOX0);
	mb1    = nvkm_falcon_rd32(sec2, NVKM_FLCN_MAILBOX1);
	cpuctl = nvkm_falcon_rd32(sec2, NVKM_FLCN_CPUCTL);
	dmactl = nvkm_falcon_rd32(sec2, NVKM_FLCN_DMACTL);

	device_printf(sc->dev,
	    "booter: %s mb0=0x%08x mb1=0x%08x cpuctl=0x%08x dmactl=0x%08x\n",
	    error == 0 ? "halted" : "timed out",
	    mb0, mb1, cpuctl, dmactl);

	return (error);

out_free:
	nvkm_dmamem_free(sc, &sc->booter_dma);
	return (error);
}

void
nvkm_booter_release(struct nvkm_softc *sc)
{
	if (sc->booter_dma.kva != NULL)
		nvkm_dmamem_free(sc, &sc->booter_dma);
}
