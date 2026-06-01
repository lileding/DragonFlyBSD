/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Falcon engine abstraction.
 *
 * A "Falcon" is an NVIDIA on-die microcontroller. Different GPU engines
 * (GSP, SEC2, NVDEC, NVENC) each have their own Falcon instance at a
 * distinct BAR0 offset. From Turing onward some Falcons can run RISC-V
 * code in place of the legacy Falcon ISA.
 *
 * This module exposes the lowest-level Falcon operations: register R/W
 * through three per-engine bases, IMEM/DMEM PIO load, reset wait, start,
 * halt wait. Higher-level concepts (ucode signing, ACR, GSP boot) build
 * on top of these.
 *
 * Source references:
 *   linux/drivers/gpu/drm/nouveau/nvkm/falcon/base.c   (algorithm)
 *   linux/drivers/gpu/drm/nouveau/nvkm/falcon/gm200.c  (Maxwell+ impl)
 *   linux/drivers/gpu/drm/nouveau/nvkm/falcon/tu102.c  (Turing extras)
 *   open-rm/src/nvidia/src/kernel/gpu/falcon/arch/turing/
 *       kernel_falcon_tu102.c                          (TU102 sequences)
 */

#ifndef _NVKM_FALCON_H_
#define _NVKM_FALCON_H_

#include "nvkm_priv.h"
#include "nvkm_reg_falcon.h"

struct nvkm_falcon {
	struct nvkm_softc	*sc;
	const char		*name;

	/* BAR0 offsets for the three register blocks belonging to this
	 * Falcon engine. addr2 (the RISC-V control block) and fbif may be
	 * 0 for engines that don't expose them. */
	uint32_t		addr;
	uint32_t		addr2;
	uint32_t		fbif;
};

void	nvkm_falcon_init(struct nvkm_falcon *flcn, struct nvkm_softc *sc,
	    const char *name, uint32_t addr, uint32_t addr2, uint32_t fbif);

static __inline uint32_t
nvkm_falcon_rd32(struct nvkm_falcon *flcn, uint32_t off)
{
	return (nvkm_rd32(flcn->sc, flcn->addr + off));
}

static __inline void
nvkm_falcon_wr32(struct nvkm_falcon *flcn, uint32_t off, uint32_t val)
{
	nvkm_wr32(flcn->sc, flcn->addr + off, val);
}

static __inline void
nvkm_falcon_mask(struct nvkm_falcon *flcn, uint32_t off,
    uint32_t mask, uint32_t val)
{
	uint32_t v = nvkm_falcon_rd32(flcn, off);

	nvkm_falcon_wr32(flcn, off, (v & ~mask) | (val & mask));
}

static __inline uint32_t
nvkm_falcon_riscv_rd32(struct nvkm_falcon *flcn, uint32_t off)
{
	return (nvkm_rd32(flcn->sc, flcn->addr2 + off));
}

static __inline void
nvkm_falcon_fbif_wr32(struct nvkm_falcon *flcn, uint32_t off, uint32_t val)
{
	nvkm_wr32(flcn->sc, flcn->fbif + off, val);
}

static __inline uint32_t
nvkm_falcon_fbif_rd32(struct nvkm_falcon *flcn, uint32_t off)
{
	return (nvkm_rd32(flcn->sc, flcn->fbif + off));
}

bool	nvkm_falcon_has_riscv(struct nvkm_falcon *flcn);
bool	nvkm_falcon_riscv_active(struct nvkm_falcon *flcn);

int	nvkm_falcon_wait_for_scrub(struct nvkm_falcon *flcn, int timeout_us);
int	nvkm_falcon_wait_for_halt(struct nvkm_falcon *flcn, int timeout_us);

void	nvkm_falcon_set_bootvec(struct nvkm_falcon *flcn, uint32_t bootvec);
void	nvkm_falcon_start(struct nvkm_falcon *flcn);

void	nvkm_falcon_disable_ctx_req(struct nvkm_falcon *flcn);

/*
 * PIO-load instruction memory.
 *
 * imem_offset must be 256-byte (block) aligned. size must be 4-byte
 * aligned. start_tag is written into IMEMT for the first block and then
 * auto-incremented per block. secure=true sets IMEMC.SECURE for HS code.
 */
int	nvkm_falcon_load_imem(struct nvkm_falcon *flcn, const void *data,
	    uint32_t imem_offset, uint32_t size, uint16_t start_tag,
	    uint8_t port, bool secure);

/*
 * PIO-load data memory. dmem_offset must be 4-byte aligned; size must be
 * 4-byte aligned.
 */
int	nvkm_falcon_load_dmem(struct nvkm_falcon *flcn, const void *data,
	    uint32_t dmem_offset, uint32_t size, uint8_t port);

/*
 * Reset and re-enable the Falcon engine (NV_PFALCON_FALCON_ENGINE @
 * base+0x3c0, bit 0 = ENGINE_RESET). After the pulse, waits for IMEM/
 * DMEM scrubbing to complete. Must be the first thing done on a
 * Falcon whose post-OVMF state is unknown -- on TU102 GSP-Falcon a
 * naive PRI read can hang the PRI hub before the engine is reset.
 * Returns 0 on success, nonzero if the scrub-wait timed out.
 *
 * Reference: nouveau gp102_flcn_reset_eng (falcon/gp102.c).
 */
int	nvkm_falcon_reset_eng(struct nvkm_falcon *flcn);

#endif /* _NVKM_FALCON_H_ */
