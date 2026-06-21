/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * SEC2 (Security 2) engine setup.
 *
 * SEC2 is a Falcon-class microcontroller used during GSP boot to run
 * the "booter" ucode, which in turn sets up the ACR (Access Controlled
 * Region) and launches GSP-RM. This file just stands up the SEC2
 * abstraction; loading the booter onto it happens in a later phase.
 *
 * Per-chipset register bases live in nvkm_priv.h. Turing SEC2 is
 * Falcon-only; no RISC-V capability.
 */

#include "nvkm_priv.h"
#include "nvkm_falcon.h"

static MALLOC_DEFINE(M_NVKM_SEC2, "nvkm_sec2", "nvkm SEC2 engine state");

int
nvkm_sec2_init(struct nvkm_softc *sc)
{
	struct nvkm_falcon *flcn;
	uint32_t hwcfg, hwcfg2;
	uint32_t imem_size_bytes;
	int error;

	flcn = kmalloc(sizeof(*flcn), M_NVKM_SEC2, M_WAITOK | M_ZERO);
	nvkm_falcon_init(flcn, sc, "sec2",
	    sc->chip->sec2_base,
	    0,				/* no RISC-V on Turing SEC2 */
	    sc->chip->sec2_fbif);
	sc->sec2 = flcn;

	hwcfg  = nvkm_falcon_rd32(flcn, NVKM_FLCN_HWCFG);
	hwcfg2 = nvkm_falcon_rd32(flcn, NVKM_FLCN_HWCFG2);

	imem_size_bytes =
	    (hwcfg & NVKM_FLCN_HWCFG_IMEM_SIZE_MASK) * NVKM_FLCN_IMEM_BLKSIZE;

	nvkm_debugf(sc->dev,
	    "sec2: HWCFG=0x%08x HWCFG2=0x%08x imem=%u bytes riscv=%s\n",
	    hwcfg, hwcfg2, imem_size_bytes,
	    (hwcfg2 & NVKM_FLCN_HWCFG2_RISCV) ? "yes" : "no");

	error = nvkm_falcon_wait_for_scrub(flcn, 100000); /* 100 ms */
	if (error != 0) {
		nvkm_debugf(sc->dev,
		    "sec2: memory scrub wait failed (%d), DMACTL=0x%08x\n",
		    error, nvkm_falcon_rd32(flcn, NVKM_FLCN_DMACTL));
		/* not fatal yet -- diagnostic phase */
	} else {
		nvkm_debugf(sc->dev,
		    "sec2: memory scrub complete, DMACTL=0x%08x CPUCTL=0x%08x\n",
		    nvkm_falcon_rd32(flcn, NVKM_FLCN_DMACTL),
		    nvkm_falcon_rd32(flcn, NVKM_FLCN_CPUCTL));
	}
	return (0);
}

void
nvkm_sec2_fini(struct nvkm_softc *sc)
{
	if (sc->sec2 != NULL) {
		kfree(sc->sec2, M_NVKM_SEC2);
		sc->sec2 = NULL;
	}
}
