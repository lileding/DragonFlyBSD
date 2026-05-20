/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VBIOS BIT (BIOS Information Table) parser. Locates the FWSEC falcon
 * ucode descriptor inside an already-extracted VBIOS image.
 *
 * Algorithm:
 *   1. Scan sc->vbios for the BIT header signature (0xB8FF + "BIT\0")
 *      and verify the per-header bytewise checksum.
 *   2. Walk BIT tokens immediately following the header until we find
 *      TokenId == BIT_TOKEN_FALCON_DATA (0x70). Its DataPtr fields point
 *      to a FALCON_UCODE_TABLE.
 *   3. The table header tells us EntryCount and EntrySize; walk the
 *      entries looking for ApplicationID == FWSEC_PROD (0x85) or
 *      FWSEC_DBG (0x45). Production is preferred when both are present.
 *   4. The matched entry's DescPtr points to a FALCON_UCODE_DESC_HEADER;
 *      its packed vDesc word carries a 1-byte version (V2 or V3 in
 *      practice). Dispatch on version, parse the body, store the result
 *      in sc->fwsec.
 *
 * Source references:
 *   open-rm/src/nvidia/src/kernel/gpu/gsp/kernel_gsp_fwsec.c
 *     (s_vbiosFindBitHeader, s_vbiosParseFwsecUcodeDescFromBit)
 *   nvkm_bit.h          (the wire-level structure definitions in DFly form)
 */

#include "nvkm_priv.h"
#include "nvkm_bit.h"

#include <sys/libkern.h>		/* bzero */

/*
 * Scan the whole VBIOS for BIT header instances. Returns the address of
 * the n-th one (start_from=0 → first; pass start_from = previous_addr + 1
 * to continue scanning).
 */
static int
nvkm_vbios_find_bit_header_from(struct nvkm_softc *sc, uint32_t start_from,
    uint32_t *bit_addr)
{
	const uint8_t *buf = sc->vbios;
	uint32_t size = sc->vbios_size;
	uint32_t addr;

	if (buf == NULL || size < 12)
		return (ENXIO);

	for (addr = start_from; addr + 12 <= size; addr++) {
		uint16_t id;
		uint32_t sig;
		uint32_t header_size;
		uint32_t j, checksum;

		id  = nvkm_le16(&buf[addr]);
		if (id != NVKM_BIT_HEADER_ID)
			continue;
		sig = nvkm_le32(&buf[addr + 2]);
		if (sig != NVKM_BIT_HEADER_SIGNATURE)
			continue;

		header_size = buf[addr + NVKM_BIT_HEADER_OFF_HEADER_SIZE];
		if (header_size == 0 || addr + header_size > size)
			continue;

		checksum = 0;
		for (j = 0; j < header_size; j++)
			checksum += buf[addr + j];

		if ((checksum & 0xff) != 0)
			continue;

		*bit_addr = addr;
		return (0);
	}

	return (ENOENT);
}

static int
nvkm_vbios_find_bit_header(struct nvkm_softc *sc, uint32_t *bit_addr)
{
	return (nvkm_vbios_find_bit_header_from(sc, 0, bit_addr));
}

/*
 * Iterate BIT tokens, return the DataPtr/DataSize/DataVersion of the first
 * token whose TokenId matches.
 */
static int
nvkm_vbios_bit_find_token(struct nvkm_softc *sc, uint32_t bit_addr,
    uint8_t want_id, uint32_t *data_ptr, uint16_t *data_size,
    uint8_t *data_version)
{
	const uint8_t *buf = sc->vbios;
	uint32_t hdr_size = buf[bit_addr + NVKM_BIT_HEADER_OFF_HEADER_SIZE];
	uint32_t tok_size = buf[bit_addr + NVKM_BIT_HEADER_OFF_TOKEN_SIZE];
	uint32_t n_tokens = buf[bit_addr + NVKM_BIT_HEADER_OFF_TOKEN_ENTRIES];
	uint32_t base, i;

	if (tok_size != NVKM_BIT_TOKEN_SIZE_6 &&
	    tok_size != NVKM_BIT_TOKEN_SIZE_8) {
		device_printf(sc->dev,
		    "BIT: unsupported token size %u\n", tok_size);
		return (ENOTSUP);
	}

	base = bit_addr + hdr_size;
	if (base + n_tokens * tok_size > sc->vbios_size)
		return (EIO);

	for (i = 0; i < n_tokens; i++) {
		uint32_t off = base + i * tok_size;
		uint8_t id  = buf[off + NVKM_BIT_TOKEN_OFF_TOKEN_ID];

		if (id != want_id)
			continue;
		*data_version = buf[off + NVKM_BIT_TOKEN_OFF_DATA_VERSION];
		*data_size    = nvkm_le16(&buf[off + NVKM_BIT_TOKEN_OFF_DATA_SIZE]);
		if (tok_size == NVKM_BIT_TOKEN_SIZE_8) {
			*data_ptr = nvkm_le32(
			    &buf[off + NVKM_BIT_TOKEN_OFF_DATA_PTR]);
		} else {
			*data_ptr = nvkm_le16(
			    &buf[off + NVKM_BIT_TOKEN_OFF_DATA_PTR]);
		}
		return (0);
	}
	return (ENOENT);
}

static int
nvkm_vbios_parse_fwsec_desc_v2(struct nvkm_softc *sc, uint32_t off,
    struct nvkm_fwsec_info *info)
{
	const uint8_t *p;

	if (off + NVKM_FUD_V2_SIZE > sc->vbios_size)
		return (EIO);
	p = &sc->vbios[off];

	info->desc_version = NVKM_FUDH_VERSION_V2;
	info->stored_size      = nvkm_le32(p + NVKM_FUD_V2_OFF_STORED_SIZE);
	info->virtual_entry    = nvkm_le32(p + NVKM_FUD_V2_OFF_VIRTUAL_ENTRY);
	info->interface_offset = nvkm_le32(p + NVKM_FUD_V2_OFF_INTERFACE_OFFSET);
	info->imem_phys_base   = nvkm_le32(p + NVKM_FUD_V2_OFF_IMEM_PHYS_BASE);
	info->imem_load_size   = nvkm_le32(p + NVKM_FUD_V2_OFF_IMEM_LOAD_SIZE);
	info->imem_virt_base   = nvkm_le32(p + NVKM_FUD_V2_OFF_IMEM_VIRT_BASE);
	info->imem_sec_base    = nvkm_le32(p + NVKM_FUD_V2_OFF_IMEM_SEC_BASE);
	info->imem_sec_size    = nvkm_le32(p + NVKM_FUD_V2_OFF_IMEM_SEC_SIZE);
	info->dmem_offset      = nvkm_le32(p + NVKM_FUD_V2_OFF_DMEM_OFFSET);
	info->dmem_phys_base   = nvkm_le32(p + NVKM_FUD_V2_OFF_DMEM_PHYS_BASE);
	info->dmem_load_size   = nvkm_le32(p + NVKM_FUD_V2_OFF_DMEM_LOAD_SIZE);
	return (0);
}

static int
nvkm_vbios_parse_fwsec_desc_v3(struct nvkm_softc *sc, uint32_t off,
    struct nvkm_fwsec_info *info)
{
	const uint8_t *p;

	if (off + NVKM_FUD_V3_SIZE > sc->vbios_size)
		return (EIO);
	p = &sc->vbios[off];

	info->desc_version = NVKM_FUDH_VERSION_V3;
	info->stored_size      = nvkm_le32(p + NVKM_FUD_V3_OFF_STORED_SIZE);
	info->pkc_data_offset  = nvkm_le32(p + NVKM_FUD_V3_OFF_PKC_DATA_OFFSET);
	info->interface_offset = nvkm_le32(p + NVKM_FUD_V3_OFF_INTERFACE_OFFSET);
	info->imem_phys_base   = nvkm_le32(p + NVKM_FUD_V3_OFF_IMEM_PHYS_BASE);
	info->imem_load_size   = nvkm_le32(p + NVKM_FUD_V3_OFF_IMEM_LOAD_SIZE);
	info->imem_virt_base   = nvkm_le32(p + NVKM_FUD_V3_OFF_IMEM_VIRT_BASE);
	info->dmem_phys_base   = nvkm_le32(p + NVKM_FUD_V3_OFF_DMEM_PHYS_BASE);
	info->dmem_load_size   = nvkm_le32(p + NVKM_FUD_V3_OFF_DMEM_LOAD_SIZE);
	info->engine_id_mask   = nvkm_le16(p + NVKM_FUD_V3_OFF_ENGINE_ID_MASK);
	info->ucode_id         = p[NVKM_FUD_V3_OFF_UCODE_ID];
	info->signature_count  = p[NVKM_FUD_V3_OFF_SIGNATURE_COUNT];
	return (0);
}

/* Parse the descriptor header at desc_off, dispatch by version. */
static int
nvkm_vbios_parse_fwsec_desc(struct nvkm_softc *sc, uint32_t desc_off,
    struct nvkm_fwsec_info *info)
{
	uint32_t vdesc;
	uint32_t version, desc_size;

	if (desc_off + 4 > sc->vbios_size)
		return (EIO);
	vdesc = nvkm_le32(&sc->vbios[desc_off]);

	if (!(vdesc & NVKM_FUDH_FLAG_VERSION_AVAIL)) {
		device_printf(sc->dev,
		    "BIT: FWSEC desc header has no VERSION_AVAIL bit (0x%08x)\n",
		    vdesc);
		return (EIO);
	}
	version   = (vdesc & NVKM_FUDH_VERSION_MASK) >> NVKM_FUDH_VERSION_SHIFT;
	desc_size = (vdesc & NVKM_FUDH_SIZE_MASK)    >> NVKM_FUDH_SIZE_SHIFT;
	info->encrypted = (vdesc & NVKM_FUDH_FLAG_ENCRYPTED) != 0;
	info->desc_offset = desc_off;
	info->desc_size   = desc_size;

	switch (version) {
	case NVKM_FUDH_VERSION_V2:
		return (nvkm_vbios_parse_fwsec_desc_v2(sc, desc_off, info));
	case NVKM_FUDH_VERSION_V3:
		return (nvkm_vbios_parse_fwsec_desc_v3(sc, desc_off, info));
	default:
		device_printf(sc->dev,
		    "BIT: FWSEC desc version %u unsupported\n", version);
		return (ENOTSUP);
	}
}

/*
 * Walk a FALCON_UCODE_TABLE and pick the FWSEC entry.
 * Returns its DescPtr and ApplicationID. Prefers PROD over DBG.
 */
static int
nvkm_vbios_find_fwsec_entry(struct nvkm_softc *sc, uint32_t table_off,
    uint32_t *desc_ptr_out, uint8_t *app_id_out)
{
	const uint8_t *buf = sc->vbios;
	uint8_t version, header_size, entry_size, entry_count;
	uint32_t entries_off, i;
	uint32_t prod_desc = 0, dbg_desc = 0;
	uint8_t  prod_app = 0,  dbg_app = 0;

	if (table_off + NVKM_FUTH_V1_SIZE > sc->vbios_size)
		return (EIO);

	version     = buf[table_off + NVKM_FUTH_OFF_VERSION];
	header_size = buf[table_off + NVKM_FUTH_OFF_HEADER_SIZE];
	entry_size  = buf[table_off + NVKM_FUTH_OFF_ENTRY_SIZE];
	entry_count = buf[table_off + NVKM_FUTH_OFF_ENTRY_COUNT];

	if (version != NVKM_FUTH_VERSION_V1) {
		device_printf(sc->dev,
		    "BIT: FALCON_UCODE_TABLE version %u unsupported\n", version);
		return (ENOTSUP);
	}
	if (entry_size < NVKM_FUTE_V1_SIZE) {
		device_printf(sc->dev,
		    "BIT: FALCON_UCODE_TABLE entry_size %u too small\n",
		    entry_size);
		return (EIO);
	}

	entries_off = table_off + header_size;
	if (entries_off + (uint32_t)entry_count * entry_size > sc->vbios_size)
		return (EIO);

	for (i = 0; i < entry_count; i++) {
		uint32_t off = entries_off + i * entry_size;
		uint8_t  app  = buf[off + NVKM_FUTE_OFF_APP_ID];
		uint32_t dptr = nvkm_le32(&buf[off + NVKM_FUTE_OFF_DESC_PTR]);

		if (app == NVKM_FUTE_APPID_FWSEC_PROD) {
			prod_desc = dptr;
			prod_app  = app;
		} else if (app == NVKM_FUTE_APPID_FWSEC_DBG) {
			dbg_desc = dptr;
			dbg_app  = app;
		}
	}

	if (prod_desc != 0) {
		*desc_ptr_out = prod_desc;
		*app_id_out   = prod_app;
		return (0);
	}
	if (dbg_desc != 0) {
		*desc_ptr_out = dbg_desc;
		*app_id_out   = dbg_app;
		return (0);
	}
	return (ENOENT);
}

/* Locate all BIT headers and report. */
static void
nvkm_vbios_dump_bit_instances(struct nvkm_softc *sc)
{
	uint32_t addr = 0;
	int n = 0;

	while (nvkm_vbios_find_bit_header_from(sc, addr, &addr) == 0) {
		device_printf(sc->dev,
		    "BIT: instance #%d at 0x%05x (%u tokens, tok_size %u, hdr %u)\n",
		    n,
		    addr,
		    sc->vbios[addr + NVKM_BIT_HEADER_OFF_TOKEN_ENTRIES],
		    sc->vbios[addr + NVKM_BIT_HEADER_OFF_TOKEN_SIZE],
		    sc->vbios[addr + NVKM_BIT_HEADER_OFF_HEADER_SIZE]);
		n++;
		addr++;	/* continue scan past this match */
		if (n >= 8)
			break;	/* sanity cap */
	}
}

int
nvkm_vbios_bit_init(struct nvkm_softc *sc)
{
	uint32_t bit_addr;
	uint32_t falcon_data_ptr;
	uint16_t falcon_data_size;
	uint8_t  falcon_data_ver;
	uint32_t table_ptr;
	uint32_t fwsec_desc_ptr;
	uint8_t  fwsec_app_id;
	int error;

	bzero(&sc->fwsec, sizeof(sc->fwsec));

	nvkm_vbios_dump_bit_instances(sc);

	error = nvkm_vbios_find_bit_header(sc, &bit_addr);
	if (error != 0) {
		device_printf(sc->dev, "BIT: header not found (%d)\n", error);
		return (error);
	}
	device_printf(sc->dev,
	    "BIT: header at 0x%05x, %u tokens (size %u each, header %u bytes)\n",
	    bit_addr,
	    sc->vbios[bit_addr + NVKM_BIT_HEADER_OFF_TOKEN_ENTRIES],
	    sc->vbios[bit_addr + NVKM_BIT_HEADER_OFF_TOKEN_SIZE],
	    sc->vbios[bit_addr + NVKM_BIT_HEADER_OFF_HEADER_SIZE]);

	error = nvkm_vbios_bit_find_token(sc, bit_addr,
	    NVKM_BIT_TOKEN_FALCON_DATA, &falcon_data_ptr, &falcon_data_size,
	    &falcon_data_ver);
	if (error != 0) {
		device_printf(sc->dev,
		    "BIT: FALCON_DATA token not found (%d)\n", error);
		return (error);
	}
	if (falcon_data_size < 4) {
		device_printf(sc->dev,
		    "BIT: FALCON_DATA data too small (%u bytes)\n",
		    falcon_data_size);
		return (EIO);
	}
	if (falcon_data_ptr + 4 > sc->vbios_size)
		return (EIO);
	table_ptr = nvkm_le32(
	    &sc->vbios[falcon_data_ptr + NVKM_BIT_FALCON_DATA_OFF_TABLE_PTR]);
	/*
	 * NVIDIA stores the Falcon ucode table pointer relative to the start
	 * of the first NV-extended ROM image (code_type 0xE0), not absolute.
	 * Relocate it. See open-rm s_vbiosParseFwsecUcodeDescFromBit.
	 */
	table_ptr += sc->vbios_expansion_rom_off;
	device_printf(sc->dev,
	    "BIT: FALCON_DATA v%u at 0x%05x, table @ 0x%05x "
	    "(raw 0x%x + ext_rom 0x%x)\n",
	    falcon_data_ver, falcon_data_ptr, table_ptr,
	    table_ptr - sc->vbios_expansion_rom_off,
	    sc->vbios_expansion_rom_off);

	error = nvkm_vbios_find_fwsec_entry(sc, table_ptr, &fwsec_desc_ptr,
	    &fwsec_app_id);
	if (error != 0) {
		device_printf(sc->dev,
		    "BIT: FWSEC entry not found in ucode table (%d)\n", error);
		return (error);
	}
	device_printf(sc->dev,
	    "BIT: FWSEC entry app_id=0x%02x (%s), desc @ 0x%05x\n",
	    fwsec_app_id,
	    fwsec_app_id == NVKM_FUTE_APPID_FWSEC_PROD ? "PROD" : "DBG",
	    fwsec_desc_ptr);

	sc->fwsec.app_id = fwsec_app_id;
	error = nvkm_vbios_parse_fwsec_desc(sc, fwsec_desc_ptr, &sc->fwsec);
	if (error != 0) {
		bzero(&sc->fwsec, sizeof(sc->fwsec));
		return (error);
	}
	sc->fwsec.present = true;

	device_printf(sc->dev,
	    "BIT: FWSEC desc V%u size=%u stored_size=%u encrypted=%d\n",
	    sc->fwsec.desc_version, sc->fwsec.desc_size,
	    sc->fwsec.stored_size, sc->fwsec.encrypted ? 1 : 0);
	device_printf(sc->dev,
	    "BIT: FWSEC IMEM phys=0x%x virt=0x%x load=%u",
	    sc->fwsec.imem_phys_base, sc->fwsec.imem_virt_base,
	    sc->fwsec.imem_load_size);
	if (sc->fwsec.desc_version == NVKM_FUDH_VERSION_V2 &&
	    sc->fwsec.imem_sec_size != 0) {
		kprintf(" sec_base=0x%x sec_size=%u",
		    sc->fwsec.imem_sec_base, sc->fwsec.imem_sec_size);
	}
	kprintf("\n");
	device_printf(sc->dev,
	    "BIT: FWSEC DMEM phys=0x%x load=%u intf=0x%x",
	    sc->fwsec.dmem_phys_base, sc->fwsec.dmem_load_size,
	    sc->fwsec.interface_offset);
	if (sc->fwsec.desc_version == NVKM_FUDH_VERSION_V2)
		kprintf(" dmem_offset=0x%x", sc->fwsec.dmem_offset);
	kprintf("\n");

	return (0);
}
