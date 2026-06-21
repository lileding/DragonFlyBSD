/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Turing chip capability and PCI-ID tables.
 */

#include "nvkm_priv.h"
#include "nvif/class.h"

extern const struct nvkm_rm_gpu tu1xx_gpu;

#define NVKM_CARD_TU100	0x160u
#define NVKM_GRAPH_UNITS(gpc, tpc) \
	(((uint32_t)(tpc) << 8) | (uint32_t)(gpc))

#define NVKM_TU10X_COMMON_FIELDS					\
	.card_type = NVKM_CARD_TU100,					\
	.fw_booter_load = "nvidia/tu102/gsp/booter_load-570.144",	\
	.fw_acr_bl = "nvidia/tu102/acr/bl",				\
	.fw_gsp = "nvidia/tu102/gsp/gsp-570.144",			\
	.fw_bootloader = "nvidia/tu102/gsp/bootloader-570.144",		\
	.fw_signature = ".fwsignature_tu10x",				\
	.sec2_base = NVKM_TU10X_SEC2_BASE,				\
	.sec2_fbif = NVKM_TU10X_SEC2_FBIF,				\
	.gsp_base = NVKM_TU10X_GSP_BASE,				\
	.gsp_fbif = NVKM_TU10X_GSP_FBIF,				\
	.gsp_riscv = NVKM_TU10X_GSP_RISCV,				\
	.display_heads = 4,						\
	.display_sors = 4,						\
	.rm_gpu = &tu1xx_gpu,						\
	.gmmu_pd3_shift = NVKM_GMMU_PD3_SHIFT,				\
	.gmmu_pd2_shift = NVKM_GMMU_PD2_SHIFT,				\
	.gmmu_pd1_shift = NVKM_GMMU_PD1_SHIFT,				\
	.gmmu_pd0_shift = NVKM_GMMU_PD0_SHIFT,				\
	.gmmu_big_shift = NVKM_GMMU_LPT_SHIFT,				\
	.gmmu_small_shift = NVKM_GMMU_SPT_SHIFT,			\
	.class_3d = TURING_A,						\
	.class_compute = TURING_COMPUTE_A,				\
	.class_copy = TURING_DMA_COPY_A,				\
	.class_twod = FERMI_TWOD_A,					\
	.class_m2mf = KEPLER_INLINE_TO_MEMORY_B

static const struct nvkm_chip_config nvkm_chip_tu102 = {
	.chip = "TU102",
	.device_name = "tu102",
	.fallback_name = "NVIDIA TU102",
	.chipset = 0x162,
	.graph_units = NVKM_GRAPH_UNITS(6, 34),
	NVKM_TU10X_COMMON_FIELDS,
};

static const struct nvkm_chip_config nvkm_chip_tu104 = {
	.chip = "TU104",
	.device_name = "tu104",
	.fallback_name = "NVIDIA TU104",
	.chipset = 0x164,
	.graph_units = NVKM_GRAPH_UNITS(6, 24),
	NVKM_TU10X_COMMON_FIELDS,
};

static const struct nvkm_chip_config nvkm_chip_tu106 = {
	.chip = "TU106",
	.device_name = "tu106",
	.fallback_name = "NVIDIA TU106",
	.chipset = 0x166,
	.graph_units = NVKM_GRAPH_UNITS(3, 18),
	NVKM_TU10X_COMMON_FIELDS,
};

static const struct nvkm_pci_device nvkm_pci_devices[] = {
	{ 0x1e02, "NVIDIA TU102 [TITAN RTX]", &nvkm_chip_tu102 },
	{ 0x1e03, "NVIDIA TU102 [GeForce RTX 2080 Ti 12GB]",
	    &nvkm_chip_tu102 },
	{ 0x1e04, "NVIDIA TU102 [GeForce RTX 2080 Ti]",
	    &nvkm_chip_tu102 },
	{ 0x1e07, "NVIDIA TU102 [GeForce RTX 2080 Ti Rev. A]",
	    &nvkm_chip_tu102 },
	{ 0x1e30, "NVIDIA TU102GL [Quadro RTX 6000/8000]",
	    &nvkm_chip_tu102 },
	{ 0x1e36, "NVIDIA TU102GL [Quadro RTX 6000]", &nvkm_chip_tu102 },
	{ 0x1e78, "NVIDIA TU102GL [Quadro RTX 6000/8000]",
	    &nvkm_chip_tu102 },

	{ 0x1e81, "NVIDIA TU104 [GeForce RTX 2080 SUPER]",
	    &nvkm_chip_tu104 },
	{ 0x1e82, "NVIDIA TU104 [GeForce RTX 2080]", &nvkm_chip_tu104 },
	{ 0x1e84, "NVIDIA TU104 [GeForce RTX 2070 SUPER]",
	    &nvkm_chip_tu104 },
	{ 0x1e87, "NVIDIA TU104 [GeForce RTX 2080 Rev. A]",
	    &nvkm_chip_tu104 },
	{ 0x1e89, "NVIDIA TU104 [GeForce RTX 2060]", &nvkm_chip_tu104 },
	{ 0x1e90, "NVIDIA TU104M [GeForce RTX 2080 Mobile]",
	    &nvkm_chip_tu104 },
	{ 0x1e91, "NVIDIA TU104M [GeForce RTX 2070 SUPER Mobile / Max-Q]",
	    &nvkm_chip_tu104 },
	{ 0x1e93, "NVIDIA TU104M [GeForce RTX 2080 SUPER Mobile / Max-Q]",
	    &nvkm_chip_tu104 },
	{ 0x1eb0, "NVIDIA TU104GL [Quadro RTX 5000]", &nvkm_chip_tu104 },
	{ 0x1eb1, "NVIDIA TU104GL [Quadro RTX 4000]", &nvkm_chip_tu104 },
	{ 0x1eb5, "NVIDIA TU104GLM [Quadro RTX 5000 Mobile / Max-Q]",
	    &nvkm_chip_tu104 },
	{ 0x1eb6, "NVIDIA TU104GLM [Quadro RTX 4000 Mobile / Max-Q]",
	    &nvkm_chip_tu104 },
	{ 0x1ec2, "NVIDIA TU104 [GeForce RTX 2070 SUPER]",
	    &nvkm_chip_tu104 },
	{ 0x1ec7, "NVIDIA TU104 [GeForce RTX 2070 SUPER]",
	    &nvkm_chip_tu104 },
	{ 0x1ed0, "NVIDIA TU104BM [GeForce RTX 2080 Mobile]",
	    &nvkm_chip_tu104 },
	{ 0x1ed1, "NVIDIA TU104BM [GeForce RTX 2070 SUPER Mobile / Max-Q]",
	    &nvkm_chip_tu104 },
	{ 0x1ed3, "NVIDIA TU104BM [GeForce RTX 2080 SUPER Mobile / Max-Q]",
	    &nvkm_chip_tu104 },

	{ 0x1f02, "NVIDIA TU106 [GeForce RTX 2070]", &nvkm_chip_tu106 },
	{ 0x1f03, "NVIDIA TU106 [GeForce RTX 2060 12GB]",
	    &nvkm_chip_tu106 },
	{ 0x1f06, "NVIDIA TU106 [GeForce RTX 2060 SUPER]",
	    &nvkm_chip_tu106 },
	{ 0x1f07, "NVIDIA TU106 [GeForce RTX 2070 Rev. A]",
	    &nvkm_chip_tu106 },
	{ 0x1f08, "NVIDIA TU106 [GeForce RTX 2060 Rev. A]",
	    &nvkm_chip_tu106 },
	{ 0x1f10, "NVIDIA TU106M [GeForce RTX 2070 Mobile]",
	    &nvkm_chip_tu106 },
	{ 0x1f11, "NVIDIA TU106M [GeForce RTX 2060 Mobile]",
	    &nvkm_chip_tu106 },
	{ 0x1f12, "NVIDIA TU106M [GeForce RTX 2060 Max-Q]",
	    &nvkm_chip_tu106 },
	{ 0x1f14, "NVIDIA TU106M [GeForce RTX 2070 Mobile / Max-Q Refresh]",
	    &nvkm_chip_tu106 },
	{ 0x1f15, "NVIDIA TU106M [GeForce RTX 2060 Mobile]",
	    &nvkm_chip_tu106 },
	{ 0x1f36, "NVIDIA TU106GLM [Quadro RTX 3000 Mobile / Max-Q]",
	    &nvkm_chip_tu106 },
	{ 0x1f50, "NVIDIA TU106BM [GeForce RTX 2070 Mobile / Max-Q]",
	    &nvkm_chip_tu106 },
	{ 0x1f51, "NVIDIA TU106BM [GeForce RTX 2060 Mobile]",
	    &nvkm_chip_tu106 },
	{ 0x1f54, "NVIDIA TU106BM [GeForce RTX 2070 Mobile]",
	    &nvkm_chip_tu106 },
	{ 0x1f55, "NVIDIA TU106BM [GeForce RTX 2060 Mobile]",
	    &nvkm_chip_tu106 },
	{ 0x1f76, "NVIDIA TU106GLM [Quadro RTX 3000 Mobile Refresh]",
	    &nvkm_chip_tu106 },

	{ 0, NULL, NULL }
};

const struct nvkm_pci_device *
nvkm_pci_device_lookup(uint16_t device)
{
	const struct nvkm_pci_device *id;

	for (id = nvkm_pci_devices; id->name != NULL; id++) {
		if (id->device == device)
			return (id);
	}
	return (NULL);
}
