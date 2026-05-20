/*-
 * SPDX-License-Identifier: MIT
 *
 * BIOS Information Table (BIT) and Falcon ucode descriptor definitions
 * used to locate FWSEC inside a VBIOS image.
 *
 * Constant values are taken verbatim from NVIDIA open-gpu-kernel-modules
 * 570.144,
 *   src/nvidia/src/kernel/gpu/gsp/kernel_gsp_fwsec.c
 * Copyright (c) 2001-2024 NVIDIA CORPORATION & AFFILIATES, MIT licensed.
 *
 * Naming converted to NVKM_BIT_*. Field layouts are documented in
 * comments above the offset defines so we never need to reach into
 * NVIDIA's typedef'd structs (which depend on NvU8/NvU16/NvU32 types
 * and the NVOC headers).
 */

#ifndef _NVKM_BIT_H_
#define _NVKM_BIT_H_

/* ----------------------------------------------------------------------
 * BIT header
 *
 * Layout (BIT_HEADER_V1_00):
 *   0x00 Id           u16  == 0xB8FF
 *   0x02 Signature    u32  == "BIT\0" (0x00544942)
 *   0x06 BCD_Version  u16
 *   0x08 HeaderSize   u8
 *   0x09 TokenSize    u8
 *   0x0A TokenEntries u8
 *   0x0B HeaderChksum u8     -- sum of HeaderSize bytes == 0 (mod 0x100)
 */
#define NVKM_BIT_HEADER_ID			0xB8FF
#define NVKM_BIT_HEADER_SIGNATURE		0x00544942u	/* "BIT\0" */
#define NVKM_BIT_HEADER_OFF_BCD_VERSION		0x06
#define NVKM_BIT_HEADER_OFF_HEADER_SIZE		0x08
#define NVKM_BIT_HEADER_OFF_TOKEN_SIZE		0x09
#define NVKM_BIT_HEADER_OFF_TOKEN_ENTRIES	0x0A
#define NVKM_BIT_HEADER_OFF_CHECKSUM		0x0B

/* ----------------------------------------------------------------------
 * BIT token
 *
 * Layout (BIT_TOKEN_V1_00):
 *   0x00 TokenId     u8
 *   0x01 DataVersion u8
 *   0x02 DataSize    u16
 *   0x04 DataPtr     u32
 *
 * Token rows are HeaderTokenSize bytes; TokenSize 8 is the standard
 * layout above. Some older BIOSes use TokenSize 6 with packed
 * DataPtr/DataSize fields; we only support the 8-byte variant.
 */
#define NVKM_BIT_TOKEN_OFF_TOKEN_ID		0x00
#define NVKM_BIT_TOKEN_OFF_DATA_VERSION		0x01
#define NVKM_BIT_TOKEN_OFF_DATA_SIZE		0x02
#define NVKM_BIT_TOKEN_OFF_DATA_PTR		0x04
/*
 * Two token layouts exist (open-rm: BIT_TOKEN_V1_00_SIZE_6 vs SIZE_8).
 * The DataPtr field is u32 in the 8-byte layout and u16 in the 6-byte
 * layout (older BIOSes); everything else is identical and at the same
 * offsets.
 */
#define NVKM_BIT_TOKEN_SIZE_6			6u
#define NVKM_BIT_TOKEN_SIZE_8			8u

#define NVKM_BIT_TOKEN_BIOSDATA			0x42	/* 'B' */
#define NVKM_BIT_TOKEN_FALCON_DATA		0x70	/* 'p' */

/* ----------------------------------------------------------------------
 * BIT_DATA_FALCON_DATA_V2 (data referenced by FALCON_DATA token)
 *
 * Layout:
 *   0x00 FalconUcodeTablePtr  u32  -> FALCON_UCODE_TABLE_HDR
 */
#define NVKM_BIT_FALCON_DATA_OFF_TABLE_PTR	0x00

/* ----------------------------------------------------------------------
 * Falcon Ucode Table header (FALCON_UCODE_TABLE_HDR_V1):
 *   0x00 Version       u8  == 1
 *   0x01 HeaderSize    u8  == 6
 *   0x02 EntrySize     u8  == 6
 *   0x03 EntryCount    u8
 *   0x04 DescVersion   u8
 *   0x05 DescSize      u8
 */
#define NVKM_FUTH_OFF_VERSION			0x00
#define NVKM_FUTH_OFF_HEADER_SIZE		0x01
#define NVKM_FUTH_OFF_ENTRY_SIZE		0x02
#define NVKM_FUTH_OFF_ENTRY_COUNT		0x03
#define NVKM_FUTH_OFF_DESC_VERSION		0x04
#define NVKM_FUTH_OFF_DESC_SIZE			0x05

#define NVKM_FUTH_VERSION_V1			1
#define NVKM_FUTH_V1_SIZE			6

/* ----------------------------------------------------------------------
 * Falcon Ucode Table entry (FALCON_UCODE_TABLE_ENTRY_V1):
 *   0x00 ApplicationID  u8
 *   0x01 TargetID       u8
 *   0x02 DescPtr        u32   -> FALCON_UCODE_DESC_HEADER (then V2 or V3 body)
 */
#define NVKM_FUTE_OFF_APP_ID			0x00
#define NVKM_FUTE_OFF_TARGET_ID			0x01
#define NVKM_FUTE_OFF_DESC_PTR			0x02

#define NVKM_FUTE_V1_SIZE			6

/* Application IDs we care about (NVIDIA's APPID_FIRMWARE_*) */
#define NVKM_FUTE_APPID_FIRMWARE_SEC_LIC	0x05
#define NVKM_FUTE_APPID_FWSEC_DBG		0x45
#define NVKM_FUTE_APPID_FWSEC_PROD		0x85

/* ----------------------------------------------------------------------
 * Falcon ucode descriptor header (FALCON_UCODE_DESC_HEADER):
 *
 *   Single u32 "vDesc" packed as:
 *     bit  0     VERSION_AVAILABLE
 *     bit  1     reserved
 *     bit  2     ENCRYPTED
 *     bits 7:3   reserved
 *     bits 15:8  VERSION (1=V1, 2=V2, 3=V3, 4=V4)
 *     bits 31:16 SIZE
 */
#define NVKM_FUDH_FLAG_VERSION_AVAIL		(1u << 0)
#define NVKM_FUDH_FLAG_ENCRYPTED		(1u << 2)
#define NVKM_FUDH_VERSION_SHIFT			8
#define NVKM_FUDH_VERSION_MASK			(0xffu << 8)
#define NVKM_FUDH_SIZE_SHIFT			16
#define NVKM_FUDH_SIZE_MASK			(0xffffu << 16)

#define NVKM_FUDH_VERSION_V1			1
#define NVKM_FUDH_VERSION_V2			2
#define NVKM_FUDH_VERSION_V3			3
#define NVKM_FUDH_VERSION_V4			4

/* ----------------------------------------------------------------------
 * FALCON_UCODE_DESC_V2 (60 bytes, used for legacy ucodes including FWSEC
 * on Turing). Layout, offsets from start of descriptor:
 *
 *   0x00 vDesc                u32   (header, see above)
 *   0x04 StoredSize           u32
 *   0x08 UncompressedSize     u32
 *   0x0C VirtualEntry         u32
 *   0x10 InterfaceOffset      u32
 *   0x14 IMEMPhysBase         u32
 *   0x18 IMEMLoadSize         u32
 *   0x1C IMEMVirtBase         u32
 *   0x20 IMEMSecBase          u32
 *   0x24 IMEMSecSize          u32
 *   0x28 DMEMOffset           u32
 *   0x2C DMEMPhysBase         u32
 *   0x30 DMEMLoadSize         u32
 *   0x34 altIMEMLoadSize      u32
 *   0x38 altDMEMLoadSize      u32
 */
#define NVKM_FUD_V2_OFF_VDESC			0x00
#define NVKM_FUD_V2_OFF_STORED_SIZE		0x04
#define NVKM_FUD_V2_OFF_UNCOMPRESSED_SIZE	0x08
#define NVKM_FUD_V2_OFF_VIRTUAL_ENTRY		0x0C
#define NVKM_FUD_V2_OFF_INTERFACE_OFFSET	0x10
#define NVKM_FUD_V2_OFF_IMEM_PHYS_BASE		0x14
#define NVKM_FUD_V2_OFF_IMEM_LOAD_SIZE		0x18
#define NVKM_FUD_V2_OFF_IMEM_VIRT_BASE		0x1C
#define NVKM_FUD_V2_OFF_IMEM_SEC_BASE		0x20
#define NVKM_FUD_V2_OFF_IMEM_SEC_SIZE		0x24
#define NVKM_FUD_V2_OFF_DMEM_OFFSET		0x28
#define NVKM_FUD_V2_OFF_DMEM_PHYS_BASE		0x2C
#define NVKM_FUD_V2_OFF_DMEM_LOAD_SIZE		0x30
#define NVKM_FUD_V2_OFF_ALT_IMEM_LOAD_SIZE	0x34
#define NVKM_FUD_V2_OFF_ALT_DMEM_LOAD_SIZE	0x38
#define NVKM_FUD_V2_SIZE			60

/* ----------------------------------------------------------------------
 * FALCON_UCODE_DESC_V3 (44 bytes, PKC-signed ucodes; not used by FWSEC
 * on Turing but kept here for symmetry with later chip generations):
 *
 *   0x00 vDesc                u32
 *   0x04 StoredSize           u32
 *   0x08 PKCDataOffset        u32
 *   0x0C InterfaceOffset      u32
 *   0x10 IMEMPhysBase         u32
 *   0x14 IMEMLoadSize         u32
 *   0x18 IMEMVirtBase         u32
 *   0x1C DMEMPhysBase         u32
 *   0x20 DMEMLoadSize         u32
 *   0x24 EngineIdMask         u16
 *   0x26 UcodeId              u8
 *   0x27 SignatureCount       u8
 *   0x28 SignatureVersions    u16
 *   0x2A Reserved             u16
 */
#define NVKM_FUD_V3_OFF_VDESC			0x00
#define NVKM_FUD_V3_OFF_STORED_SIZE		0x04
#define NVKM_FUD_V3_OFF_PKC_DATA_OFFSET		0x08
#define NVKM_FUD_V3_OFF_INTERFACE_OFFSET	0x0C
#define NVKM_FUD_V3_OFF_IMEM_PHYS_BASE		0x10
#define NVKM_FUD_V3_OFF_IMEM_LOAD_SIZE		0x14
#define NVKM_FUD_V3_OFF_IMEM_VIRT_BASE		0x18
#define NVKM_FUD_V3_OFF_DMEM_PHYS_BASE		0x1C
#define NVKM_FUD_V3_OFF_DMEM_LOAD_SIZE		0x20
#define NVKM_FUD_V3_OFF_ENGINE_ID_MASK		0x24
#define NVKM_FUD_V3_OFF_UCODE_ID		0x26
#define NVKM_FUD_V3_OFF_SIGNATURE_COUNT		0x27
#define NVKM_FUD_V3_OFF_SIGNATURE_VERSIONS	0x28
#define NVKM_FUD_V3_SIZE			44

#endif /* _NVKM_BIT_H_ */
