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
 * VBIOS image cache. PCI Option ROMs are chained: a legacy x86 image,
 * one or more UEFI images, and (on NVIDIA cards) private images chained
 * via NPDE that carry the FwSec firmware needed by GSP boot. NVIDIA's
 * own reader sizes this to the full 1 MiB PROM aperture.
 */
#define NVKM_VBIOS_MAX_SIZE	0x100000

#define NVKM_NUM_BARS		6

/*
 * Parsed FWSEC ucode descriptor, populated by nvkm_vbios_bit_init from the
 * VBIOS image. Fields common to V2 and V3 are flattened; version-specific
 * fields live behind a version tag. desc_offset points into sc->vbios where
 * the original FALCON_UCODE_DESC starts.
 */
struct nvkm_fwsec_info {
	bool		present;
	uint8_t		app_id;		/* PROD (0x85) or DBG (0x45) */
	uint32_t	desc_offset;
	uint32_t	desc_version;	/* 2 or 3 */
	uint32_t	desc_size;
	bool		encrypted;

	uint32_t	stored_size;
	uint32_t	interface_offset;
	uint32_t	imem_phys_base;
	uint32_t	imem_load_size;
	uint32_t	imem_virt_base;

	uint32_t	dmem_phys_base;
	uint32_t	dmem_load_size;

	/* V2 only */
	uint32_t	imem_sec_base;
	uint32_t	imem_sec_size;
	uint32_t	dmem_offset;
	uint32_t	virtual_entry;

	/* V3 only */
	uint32_t	pkc_data_offset;
	uint16_t	engine_id_mask;
	uint8_t		ucode_id;
	uint8_t		signature_count;
};

struct nvkm_softc {
	device_t		dev;

	int			bar_rid[NVKM_NUM_BARS];
	struct resource		*bar_res[NVKM_NUM_BARS];

	uint8_t			*vbios;
	uint32_t		vbios_size;
	/*
	 * Offset of the first NVIDIA-extended ROM image (code_type 0xE0)
	 * within sc->vbios. Several VBIOS-internal pointers (notably the
	 * Falcon ucode table pointer reached via BIT) are stored as offsets
	 * relative to this base, not absolute, so we have to remember it.
	 * Zero if no extended image is present.
	 */
	uint32_t		vbios_expansion_rom_off;

	struct nvkm_fwsec_info	fwsec;
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

/* nvkm_vbios_bit.c */
int	nvkm_vbios_bit_init(struct nvkm_softc *sc);

#endif /* _NVKM_PRIV_H_ */
