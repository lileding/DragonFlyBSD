/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Turing chip capability and PCI ID table.
 */

#include "nvgpu_chip.h"

#define NVGPU_GRAPH_UNITS(gpc, tpc) \
	(((uint32_t)(tpc) << 8) | (uint32_t)(gpc))

#define NVGPU_CLASS_FERMI_TWOD_A		0x0000902du
#define NVGPU_CLASS_KEPLER_INLINE_TO_MEMORY_B	0x0000a140u
#define NVGPU_CLASS_TURING_A			0x0000c597u
#define NVGPU_CLASS_TURING_DMA_COPY_A		0x0000c5b5u
#define NVGPU_CLASS_TURING_COMPUTE_A		0x0000c5c0u

#define NVGPU_GSP_FW(chip, name) "nvidia/" chip "/gsp/" name "-570.144"
#define NVGPU_ACR_FW(chip) "nvidia/" chip "/acr/bl"

#define NVGPU_TU10X_COMMON_FIELDS(fw_chip) \
	.card_type = NVGPU_CARD_TU100, \
	.fw_booter_load = NVGPU_GSP_FW(fw_chip, "booter_load"), \
	.fw_booter_unload = NVGPU_GSP_FW(fw_chip, "booter_unload"), \
	.fw_acr_bl = NVGPU_ACR_FW(fw_chip), \
	.fw_gsp = NVGPU_GSP_FW(fw_chip, "gsp"), \
	.fw_bootloader = NVGPU_GSP_FW(fw_chip, "bootloader"), \
	.fw_signature = ".fwsignature_tu10x", \
	.sec2_base = NVGPU_TU10X_SEC2_BASE, \
	.sec2_fbif = NVGPU_TU10X_SEC2_FBIF, \
	.gsp_base = NVGPU_TU10X_GSP_BASE, \
	.gsp_fbif = NVGPU_TU10X_GSP_FBIF, \
	.gsp_riscv = NVGPU_TU10X_GSP_RISCV, \
	.display_heads = 4, \
	.display_sors = 4, \
	.display_windows = 8, \
	.display_cursors = 4, \
	.gmmu_pd3_shift = NVGPU_GMMU_PD3_SHIFT, \
	.gmmu_pd2_shift = NVGPU_GMMU_PD2_SHIFT, \
	.gmmu_pd1_shift = NVGPU_GMMU_PD1_SHIFT, \
	.gmmu_pd0_shift = NVGPU_GMMU_PD0_SHIFT, \
	.gmmu_big_shift = NVGPU_GMMU_LPT_SHIFT, \
	.gmmu_small_shift = NVGPU_GMMU_SPT_SHIFT, \
	.class_3d = NVGPU_CLASS_TURING_A, \
	.class_compute = NVGPU_CLASS_TURING_COMPUTE_A, \
	.class_copy = NVGPU_CLASS_TURING_DMA_COPY_A, \
	.class_twod = NVGPU_CLASS_FERMI_TWOD_A, \
	.class_m2mf = NVGPU_CLASS_KEPLER_INLINE_TO_MEMORY_B

static const struct nvgpu_chip_config nvgpu_chip_tu102 = {
	.chip = "TU102",
	.device_name = "tu102",
	.fallback_name = "NVIDIA TU102",
	.chipset = 0x162,
	.graph_units = NVGPU_GRAPH_UNITS(6, 34),
	NVGPU_TU10X_COMMON_FIELDS("tu102"),
};

static const struct nvgpu_chip_config nvgpu_chip_tu104 = {
	.chip = "TU104",
	.device_name = "tu104",
	.fallback_name = "NVIDIA TU104",
	.chipset = 0x164,
	.graph_units = NVGPU_GRAPH_UNITS(6, 24),
	NVGPU_TU10X_COMMON_FIELDS("tu104"),
};

static const struct nvgpu_chip_config nvgpu_chip_tu106 = {
	.chip = "TU106",
	.device_name = "tu106",
	.fallback_name = "NVIDIA TU106",
	.chipset = 0x166,
	.graph_units = NVGPU_GRAPH_UNITS(3, 18),
	NVGPU_TU10X_COMMON_FIELDS("tu106"),
};

static const struct nvgpu_pci_device nvgpu_pci_devices[] = {
	{ 0x1e02, "NVIDIA TU102 [TITAN RTX]", &nvgpu_chip_tu102 },
	{ 0x1e03, "NVIDIA TU102 [GeForce RTX 2080 Ti 12GB]", &nvgpu_chip_tu102 },
	{ 0x1e04, "NVIDIA TU102 [GeForce RTX 2080 Ti]", &nvgpu_chip_tu102 },
	{ 0x1e07, "NVIDIA TU102 [GeForce RTX 2080 Ti Rev. A]", &nvgpu_chip_tu102 },
	{ 0x1e30, "NVIDIA TU102GL [Quadro RTX 6000/8000]", &nvgpu_chip_tu102 },
	{ 0x1e36, "NVIDIA TU102GL [Quadro RTX 6000]", &nvgpu_chip_tu102 },
	{ 0x1e78, "NVIDIA TU102GL [Quadro RTX 6000/8000]", &nvgpu_chip_tu102 },
	{ 0x1e81, "NVIDIA TU104 [GeForce RTX 2080 SUPER]", &nvgpu_chip_tu104 },
	{ 0x1e82, "NVIDIA TU104 [GeForce RTX 2080]", &nvgpu_chip_tu104 },
	{ 0x1e84, "NVIDIA TU104 [GeForce RTX 2070 SUPER]", &nvgpu_chip_tu104 },
	{ 0x1e87, "NVIDIA TU104 [GeForce RTX 2080 Rev. A]", &nvgpu_chip_tu104 },
	{ 0x1e89, "NVIDIA TU104 [GeForce RTX 2060]", &nvgpu_chip_tu104 },
	{ 0x1e90, "NVIDIA TU104M [GeForce RTX 2080 Mobile]", &nvgpu_chip_tu104 },
	{ 0x1e91, "NVIDIA TU104M [GeForce RTX 2070 SUPER Mobile / Max-Q]", &nvgpu_chip_tu104 },
	{ 0x1e93, "NVIDIA TU104M [GeForce RTX 2080 SUPER Mobile / Max-Q]", &nvgpu_chip_tu104 },
	{ 0x1eb0, "NVIDIA TU104GL [Quadro RTX 5000]", &nvgpu_chip_tu104 },
	{ 0x1eb1, "NVIDIA TU104GL [Quadro RTX 4000]", &nvgpu_chip_tu104 },
	{ 0x1eb5, "NVIDIA TU104GLM [Quadro RTX 5000 Mobile / Max-Q]", &nvgpu_chip_tu104 },
	{ 0x1eb6, "NVIDIA TU104GLM [Quadro RTX 4000 Mobile / Max-Q]", &nvgpu_chip_tu104 },
	{ 0x1ec2, "NVIDIA TU104 [GeForce RTX 2070 SUPER]", &nvgpu_chip_tu104 },
	{ 0x1ec7, "NVIDIA TU104 [GeForce RTX 2070 SUPER]", &nvgpu_chip_tu104 },
	{ 0x1ed0, "NVIDIA TU104BM [GeForce RTX 2080 Mobile]", &nvgpu_chip_tu104 },
	{ 0x1ed1, "NVIDIA TU104BM [GeForce RTX 2070 SUPER Mobile / Max-Q]", &nvgpu_chip_tu104 },
	{ 0x1ed3, "NVIDIA TU104BM [GeForce RTX 2080 SUPER Mobile / Max-Q]", &nvgpu_chip_tu104 },
	{ 0x1f02, "NVIDIA TU106 [GeForce RTX 2070]", &nvgpu_chip_tu106 },
	{ 0x1f03, "NVIDIA TU106 [GeForce RTX 2060 12GB]", &nvgpu_chip_tu106 },
	{ 0x1f06, "NVIDIA TU106 [GeForce RTX 2060 SUPER]", &nvgpu_chip_tu106 },
	{ 0x1f07, "NVIDIA TU106 [GeForce RTX 2070 Rev. A]", &nvgpu_chip_tu106 },
	{ 0x1f08, "NVIDIA TU106 [GeForce RTX 2060 Rev. A]", &nvgpu_chip_tu106 },
	{ 0x1f10, "NVIDIA TU106M [GeForce RTX 2070 Mobile]", &nvgpu_chip_tu106 },
	{ 0x1f11, "NVIDIA TU106M [GeForce RTX 2060 Mobile]", &nvgpu_chip_tu106 },
	{ 0x1f12, "NVIDIA TU106M [GeForce RTX 2060 Max-Q]", &nvgpu_chip_tu106 },
	{ 0x1f14, "NVIDIA TU106M [GeForce RTX 2070 Mobile / Max-Q Refresh]", &nvgpu_chip_tu106 },
	{ 0x1f15, "NVIDIA TU106M [GeForce RTX 2060 Mobile]", &nvgpu_chip_tu106 },
	{ 0x1f36, "NVIDIA TU106GLM [Quadro RTX 3000 Mobile / Max-Q]", &nvgpu_chip_tu106 },
	{ 0x1f50, "NVIDIA TU106BM [GeForce RTX 2070 Mobile / Max-Q]", &nvgpu_chip_tu106 },
	{ 0x1f51, "NVIDIA TU106BM [GeForce RTX 2060 Mobile]", &nvgpu_chip_tu106 },
	{ 0x1f54, "NVIDIA TU106BM [GeForce RTX 2070 Mobile]", &nvgpu_chip_tu106 },
	{ 0x1f55, "NVIDIA TU106BM [GeForce RTX 2060 Mobile]", &nvgpu_chip_tu106 },
	{ 0x1f76, "NVIDIA TU106GLM [Quadro RTX 3000 Mobile Refresh]", &nvgpu_chip_tu106 },
	{ 0, NULL, NULL }
};

/* Find immutable chip metadata for a PCI device ID. */
/* Return borrowed static chip metadata for device, or NULL.  Lock-free. */
const struct nvgpu_pci_device *
nvgpu_chip_pci_lookup(uint16_t device)
{
	const struct nvgpu_pci_device *id;

	for (id = nvgpu_pci_devices; id->name != NULL; id++) {
		if (id->device == device)
			return (id);
	}
	return (NULL);
}
