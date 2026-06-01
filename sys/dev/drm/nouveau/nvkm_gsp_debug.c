/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM development-time debug sysctls.
 *
 *   dev.drm.0.loginit    (opaque) -- 64 KiB LIBOS LOGINIT buffer (raw)
 *   dev.drm.0.logintr    (opaque) -- 64 KiB LIBOS LOGINTR buffer (raw)
 *   dev.drm.0.logrm      (opaque) -- 64 KiB LIBOS LOGRM   buffer (raw)
 *   dev.drm.0.gsp_state  (string) -- human-readable snapshot of GSP state
 *   dev.drm.0.vram_state (string) -- VRAM drm_mm allocation summary
 *
 * Use from outside the box:
 *   ssh dfly 'doas sysctl -b dev.drm.0.loginit' > /tmp/loginit.bin
 *   ssh dfly 'doas sysctl    dev.drm.0.gsp_state'
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

static int
nvkm_gsp_sysctl_vram_state(SYSCTL_HANDLER_ARGS)
{
	struct nvkm_softc *sc = arg1;
	struct nvkm_vram_alloc *alloc;
	struct drm_mm_node *hole;
	struct sbuf sb;
	char buf[4096];
	uint64_t total, active, free_bytes, max_free;
	uint64_t hole_start, hole_end, hole_size;
	int i, err, range_count, omitted_count;

	sbuf_new(&sb, buf, sizeof(buf), SBUF_FIXEDLEN);

	total = sc->vram_bump_limit - sc->vram_bump_base;
	active = 0;
	free_bytes = 0;
	max_free = 0;

	lockmgr(&sc->vram_lock, LK_EXCLUSIVE);
	for (i = 0; i < NVKM_VRAM_KIND_COUNT; i++)
		active += sc->vram_alloc_bytes[i];
	drm_mm_for_each_hole(hole, &sc->vram_mm, hole_start, hole_end) {
		hole_size = hole_end - hole_start;
		free_bytes += hole_size;
		if (hole_size > max_free)
			max_free = hole_size;
	}

	sbuf_printf(&sb, "window_base = 0x%016llx\n",
	    (unsigned long long)sc->vram_bump_base);
	sbuf_printf(&sb, "window_limit = 0x%016llx\n",
	    (unsigned long long)sc->vram_bump_limit);
	sbuf_printf(&sb, "window_bytes = 0x%016llx\n",
	    (unsigned long long)total);
	sbuf_printf(&sb, "active_bytes = 0x%016llx\n",
	    (unsigned long long)active);
	sbuf_printf(&sb, "free_bytes = 0x%016llx\n",
	    (unsigned long long)free_bytes);
	sbuf_printf(&sb, "max_free_bytes = 0x%016llx\n",
	    (unsigned long long)max_free);

	sbuf_cat(&sb, "\nkind active_count active_bytes\n");
	for (i = 0; i < NVKM_VRAM_KIND_COUNT; i++) {
		if (sc->vram_alloc_count[i] == 0 &&
		    sc->vram_alloc_bytes[i] == 0)
			continue;
		sbuf_printf(&sb, "%s %u 0x%016llx\n",
		    nvkm_vram_kind_name(i), sc->vram_alloc_count[i],
		    (unsigned long long)sc->vram_alloc_bytes[i]);
	}

	sbuf_cat(&sb, "\nactive_ranges\n");
	range_count = 0;
	omitted_count = 0;
	TAILQ_FOREACH(alloc, &sc->vram_allocs, alloc_link) {
		if (range_count++ >= 24) {
			omitted_count++;
			continue;
		}
		sbuf_printf(&sb,
		    "%s 0x%016llx 0x%016llx owner=%p free=%u\n",
		    nvkm_vram_kind_name(alloc->kind),
		    (unsigned long long)alloc->paddr,
		    (unsigned long long)alloc->size,
		    alloc->owner, alloc->free);
	}
	if (omitted_count != 0)
		sbuf_printf(&sb, "... omitted_ranges = %d\n", omitted_count);
	lockmgr(&sc->vram_lock, LK_RELEASE);

	err = sbuf_finish(&sb);
	if (err != 0) {
		sbuf_delete(&sb);
		return (err);
	}
	err = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb) + 1);
	sbuf_delete(&sb);
	return (err);
}


static int
nvkm_gsp_sysctl_state_summary(SYSCTL_HANDLER_ARGS)
{
	struct nvkm_softc *sc = arg1;
	struct sbuf sb;
	char buf[4096];
	uint32_t bar1_used, bar1_total;
	uint32_t vmm_pd0_count, vmm_pt_count, sparse_region_count;
	uint64_t valid_pte_count;
	int err;

	bar1_used = 0;
	bar1_total = 0;
	vmm_pd0_count = 0;
	vmm_pt_count = 0;
	valid_pte_count = 0;
	sparse_region_count = 0;
	nvkm_gsp_bar1_count_gva(sc, &bar1_used, &bar1_total);
	nvkm_gsp_vmm_snapshot(sc->gsp_vmm, &vmm_pd0_count, &vmm_pt_count,
	    &valid_pte_count, &sparse_region_count);

	sbuf_new(&sb, buf, sizeof(buf), SBUF_FIXEDLEN);
	sbuf_printf(&sb, "fence_context = 0x%016llx\n",
	    (unsigned long long)sc->fence_context);
	sbuf_printf(&sb, "fence_seqno = %u\n", sc->fence_seqno);

	sbuf_cat(&sb, "\nexec\n");
	sbuf_printf(&sb, "submit_count = %llu\n",
	    (unsigned long long)sc->exec_submit_count);
	sbuf_printf(&sb, "signal_only_count = %llu\n",
	    (unsigned long long)sc->exec_signal_only_count);
	sbuf_printf(&sb, "timeout_count = %llu\n",
	    (unsigned long long)sc->exec_timeout_count);
	sbuf_printf(&sb, "internal_fence_count = %llu\n",
	    (unsigned long long)sc->exec_internal_fence_count);
	sbuf_printf(&sb, "signal_fence_count = %llu\n",
	    (unsigned long long)sc->exec_signal_fence_count);
	sbuf_printf(&sb, "resv_attach_calls = %llu\n",
	    (unsigned long long)sc->exec_resv_attach_calls);
	sbuf_printf(&sb, "resv_attach_bos = %llu\n",
	    (unsigned long long)sc->exec_resv_attach_bos);

	sbuf_cat(&sb, "\nexec_profile_us\n");
	sbuf_printf(&sb, "token_wait_us = %llu\n",
	    (unsigned long long)sc->exec_profile_token_wait_us);
	sbuf_printf(&sb, "wait_sync_us = %llu\n",
	    (unsigned long long)sc->exec_profile_wait_sync_us);
	sbuf_printf(&sb, "push_build_us = %llu\n",
	    (unsigned long long)sc->exec_profile_push_build_us);
	sbuf_printf(&sb, "prepare_signal_us = %llu\n",
	    (unsigned long long)sc->exec_profile_prepare_signal_us);
	sbuf_printf(&sb, "attach_resv_us = %llu\n",
	    (unsigned long long)sc->exec_profile_attach_resv_us);
	sbuf_printf(&sb, "flush_cpu_us = %llu\n",
	    (unsigned long long)sc->exec_profile_flush_cpu_us);
	sbuf_printf(&sb, "cache_flush_us = %llu\n",
	    (unsigned long long)sc->exec_profile_cache_flush_us);
	sbuf_printf(&sb, "doorbell_us = %llu\n",
	    (unsigned long long)sc->exec_profile_doorbell_us);
	sbuf_printf(&sb, "poll_us = %llu\n",
	    (unsigned long long)sc->exec_profile_poll_us);
	sbuf_printf(&sb, "cleanup_us = %llu\n",
	    (unsigned long long)sc->exec_profile_cleanup_us);
	sbuf_printf(&sb, "poll_iters = %llu\n",
	    (unsigned long long)sc->exec_profile_poll_iters);
	sbuf_printf(&sb, "pushes = %llu\n",
	    (unsigned long long)sc->exec_profile_pushes);
	sbuf_printf(&sb, "cpu_bind_scanned = %llu\n",
	    (unsigned long long)sc->exec_profile_cpu_bind_scanned);
	sbuf_printf(&sb, "cpu_bind_flushed = %llu\n",
	    (unsigned long long)sc->exec_profile_cpu_bind_flushed);

	sbuf_cat(&sb, "\nsyncobj\n");
	sbuf_printf(&sb, "wait_count = %llu\n",
	    (unsigned long long)sc->sync_wait_count);
	sbuf_printf(&sb, "wait_error_count = %llu\n",
	    (unsigned long long)sc->sync_wait_error_count);
	sbuf_printf(&sb, "signal_count = %llu\n",
	    (unsigned long long)sc->sync_signal_count);
	sbuf_printf(&sb, "signal_error_count = %llu\n",
	    (unsigned long long)sc->sync_signal_error_count);

	sbuf_cat(&sb, "\ngsp_events\n");
	sbuf_printf(&sb, "post_event_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_count);
	sbuf_printf(&sb, "post_event_short_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_short_count);
	sbuf_printf(&sb, "post_event_bad_size_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_bad_size_count);
	sbuf_printf(&sb, "post_event_unhandled_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_unhandled_count);
	sbuf_printf(&sb, "post_event_last_client = 0x%08x\n",
	    sc->gsp_post_event_last_client);
	sbuf_printf(&sb, "post_event_last_event = 0x%08x\n",
	    sc->gsp_post_event_last_event);
	sbuf_printf(&sb, "post_event_last_notify_index = %u\n",
	    sc->gsp_post_event_last_notify_index);
	sbuf_printf(&sb, "post_event_last_data = 0x%08x\n",
	    sc->gsp_post_event_last_data);
	sbuf_printf(&sb, "post_event_last_status = 0x%08x\n",
	    sc->gsp_post_event_last_status);
	sbuf_printf(&sb, "post_event_last_data_size = %u\n",
	    sc->gsp_post_event_last_data_size);

	sbuf_cat(&sb, "\nreservation\n");
	sbuf_printf(&sb, "bo_wait_count = %llu\n",
	    (unsigned long long)sc->bo_resv_wait_count);
	sbuf_printf(&sb, "bo_wait_error_count = %llu\n",
	    (unsigned long long)sc->bo_resv_wait_error_count);
	sbuf_printf(&sb, "vm_bind_wait_count = %llu\n",
	    (unsigned long long)sc->vm_bind_wait_count);
	sbuf_printf(&sb, "vm_bind_wait_error_count = %llu\n",
	    (unsigned long long)sc->vm_bind_wait_error_count);
	sbuf_printf(&sb, "cpu_prep_wait_count = %llu\n",
	    (unsigned long long)sc->cpu_prep_wait_count);
	sbuf_printf(&sb, "cpu_prep_wait_error_count = %llu\n",
	    (unsigned long long)sc->cpu_prep_wait_error_count);

	sbuf_cat(&sb, "\nbar1\n");
	sbuf_printf(&sb, "gva_used = %u\n", bar1_used);
	sbuf_printf(&sb, "gva_total = %u\n", bar1_total);
	sbuf_printf(&sb, "gva_free = %u\n", bar1_total - bar1_used);

	sbuf_cat(&sb, "\nvmm\n");
	sbuf_printf(&sb, "user_pd0_count = %u\n", vmm_pd0_count);
	sbuf_printf(&sb, "user_pt_count = %u\n", vmm_pt_count);
	sbuf_printf(&sb, "valid_pte_count = %llu\n",
	    (unsigned long long)valid_pte_count);
	sbuf_printf(&sb, "sparse_region_count = %u\n", sparse_region_count);

	err = sbuf_finish(&sb);
	if (err != 0) {
		sbuf_delete(&sb);
		return (err);
	}
	err = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb) + 1);
	sbuf_delete(&sb);
	return (err);
}

void
nvkm_gsp_debug_publish_sysctl(struct nvkm_softc *sc,
    struct sysctl_ctx_list *ctx, struct sysctl_oid *parent)
{
	struct sysctl_oid_list *children = SYSCTL_CHILDREN(parent);

	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "debug",
	    CTLFLAG_RW, &nvkm_debug, 0,
	    "Enable verbose nvkm printf logging");
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
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "vram_state",
	    CTLTYPE_STRING | CTLFLAG_RD, sc, 0,
	    nvkm_gsp_sysctl_vram_state, "A",
	    "VRAM drm_mm allocation summary");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "state",
	    CTLTYPE_STRING | CTLFLAG_RD, sc, 0,
	    nvkm_gsp_sysctl_state_summary, "A",
	    "compute path counters and allocator snapshots");
}
