/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Falcon engine low-level operations. See nvkm_falcon.h for the API.
 *
 * Implementation modelled after nouveau's nvkm/falcon/base.c and
 * gm200.c (which together cover Turing); register-level details
 * cross-checked against open-rm 570.144 kernel_falcon_tu102.c.
 */

#include "nvkm_falcon.h"

#include <sys/libkern.h>		/* memcpy */

#define NVKM_FALCON_WAIT_STEP_US	10

void
nvkm_falcon_init(struct nvkm_falcon *flcn, struct nvkm_softc *sc,
    const char *name, uint32_t addr, uint32_t addr2, uint32_t fbif)
{
	flcn->sc = sc;
	flcn->name = name;
	flcn->addr = addr;
	flcn->addr2 = addr2;
	flcn->fbif = fbif;
}

bool
nvkm_falcon_has_riscv(struct nvkm_falcon *flcn)
{
	uint32_t cfg = nvkm_falcon_rd32(flcn, NVKM_FLCN_HWCFG2);

	return ((cfg & NVKM_FLCN_HWCFG2_RISCV) != 0);
}

bool
nvkm_falcon_riscv_active(struct nvkm_falcon *flcn)
{
	/*
	 * Per nouveau falcon/tu102.c, the RISC-V "active" indicator on
	 * Turing lives at offset 0x240 of the secondary (RISC-V) register
	 * block.
	 */
	if (flcn->addr2 == 0)
		return (false);
	return ((nvkm_falcon_riscv_rd32(flcn, 0x240) & 0x00000001) != 0);
}

int
nvkm_falcon_reset_eng(struct nvkm_falcon *flcn)
{
	uint32_t v;

	/*
	 * Reset register lives at base+0x3c0 (NV_PFALCON_FALCON_ENGINE).
	 * Bit 0 is the engine reset / power-up control. We assert it, hold
	 * briefly, then deassert -- this brings the engine out of the
	 * post-OVMF unknown state and gates power on. After deassert the
	 * engine begins scrubbing IMEM/DMEM; wait for it to finish.
	 * Mirrors nouveau gp102_flcn_reset_eng.
	 */
	(void)v;
	/*
	 * Write-only: avoid an initial read of 0x3c0 in case it too is
	 * unsafe in the unknown state. Bit 0 is the only meaningful bit;
	 * other bits read as 0 on TU102 and writing 0 to them is harmless.
	 */
	nvkm_falcon_wr32(flcn, 0x3c0, 0x1u);
	DELAY(10);
	nvkm_falcon_wr32(flcn, 0x3c0, 0x0u);

	return (nvkm_falcon_wait_for_scrub(flcn, 100000)); /* 100 ms */
}

int
nvkm_falcon_wait_for_scrub(struct nvkm_falcon *flcn, int timeout_us)
{
	int waited = 0;

	for (;;) {
		uint32_t v = nvkm_falcon_rd32(flcn, NVKM_FLCN_DMACTL);

		if ((v & NVKM_FLCN_DMACTL_SCRUBBING_MASK) == 0)
			return (0);
		if (waited >= timeout_us)
			return (ETIMEDOUT);
		DELAY(NVKM_FALCON_WAIT_STEP_US);
		waited += NVKM_FALCON_WAIT_STEP_US;
	}
}

int
nvkm_falcon_wait_for_halt(struct nvkm_falcon *flcn, int timeout_us)
{
	int waited = 0;

	for (;;) {
		uint32_t v = nvkm_falcon_rd32(flcn, NVKM_FLCN_CPUCTL);

		if (v & NVKM_FLCN_CPUCTL_HALTED)
			return (0);
		if (waited >= timeout_us)
			return (ETIMEDOUT);
		DELAY(NVKM_FALCON_WAIT_STEP_US);
		waited += NVKM_FALCON_WAIT_STEP_US;
	}
}

void
nvkm_falcon_set_bootvec(struct nvkm_falcon *flcn, uint32_t bootvec)
{
	nvkm_falcon_wr32(flcn, NVKM_FLCN_BOOTVEC, bootvec);
}

/*
 * Start the Falcon CPU. Turing offers two control registers: CPUCTL
 * (privileged) and CPUCTL_ALIAS (NS-accessible). When ALIAS_EN is set in
 * CPUCTL, we must use the alias.
 */
void
nvkm_falcon_start(struct nvkm_falcon *flcn)
{
	uint32_t cpuctl = nvkm_falcon_rd32(flcn, NVKM_FLCN_CPUCTL);

	if (cpuctl & NVKM_FLCN_CPUCTL_ALIAS_EN) {
		nvkm_falcon_wr32(flcn, NVKM_FLCN_CPUCTL_ALIAS,
		    NVKM_FLCN_CPUCTL_ALIAS_STARTCPU);
	} else {
		nvkm_falcon_wr32(flcn, NVKM_FLCN_CPUCTL,
		    NVKM_FLCN_CPUCTL_STARTCPU);
	}
}

void
nvkm_falcon_disable_ctx_req(struct nvkm_falcon *flcn)
{
	uint32_t v;

	if (flcn->fbif == 0)
		return;
	v = nvkm_falcon_fbif_rd32(flcn, NVKM_FBIF_CTL);
	v |= NVKM_FBIF_CTL_ALLOW_PHYS_NO_CTX;
	nvkm_falcon_fbif_wr32(flcn, NVKM_FBIF_CTL, v);

	/*
	 * Per open-rm kflcnDisableCtxReq_TU102: BOTH FBIF_CTL and DMACTL
	 * must be updated. Clearing DMACTL clears REQUIRE_CTX (bit 0)
	 * which otherwise blocks DMA from sysmem when no context is bound.
	 */
	nvkm_falcon_wr32(flcn, NVKM_FLCN_DMACTL, 0);
}

/*
 * IMEM load helper. Loads one block (up to NVKM_FLCN_IMEM_BLKSIZE bytes)
 * via a single IMEMT + N×IMEMD sequence. The starting IMEMC has already
 * been set by the caller; AINCW makes each IMEMD write advance OFFS.
 */
static void
nvkm_falcon_imem_wr_block(struct nvkm_falcon *flcn, uint8_t port,
    const uint8_t *src, uint32_t bytes, uint16_t tag)
{
	uint32_t i;

	nvkm_falcon_wr32(flcn, NVKM_FLCN_IMEMT(port), tag);
	for (i = 0; i + 4 <= bytes; i += 4) {
		uint32_t w = (uint32_t)src[i + 0] |
		    ((uint32_t)src[i + 1] <<  8) |
		    ((uint32_t)src[i + 2] << 16) |
		    ((uint32_t)src[i + 3] << 24);
		nvkm_falcon_wr32(flcn, NVKM_FLCN_IMEMD(port), w);
	}
}

int
nvkm_falcon_load_imem(struct nvkm_falcon *flcn, const void *data,
    uint32_t imem_offset, uint32_t size, uint16_t start_tag,
    uint8_t port, bool secure)
{
	const uint8_t *src = data;
	uint32_t imemc, remaining, block_bytes;
	uint16_t tag = start_tag;

	if ((imem_offset & (NVKM_FLCN_IMEM_BLKSIZE - 1)) != 0)
		return (EINVAL);
	if ((size & (NVKM_FLCN_ACCESS_ALIGN - 1)) != 0)
		return (EINVAL);

	/*
	 * IMEMC layout: BLK in bits 15:8, OFFS (word index) in bits 7:2.
	 * Because imem_offset is block-aligned (low 8 bits zero) the byte
	 * value can be ORed directly: bits 15:8 carry the block, bits 7:2
	 * carry zero offset. Enable AINCW so each IMEMD write advances
	 * the address by 4 bytes.
	 */
	imemc = (imem_offset & 0xffffu) | NVKM_FLCN_IMEMC_AINCW;
	if (secure)
		imemc |= NVKM_FLCN_IMEMC_SECURE;
	nvkm_falcon_wr32(flcn, NVKM_FLCN_IMEMC(port), imemc);

	remaining = size;
	while (remaining > 0) {
		block_bytes = remaining > NVKM_FLCN_IMEM_BLKSIZE ?
		    NVKM_FLCN_IMEM_BLKSIZE : remaining;
		nvkm_falcon_imem_wr_block(flcn, port, src, block_bytes, tag);
		src += block_bytes;
		remaining -= block_bytes;
		tag++;
	}

	return (0);
}

int
nvkm_falcon_load_dmem(struct nvkm_falcon *flcn, const void *data,
    uint32_t dmem_offset, uint32_t size, uint8_t port)
{
	const uint8_t *src = data;
	uint32_t dmemc, i;

	if ((dmem_offset & (NVKM_FLCN_ACCESS_ALIGN - 1)) != 0)
		return (EINVAL);
	if ((size & (NVKM_FLCN_ACCESS_ALIGN - 1)) != 0)
		return (EINVAL);

	/*
	 * DMEM has no tags; just set up the starting block/offset with
	 * AINCW and stream IMEMD writes. dmem_offset uses the same
	 * encoding as IMEMC for BLK/OFFS.
	 */
	dmemc = (dmem_offset & 0xffffu) | NVKM_FLCN_DMEMC_AINCW;
	nvkm_falcon_wr32(flcn, NVKM_FLCN_DMEMC(port), dmemc);

	for (i = 0; i + 4 <= size; i += 4) {
		uint32_t w = (uint32_t)src[i + 0] |
		    ((uint32_t)src[i + 1] <<  8) |
		    ((uint32_t)src[i + 2] << 16) |
		    ((uint32_t)src[i + 3] << 24);
		nvkm_falcon_wr32(flcn, NVKM_FLCN_DMEMD(port), w);
	}

	return (0);
}
