/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VBIOS reading via the BAR0 PROM aperture.
 *
 * Strategy (Phase 0.1): read the on-card SPI ROM through the PROM window
 * exposed in BAR0 at offset 0x300000. This is the simplest of the methods
 * Linux nouveau supports (PRAMIN / PROM / ACPI_ROM / PCI Expansion ROM /
 * Open Firmware / platform). It has only one prerequisite: clear bit 0 of
 * the ROM-shadow register at BAR0 + 0x088050 before reading, restore it
 * after.
 *
 * PCI Option ROM images are chained. Each image starts with a 0x55 0xAA
 * signature, has a 16-bit LE PCIR pointer at offset 0x18, and the PCIR
 * structure carries an "is-last" bit and an image-length field. We walk
 * the full chain so the FwSec blob needed for GSP boot (in a later
 * image) is reachable.
 */

#include "nvkm_priv.h"

static MALLOC_DEFINE(M_NVKM_VBIOS, "nvkm_vbios", "nvkm VBIOS image cache");

static void
nvkm_rom_shadow(struct nvkm_softc *sc, bool enable)
{
	uint32_t v = nvkm_rd32(sc, NV_PMC_ROM_SHADOW);

	if (enable)
		v |= NV_PMC_ROM_SHADOW_EN;
	else
		v &= ~NV_PMC_ROM_SHADOW_EN;
	nvkm_wr32(sc, NV_PMC_ROM_SHADOW, v);
}

static void
nvkm_prom_read(struct nvkm_softc *sc, uint8_t *buf, uint32_t offset,
    uint32_t length)
{
	uint32_t i, word;

	KASSERT((offset & 3) == 0, ("PROM offset not 4-byte aligned"));
	KASSERT((length & 3) == 0, ("PROM length not 4-byte aligned"));

	for (i = 0; i < length; i += 4) {
		word = nvkm_rd32(sc, NV_PROM + offset + i);
		buf[i + 0] = (uint8_t)(word >>  0);
		buf[i + 1] = (uint8_t)(word >>  8);
		buf[i + 2] = (uint8_t)(word >> 16);
		buf[i + 3] = (uint8_t)(word >> 24);
	}
}

/*
 * Walk one PCI Option ROM image starting at sc->vbios[offset].
 * On success, fills *size with the image length in bytes, *last with the
 * is-last-image indicator, and prints a one-line summary.
 */
static int
nvkm_bios_parse_image(struct nvkm_softc *sc, uint32_t offset, int idx,
    uint32_t *size, int *last)
{
	uint16_t pcir_rel, pcir_vendor, pcir_device;
	uint32_t pcir, pcir_class, image_bytes;
	uint8_t  pcir_code_type, pcir_indicator;

	if (offset + 0x20 > sc->vbios_size)
		return (ENOSPC);

	if (sc->vbios[offset + 0] != 0x55 ||
	    sc->vbios[offset + 1] != 0xaa) {
		device_printf(sc->dev,
		    "VBIOS image %d: bad signature %02x %02x at 0x%05x\n",
		    idx, sc->vbios[offset + 0], sc->vbios[offset + 1], offset);
		return (EIO);
	}

	pcir_rel = (uint16_t)sc->vbios[offset + 0x18] |
	    ((uint16_t)sc->vbios[offset + 0x19] << 8);
	pcir = offset + pcir_rel;

	if (pcir + 0x18 > sc->vbios_size ||
	    memcmp(&sc->vbios[pcir], "PCIR", 4) != 0) {
		device_printf(sc->dev,
		    "VBIOS image %d: PCIR not found (off=0x%05x rel=0x%04x)\n",
		    idx, offset, pcir_rel);
		return (EIO);
	}

	pcir_vendor    = (uint16_t)sc->vbios[pcir + 4] |
	    ((uint16_t)sc->vbios[pcir + 5] << 8);
	pcir_device    = (uint16_t)sc->vbios[pcir + 6] |
	    ((uint16_t)sc->vbios[pcir + 7] << 8);
	pcir_class     = (uint32_t)sc->vbios[pcir + 0x0d] |
	    ((uint32_t)sc->vbios[pcir + 0x0e] << 8) |
	    ((uint32_t)sc->vbios[pcir + 0x0f] << 16);
	image_bytes    = ((uint32_t)sc->vbios[pcir + 0x10] |
	    ((uint32_t)sc->vbios[pcir + 0x11] << 8)) * 512;
	pcir_code_type = sc->vbios[pcir + 0x14];
	pcir_indicator = sc->vbios[pcir + 0x15];

	device_printf(sc->dev,
	    "VBIOS image %d at 0x%05x: %u bytes vendor=0x%04x device=0x%04x "
	    "class=0x%06x code_type=0x%02x last=%d\n",
	    idx, offset, image_bytes,
	    pcir_vendor, pcir_device, pcir_class,
	    pcir_code_type, (pcir_indicator & 0x80) ? 1 : 0);

	*size = image_bytes;
	*last = (pcir_indicator & 0x80) ? 1 : 0;
	return (0);
}

int
nvkm_bios_init(struct nvkm_softc *sc)
{
	uint32_t offset, size;
	int idx, last, error;

	sc->vbios = kmalloc(NVKM_VBIOS_MAX_SIZE, M_NVKM_VBIOS,
	    M_WAITOK | M_ZERO);
	sc->vbios_size = NVKM_VBIOS_MAX_SIZE;

	nvkm_rom_shadow(sc, false);
	nvkm_prom_read(sc, sc->vbios, 0, NVKM_VBIOS_MAX_SIZE);
	nvkm_rom_shadow(sc, true);

	if (sc->vbios[0] != 0x55 || sc->vbios[1] != 0xaa) {
		device_printf(sc->dev,
		    "VBIOS: no 55AA at offset 0 (got %02x %02x)\n",
		    sc->vbios[0], sc->vbios[1]);
		kfree(sc->vbios, M_NVKM_VBIOS);
		sc->vbios = NULL;
		sc->vbios_size = 0;
		return (EIO);
	}

	offset = 0;
	idx = 0;
	last = 0;
	while (!last) {
		error = nvkm_bios_parse_image(sc, offset, idx, &size, &last);
		if (error != 0) {
			device_printf(sc->dev,
			    "VBIOS: chain truncated at image %d (offset 0x%05x)\n",
			    idx, offset);
			break;
		}
		idx++;
		if (last)
			break;
		offset += size;
	}

	if (last)
		sc->vbios_size = offset + size;

	device_printf(sc->dev, "VBIOS: %d image%s, total %u bytes\n",
	    idx, idx == 1 ? "" : "s", sc->vbios_size);

	return (0);
}

void
nvkm_bios_fini(struct nvkm_softc *sc)
{
	if (sc->vbios != NULL) {
		kfree(sc->vbios, M_NVKM_VBIOS);
		sc->vbios = NULL;
		sc->vbios_size = 0;
	}
}
