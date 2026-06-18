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
#include "nvkm_gsp_vmm.h"
#include "nvkm_gsp_rm.h"

#include <sys/sysctl.h>
#include <sys/sbuf.h>

#define NVKM_CPU_INTR_LEAF(i)	(0x00b81000u + (i) * 4u)
#define NVKM_CPU_INTR_TOP	0x00b81600u

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

static const char *
nvkm_debug_pte_kind_name(uint8_t kind)
{
	switch (kind) {
	case 0x00: return ("pitch");
	case 0x01: return ("z16");
	case 0x02: return ("s8");
	case 0x03: return ("s8z24");
	case 0x04: return ("zf32_x24s8");
	case 0x05: return ("z24s8");
	case 0x06: return ("generic");
	case 0x07: return ("invalid");
	case 0x08: return ("generic_compressible");
	case 0x09: return ("generic_compressible_disable_plc");
	case 0x0a: return ("s8_compressible_disable_plc");
	case 0x0b: return ("z16_compressible_disable_plc");
	case 0x0c: return ("s8z24_compressible_disable_plc");
	case 0x0d: return ("zf32_x24s8_compressible_disable_plc");
	case 0x0e: return ("z24s8_compressible_disable_plc");
	case 0x0f: return ("smsked_message");
	default: return ("other");
	}
}

static uint8_t
nvkm_debug_pte_kind(uint64_t pte)
{
	return ((uint8_t)(pte >> NV_PTE_KIND_SHIFT));
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
	struct sbuf *sb;
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

	sb = sbuf_new_auto();
	if (sb == NULL)
		return (ENOMEM);
	sbuf_printf(sb, "fence_context = 0x%016llx\n",
	    (unsigned long long)sc->fence_context);
	sbuf_printf(sb, "fence_seqno = %u\n", sc->fence_seqno);

	sbuf_cat(sb, "\nexec\n");
	sbuf_printf(sb, "submit_count = %llu\n",
	    (unsigned long long)sc->exec_submit_count);
	sbuf_printf(sb, "signal_only_count = %llu\n",
	    (unsigned long long)sc->exec_signal_only_count);
	sbuf_printf(sb, "timeout_count = %llu\n",
	    (unsigned long long)sc->exec_timeout_count);
	sbuf_printf(sb, "internal_fence_count = %llu\n",
	    (unsigned long long)sc->exec_internal_fence_count);
	sbuf_printf(sb, "signal_fence_count = %llu\n",
	    (unsigned long long)sc->exec_signal_fence_count);
	sbuf_printf(sb, "resv_attach_calls = %llu\n",
	    (unsigned long long)sc->exec_resv_attach_calls);
	sbuf_printf(sb, "resv_attach_bos = %llu\n",
	    (unsigned long long)sc->exec_resv_attach_bos);
	sbuf_printf(sb, "resv_attach_signaled_count = %llu\n",
	    (unsigned long long)sc->exec_resv_attach_signaled_count);
	sbuf_printf(sb, "resv_attach_pending_count = %llu\n",
	    (unsigned long long)sc->exec_resv_attach_pending_count);
	sbuf_printf(sb, "vm_bind_resv_attach_signaled_count = %llu\n",
	    (unsigned long long)sc->vm_bind_resv_attach_signaled_count);
	sbuf_printf(sb, "vm_bind_resv_attach_pending_count = %llu\n",
	    (unsigned long long)sc->vm_bind_resv_attach_pending_count);
	sbuf_printf(sb, "async_pending_count = %llu\n",
	    (unsigned long long)sc->exec_async_pending_count);
	sbuf_printf(sb, "async_complete_count = %llu\n",
	    (unsigned long long)sc->exec_async_complete_count);
	sbuf_printf(sb, "async_wait_count = %llu\n",
	    (unsigned long long)sc->exec_async_wait_count);
	sbuf_printf(sb, "async_wait_error_count = %llu\n",
	    (unsigned long long)sc->exec_async_wait_error_count);
	sbuf_printf(sb, "pending_signal_count = %llu\n",
	    (unsigned long long)sc->exec_pending_signal_count);
	sbuf_printf(sb, "pending_signal_error_count = %llu\n",
	    (unsigned long long)sc->exec_pending_signal_error_count);
	sbuf_printf(sb, "pending_signal_fence_count = %llu\n",
	    (unsigned long long)sc->exec_pending_signal_fence_count);
	sbuf_printf(sb, "pending_signal_already_signaled_count = %llu\n",
	    (unsigned long long)sc->exec_pending_signal_already_signaled_count);
	sbuf_printf(sb, "pending_signal_hw_ready_count = %llu\n",
	    (unsigned long long)sc->exec_pending_signal_hw_ready_count);
	sbuf_printf(sb, "job_done_signal_count = %llu\n",
	    (unsigned long long)sc->job_done_signal_count);
	sbuf_printf(sb, "job_done_signal_error_count = %llu\n",
	    (unsigned long long)sc->job_done_signal_error_count);
	sbuf_printf(sb, "job_done_signal_already_signaled_count = %llu\n",
	    (unsigned long long)sc->job_done_signal_already_signaled_count);
	sbuf_printf(sb, "job_done_signal_hw_ready_count = %llu\n",
	    (unsigned long long)sc->job_done_signal_hw_ready_count);

	sbuf_cat(sb, "\nexec_profile_us\n");
	sbuf_printf(sb, "token_wait_us = %llu\n",
	    (unsigned long long)sc->exec_profile_token_wait_us);
	sbuf_printf(sb, "wait_sync_us = %llu\n",
	    (unsigned long long)sc->exec_profile_wait_sync_us);
	sbuf_printf(sb, "push_build_us = %llu\n",
	    (unsigned long long)sc->exec_profile_push_build_us);
	sbuf_printf(sb, "prepare_signal_us = %llu\n",
	    (unsigned long long)sc->exec_profile_prepare_signal_us);
	sbuf_printf(sb, "attach_resv_us = %llu\n",
	    (unsigned long long)sc->exec_profile_attach_resv_us);
	sbuf_printf(sb, "flush_cpu_us = %llu\n",
	    (unsigned long long)sc->exec_profile_flush_cpu_us);
	sbuf_printf(sb, "cache_flush_us = %llu\n",
	    (unsigned long long)sc->exec_profile_cache_flush_us);
	sbuf_printf(sb, "doorbell_us = %llu\n",
	    (unsigned long long)sc->exec_profile_doorbell_us);
	sbuf_printf(sb, "poll_us = %llu\n",
	    (unsigned long long)sc->exec_profile_poll_us);
	sbuf_printf(sb, "cleanup_us = %llu\n",
	    (unsigned long long)sc->exec_profile_cleanup_us);
	sbuf_printf(sb, "poll_iters = %llu\n",
	    (unsigned long long)sc->exec_profile_poll_iters);
	sbuf_printf(sb, "pushes = %llu\n",
	    (unsigned long long)sc->exec_profile_pushes);
	sbuf_printf(sb, "cpu_bind_scanned = %llu\n",
	    (unsigned long long)sc->exec_profile_cpu_bind_scanned);
	sbuf_printf(sb, "cpu_bind_flushed = %llu\n",
	    (unsigned long long)sc->exec_profile_cpu_bind_flushed);

	sbuf_cat(sb, "\nexec_trace\n");
	sbuf_printf(sb, "next = %u\n", sc->exec_trace_next);
	{
		uint32_t end = sc->exec_trace_next;
		uint32_t start = end > 8 ? end - 8 : 0;

		for (uint32_t pos = start; pos < end; pos++) {
			const struct nvkm_drm_exec_trace *trace;

			trace = &sc->exec_trace[pos %
			    NVKM_DRM_EXEC_TRACE_COUNT];
			sbuf_printf(sb,
			    "trace[%u] seq=%llu ch=%u chid=%u slot=%u gpf=%u push=%u/%u va=0x%016llx len=0x%08x flags=0x%08x binding=0x%016llx+0x%016llx obj=0x%jx domain=0x%x paddr=0x%016llx cpu=%u done=%u err=%d\n",
			    pos, (unsigned long long)trace->seq,
			    trace->channel, trace->chid, trace->post_slot,
			    trace->gpf_index, trace->push_index,
			    trace->push_count, (unsigned long long)trace->va,
			    trace->va_len, trace->flags,
			    (unsigned long long)trace->binding_addr,
			    (unsigned long long)trace->binding_size,
			    (uintmax_t)trace->obj, trace->bo_domain,
			    (unsigned long long)trace->bo_paddr,
			    trace->cpu_mapped, trace->completed,
			    trace->error);
		}
	}

	sbuf_cat(sb, "\nsyncobj\n");
	sbuf_printf(sb, "wait_count = %llu\n",
	    (unsigned long long)sc->sync_wait_count);
	sbuf_printf(sb, "wait_error_count = %llu\n",
	    (unsigned long long)sc->sync_wait_error_count);
	sbuf_printf(sb, "wait_already_signaled_count = %llu\n",
	    (unsigned long long)sc->sync_wait_already_signaled_count);
	sbuf_printf(sb, "wait_blocking_count = %llu\n",
	    (unsigned long long)sc->sync_wait_blocking_count);
	sbuf_printf(sb, "wait_blocking_us = %llu\n",
	    (unsigned long long)sc->sync_wait_blocking_us);
	sbuf_printf(sb, "wait_local_count = %llu\n",
	    (unsigned long long)sc->sync_wait_local_count);
	sbuf_printf(sb, "wait_external_count = %llu\n",
	    (unsigned long long)sc->sync_wait_external_count);
	sbuf_printf(sb, "job_wait_armed_count = %llu\n",
	    (unsigned long long)sc->sync_job_wait_armed_count);
	sbuf_printf(sb, "job_dep_cb_count = %llu\n",
	    (unsigned long long)sc->sync_job_dep_cb_count);
	sbuf_printf(sb, "job_dep_queue_count = %llu\n",
	    (unsigned long long)sc->sync_job_dep_queue_count);
	sbuf_printf(sb, "job_dep_queue_error_count = %llu\n",
	    (unsigned long long)sc->sync_job_dep_queue_error_count);
	sbuf_printf(sb, "job_ready_count = %llu\n",
	    (unsigned long long)sc->sync_job_ready_count);
	sbuf_printf(sb, "job_cancel_count = %llu\n",
	    (unsigned long long)sc->sync_job_cancel_count);
	sbuf_printf(sb, "signal_count = %llu\n",
	    (unsigned long long)sc->sync_signal_count);
	sbuf_printf(sb, "signal_error_count = %llu\n",
	    (unsigned long long)sc->sync_signal_error_count);

	sbuf_cat(sb, "\nsync_diag\n");
	sbuf_printf(sb, "enable = %d\n", sc->sync_diag_enable);
	sbuf_printf(sb, "job_enter_count = %llu\n",
	    (unsigned long long)sc->job_diag_enter_count);
	sbuf_printf(sb, "job_leave_count = %llu\n",
	    (unsigned long long)sc->job_diag_leave_count);
	sbuf_printf(sb, "job_active_seq = %llu\n",
	    (unsigned long long)sc->job_diag_active_seq);
	sbuf_printf(sb, "job_active_start_us = %llu\n",
	    (unsigned long long)sc->job_diag_active_start_us);
	sbuf_printf(sb, "job_active_stage = %u\n",
	    sc->job_diag_active_stage);
	sbuf_printf(sb, "job_active_type = %u\n",
	    sc->job_diag_active_type);
	sbuf_printf(sb, "job_active_channel = %u\n",
	    sc->job_diag_active_channel);
	sbuf_printf(sb, "job_active_wait_count = %u\n",
	    sc->job_diag_active_wait_count);
	sbuf_printf(sb, "job_active_dep_pending = %u\n",
	    sc->job_diag_active_dep_pending);
	sbuf_printf(sb, "job_active_sig_count = %u\n",
	    sc->job_diag_active_sig_count);
	sbuf_printf(sb, "job_last_seq = %llu\n",
	    (unsigned long long)sc->job_diag_last_seq);
	sbuf_printf(sb, "job_last_us = %llu\n",
	    (unsigned long long)sc->job_diag_last_us);
	sbuf_printf(sb, "job_last_stage = %u\n",
	    sc->job_diag_last_stage);
	sbuf_printf(sb, "job_last_ret = %d\n",
	    sc->job_diag_last_ret);
	sbuf_printf(sb, "job_slow_count = %llu\n",
	    (unsigned long long)sc->job_diag_slow_count);
	sbuf_printf(sb, "job_slow_us_max = %llu\n",
	    (unsigned long long)sc->job_diag_slow_us_max);
	sbuf_printf(sb, "exec_enter_count = %llu\n",
	    (unsigned long long)sc->exec_diag_enter_count);
	sbuf_printf(sb, "exec_leave_count = %llu\n",
	    (unsigned long long)sc->exec_diag_leave_count);
	sbuf_printf(sb, "exec_active_seq = %llu\n",
	    (unsigned long long)sc->exec_diag_active_seq);
	sbuf_printf(sb, "exec_active_start_us = %llu\n",
	    (unsigned long long)sc->exec_diag_active_start_us);
	sbuf_printf(sb, "exec_active_stage = %u\n",
	    sc->exec_diag_active_stage);
	sbuf_printf(sb, "exec_active_channel = %u\n",
	    sc->exec_diag_active_channel);
	sbuf_printf(sb, "exec_active_push_count = %u\n",
	    sc->exec_diag_active_push_count);
	sbuf_printf(sb, "exec_active_sig_count = %u\n",
	    sc->exec_diag_active_sig_count);
	sbuf_printf(sb, "exec_active_put = %u\n",
	    sc->exec_diag_active_put);
	sbuf_printf(sb, "exec_active_slot = %u\n",
	    sc->exec_diag_active_slot);
	sbuf_printf(sb, "exec_last_seq = %llu\n",
	    (unsigned long long)sc->exec_diag_last_seq);
	sbuf_printf(sb, "exec_last_us = %llu\n",
	    (unsigned long long)sc->exec_diag_last_us);
	sbuf_printf(sb, "exec_last_stage = %u\n",
	    sc->exec_diag_last_stage);
	sbuf_printf(sb, "exec_last_channel = %u\n",
	    sc->exec_diag_last_channel);
	sbuf_printf(sb, "exec_last_push_count = %u\n",
	    sc->exec_diag_last_push_count);
	sbuf_printf(sb, "exec_last_sig_count = %u\n",
	    sc->exec_diag_last_sig_count);
	sbuf_printf(sb, "exec_last_put = %u\n", sc->exec_diag_last_put);
	sbuf_printf(sb, "exec_last_slot = %u\n", sc->exec_diag_last_slot);
	sbuf_printf(sb, "exec_last_ret = %d\n", sc->exec_diag_last_ret);
	sbuf_printf(sb, "exec_slow_count = %llu\n",
	    (unsigned long long)sc->exec_diag_slow_count);
	sbuf_printf(sb, "exec_slow_us_max = %llu\n",
	    (unsigned long long)sc->exec_diag_slow_us_max);

	sbuf_cat(sb, "\nirq\n");
	sbuf_printf(sb, "isr_count = %llu\n",
	    (unsigned long long)sc->irq_isr_count);
	sbuf_printf(sb, "msi_rearm_count = %llu\n",
	    (unsigned long long)sc->irq_msi_rearm_count);
	sbuf_printf(sb, "empty_count = %llu\n",
	    (unsigned long long)sc->irq_empty_count);
	sbuf_printf(sb, "unhandled_leaf_count = %llu\n",
	    (unsigned long long)sc->irq_unhandled_leaf_count);
	sbuf_printf(sb, "last_stat = 0x%08x\n", sc->irq_last_stat);
	sbuf_printf(sb, "last_top = 0x%08x\n", sc->irq_last_top);
	sbuf_printf(sb, "last_unhandled_leaf = %u\n",
	    sc->irq_last_unhandled_leaf);
	sbuf_printf(sb, "last_unhandled_mask = 0x%08x\n",
	    sc->irq_last_unhandled_mask);

	sbuf_cat(sb, "\nkms\n");
	sbuf_printf(sb, "auto_count = %llu\n",
	    (unsigned long long)sc->kms_auto_count);
	sbuf_printf(sb, "hotplug_count = %llu\n",
	    (unsigned long long)sc->kms_hotplug_count);
	sbuf_printf(sb, "restore_skip_primary_count = %llu\n",
	    (unsigned long long)sc->kms_restore_skip_primary_count);
	sbuf_printf(sb, "restore_last_primary_count = %u\n",
	    sc->kms_restore_last_primary_count);
	sbuf_printf(sb, "restore_last_open_count = %d\n",
	    sc->kms_restore_last_open_count);
	sbuf_printf(sb, "fb_create_count = %llu\n",
	    (unsigned long long)sc->kms_fb_create_count);
	sbuf_printf(sb, "fb_create_error_count = %llu\n",
	    (unsigned long long)sc->kms_fb_create_error_count);
	sbuf_printf(sb, "fb_create_blocklinear_count = %llu\n",
	    (unsigned long long)sc->kms_fb_create_blocklinear_count);
	sbuf_printf(sb, "fb_create_linear_count = %llu\n",
	    (unsigned long long)sc->kms_fb_create_linear_count);
	sbuf_printf(sb, "fb_destroy_count = %llu\n",
	    (unsigned long long)sc->kms_fb_destroy_count);
	sbuf_printf(sb, "page_flip_count = %llu\n",
	    (unsigned long long)sc->kms_page_flip_count);
	sbuf_printf(sb, "page_flip_event_count = %llu\n",
	    (unsigned long long)sc->kms_page_flip_event_count);
	sbuf_printf(sb, "page_flip_error_count = %llu\n",
	    (unsigned long long)sc->kms_page_flip_error_count);
	sbuf_printf(sb, "atomic_commit_tail_count = %llu\n",
	    (unsigned long long)sc->kms_atomic_commit_tail_count);
	sbuf_printf(sb, "atomic_vblank_wait_count = %llu\n",
	    (unsigned long long)sc->kms_atomic_vblank_wait_count);
	sbuf_printf(sb, "plane_update_count = %llu\n",
	    (unsigned long long)sc->kms_plane_update_count);
	sbuf_printf(sb, "plane_disable_count = %llu\n",
	    (unsigned long long)sc->kms_plane_disable_count);
	sbuf_printf(sb, "prepare_fb_count = %llu\n",
	    (unsigned long long)sc->kms_prepare_fb_count);
	sbuf_printf(sb, "prepare_fb_error_count = %llu\n",
	    (unsigned long long)sc->kms_prepare_fb_error_count);
	sbuf_printf(sb, "cleanup_fb_count = %llu\n",
	    (unsigned long long)sc->kms_cleanup_fb_count);
	sbuf_printf(sb, "scanout_pin_count = %llu\n",
	    (unsigned long long)sc->kms_scanout_pin_count);
	sbuf_printf(sb, "scanout_unpin_count = %llu\n",
	    (unsigned long long)sc->kms_scanout_unpin_count);
	sbuf_printf(sb, "commit_error_count = %llu\n",
	    (unsigned long long)sc->kms_commit_error_count);
	sbuf_printf(sb, "last_error = %d\n", sc->kms_last_error);
	sbuf_printf(sb, "last_head = %u\n", sc->kms_last_head);
	sbuf_printf(sb, "last_win = %u\n", sc->kms_last_win);
	sbuf_printf(sb, "push_trace = %d\n", sc->kms_push_trace);

	sbuf_cat(sb, "\nscanout\n");
	nvkm_dispnv50_debug_sbuf(sc, sb);

	sbuf_cat(sb, "\ngsp_events\n");
	sbuf_printf(sb, "post_event_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_count);
	sbuf_printf(sb, "post_event_short_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_short_count);
	sbuf_printf(sb, "post_event_bad_size_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_bad_size_count);
	sbuf_printf(sb, "post_event_unhandled_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_unhandled_count);
	sbuf_printf(sb, "post_event_nonstall_count = %llu\n",
	    (unsigned long long)sc->gsp_post_event_nonstall_count);
	sbuf_printf(sb, "msgq_null_event_drop_count = %llu\n",
	    (unsigned long long)sc->gsp_msgq_null_event_drop_count);
	sbuf_printf(sb, "msgq_null_event_drop_bytes = %llu\n",
	    (unsigned long long)sc->gsp_msgq_null_event_drop_bytes);
	sbuf_printf(sb, "msgq_null_event_last_fn = 0x%08x\n",
	    sc->gsp_msgq_null_event_last_fn);
	sbuf_printf(sb, "nocat_count = %llu\n",
	    (unsigned long long)sc->gsp_nocat_count);
	sbuf_printf(sb, "nocat_short_count = %llu\n",
	    (unsigned long long)sc->gsp_nocat_short_count);
	sbuf_printf(sb, "nocat_last_flags = 0x%08x\n",
	    sc->gsp_nocat_last_flags);
	sbuf_printf(sb, "nocat_last_timestamp = 0x%016llx\n",
	    (unsigned long long)sc->gsp_nocat_last_timestamp);
	sbuf_printf(sb, "nocat_last_rec_type = %u\n",
	    sc->gsp_nocat_last_rec_type);
	sbuf_printf(sb, "nocat_last_bugcheck = 0x%08x\n",
	    sc->gsp_nocat_last_bugcheck);
	sbuf_printf(sb, "nocat_last_source = \"%s\"\n",
	    sc->gsp_nocat_last_source);
	sbuf_printf(sb, "nocat_last_subsystem = 0x%08x\n",
	    sc->gsp_nocat_last_subsystem);
	sbuf_printf(sb, "nocat_last_error_code = 0x%016llx\n",
	    (unsigned long long)sc->gsp_nocat_last_error_code);
	sbuf_printf(sb, "nocat_last_engine = \"%s\"\n",
	    sc->gsp_nocat_last_engine);
	sbuf_printf(sb, "nocat_last_tdr_reason = 0x%08x\n",
	    sc->gsp_nocat_last_tdr_reason);
	sbuf_printf(sb, "nocat_last_diag_len = %u\n",
	    sc->gsp_nocat_last_diag_len);
	sbuf_printf(sb, "nocat_last_diag_stack = "
	    "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
	    sc->gsp_nocat_last_diag[0], sc->gsp_nocat_last_diag[1],
	    sc->gsp_nocat_last_diag[2], sc->gsp_nocat_last_diag[3],
	    sc->gsp_nocat_last_diag[4], sc->gsp_nocat_last_diag[5],
	    sc->gsp_nocat_last_diag[6], sc->gsp_nocat_last_diag[7],
	    sc->gsp_nocat_last_diag[8], sc->gsp_nocat_last_diag[9]);
	sbuf_printf(sb, "nocat_last_diag_count = %u\n",
	    sc->gsp_nocat_last_diag[10]);
	sbuf_printf(sb, "post_event_last_client = 0x%08x\n",
	    sc->gsp_post_event_last_client);
	sbuf_printf(sb, "post_event_last_event = 0x%08x\n",
	    sc->gsp_post_event_last_event);
	sbuf_printf(sb, "post_event_last_notify_index = %u\n",
	    sc->gsp_post_event_last_notify_index);
	sbuf_printf(sb, "post_event_last_data = 0x%08x\n",
	    sc->gsp_post_event_last_data);
	sbuf_printf(sb, "post_event_last_status = 0x%08x\n",
	    sc->gsp_post_event_last_status);
	sbuf_printf(sb, "post_event_last_data_size = %u\n",
	    sc->gsp_post_event_last_data_size);
	sbuf_printf(sb, "nonstall_event_register_count = %llu\n",
	    (unsigned long long)sc->gsp_nonstall_event_register_count);
	sbuf_printf(sb, "nonstall_event_register_error_count = %llu\n",
	    (unsigned long long)sc->gsp_nonstall_event_register_error_count);
	sbuf_printf(sb, "nonstall_event_handle = 0x%08x\n",
	    sc->gsp_nonstall_event_handle);
	sbuf_printf(sb, "nonstall_event_last_error = %u\n",
	    sc->gsp_nonstall_event_last_error);
	sbuf_printf(sb, "nonstall_intr_count = %llu\n",
	    (unsigned long long)sc->gsp_nonstall_intr_count);
	sbuf_printf(sb, "nonstall_intr_last_leaf = %u\n",
	    sc->gsp_nonstall_intr_last_leaf);
	sbuf_printf(sb, "nonstall_intr_last_mask = 0x%08x\n",
	    sc->gsp_nonstall_intr_last_mask);
	sbuf_printf(sb, "nonstall_intr_last_top = 0x%08x\n",
	    sc->gsp_nonstall_intr_last_top);
	sbuf_printf(sb, "nonstall_intr_top_now = 0x%08x\n",
	    nvkm_gsp_rd32_safe(sc, NVKM_CPU_INTR_TOP));
	sbuf_printf(sb, "gsp_falcon_intr_count = %llu\n",
	    (unsigned long long)sc->gsp_falcon_intr_count);
	sbuf_printf(sb, "gsp_falcon_msgq_wake_count = %llu\n",
	    (unsigned long long)sc->gsp_falcon_msgq_wake_count);
	sbuf_printf(sb, "gsp_falcon_unexpected_count = %llu\n",
	    (unsigned long long)sc->gsp_falcon_unexpected_count);
	sbuf_printf(sb, "gsp_falcon_last_stat = 0x%08x\n",
	    sc->gsp_falcon_last_stat);
	sbuf_printf(sb, "disp_intr_count = %llu\n",
	    (unsigned long long)sc->gsp_disp_intr_count);
	sbuf_printf(sb, "disp_intr_last_leaf = %u\n",
	    sc->gsp_disp_intr_last_leaf);
	sbuf_printf(sb, "disp_intr_last_mask = 0x%08x\n",
	    sc->gsp_disp_intr_last_mask);
	sbuf_printf(sb, "disp_vblank_mask = 0x%08x\n",
	    sc->gsp_disp_vblank_mask);
	for (uint32_t head = 0; head < 4; head++)
		sbuf_printf(sb, "disp_head_status[%u] = 0x%08x\n",
		    head, sc->gsp_disp_head_status[head]);
	sbuf_printf(sb, "other_stall_count = %llu\n",
	    (unsigned long long)sc->gsp_other_stall_count);
	sbuf_printf(sb, "other_stall_last_leaf = %u\n",
	    sc->gsp_other_stall_last_leaf);
	sbuf_printf(sb, "other_stall_last_mask = 0x%08x\n",
	    sc->gsp_other_stall_last_mask);
	for (uint32_t leaf = 0; leaf < 8; leaf++)
		sbuf_printf(sb, "stall_leaf_mask[%u] = 0x%08x\n",
		    leaf, sc->gsp_stall_leaf_mask[leaf]);
	for (uint32_t leaf = 0; leaf < 8; leaf++)
		sbuf_printf(sb, "gsp_leaf_mask[%u] = 0x%08x\n",
		    leaf, sc->gsp_engine_leaf_mask[leaf]);
	for (uint32_t leaf = 0; leaf < 8; leaf++)
		sbuf_printf(sb, "disp_leaf_mask[%u] = 0x%08x\n",
		    leaf, sc->gsp_disp_leaf_mask[leaf]);
	for (uint32_t leaf = 0; leaf < 8; leaf++)
		sbuf_printf(sb, "nonstall_leaf_mask[%u] = 0x%08x\n",
		    leaf, sc->gsp_nonstall_leaf_mask[leaf]);
	for (uint32_t leaf = 0; leaf < 8; leaf++)
		sbuf_printf(sb, "nonstall_leaf_pending[%u] = 0x%08x\n",
		    leaf, nvkm_gsp_rd32_safe(sc, NVKM_CPU_INTR_LEAF(leaf)));

	sbuf_cat(sb, "\nrc\n");
	sbuf_printf(sb, "triggered_count = %llu\n",
	    (unsigned long long)sc->rc_triggered_count);
	sbuf_printf(sb, "last_engine_type = %u\n", sc->rc_last_engine_type);
	sbuf_printf(sb, "last_chid = %u\n", sc->rc_last_chid);
	sbuf_printf(sb, "last_except_level = %u\n",
	    sc->rc_last_except_level);
	sbuf_printf(sb, "last_except_type = 0x%08x\n",
	    sc->rc_last_except_type);
	sbuf_printf(sb, "last_scope = %u\n", sc->rc_last_scope);
	sbuf_printf(sb, "last_mmu_fault_addr = 0x%016llx\n",
	    (unsigned long long)sc->rc_last_mmu_fault_addr);
	sbuf_printf(sb, "last_mmu_fault_type = 0x%08x\n",
	    sc->rc_last_mmu_fault_type);
	sbuf_printf(sb, "last_journal_size = %u\n",
	    sc->rc_last_journal_size);
	sbuf_printf(sb,
	    "fault_pte_snapshot va=0x%016llx pd2=%u pd1=%u pd0=%u spt=%u has_pt=%u pte=0x%016llx kind=0x%02x(%s) sparse_pte=0x%016llx\n",
	    (unsigned long long)sc->rc_fault_pte_va,
	    sc->rc_fault_pte_pd2_idx, sc->rc_fault_pte_pd1_idx,
	    sc->rc_fault_pte_pd0_idx, sc->rc_fault_pte_spt_idx,
	    sc->rc_fault_pte_has_pt,
	    (unsigned long long)sc->rc_fault_pte,
	    nvkm_debug_pte_kind(sc->rc_fault_pte),
	    nvkm_debug_pte_kind_name(
	    nvkm_debug_pte_kind(sc->rc_fault_pte)),
	    (unsigned long long)nvkm_pte_to_sparse());
	sbuf_printf(sb, "fault_pending_count = %u\n",
	    sc->rc_fault_pending_count);
	sbuf_printf(sb, "fault_binding_count = %u\n",
	    sc->rc_fault_binding_count);
	if (sc->rc_fault_binding_count != 0) {
		sbuf_printf(sb,
		    "fault_binding addr=0x%016llx size=0x%016llx grefcnt=%u\n",
		    (unsigned long long)sc->rc_fault_binding_addr,
		    (unsigned long long)sc->rc_fault_binding_size,
		    sc->rc_fault_binding_grefcnt);
	}
	if (sc->rc_fault_nearest_lo_size != 0) {
		sbuf_printf(sb,
		    "fault_binding_nearest_lo addr=0x%016llx size=0x%016llx delta=0x%016llx\n",
		    (unsigned long long)sc->rc_fault_nearest_lo_addr,
		    (unsigned long long)sc->rc_fault_nearest_lo_size,
		    (unsigned long long)(sc->rc_last_mmu_fault_addr -
		    (sc->rc_fault_nearest_lo_addr +
		    sc->rc_fault_nearest_lo_size)));
	}
	if (sc->rc_fault_nearest_hi_size != 0) {
		sbuf_printf(sb,
		    "fault_binding_nearest_hi addr=0x%016llx size=0x%016llx delta=0x%016llx\n",
		    (unsigned long long)sc->rc_fault_nearest_hi_addr,
		    (unsigned long long)sc->rc_fault_nearest_hi_size,
		    (unsigned long long)(sc->rc_fault_nearest_hi_addr -
		    sc->rc_last_mmu_fault_addr));
	}
	sbuf_printf(sb, "fault_push_scan_count = %u\n",
	    sc->rc_fault_push_scan_count);
	sbuf_printf(sb, "fault_push_hit_count = %u\n",
	    sc->rc_fault_push_hit_count);
	if (sc->rc_fault_push_hit_count != 0) {
		sbuf_printf(sb,
		    "fault_push_hit seq=%llu va=0x%016llx dword=%u\n",
		    (unsigned long long)sc->rc_fault_push_hit_seq,
		    (unsigned long long)sc->rc_fault_push_hit_va,
		    sc->rc_fault_push_hit_dword);
	}
	sbuf_printf(sb, "fault_data_scan_count = %u\n",
	    sc->rc_fault_data_scan_count);
	sbuf_printf(sb, "fault_data_hit_count = %u\n",
	    sc->rc_fault_data_hit_count);
	if (sc->rc_fault_data_hit_count != 0) {
		sbuf_printf(sb,
		    "fault_data_hit addr=0x%016llx size=0x%016llx offset=0x%016llx value=0x%016llx\n",
		    (unsigned long long)sc->rc_fault_data_hit_addr,
		    (unsigned long long)sc->rc_fault_data_hit_size,
		    (unsigned long long)sc->rc_fault_data_hit_offset,
		    (unsigned long long)sc->rc_fault_data_hit_value);
	}
	if (sc->rc_last_mmu_fault_addr != 0) {
		uint32_t matches = 0;

		for (uint32_t i = 0; i < NVKM_DRM_EXEC_TRACE_COUNT; i++) {
			const struct nvkm_drm_exec_trace *trace =
			    &sc->exec_trace[i];

			if (trace->seq == 0)
				continue;
			if (sc->rc_last_mmu_fault_addr < trace->va ||
			    sc->rc_last_mmu_fault_addr >=
			    trace->va + trace->va_len)
				continue;
			sbuf_printf(sb,
			    "fault_trace_match seq=%llu ch=%u chid=%u slot=%u gpf=%u push=%u/%u va=0x%016llx len=0x%08x done=%u err=%d\n",
			    (unsigned long long)trace->seq, trace->channel,
			    trace->chid, trace->post_slot, trace->gpf_index,
			    trace->push_index, trace->push_count,
			    (unsigned long long)trace->va, trace->va_len,
			    trace->completed, trace->error);
			matches++;
		}
		sbuf_printf(sb, "fault_trace_matches = %u\n", matches);
	}

	sbuf_cat(sb, "\nvm_trace\n");
	sbuf_printf(sb, "next = %u\n", sc->vm_trace_next);
	sbuf_printf(sb, "seq = %llu\n",
	    (unsigned long long)sc->vm_trace_seq);
	if (sc->rc_last_mmu_fault_addr != 0) {
		struct nvkm_gsp_vmm_pte_info pte_info;
		const struct nvkm_drm_vm_trace *nearest_lo = NULL;
		const struct nvkm_drm_vm_trace *nearest_hi = NULL;
		uint64_t context_seq = 0;
		uint32_t matches = 0;
		uint32_t shown = 0;

		nvkm_gsp_vmm_read_pte(sc->gsp_vmm,
		    sc->rc_last_mmu_fault_addr, &pte_info);
		sbuf_printf(sb,
		    "fault_pte va=0x%016llx pd2=%u pd1=%u pd0=%u spt=%u has_pt=%u pte=0x%016llx kind=0x%02x(%s) sparse_pte=0x%016llx\n",
		    (unsigned long long)pte_info.va, pte_info.pd2_idx,
		    pte_info.pd1_idx, pte_info.pd0_idx, pte_info.spt_idx,
		    pte_info.has_pt, (unsigned long long)pte_info.pte,
		    nvkm_debug_pte_kind(pte_info.pte),
		    nvkm_debug_pte_kind_name(nvkm_debug_pte_kind(pte_info.pte)),
		    (unsigned long long)nvkm_pte_to_sparse());

		for (uint32_t i = 0; i < NVKM_DRM_VM_TRACE_COUNT; i++) {
			const struct nvkm_drm_vm_trace *trace =
			    &sc->vm_trace[i];

			if (trace->seq == 0)
				continue;
			if (trace->addr <= sc->rc_last_mmu_fault_addr &&
			    (nearest_lo == NULL ||
			     trace->addr > nearest_lo->addr))
				nearest_lo = trace;
			if (trace->addr > sc->rc_last_mmu_fault_addr &&
			    (nearest_hi == NULL ||
			     trace->addr < nearest_hi->addr))
				nearest_hi = trace;
			if (sc->rc_last_mmu_fault_addr < trace->addr ||
			    sc->rc_last_mmu_fault_addr >=
			    trace->addr + trace->range)
				continue;
			matches++;
			if (trace->seq > context_seq)
				context_seq = trace->seq;
		}
		sbuf_printf(sb, "fault_vm_matches = %u\n", matches);
		if (nearest_lo != NULL) {
			sbuf_printf(sb,
			    "fault_vm_nearest_lo seq=%llu action=%u addr=0x%016llx range=0x%016llx delta=0x%016llx\n",
			    (unsigned long long)nearest_lo->seq,
			    nearest_lo->action,
			    (unsigned long long)nearest_lo->addr,
			    (unsigned long long)nearest_lo->range,
			    (unsigned long long)(sc->rc_last_mmu_fault_addr -
			    nearest_lo->addr));
		}
		if (nearest_hi != NULL) {
			sbuf_printf(sb,
			    "fault_vm_nearest_hi seq=%llu action=%u addr=0x%016llx range=0x%016llx delta=0x%016llx\n",
			    (unsigned long long)nearest_hi->seq,
			    nearest_hi->action,
			    (unsigned long long)nearest_hi->addr,
			    (unsigned long long)nearest_hi->range,
			    (unsigned long long)(nearest_hi->addr -
			    sc->rc_last_mmu_fault_addr));
		}
		for (uint32_t n = 0; n < NVKM_DRM_VM_TRACE_COUNT &&
		    shown < 16; n++) {
			const struct nvkm_drm_vm_trace *trace;
			uint32_t idx;

			idx = (sc->vm_trace_next + NVKM_DRM_VM_TRACE_COUNT -
			    1 - n) % NVKM_DRM_VM_TRACE_COUNT;
			trace = &sc->vm_trace[idx];
			if (trace->seq == 0)
				continue;
			if (sc->rc_last_mmu_fault_addr < trace->addr ||
			    sc->rc_last_mmu_fault_addr >=
			    trace->addr + trace->range)
				continue;
			sbuf_printf(sb,
			    "fault_vm_match seq=%llu action=%u flags=0x%08x pte_kind=0x%02x(%s) handle=%u addr=0x%016llx range=0x%016llx bo_off=0x%016llx obj=0x%jx domain=0x%x tile_mode=0x%08x tile_flags=0x%08x paddr=0x%016llx size=0x%016llx cpu=%u err=%d\n",
			    (unsigned long long)trace->seq, trace->action,
			    trace->flags, trace->pte_kind,
			    nvkm_debug_pte_kind_name(trace->pte_kind),
			    trace->handle,
			    (unsigned long long)trace->addr,
			    (unsigned long long)trace->range,
			    (unsigned long long)trace->bo_offset,
			    (uintmax_t)trace->obj, trace->bo_domain,
			    trace->bo_tile_mode, trace->bo_tile_flags,
			    (unsigned long long)trace->bo_paddr,
			    (unsigned long long)trace->bo_size,
			    trace->cpu_mapped, trace->error);
			shown++;
		}
		if (context_seq != 0) {
			uint64_t first_seq;

			first_seq = context_seq > 4 ? context_seq - 4 : 1;
			sbuf_cat(sb, "fault_vm_context\n");
			for (uint32_t i = 0; i < NVKM_DRM_VM_TRACE_COUNT; i++) {
				const struct nvkm_drm_vm_trace *trace =
				    &sc->vm_trace[i];

				if (trace->seq < first_seq ||
				    trace->seq > context_seq + 4)
					continue;
				sbuf_printf(sb,
				    "vm seq=%llu action=%u flags=0x%08x pte_kind=0x%02x(%s) handle=%u addr=0x%016llx range=0x%016llx tile_mode=0x%08x tile_flags=0x%08x err=%d\n",
				    (unsigned long long)trace->seq, trace->action,
				    trace->flags, trace->pte_kind,
				    nvkm_debug_pte_kind_name(trace->pte_kind),
				    trace->handle,
				    (unsigned long long)trace->addr,
				    (unsigned long long)trace->range,
				    trace->bo_tile_mode, trace->bo_tile_flags,
				    trace->error);
			}
		}
	}

	static const char *const bo_size_bucket_names[NVKM_BO_SIZE_BUCKET_COUNT] = {
		"le_4k",
		"le_16k",
		"le_64k",
		"le_256k",
		"le_1m",
		"le_4m",
		"gt_4m",
	};

	sbuf_cat(sb, "\nbo\n");
	sbuf_printf(sb, "gem_new_count = %llu\n",
	    (unsigned long long)sc->bo_gem_new_count);
	sbuf_printf(sb, "gem_new_vram_count = %llu\n",
	    (unsigned long long)sc->bo_gem_new_vram_count);
	sbuf_printf(sb, "gem_new_gart_count = %llu\n",
	    (unsigned long long)sc->bo_gem_new_gart_count);
	sbuf_printf(sb, "gem_new_mappable_req_count = %llu\n",
	    (unsigned long long)sc->bo_gem_new_mappable_req_count);
	sbuf_printf(sb, "gem_new_mappable_vram_count = %llu\n",
	    (unsigned long long)sc->bo_gem_new_mappable_vram_count);
	sbuf_printf(sb, "gem_new_mappable_gart_count = %llu\n",
	    (unsigned long long)sc->bo_gem_new_mappable_gart_count);
		sbuf_printf(sb, "gem_new_map_handle_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_map_handle_count);
		sbuf_printf(sb, "gem_new_req_cpu_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_req_cpu_count);
		sbuf_printf(sb, "gem_new_req_vram_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_req_vram_count);
		sbuf_printf(sb, "gem_new_req_gart_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_req_gart_count);
		sbuf_printf(sb, "gem_new_req_vram_gart_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_req_vram_gart_count);
		sbuf_printf(sb, "gem_new_req_no_domain_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_req_no_domain_count);
		sbuf_printf(sb, "gem_new_req_coherent_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_req_coherent_count);
		sbuf_printf(sb, "gem_new_req_no_share_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_req_no_share_count);
		sbuf_printf(sb, "gem_new_req_tiled_count = %llu\n",
		    (unsigned long long)sc->bo_gem_new_req_tiled_count);
		for (uint32_t i = 0; i < NVKM_BO_SIZE_BUCKET_COUNT; i++) {
			sbuf_printf(sb, "gem_new_size_%s = %llu\n",
			    bo_size_bucket_names[i],
			    (unsigned long long)sc->bo_gem_new_size_bucket[i]);
		sbuf_printf(sb, "gem_new_gart_size_%s = %llu\n",
		    bo_size_bucket_names[i],
		    (unsigned long long)sc->bo_gem_new_gart_size_bucket[i]);
			sbuf_printf(sb, "gem_new_mappable_size_%s = %llu\n",
			    bo_size_bucket_names[i],
			    (unsigned long long)sc->bo_gem_new_mappable_size_bucket[i]);
		}
		sbuf_printf(sb, "gem_new_trace_seq = %llu\n",
		    (unsigned long long)sc->bo_gem_new_trace_seq);
		{
			uint64_t seq = sc->bo_gem_new_trace_seq;
			uint32_t count = seq < NVKM_BO_GEM_NEW_TRACE_COUNT ?
			    (uint32_t)seq : NVKM_BO_GEM_NEW_TRACE_COUNT;

			for (uint32_t i = 0; i < count; i++) {
				const struct nvkm_bo_gem_new_trace *trace;
				uint32_t idx = (uint32_t)((seq - count + i) %
				    NVKM_BO_GEM_NEW_TRACE_COUNT);

				trace = &sc->bo_gem_new_trace[idx];
				if (trace->seq == 0)
					continue;
				sbuf_printf(sb,
				    "gem_new_trace[%02u] seq=%llu pid=%u comm=%s "
				    "handle=%u req_domain=0x%08x domain=0x%08x "
				    "req_size=0x%llx size=0x%llx map_handle=0x%016llx "
				    "tile_mode=0x%08x tile_flags=0x%08x "
				    "mappable_req=%u cpu_mappable=%u\n",
				    i, (unsigned long long)trace->seq,
				    trace->pid, trace->comm, trace->handle,
				    trace->req_domain, trace->domain,
				    (unsigned long long)trace->req_size,
				    (unsigned long long)trace->size,
				    (unsigned long long)trace->map_handle,
				    trace->tile_mode,
				    trace->tile_flags, trace->mappable_req,
				    trace->cpu_mappable);
			}
		}
		sbuf_printf(sb, "gem_free_count = %llu\n",
		    (unsigned long long)sc->bo_gem_free_count);
	sbuf_printf(sb, "dumb_create_count = %llu\n",
	    (unsigned long long)sc->bo_dumb_create_count);
	sbuf_printf(sb, "dumb_create_vram_count = %llu\n",
	    (unsigned long long)sc->bo_dumb_create_vram_count);
	sbuf_printf(sb, "dumb_create_gart_count = %llu\n",
	    (unsigned long long)sc->bo_dumb_create_gart_count);
	sbuf_printf(sb, "bar1_fault_count = %llu\n",
	    (unsigned long long)sc->bo_bar1_fault_count);
	sbuf_printf(sb, "bar1_fault_map_count = %llu\n",
	    (unsigned long long)sc->bo_bar1_fault_map_count);
	sbuf_printf(sb, "bar1_fault_error_count = %llu\n",
	    (unsigned long long)sc->bo_bar1_fault_error_count);
	sbuf_printf(sb, "bar1_map_count = %llu\n",
	    (unsigned long long)sc->bo_bar1_map_count);
	sbuf_printf(sb, "bar1_map_error_count = %llu\n",
	    (unsigned long long)sc->bo_bar1_map_error_count);
	sbuf_printf(sb, "bar1_unmap_count = %llu\n",
	    (unsigned long long)sc->bo_bar1_unmap_count);
	sbuf_printf(sb, "sysmem_active_count = %llu\n",
	    (unsigned long long)sc->bo_sysmem_active_count);
	sbuf_printf(sb, "sysmem_active_bytes = 0x%016llx\n",
	    (unsigned long long)sc->bo_sysmem_active_bytes);
	sbuf_printf(sb, "sysmem_high_bytes = 0x%016llx\n",
	    (unsigned long long)sc->bo_sysmem_high_bytes);
	sbuf_printf(sb, "vram_active_count = %llu\n",
	    (unsigned long long)sc->bo_vram_active_count);
	sbuf_printf(sb, "vram_active_bytes = 0x%016llx\n",
	    (unsigned long long)sc->bo_vram_active_bytes);
	sbuf_printf(sb, "vram_high_bytes = 0x%016llx\n",
	    (unsigned long long)sc->bo_vram_high_bytes);
	sbuf_printf(sb, "alloc_fail_count = %llu\n",
	    (unsigned long long)sc->bo_alloc_fail_count);
	sbuf_printf(sb, "alloc_fail_path = %u\n",
	    sc->bo_alloc_fail_path);
	sbuf_printf(sb, "alloc_fail_error = %d\n",
	    sc->bo_alloc_fail_error);
	sbuf_printf(sb, "alloc_fail_domain = 0x%08x\n",
	    sc->bo_alloc_fail_domain);
	sbuf_printf(sb, "alloc_fail_size = 0x%016llx\n",
	    (unsigned long long)sc->bo_alloc_fail_size);

	sbuf_cat(sb, "\nreservation\n");
	sbuf_printf(sb, "bo_wait_count = %llu\n",
	    (unsigned long long)sc->bo_resv_wait_count);
	sbuf_printf(sb, "bo_wait_error_count = %llu\n",
	    (unsigned long long)sc->bo_resv_wait_error_count);
	sbuf_printf(sb, "vm_init_kernel_addr = 0x%016llx\n",
	    (unsigned long long)sc->vm_init_kernel_addr);
	sbuf_printf(sb, "vm_init_kernel_size = 0x%016llx\n",
	    (unsigned long long)sc->vm_init_kernel_size);
	sbuf_printf(sb, "vm_bind_ioctl_count = %llu\n",
	    (unsigned long long)sc->vm_bind_ioctl_count);
	sbuf_printf(sb, "vm_bind_op_count = %llu\n",
	    (unsigned long long)sc->vm_bind_op_count);
	sbuf_printf(sb, "vm_bind_max_op_count = %u\n",
	    sc->vm_bind_max_op_count);
	sbuf_printf(sb, "vm_bind_async_count = %llu\n",
	    (unsigned long long)sc->vm_bind_async_count);
	sbuf_printf(sb, "vm_bind_sync_count = %llu\n",
	    (unsigned long long)sc->vm_bind_sync_count);
	sbuf_printf(sb, "vm_bind_fast_count = %llu\n",
	    (unsigned long long)sc->vm_bind_fast_count);
	sbuf_printf(sb, "vm_bind_fast_error_count = %llu\n",
	    (unsigned long long)sc->vm_bind_fast_error_count);
	sbuf_printf(sb, "vm_bind_wait_count = %llu\n",
	    (unsigned long long)sc->vm_bind_wait_count);
	sbuf_printf(sb, "vm_bind_wait_error_count = %llu\n",
	    (unsigned long long)sc->vm_bind_wait_error_count);
	sbuf_printf(sb, "vm_bind_error_count = %llu\n",
	    (unsigned long long)sc->vm_bind_error_count);
	sbuf_printf(sb, "vm_bind_busy_count = %llu\n",
	    (unsigned long long)sc->vm_bind_busy_count);
	sbuf_printf(sb, "vm_bind_last_error = %d\n",
	    sc->vm_bind_last_error);
	sbuf_printf(sb, "vm_bind_last_op = %u\n",
	    sc->vm_bind_last_op);
	sbuf_printf(sb, "vm_bind_last_flags = 0x%08x\n",
	    sc->vm_bind_last_flags);
	sbuf_printf(sb, "vm_bind_last_handle = %u\n",
	    sc->vm_bind_last_handle);
	sbuf_printf(sb, "vm_bind_last_addr = 0x%016llx\n",
	    (unsigned long long)sc->vm_bind_last_addr);
	sbuf_printf(sb, "vm_bind_last_range = 0x%016llx\n",
	    (unsigned long long)sc->vm_bind_last_range);
	sbuf_printf(sb, "vm_bind_last_bo_offset = 0x%016llx\n",
	    (unsigned long long)sc->vm_bind_last_bo_offset);
	sbuf_printf(sb, "vm_bind_busy_state = %u\n",
	    sc->vm_bind_busy_state);
	sbuf_printf(sb, "vm_bind_busy_refs = %u\n",
	    sc->vm_bind_busy_refs);
	sbuf_printf(sb, "vm_bind_busy_exec_refs = %u\n",
	    sc->vm_bind_busy_exec_refs);
	sbuf_printf(sb, "vm_bind_busy_addr = 0x%016llx\n",
	    (unsigned long long)sc->vm_bind_busy_addr);
	sbuf_printf(sb, "vm_bind_busy_size = 0x%016llx\n",
	    (unsigned long long)sc->vm_bind_busy_size);
	sbuf_printf(sb, "vm_bind_empty_clear_skip_count = %llu\n",
	    (unsigned long long)sc->vm_bind_empty_clear_skip_count);
	sbuf_printf(sb, "vm_bind_empty_clear_skip_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_empty_clear_skip_pages);
	sbuf_printf(sb, "vm_bind_replace_clear_skip_count = %llu\n",
	    (unsigned long long)sc->vm_bind_replace_clear_skip_count);
	sbuf_printf(sb, "vm_bind_replace_clear_skip_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_replace_clear_skip_pages);
	sbuf_printf(sb, "vm_bind_map_count = %llu\n",
	    (unsigned long long)sc->vm_bind_map_count);
	sbuf_printf(sb, "vm_bind_map_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_map_pages);
	sbuf_printf(sb, "vm_bind_unmap_count = %llu\n",
	    (unsigned long long)sc->vm_bind_unmap_count);
	sbuf_printf(sb, "vm_bind_unmap_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_unmap_pages);
	sbuf_printf(sb, "vm_bind_map_null_count = %llu\n",
	    (unsigned long long)sc->vm_bind_map_null_count);
	sbuf_printf(sb, "vm_bind_map_null_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_map_null_pages);
	sbuf_printf(sb, "vm_bind_map_sparse_count = %llu\n",
	    (unsigned long long)sc->vm_bind_map_sparse_count);
	sbuf_printf(sb, "vm_bind_map_sparse_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_map_sparse_pages);
	sbuf_printf(sb, "vm_bind_unmap_sparse_count = %llu\n",
	    (unsigned long long)sc->vm_bind_unmap_sparse_count);
	sbuf_printf(sb, "vm_bind_unmap_sparse_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_unmap_sparse_pages);
	sbuf_printf(sb, "vm_bind_clear_unmap_count = %llu\n",
	    (unsigned long long)sc->vm_bind_clear_unmap_count);
	sbuf_printf(sb, "vm_bind_clear_unmap_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_clear_unmap_pages);
	sbuf_printf(sb, "vm_bind_clear_map_null_count = %llu\n",
	    (unsigned long long)sc->vm_bind_clear_map_null_count);
	sbuf_printf(sb, "vm_bind_clear_map_null_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_clear_map_null_pages);
	sbuf_printf(sb, "vm_bind_clear_map_sparse_count = %llu\n",
	    (unsigned long long)sc->vm_bind_clear_map_sparse_count);
	sbuf_printf(sb, "vm_bind_clear_map_sparse_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_clear_map_sparse_pages);
	sbuf_printf(sb, "vm_bind_clear_unmap_sparse_count = %llu\n",
	    (unsigned long long)sc->vm_bind_clear_unmap_sparse_count);
	sbuf_printf(sb, "vm_bind_clear_unmap_sparse_pages = %llu\n",
	    (unsigned long long)sc->vm_bind_clear_unmap_sparse_pages);

	sbuf_cat(sb, "\nvm_bind_pte_kind\n");
	for (uint32_t i = 0; i < NVKM_DRM_VM_BIND_PTE_KIND_COUNT; i++) {
		if (sc->vm_bind_map_kind_count[i] == 0 &&
		    sc->vm_bind_clear_kind_count[i] == 0)
			continue;
		sbuf_printf(sb,
		    "kind[0x%02x] map=%llu map_pages=%llu clear=%llu clear_pages=%llu\n",
		    i,
		    (unsigned long long)sc->vm_bind_map_kind_count[i],
		    (unsigned long long)sc->vm_bind_map_kind_pages[i],
		    (unsigned long long)sc->vm_bind_clear_kind_count[i],
		    (unsigned long long)sc->vm_bind_clear_kind_pages[i]);
	}

	sbuf_cat(sb, "\nvm_bind_size_pages\n");
	for (uint32_t i = 0; i < NVKM_DRM_VM_BIND_SIZE_BUCKET_COUNT; i++) {
		uint64_t min_pages;
		uint64_t max_pages;

		if (sc->vm_bind_map_size_count[i] == 0 &&
		    sc->vm_bind_clear_size_count[i] == 0)
			continue;
		min_pages = i == 0 ? 1 : (1ULL << (i - 1)) + 1;
		max_pages = 1ULL << i;
		if (i + 1 == NVKM_DRM_VM_BIND_SIZE_BUCKET_COUNT) {
			sbuf_printf(sb,
			    "bucket[%02u] pages>=%llu map=%llu map_pages=%llu clear=%llu clear_pages=%llu\n",
			    i, (unsigned long long)min_pages,
			    (unsigned long long)sc->vm_bind_map_size_count[i],
			    (unsigned long long)sc->vm_bind_map_size_pages[i],
			    (unsigned long long)sc->vm_bind_clear_size_count[i],
			    (unsigned long long)sc->vm_bind_clear_size_pages[i]);
		} else {
			sbuf_printf(sb,
			    "bucket[%02u] pages=%llu-%llu map=%llu map_pages=%llu clear=%llu clear_pages=%llu\n",
			    i, (unsigned long long)min_pages,
			    (unsigned long long)max_pages,
			    (unsigned long long)sc->vm_bind_map_size_count[i],
			    (unsigned long long)sc->vm_bind_map_size_pages[i],
			    (unsigned long long)sc->vm_bind_clear_size_count[i],
			    (unsigned long long)sc->vm_bind_clear_size_pages[i]);
		}
	}

	sbuf_cat(sb, "\nvm_bind_profile_us\n");
	sbuf_printf(sb, "wait_us = %llu\n",
	    (unsigned long long)sc->vm_bind_profile_wait_us);
	sbuf_printf(sb, "copyin_us = %llu\n",
	    (unsigned long long)sc->vm_bind_profile_copyin_us);
	sbuf_printf(sb, "token_wait_us = %llu\n",
	    (unsigned long long)sc->vm_bind_profile_token_wait_us);
	sbuf_printf(sb, "apply_us = %llu\n",
	    (unsigned long long)sc->vm_bind_profile_apply_us);
	sbuf_printf(sb, "flush_us = %llu\n",
	    (unsigned long long)sc->vm_bind_profile_flush_us);
	sbuf_printf(sb, "signal_us = %llu\n",
	    (unsigned long long)sc->vm_bind_profile_signal_us);
	sbuf_printf(sb, "total_us = %llu\n",
	    (unsigned long long)sc->vm_bind_profile_total_us);

	sbuf_cat(sb, "\nprime\n");
	sbuf_printf(sb, "prime_handle_to_fd_count = %llu\n",
	    (unsigned long long)sc->prime_handle_to_fd_count);
	sbuf_printf(sb, "prime_handle_to_fd_error_count = %llu\n",
	    (unsigned long long)sc->prime_handle_to_fd_error_count);
	sbuf_printf(sb, "prime_fd_to_handle_count = %llu\n",
	    (unsigned long long)sc->prime_fd_to_handle_count);
	sbuf_printf(sb, "prime_fd_to_handle_error_count = %llu\n",
	    (unsigned long long)sc->prime_fd_to_handle_error_count);
	sbuf_printf(sb, "prime_dma_buf_export_count = %llu\n",
	    (unsigned long long)sc->prime_dma_buf_export_count);
	sbuf_printf(sb, "cpu_prep_wait_count = %llu\n",
	    (unsigned long long)sc->cpu_prep_wait_count);
	sbuf_printf(sb, "cpu_prep_wait_error_count = %llu\n",
	    (unsigned long long)sc->cpu_prep_wait_error_count);
	sbuf_printf(sb, "cpu_fini_count = %llu\n",
	    (unsigned long long)sc->cpu_fini_count);
	sbuf_printf(sb, "cpu_fini_flush_count = %llu\n",
	    (unsigned long long)sc->cpu_fini_flush_count);
	sbuf_printf(sb, "cpu_fini_flush_us = %llu\n",
	    (unsigned long long)sc->cpu_fini_flush_us);

	{
		struct nvkm_hotproc_slot hotproc[NVKM_HOTPROC_SLOT_COUNT];

		memset(hotproc, 0, sizeof(hotproc));
		nvkm_hotproc_snapshot(sc, hotproc, NVKM_HOTPROC_SLOT_COUNT);
		sbuf_cat(sb, "\nhotproc\n");
		for (uint32_t i = 0; i < NVKM_HOTPROC_SLOT_COUNT; i++) {
			const struct nvkm_hotproc_slot *slot = &hotproc[i];

			if (!slot->active)
				continue;
			sbuf_printf(sb,
			    "hotproc[%02u] pid=%d comm=%s vm_bind_ioctl=%llu vm_bind_ops=%llu vm_bind_sync=%llu vm_bind_async=%llu vm_bind_waits=%llu vm_bind_sigs=%llu vm_bind_map=%llu vm_bind_map_pages=%llu vm_bind_unmap=%llu vm_bind_unmap_pages=%llu vm_bind_sparse=%llu vm_bind_other=%llu vm_bind_max_pages=%llu prime_handle_to_fd=%llu prime_repeat=%llu prime_seen=%llu prime_overflow=%llu gem_new=%llu\n",
			    i, slot->pid, slot->comm,
			    (unsigned long long)slot->vm_bind_ioctl_count,
			    (unsigned long long)slot->vm_bind_op_count,
			    (unsigned long long)slot->vm_bind_sync_count,
			    (unsigned long long)slot->vm_bind_async_count,
			    (unsigned long long)slot->vm_bind_wait_count,
			    (unsigned long long)slot->vm_bind_sig_count,
			    (unsigned long long)slot->vm_bind_map_count,
			    (unsigned long long)slot->vm_bind_map_pages,
			    (unsigned long long)slot->vm_bind_unmap_count,
			    (unsigned long long)slot->vm_bind_unmap_pages,
			    (unsigned long long)slot->vm_bind_sparse_count,
			    (unsigned long long)slot->vm_bind_other_count,
			    (unsigned long long)slot->vm_bind_max_pages,
			    (unsigned long long)slot->prime_handle_to_fd_count,
			    (unsigned long long)slot->prime_handle_repeat_count,
			    (unsigned long long)slot->prime_handle_seen_count,
			    (unsigned long long)slot->prime_handle_overflow_count,
			    (unsigned long long)slot->gem_new_count);
		}
	}

	sbuf_cat(sb, "\nbar1\n");
	sbuf_printf(sb, "gva_used = %u\n", bar1_used);
	sbuf_printf(sb, "gva_total = %u\n", bar1_total);
	sbuf_printf(sb, "gva_free = %u\n", bar1_total - bar1_used);

	sbuf_cat(sb, "\nvmm\n");
	sbuf_printf(sb, "flush_count = %llu\n",
	    (unsigned long long)sc->vmm_flush_count);
	sbuf_printf(sb, "flush_us = %llu\n",
	    (unsigned long long)sc->vmm_flush_us);
	sbuf_printf(sb, "pte_fast_write_count = %llu\n",
	    (unsigned long long)sc->vmm_pte_fast_write_count);
	sbuf_printf(sb, "pte_bulk_write_count = %llu\n",
	    (unsigned long long)sc->vmm_pte_bulk_write_count);
	sbuf_printf(sb, "pte_bulk_write_pages = %llu\n",
	    (unsigned long long)sc->vmm_pte_bulk_write_pages);
	sbuf_printf(sb, "pte_fast_clear_count = %llu\n",
	    (unsigned long long)sc->vmm_pte_fast_clear_count);
	sbuf_printf(sb, "pte_fast_invalid_clear_count = %llu\n",
	    (unsigned long long)sc->vmm_pte_fast_invalid_clear_count);
	sbuf_printf(sb, "pte_fast_sparse_clear_count = %llu\n",
	    (unsigned long long)sc->vmm_pte_fast_sparse_clear_count);
	sbuf_printf(sb, "pte_bulk_clear_count = %llu\n",
	    (unsigned long long)sc->vmm_pte_bulk_clear_count);
	sbuf_printf(sb, "pte_bulk_clear_pages = %llu\n",
	    (unsigned long long)sc->vmm_pte_bulk_clear_pages);
	sbuf_printf(sb, "pte_read_modify_write_count = %llu\n",
	    (unsigned long long)sc->vmm_pte_read_modify_write_count);
	sbuf_printf(sb, "user_pd0_count = %u\n", vmm_pd0_count);
	sbuf_printf(sb, "user_pt_count = %u\n", vmm_pt_count);
	sbuf_printf(sb, "valid_pte_count = %llu\n",
	    (unsigned long long)valid_pte_count);
	sbuf_printf(sb, "sparse_region_count = %u\n", sparse_region_count);

	err = sbuf_finish(sb);
	if (err != 0) {
		sbuf_delete(sb);
		return (err);
	}
	err = SYSCTL_OUT(req, sbuf_data(sb), sbuf_len(sb) + 1);
	sbuf_delete(sb);
	return (err);
}

static int
nvkm_gsp_sysctl_vm_trace(SYSCTL_HANDLER_ARGS)
{
	struct nvkm_softc *sc = arg1;
	struct sbuf *sb;
	uint32_t end, start;
	int err;

	sb = sbuf_new_auto();
	if (sb == NULL)
		return (ENOMEM);
	sbuf_printf(sb, "next = %u\n", sc->vm_trace_next);
	sbuf_printf(sb, "seq = %llu\n",
	    (unsigned long long)sc->vm_trace_seq);

	end = sc->vm_trace_next;
	start = end > NVKM_DRM_VM_TRACE_COUNT ?
	    end - NVKM_DRM_VM_TRACE_COUNT : 0;
	for (uint32_t pos = start; pos < end; pos++) {
		const struct nvkm_drm_vm_trace *trace;

		trace = &sc->vm_trace[pos % NVKM_DRM_VM_TRACE_COUNT];
		if (trace->seq == 0)
			continue;
		sbuf_printf(sb,
		    "trace[%u] seq=%llu action=%u flags=0x%08x pte_kind=0x%02x(%s) handle=%u addr=0x%016llx range=0x%016llx bo_off=0x%016llx obj=0x%jx domain=0x%x tile_mode=0x%08x tile_flags=0x%08x paddr=0x%016llx size=0x%016llx cpu=%u err=%d\n",
		    pos, (unsigned long long)trace->seq, trace->action,
		    trace->flags, trace->pte_kind,
		    nvkm_debug_pte_kind_name(trace->pte_kind),
		    trace->handle,
		    (unsigned long long)trace->addr,
		    (unsigned long long)trace->range,
		    (unsigned long long)trace->bo_offset,
		    (uintmax_t)trace->obj, trace->bo_domain,
		    trace->bo_tile_mode, trace->bo_tile_flags,
		    (unsigned long long)trace->bo_paddr,
		    (unsigned long long)trace->bo_size, trace->cpu_mapped,
		    trace->error);
	}

	err = sbuf_finish(sb);
	if (err != 0) {
		sbuf_delete(sb);
		return (err);
	}
	err = SYSCTL_OUT(req, sbuf_data(sb), sbuf_len(sb) + 1);
	sbuf_delete(sb);
	return (err);
}

/* Dump the GSP RPC ring trace (host<->GSP RPC TX/RX/EVENT/STALE), newest at
 * the bottom. Pairs with dev.drm.0.gsp_rpc_trace_on to gate recording. */
static int
nvkm_gsp_sysctl_rpc_trace(SYSCTL_HANDLER_ARGS)
{
	struct nvkm_softc *sc = arg1;
	static const char * const dirs[4] = { "TX   ", "RX   ", "EVENT", "STALE" };
	struct sbuf *sb;
	uint32_t head, total, show, k, i;
	int err;

	sb = sbuf_new_auto();
	if (sb == NULL)
		return (ENOMEM);
	head = sc->gsp_rpc_trace_head;
	total = (head < NVKM_GSP_RPC_TRACE_N) ? head : NVKM_GSP_RPC_TRACE_N;
	show = (total < 180u) ? total : 180u;
	sbuf_printf(sb, "gsp_rpc_trace on=%d head=%u showing last %u of %u\n",
	    sc->gsp_rpc_trace_on, head, show, total);
	for (k = 0; k < show; k++) {
		struct nvkm_gsp_rpc_trace_ent *e;
		i = (head - show + k) % NVKM_GSP_RPC_TRACE_N;
		e = &sc->gsp_rpc_trace[i];
		sbuf_printf(sb,
		    "%4u %s t=%llu fn=%-5u seq=%-6u aux=0x%x aux2=0x%x dt=%uus\n",
		    k, dirs[e->dir & 3u],
		    (unsigned long long)e->time_us, e->fn, e->seq,
		    e->aux, e->aux2, e->latency_us);
	}
	err = sbuf_finish(sb);
	if (err == 0)
		err = SYSCTL_OUT(req, sbuf_data(sb), sbuf_len(sb) + 1);
	sbuf_delete(sb);
	return (err);
}

static int
nvkm_gsp_sysctl_perf_state(SYSCTL_HANDLER_ARGS)
{
	struct nvkm_softc *sc = arg1;
	struct sbuf *sb;
	uint32_t current_pstate;
	const char *name;
	int err, pstate_err;

	sb = sbuf_new_auto();
	if (sb == NULL)
		return (ENOMEM);

	pstate_err = nvkm_gsp_query_perf_current_pstate(sc, &current_pstate);

	if (pstate_err != 0) {
		sbuf_printf(sb, "current_pstate_error = %d\n", pstate_err);
		goto out;
	}

	switch (current_pstate) {
	case NV2080_CTRL_PERF_PSTATES_P0:
		name = "P0";
		break;
	case NV2080_CTRL_PERF_PSTATES_P8:
		name = "P8";
		break;
	case NV2080_CTRL_PERF_PSTATES_UNDEFINED:
		name = "undefined";
		break;
	default:
		name = "unknown";
		break;
	}

	sbuf_printf(sb, "current_pstate = 0x%08x\n", current_pstate);
	sbuf_printf(sb, "current_pstate_name = %s\n", name);

out:
	err = sbuf_finish(sb);
	if (err == 0)
		err = SYSCTL_OUT(req, sbuf_data(sb), sbuf_len(sb) + 1);
	sbuf_delete(sb);
	return (err);
}

/* Provided by nvkm_drm_kms.c. */
int nvkm_drm_kms_light_up(struct nvkm_softc *sc);

/* Write 1 to drive a driver-internal atomic modeset (light up the screen). */
static int
nvkm_gsp_sysctl_kms_lightup(SYSCTL_HANDLER_ARGS)
{
	struct nvkm_softc *sc = arg1;
	int val = 0, err;

	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == 0)
		return (err);
	if (val != 0)
		(void)nvkm_drm_kms_light_up(sc);
	return (0);
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
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "vm_trace",
	    CTLTYPE_STRING | CTLFLAG_RD, sc, 0,
	    nvkm_gsp_sysctl_vm_trace, "A",
	    "nouveau VM_BIND trace ring");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "kms_push_trace",
	    CTLFLAG_RW, &sc->kms_push_trace, 0,
	    "Enable verbose dispnv50 DMAC push logging");
	sc->vma_tilemode = 1;	/* per-VMA PTE kind; 0 = legacy BO tiling */
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "vma_tilemode",
	    CTLFLAG_RW, &sc->vma_tilemode, 0,
	    "Report HAS_VMA_TILEMODE to userspace (kill switch for tiled "
	    "rendering debug; takes effect at vulkan device open)");
	sc->sync_diag_enable = 0;
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "sync_diag_enable",
	    CTLFLAG_RW, &sc->sync_diag_enable, 0,
	    "Enable nvkm sync/submit active-call diagnostics");
	sc->gsp_rpc_trace_on = 0;	/* opt-in; GSP events are X11 hot path. */
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "gsp_rpc_trace_on",
	    CTLFLAG_RW, &sc->gsp_rpc_trace_on, 0,
	    "Enable GSP RPC ring trace recording");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "gsp_rpc_trace",
	    CTLTYPE_STRING | CTLFLAG_RD, sc, 0,
	    nvkm_gsp_sysctl_rpc_trace, "A",
	    "GSP RPC ring trace (host<->GSP TX/RX/EVENT/STALE)");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "perf_state",
	    CTLTYPE_STRING | CTLFLAG_RD, sc, 0,
	    nvkm_gsp_sysctl_perf_state, "A",
	    "GSP RM current performance pstate");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "kms_lightup",
	    CTLTYPE_INT | CTLFLAG_RW, sc, 0,
	    nvkm_gsp_sysctl_kms_lightup, "I",
	    "write 1 to drive an internal atomic modeset (light up the screen)");
}
