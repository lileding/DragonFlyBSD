/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-Falcon engine setup (the HS host for FwSec on Turing).
 *
 * On TU102 the GSP engine contains two cores at distinct PRI bases:
 *   - Falcon core  @ 0x110000 (PRI base, FBIF @ +0x600)
 *   - RISC-V core  @ 0x111000 (NV_FALCON2_GSP_BASE)
 *
 * FwSec-FRTS and FwSec-SB are NVIDIA-signed HS firmware that must
 * run on the Falcon core specifically -- their signatures are bound
 * to this engine. After FwSec completes, the booter (running on
 * SEC2, not GSP-Falcon) is responsible for switching the GSP into
 * RISC-V mode and launching GSP-RM from WPR2.
 *
 * IMPORTANT: at attach time GSP-Falcon may be in an unknown post-OVMF
 * state where naive PRI reads to its register block hang the PRI hub
 * (QEMU cannot recover this and the VM dies). We must therefore not
 * touch GSP registers from init at all -- only allocate state. The
 * engine is brought to a known state by an explicit reset_eng (write
 * 1->0 to NV_PFALCON_FALCON_ENGINE @ base+0x3c0) inside
 * nvkm_fwsec_run_frts, immediately before the first register touch.
 *
 * Reference: open-rm 570.144
 *   src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c:65
 *   linux nouveau falcon/gp102.c:gp102_flcn_reset_eng
 *   src/common/inc/swref/published/turing/tu102/dev_gsp_addendum.h
 */

#include "nvkm_priv.h"
#include "nvkm_falcon.h"

static MALLOC_DEFINE(M_NVKM_GSP, "nvkm_gsp", "nvkm GSP-Falcon state");

int
nvkm_gsp_init(struct nvkm_softc *sc)
{
	struct nvkm_falcon *flcn;

	flcn = kmalloc(sizeof(*flcn), M_NVKM_GSP, M_WAITOK | M_ZERO);
	nvkm_falcon_init(flcn, sc, "gsp",
	    sc->chip->gsp_base,
	    sc->chip->gsp_riscv,
	    sc->chip->gsp_fbif);
	sc->gsp = flcn;

	nvkm_debugf(sc->dev,
	    "gsp: handle allocated (PRI base 0x%x, RISC-V base 0x%x, "
	    "FBIF 0x%x); register access deferred until reset\n",
	    sc->chip->gsp_base, sc->chip->gsp_riscv, sc->chip->gsp_fbif);
	return (0);
}

void
nvkm_gsp_fini(struct nvkm_softc *sc)
{
	if (sc->gsp != NULL) {
		kfree(sc->gsp, M_NVKM_GSP);
		sc->gsp = NULL;
	}
}
