/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Phase 2 milestone: round-trip GET_GSP_STATIC_INFO RPC.
 * Extracts internal client/device/subdevice handles + BAR PDE bases
 * for use by Phase 3.
 *
 * Field offsets are r570-specific (derived empirically from a dump of
 * the GspStaticConfigInfo reply). When we eventually mirror the full
 * struct in nvkm_gsp_abi.h we can replace these magic offsets with
 * struct field accessors.
 */

#include "nvkm_priv.h"
#include <sys/libkern.h>

#define NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO	65
#define NVKM_STATIC_INFO_SIZE	0xa00	/* covers fields up to ecidInfo */

/* r570 GspStaticConfigInfo field offsets (verified via dump). */
#define OFF_BAR1_PDE_BASE		0x600	/* NvU64 */
#define OFF_BAR2_PDE_BASE		0x608	/* NvU64 */
#define OFF_GPU_NAME_STRING		0x4ec	/* char[64] */
#define OFF_H_INTERNAL_CLIENT		0x640	/* NvU32 */
#define OFF_H_INTERNAL_DEVICE		0x644	/* NvU32 */
#define OFF_H_INTERNAL_SUBDEVICE	0x648	/* NvU32 */

int
nvkm_gsp_get_static_info(struct nvkm_softc *sc)
{
	uint8_t *r;
	uint32_t i, j, m, scan;
	char buf[80];

	device_printf(sc->dev, "static_info: issuing fn=65 RECV...\n");

	r = nvkm_gsp_rpc_rd(sc,
	    NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO,
	    NVKM_STATIC_INFO_SIZE);
	if (r == NULL) {
		device_printf(sc->dev, "static_info: rpc_rd failed\n");
		return (EIO);
	}

	sc->gsp_internal_client    = *(uint32_t *)(r + OFF_H_INTERNAL_CLIENT);
	sc->gsp_internal_device    = *(uint32_t *)(r + OFF_H_INTERNAL_DEVICE);
	sc->gsp_internal_subdevice = *(uint32_t *)(r + OFF_H_INTERNAL_SUBDEVICE);
	sc->gsp_bar1_pdb           = *(uint64_t *)(r + OFF_BAR1_PDE_BASE);
	sc->gsp_bar2_pdb           = *(uint64_t *)(r + OFF_BAR2_PDE_BASE);

	/* Extract GPU name string for the dmesg. */
	scan = NVKM_STATIC_INFO_SIZE;
	for (i = 0; i + 16 <= scan; i++) {
		if (memcmp(r + i, "NVIDIA", 6) == 0 ||
		    memcmp(r + i, "GeForce", 7) == 0 ||
		    memcmp(r + i, "RTX", 3) == 0) {
			m = 0;
			for (j = 0; j < 79 && i + j < scan; j++) {
				char c = r[i + j];
				if (c == 0)
					break;
				if (c >= 0x20 && c < 0x7f)
					buf[m++] = c;
			}
			buf[m] = 0;
			device_printf(sc->dev,
			    "static_info: GPU = \"%s\"\n", buf);
			break;
		}
	}

	device_printf(sc->dev,
	    "static_info: hInternalClient=0x%08x device=0x%08x subdevice=0x%08x\n",
	    sc->gsp_internal_client, sc->gsp_internal_device,
	    sc->gsp_internal_subdevice);
	device_printf(sc->dev,
	    "static_info: bar1PdeBase=0x%llx bar2PdeBase=0x%llx\n",
	    (unsigned long long)sc->gsp_bar1_pdb,
	    (unsigned long long)sc->gsp_bar2_pdb);

	nvkm_gsp_rpc_done(sc, r);
	return (0);
}
