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

int
nvkm_bios_init(struct nvkm_softc *sc)
{
	uint16_t pcir_off, pcir_vendor, pcir_device;
	uint32_t pcir_class, image_bytes;

	sc->vbios = kmalloc(NVKM_VBIOS_MAX_SIZE, M_NVKM_VBIOS,
	    M_WAITOK | M_ZERO);
	sc->vbios_size = NVKM_VBIOS_MAX_SIZE;

	nvkm_rom_shadow(sc, false);
	nvkm_prom_read(sc, sc->vbios, 0, NVKM_VBIOS_MAX_SIZE);
	nvkm_rom_shadow(sc, true);

	if (sc->vbios[0] != 0x55 || sc->vbios[1] != 0xaa) {
		device_printf(sc->dev,
		    "VBIOS: missing 55AA signature (got %02x %02x); "
		    "first 16 bytes: "
		    "%02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x\n",
		    sc->vbios[0], sc->vbios[1],
		    sc->vbios[0],  sc->vbios[1],  sc->vbios[2],  sc->vbios[3],
		    sc->vbios[4],  sc->vbios[5],  sc->vbios[6],  sc->vbios[7],
		    sc->vbios[8],  sc->vbios[9],  sc->vbios[10], sc->vbios[11],
		    sc->vbios[12], sc->vbios[13], sc->vbios[14], sc->vbios[15]);
		kfree(sc->vbios, M_NVKM_VBIOS);
		sc->vbios = NULL;
		sc->vbios_size = 0;
		return (EIO);
	}

	/* Size in 512-byte units stored at offset 2 of the image header. */
	image_bytes = (uint32_t)sc->vbios[2] * 512;

	/* PCIR data structure pointer is a 16-bit LE value at offset 0x18. */
	pcir_off = (uint16_t)sc->vbios[0x18] |
	    ((uint16_t)sc->vbios[0x19] << 8);

	device_printf(sc->dev,
	    "VBIOS: signature OK, image=%u bytes, PCIR ptr=0x%04x\n",
	    image_bytes, pcir_off);

	if (pcir_off + 0x18 > sc->vbios_size ||
	    memcmp(&sc->vbios[pcir_off], "PCIR", 4) != 0) {
		device_printf(sc->dev,
		    "VBIOS: PCIR structure not found at 0x%04x\n", pcir_off);
		return (0);
	}

	pcir_vendor = (uint16_t)sc->vbios[pcir_off + 4] |
	    ((uint16_t)sc->vbios[pcir_off + 5] << 8);
	pcir_device = (uint16_t)sc->vbios[pcir_off + 6] |
	    ((uint16_t)sc->vbios[pcir_off + 7] << 8);
	/* PCI class code is a 3-byte LE field at PCIR offset 0x0d. */
	pcir_class  = (uint32_t)sc->vbios[pcir_off + 0x0d] |
	    ((uint32_t)sc->vbios[pcir_off + 0x0e] << 8) |
	    ((uint32_t)sc->vbios[pcir_off + 0x0f] << 16);

	device_printf(sc->dev,
	    "VBIOS PCIR: vendor=0x%04x device=0x%04x class=0x%06x "
	    "code_type=0x%02x last=%d\n",
	    pcir_vendor, pcir_device, pcir_class,
	    sc->vbios[pcir_off + 0x14],
	    (sc->vbios[pcir_off + 0x15] & 0x80) ? 1 : 0);

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
