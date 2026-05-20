/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VBIOS reading via the BAR0 PROM aperture.
 *
 * The on-card SPI ROM is exposed in BAR0 at offset NV_PROM (0x300000),
 * 1 MiB wide. Clearing bit 0 of the ROM-shadow control register at
 * BAR0 + 0x088050 enables reading; the bit is restored afterwards so
 * the host's option-ROM emulator continues to work.
 *
 * PCI Option ROMs are chained. Each image starts with the standard
 * 0x55 0xAA signature, carries a PCIR "PCI Data Structure" at the
 * pointer in offset 0x18, and (for NVIDIA images) follows that with a
 * private "NPDE" extension at the next 16-byte boundary. NPDE, when
 * present, supplies the actual sub-image length and the last-image flag,
 * overriding the equivalent PCIR fields. See nvkm_rom.h for the
 * authoritative definitions copied from open-rm.
 *
 * Source references:
 *   linux/drivers/gpu/drm/nouveau/nvkm/subdev/bios/shadowrom.c     (PROM)
 *   linux/drivers/gpu/drm/nouveau/nvkm/subdev/pci/base.c           (rom_shadow)
 *   open-rm/src/nvidia/src/kernel/gpu/gsp/arch/turing/
 *      kernel_gsp_vbios_tu102.c                                     (NPDE chain)
 */

#include "nvkm_priv.h"
#include "nvkm_rom.h"

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
 * Parse a single PCI Option ROM image at sc->vbios[offset].
 *
 * On success returns 0 and fills *size with the byte size to advance to
 * reach the next image, *last with the is-last-image flag, *npde with
 * 1 if an NPDE extension was used to override PCIR fields.
 */
static int
nvkm_bios_parse_image(struct nvkm_softc *sc, uint32_t offset, int idx,
    uint32_t *size, int *last, int *npde_used)
{
	uint16_t pcir_rel, pcir_struct_len, vendor, device;
	uint32_t pcir, class_code, image_bytes, advance_bytes;
	uint8_t  code_type, indicator;
	uint32_t npde_off;

	*npde_used = 0;

	if (offset + 0x20 > sc->vbios_size)
		return (ENOSPC);

	{
		uint16_t sig = nvkm_le16(&sc->vbios[offset]);

		if (sig != NVKM_ROM_SIG_STD &&
		    sig != NVKM_ROM_SIG_NV &&
		    sig != NVKM_ROM_SIG_NV2) {
			device_printf(sc->dev,
			    "VBIOS image %d: bad ROM signature 0x%04x at 0x%05x\n",
			    idx, sig, offset);
			return (EIO);
		}
	}

	pcir_rel = nvkm_le16(&sc->vbios[offset + NVKM_ROM_HDR_OFF_PCIR_PTR]);
	pcir = offset + pcir_rel;

	if (pcir + 0x18 > sc->vbios_size) {
		device_printf(sc->dev,
		    "VBIOS image %d: PCIR beyond buffer (off=0x%05x rel=0x%04x)\n",
		    idx, offset, pcir_rel);
		return (EIO);
	}
	{
		uint32_t pcir_sig = nvkm_le32(&sc->vbios[pcir + NVKM_PCIR_OFF_SIG]);

		if (pcir_sig != NVKM_PCIR_SIG_PCIR &&
		    pcir_sig != NVKM_PCIR_SIG_NPDS &&
		    pcir_sig != NVKM_PCIR_SIG_RGIS) {
			device_printf(sc->dev,
			    "VBIOS image %d: bad PCIR signature 0x%08x at 0x%05x\n",
			    idx, pcir_sig, pcir);
			return (EIO);
		}
	}

	pcir_struct_len = nvkm_le16(&sc->vbios[pcir + NVKM_PCIR_OFF_STRUCT_LEN]);
	vendor          = nvkm_le16(&sc->vbios[pcir + NVKM_PCIR_OFF_VENDOR_ID]);
	device          = nvkm_le16(&sc->vbios[pcir + NVKM_PCIR_OFF_DEVICE_ID]);
	class_code      = nvkm_le24(&sc->vbios[pcir + NVKM_PCIR_OFF_CLASS_CODE]);
	image_bytes     = (uint32_t)nvkm_le16(&sc->vbios[pcir + NVKM_PCIR_OFF_IMAGE_LEN])
	    * NVKM_ROM_BLOCK_SIZE;
	code_type       = sc->vbios[pcir + NVKM_PCIR_OFF_CODE_TYPE];
	indicator       = sc->vbios[pcir + NVKM_PCIR_OFF_INDICATOR];

	advance_bytes = image_bytes;
	*last = (indicator & NVKM_PCIR_LAST_IMAGE_BIT) ? 1 : 0;

	/*
	 * Look for an NPDE extension at the 16-byte-aligned offset right
	 * after the PCIR. NPDE.subimage_len overrides PCIR.image_len for
	 * the purpose of advancing to the next image; NPDE.last_image (if
	 * within NPDE's length) overrides PCIR's last-image bit.
	 */
	npde_off = (pcir + pcir_struct_len + 0xFu) & ~0xFu;
	if (npde_off + 0x10u <= sc->vbios_size &&
	    nvkm_le32(&sc->vbios[npde_off + NVKM_NPDE_OFF_SIG]) == NVKM_NPDE_SIG) {
		uint16_t rev = nvkm_le16(&sc->vbios[npde_off + NVKM_NPDE_OFF_REV]);
		uint16_t npde_len = nvkm_le16(&sc->vbios[npde_off + NVKM_NPDE_OFF_LEN]);

		if (rev == NVKM_NPDE_REV_10 || rev == NVKM_NPDE_REV_11) {
			uint32_t sub_bytes = (uint32_t)nvkm_le16(
			    &sc->vbios[npde_off + NVKM_NPDE_OFF_SUBIMAGE_LEN]) *
			    NVKM_ROM_BLOCK_SIZE;

			*npde_used = 1;
			advance_bytes = sub_bytes;

			if (NVKM_NPDE_OFF_LAST_IMAGE + 1u <= npde_len) {
				*last = (sc->vbios[npde_off +
				    NVKM_NPDE_OFF_LAST_IMAGE] &
				    NVKM_PCIR_LAST_IMAGE_BIT) ? 1 : 0;
			} else if (sub_bytes < image_bytes) {
				/*
				 * NPDE didn't carry the last-image bit but
				 * sub-image is smaller than full image: by
				 * convention, more images follow.
				 */
				*last = 0;
			}
		}
	}

	device_printf(sc->dev,
	    "VBIOS image %d at 0x%05x: %u bytes vendor=0x%04x device=0x%04x "
	    "class=0x%06x code_type=0x%02x last=%d%s\n",
	    idx, offset, advance_bytes,
	    vendor, device, class_code, code_type, *last,
	    *npde_used ? " (NPDE)" : "");

	*size = advance_bytes;
	return (0);
}

int
nvkm_bios_init(struct nvkm_softc *sc)
{
	uint32_t offset, size;
	int idx, last, npde_used, error;

	sc->vbios = kmalloc(NVKM_VBIOS_MAX_SIZE, M_NVKM_VBIOS,
	    M_WAITOK | M_ZERO);
	sc->vbios_size = NVKM_VBIOS_MAX_SIZE;

	nvkm_rom_shadow(sc, false);
	nvkm_prom_read(sc, sc->vbios, 0, NVKM_VBIOS_MAX_SIZE);
	nvkm_rom_shadow(sc, true);

	{
		uint16_t sig = nvkm_le16(&sc->vbios[0]);

		if (sig != NVKM_ROM_SIG_STD &&
		    sig != NVKM_ROM_SIG_NV &&
		    sig != NVKM_ROM_SIG_NV2) {
			device_printf(sc->dev,
			    "VBIOS: no valid ROM signature at offset 0 (got 0x%04x)\n",
			    sig);
			kfree(sc->vbios, M_NVKM_VBIOS);
			sc->vbios = NULL;
			sc->vbios_size = 0;
			return (EIO);
		}
	}

	offset = 0;
	idx = 0;
	last = 0;
	size = 0;
	while (!last) {
		error = nvkm_bios_parse_image(sc, offset, idx, &size, &last,
		    &npde_used);
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
