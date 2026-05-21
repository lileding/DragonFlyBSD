/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM development-time debug sysctls.
 *
 *   dev.nvkm.0.loginit   (opaque) -- 64 KiB LIBOS LOGINIT buffer (raw)
 *   dev.nvkm.0.logintr   (opaque) -- 64 KiB LIBOS LOGINTR buffer (raw)
 *   dev.nvkm.0.logrm     (opaque) -- 64 KiB LIBOS LOGRM   buffer (raw)
 *   dev.nvkm.0.gsp_state (string) -- human-readable snapshot of GSP state
 *
 * Use from outside the box:
 *   ssh dfly 'doas sysctl -b dev.nvkm.0.loginit' > /tmp/loginit.bin
 *   ssh dfly 'doas sysctl    dev.nvkm.0.gsp_state'
 *
 * No parsing yet -- buffers are dumped verbatim so we can inspect with
 * strings/xxd while we still don't have the .fwlogging_* format table
 * extracted from the GSP-RM ELF.
 */

#include "nvkm_priv.h"

#include <sys/sysctl.h>
#include <sys/sbuf.h>

static int
nvkm_gsp_sysctl_blob(SYSCTL_HANDLER_ARGS)
{
	struct nvkm_dmamem *dm = arg1;

	if (dm == NULL || dm->kva == NULL || dm->size == 0)
		return (ENXIO);
	return (SYSCTL_OUT(req, dm->kva, dm->size));
}

static uint32_t
nvkm_gsp_rd32_safe(struct nvkm_softc *sc, uint32_t off)
{
	if (sc->bar_res[0] == NULL)
		return (0xdeadbeefu);
	return (nvkm_rd32(sc, off));
}

static int
nvkm_gsp_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	struct nvkm_softc *sc = arg1;
	struct sbuf sb;
	char buf[2048];
	uint32_t riscv_status, mb0, mb1, sctl;
	uint8_t *cmdq, *msgq;
	uint32_t cmdq_wptr, cmdq_rptr, msgq_wptr, msgq_rptr;
	uint64_t loginit_put, logintr_put, logrm_put;
	int err;

	sbuf_new(&sb, buf, sizeof(buf), SBUF_FIXEDLEN);

	riscv_status = nvkm_gsp_rd32_safe(sc, 0x111240);
	mb0          = nvkm_gsp_rd32_safe(sc, NVKM_TU102_GSP_BASE + 0x040);
	mb1          = nvkm_gsp_rd32_safe(sc, NVKM_TU102_GSP_BASE + 0x044);
	sctl         = nvkm_gsp_rd32_safe(sc, NVKM_TU102_GSP_BASE + 0x240);

	sbuf_printf(&sb, "RISCV_STATUS = 0x%08x  (bit0 ACTIVE_STAT)\n", riscv_status);
	sbuf_printf(&sb, "GSP MB0      = 0x%08x\n", mb0);
	sbuf_printf(&sb, "GSP MB1      = 0x%08x\n", mb1);
	sbuf_printf(&sb, "GSP SCTL     = 0x%08x  (0x7000 = RISC-V mode)\n", sctl);

	if (sc->gsp_shm.kva != NULL) {
		cmdq = (uint8_t *)sc->gsp_shm.kva + sc->gsp_shm_cmdq_off;
		msgq = (uint8_t *)sc->gsp_shm.kva + sc->gsp_shm_msgq_off;
		cmdq_wptr = *(volatile uint32_t *)(cmdq + 0x10);
		cmdq_rptr = *(volatile uint32_t *)(cmdq + 32);     /* rxHdrOff */
		msgq_wptr = *(volatile uint32_t *)(msgq + 0x10);
		msgq_rptr = *(volatile uint32_t *)(msgq + 32);
		sbuf_printf(&sb,
		    "cmdq @0x%lx kva=%p wptr=%u rptr(GSP)=%u\n",
		    (unsigned long)(sc->gsp_shm.paddr + sc->gsp_shm_cmdq_off),
		    cmdq, cmdq_wptr, cmdq_rptr);
		sbuf_printf(&sb,
		    "msgq @0x%lx kva=%p wptr(GSP)=%u rptr=%u\n",
		    (unsigned long)(sc->gsp_shm.paddr + sc->gsp_shm_msgq_off),
		    msgq, msgq_wptr, msgq_rptr);
	} else {
		sbuf_cat(&sb, "shm not allocated\n");
	}

	loginit_put = (sc->gsp_loginit.kva != NULL)
	    ? *(volatile uint64_t *)sc->gsp_loginit.kva : 0;
	logintr_put = (sc->gsp_logintr.kva != NULL)
	    ? *(volatile uint64_t *)sc->gsp_logintr.kva : 0;
	logrm_put   = (sc->gsp_logrm.kva   != NULL)
	    ? *(volatile uint64_t *)sc->gsp_logrm.kva   : 0;
	sbuf_printf(&sb, "LOGINIT put = 0x%016llx (size=%zu)\n",
	    (unsigned long long)loginit_put,
	    (size_t)(sc->gsp_loginit.size));
	sbuf_printf(&sb, "LOGINTR put = 0x%016llx (size=%zu)\n",
	    (unsigned long long)logintr_put,
	    (size_t)(sc->gsp_logintr.size));
	sbuf_printf(&sb, "LOGRM   put = 0x%016llx (size=%zu)\n",
	    (unsigned long long)logrm_put,
	    (size_t)(sc->gsp_logrm.size));

	sbuf_finish(&sb);
	err = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb) + 1);
	sbuf_delete(&sb);
	return (err);
}

void
nvkm_gsp_debug_publish_sysctl(struct nvkm_softc *sc,
    struct sysctl_ctx_list *ctx, struct sysctl_oid *parent)
{
	struct sysctl_oid_list *children = SYSCTL_CHILDREN(parent);

	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "loginit",
	    CTLTYPE_OPAQUE | CTLFLAG_RD, &sc->gsp_loginit, 0,
	    nvkm_gsp_sysctl_blob, "S", "GSP LIBOS LOGINIT buffer (raw)");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "logintr",
	    CTLTYPE_OPAQUE | CTLFLAG_RD, &sc->gsp_logintr, 0,
	    nvkm_gsp_sysctl_blob, "S", "GSP LIBOS LOGINTR buffer (raw)");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "logrm",
	    CTLTYPE_OPAQUE | CTLFLAG_RD, &sc->gsp_logrm, 0,
	    nvkm_gsp_sysctl_blob, "S", "GSP LIBOS LOGRM buffer (raw)");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "gsp_state",
	    CTLTYPE_STRING | CTLFLAG_RD, sc, 0,
	    nvkm_gsp_sysctl_state, "A",
	    "GSP boot/runtime state snapshot");
}
