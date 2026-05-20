/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Internal definitions shared across nvkm translation units.
 */

#ifndef _NVKM_PRIV_H_
#define _NVKM_PRIV_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/malloc.h>

#define NVKM_PCI_VENDOR_NVIDIA	0x10de

/* Initial supported device. Phase 0 targets only TU102. */
#define NVKM_PCI_DEVICE_TU102	0x1e07

/* PMC: NVIDIA Master Control. Chip identification. */
#define NV_PMC_BOOT_0		0x00000000

/*
 * TU102 SEC2 engine BAR0 offsets.
 *   NV_PSEC                 = 0x840000..0x843fff
 *   NV_PSEC_FBIF_BASE       = 0x840600
 *   NV_PSEC_FALCON_ENGINE   = 0x8403c0
 * Turing SEC2 is Falcon-only (no RISC-V), so the second register block
 * is unused. From open-rm dev_sec_pri.h / dev_sec_addendum.h.
 */
#define NVKM_TU102_SEC2_BASE	0x00840000
#define NVKM_TU102_SEC2_FBIF	0x00840600

/*
 * PCI cfg-space mirror inside BAR0. cfg.addr is the same (0x088000) for the
 * entire gp100 family and later, including Turing. See linux/drivers/gpu/drm/
 * nouveau/nvkm/subdev/pci/gp100.c.
 */
#define NV_PMC_PCI_CFG		0x00088000
#define NV_PMC_ROM_SHADOW	(NV_PMC_PCI_CFG + 0x50)
#define   NV_PMC_ROM_SHADOW_EN	0x00000001

/* PROM aperture in BAR0: SPI ROM contents visible as MMIO. */
#define NV_PROM			0x00300000
#define NV_PROM_SIZE		0x00100000	/* 1 MiB aperture */

/*
 * PRAMIN aperture: a BAR0-visible 1 MiB window into VRAM. The window's
 * VRAM base is set via NV_PBUS_PRAMIN @ 0x001700 (value is base >> 16).
 * GPU bootcode places the active VBIOS image in VRAM at attach; reading
 * via PRAMIN gets the GPU's working copy, which differs from the raw
 * PROM contents on cards that "stitch" pointers at boot time.
 */
#define NV_PRAMIN		0x00700000
#define NV_PRAMIN_SIZE		0x00100000
#define NV_PBUS_PRAMIN		0x00001700	/* window base, shifted right 16 */

/*
 * Display control regs used to discover where the GPU has staged its
 * runtime VBIOS copy in VRAM. (See nouveau shadowramin.c.)
 *   NV_PDISP_VGA_CR  : present for Volta and Turing (we use Turing path)
 *   bit 3            : aperture enabled
 *   bits 1:0 == 1    : aperture target is VRAM
 *   bits 31:8        : staging address >> 8
 */
#define NV_PDISP_VGA_CR			0x00625f04
#define   NV_PDISP_VGA_CR_TARGET_VRAM	0x00000001u
#define   NV_PDISP_VGA_CR_TARGET_MASK	0x00000003u
#define   NV_PDISP_VGA_CR_ENABLED	0x00000008u
#define NV_PDISP_GENERAL_CTL		0x00021c04
#define   NV_PDISP_GENERAL_CTL_DISABLED	0x00000001u

/*
 * VBIOS image cache. PCI Option ROMs are chained: a legacy x86 image,
 * one or more UEFI images, and (on NVIDIA cards) private images chained
 * via NPDE that carry the FwSec firmware needed by GSP boot. NVIDIA's
 * own reader sizes this to the full 1 MiB PROM aperture.
 */
#define NVKM_VBIOS_MAX_SIZE	0x100000

#define NVKM_NUM_BARS		6

struct firmware;
struct nvkm_falcon;

/*
 * A chunk of system memory that is mapped for DMA and visible to both
 * the host CPU (via kva) and the GPU (via paddr). Used for booter and
 * GSP image staging, WPR descriptors, msgq rings, and similar.
 */
struct nvkm_dmamem {
	void		*kva;
	bus_addr_t	paddr;
	bus_size_t	size;
	bus_dma_tag_t	tag;
	bus_dmamap_t	map;
};

/*
 * Parsed view of a booter / HS-signed Falcon ucode container.
 * All offsets are bytes from the start of the blob.
 */
struct nvkm_booter_info {
	const uint8_t	*blob;
	uint32_t	blob_size;
	uint32_t	data_offset;	/* code+data section start in blob */
	uint32_t	data_size;

	uint32_t	nmem_offset;	/* non-secure code, in blob */
	uint32_t	nmem_size;
	uint32_t	imem_offset;	/* secure code, in blob */
	uint32_t	imem_size;
	uint32_t	dmem_offset;	/* DMEM data, in blob */
	uint32_t	dmem_size;
	uint32_t	boot_addr;	/* Falcon BOOTVEC value */

	uint32_t	sig_prod_offset;
	uint32_t	sig_prod_size;
	uint32_t	patch_loc;
	uint32_t	patch_sig;
	uint32_t	num_sig;
};

struct nvkm_softc {
	device_t		dev;

	int			bar_rid[NVKM_NUM_BARS];
	struct resource		*bar_res[NVKM_NUM_BARS];

	uint8_t			*vbios;
	uint32_t		vbios_size;

	const struct firmware	*fw_booter_load;

	struct nvkm_falcon	*sec2;

	struct nvkm_booter_info	booter;
	struct nvkm_dmamem	booter_dma;	/* staged booter image */
};

static __inline uint32_t
nvkm_rd32(struct nvkm_softc *sc, uint32_t offset)
{
	return (bus_read_4(sc->bar_res[0], offset));
}

static __inline void
nvkm_wr32(struct nvkm_softc *sc, uint32_t offset, uint32_t val)
{
	bus_write_4(sc->bar_res[0], offset, val);
}

/* Little-endian byte-buffer accessors used by ROM/VBIOS parsers. */
static __inline uint16_t
nvkm_le16(const uint8_t *p)
{
	return ((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static __inline uint32_t
nvkm_le32(const uint8_t *p)
{
	return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static __inline uint32_t
nvkm_le24(const uint8_t *p)
{
	return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16));
}

/* nvkm_bios.c */
int	nvkm_bios_init(struct nvkm_softc *sc);
void	nvkm_bios_fini(struct nvkm_softc *sc);

/* nvkm_fw.c */
int	nvkm_fw_init(struct nvkm_softc *sc);
void	nvkm_fw_fini(struct nvkm_softc *sc);

/* nvkm_sec2.c */
int	nvkm_sec2_init(struct nvkm_softc *sc);
void	nvkm_sec2_fini(struct nvkm_softc *sc);

/* nvkm_mem.c -- DMA-coherent memory helpers */
int	nvkm_dmamem_alloc(struct nvkm_softc *sc, bus_size_t size,
	    bus_size_t alignment, struct nvkm_dmamem *out);
void	nvkm_dmamem_free(struct nvkm_softc *sc, struct nvkm_dmamem *mem);

/* nvkm_booter.c -- HS Falcon container parser + boot */
struct firmware;
int	nvkm_booter_parse(struct nvkm_softc *sc, const struct firmware *fw,
	    struct nvkm_booter_info *info);
int	nvkm_booter_load_and_start(struct nvkm_softc *sc);
void	nvkm_booter_release(struct nvkm_softc *sc);

#endif /* _NVKM_PRIV_H_ */
