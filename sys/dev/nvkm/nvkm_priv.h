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
 * VBIOS image cache. PCI Option ROMs are chained: a legacy x86 image is
 * commonly followed by one or more UEFI images. The PROM aperture itself
 * is 1 MiB; we cap the cache at 256 KiB which is enough for both images
 * plus the FwSec blob needed by GSP boot.
 */
#define NVKM_VBIOS_MAX_SIZE	0x40000

#define NVKM_NUM_BARS		6

struct nvkm_softc {
	device_t		dev;

	int			bar_rid[NVKM_NUM_BARS];
	struct resource		*bar_res[NVKM_NUM_BARS];

	uint8_t			*vbios;
	uint32_t		vbios_size;
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

/* nvkm_bios.c */
int	nvkm_bios_init(struct nvkm_softc *sc);
void	nvkm_bios_fini(struct nvkm_softc *sc);

#endif /* _NVKM_PRIV_H_ */
