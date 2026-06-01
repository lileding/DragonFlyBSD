/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * PCI bus attachment for nvkm.
 *
 * Phase 0.0: identify the GPU, map its BARs, and confirm MMIO works by
 * reading PMC_BOOT_0 (the chip identification register).
 *
 * Phase 0.1: read VBIOS via the BAR0 PROM aperture. See nvkm_bios.c.
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include "nvkm_gsp_vmm.h"
#include <linux/dma-fence.h>
#include "nvkm_falcon.h"

#include <drm/drmP.h>            /* struct drm_softc, kzalloc, GFP_KERNEL */

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>
#include <sys/sysctl.h>
#include <sys/kthread.h>

#define NVKM_GSP_DEBUG_NOCAT	0
#define NVKM_RUN_SUBMIT_TEST	0

struct nvkm_pci_id {
	uint16_t	device;
	const char	*name;
};

static const struct nvkm_pci_id nvkm_pci_ids[] = {
	{ NVKM_PCI_DEVICE_TU102, "NVIDIA TU102 (RTX 2080 Ti family)" },
	{ 0, NULL }
};

static const struct nvkm_pci_id *
nvkm_pci_match(device_t dev)
{
	const struct nvkm_pci_id *id;

	if (pci_get_vendor(dev) != NVKM_PCI_VENDOR_NVIDIA)
		return (NULL);
	for (id = nvkm_pci_ids; id->name != NULL; id++) {
		if (id->device == pci_get_device(dev))
			return (id);
	}
	return (NULL);
}

static int
nvkm_pci_probe(device_t dev)
{
	const struct nvkm_pci_id *id;

	id = nvkm_pci_match(dev);
	if (id == NULL)
		return (ENXIO);
	device_set_desc(dev, id->name);
	return (BUS_PROBE_DEFAULT);
}

static void
nvkm_pci_release_bars(struct nvkm_softc *sc)
{
	int i;

	for (i = 0; i < NVKM_NUM_BARS; i++) {
		if (sc->bar_res[i] != NULL) {
			bus_release_resource(sc->dev, SYS_RES_MEMORY,
			    sc->bar_rid[i], sc->bar_res[i]);
			sc->bar_res[i] = NULL;
		}
	}
}

#if NVKM_RUN_SUBMIT_TEST
static void
nvkm_gsp_test_kthread(void *arg)
{
	struct nvkm_softc *sc = arg;

	/* Let drm_register, drain kthread, msgq settle before submit. */
	tsleep(&sc->gsp_test_td, 0, "gsp_twrm", hz * 3);
	nvkm_debugf(sc->dev, "gsp: submit_test kthread starting\n");
	(void)nvkm_gsp_submit_test(sc);
	nvkm_debugf(sc->dev, "gsp: submit_test kthread exiting\n");
	sc->gsp_test_done = true;
	wakeup(&sc->gsp_test_done);
	kthread_exit();
}
#endif

static void
nvkm_gsp_drain_kthread(void *arg)
{
	struct nvkm_softc *sc = arg;
	/* Wait 2 sec for attach to finish before draining. */
	tsleep(&sc->gsp_drain_td, 0, "gsp_warm", hz * 2);
	nvkm_debugf(sc->dev, "gsp: msgq drain kthread started\n");
	while (!sc->gsp_drain_exit) {
		(void)nvkm_gsp_msg_dispatch_all(sc);
		tsleep(&sc->gsp_drain_td, 0, "gsp_drain", hz / 10);
	}
	nvkm_debugf(sc->dev, "gsp: msgq drain kthread exiting\n");
	kthread_exit();
}

static void
nvkm_gsp_isr(void *arg)
{
	struct nvkm_softc *sc = arg;
	uint32_t intr, inte, stat;

	intr = nvkm_rd32(sc, NVKM_TU102_GSP_BASE + 0x0008);
	/* Falcon riscv_irqmask: addr2 (0x1000) + 0x2b4 */
	inte = nvkm_rd32(sc, NVKM_TU102_GSP_BASE + 0x1000 + 0x2b4);
	stat = intr & inte;
	if (stat == 0)
		return;

	if (stat & 0x40) {
		/* doorbell from GSP-RM: drain msgq, dispatch events */
		nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x004, 0x40);
		(void)nvkm_gsp_msg_dispatch_all(sc);
		stat &= ~0x40;
	}
	if (stat != 0) {
		nvkm_debugf(sc->dev,
		    "gsp_isr: unexpected stat=0x%x\n", stat);
		nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x014, stat);
		nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x004, stat);
	}
	/* Falcon INTR_RETRIGGER0 (per gm200_flcn pattern) */
	nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x16c, 0x1);
}

static int
nvkm_gsp_evt_post_event(void *priv, uint32_t fn, void *repv, uint32_t repc)
{
	struct nvkm_softc *sc = priv;
	const uint8_t *p = repv;
	uint32_t h_client, h_event, notify_index, data, status;
	uint32_t event_data_size;
	uint16_t info16;
	uint8_t b_notify_list;

	(void)fn;
	sc->gsp_post_event_count++;

	if (repc < 32) {
		sc->gsp_post_event_short_count++;
		nvkm_debugf(sc->dev,
		    "gsp_event: POST_EVENT short len=%u\n", repc);
		return (EINVAL);
	}

	h_client = *(const uint32_t *)(const void *)(p + 0);
	h_event = *(const uint32_t *)(const void *)(p + 4);
	notify_index = *(const uint32_t *)(const void *)(p + 8);
	data = *(const uint32_t *)(const void *)(p + 12);
	info16 = *(const uint16_t *)(const void *)(p + 16);
	status = *(const uint32_t *)(const void *)(p + 20);
	event_data_size = *(const uint32_t *)(const void *)(p + 24);
	b_notify_list = *(const uint8_t *)(const void *)(p + 28);

	sc->gsp_post_event_last_client = h_client;
	sc->gsp_post_event_last_event = h_event;
	sc->gsp_post_event_last_notify_index = notify_index;
	sc->gsp_post_event_last_data = data;
	sc->gsp_post_event_last_status = status;
	sc->gsp_post_event_last_data_size = event_data_size;

	if (event_data_size != repc - 32) {
		sc->gsp_post_event_bad_size_count++;
		nvkm_debugf(sc->dev,
		    "gsp_event: POST_EVENT bad size len=%u eventDataSize=%u "
		    "client=0x%08x event=0x%08x notify=%u data=0x%08x "
		    "info16=0x%04x status=0x%08x notifyList=%u\n",
		    repc, event_data_size, h_client, h_event, notify_index, data,
		    info16, status, b_notify_list);
		return (EINVAL);
	}

	if (h_event == sc->gsp_nonstall_event_handle &&
	    h_event != 0 && status == 0)
		sc->gsp_post_event_nonstall_count++;

	/*
	 * This is the GSP-only event handoff point.  Future channel completion
	 * support should look up hClient/hEvent here and signal completed
	 * EXEC fences after checking their GPU-written completion markers.
	 */
	sc->gsp_post_event_unhandled_count++;
	nvkm_debugf(sc->dev,
	    "gsp_event: POST_EVENT client=0x%08x event=0x%08x notify=%u "
	    "data=0x%08x info16=0x%04x status=0x%08x dataSize=%u "
	    "notifyList=%u\n",
	    h_client, h_event, notify_index, data, info16, status,
	    event_data_size, b_notify_list);

	return (0);
}

static int
nvkm_gsp_evt_nocat(void *priv, uint32_t fn, void *repv, uint32_t repc)
{
#if NVKM_GSP_DEBUG_NOCAT
	struct nvkm_softc *sc = priv;
	const uint8_t *p = repv;
	char source[66];
	char faulting_engine[66];
	uint32_t flags, bugcheck, subsystem, tdr_reason, diag_len;
	uint64_t timestamp, error_code;
	uint8_t rec_type;

	(void)fn;
	if (repc < 180) {
		nvkm_debugf(sc->dev, "NOCAT: short msg len=%u\n", repc);
		return (0);
	}

	flags = *(const uint32_t *)(const void *)(p + 0);
	timestamp = *(const uint64_t *)(const void *)(p + 8);
	rec_type = *(const uint8_t *)(const void *)(p + 16);
	bugcheck = *(const uint32_t *)(const void *)(p + 20);
	memcpy(source, p + 24, 65);
	source[65] = '\0';
	subsystem = *(const uint32_t *)(const void *)(p + 92);
	error_code = *(const uint64_t *)(const void *)(p + 96);
	memcpy(faulting_engine, p + 104, 65);
	faulting_engine[65] = '\0';
	tdr_reason = *(const uint32_t *)(const void *)(p + 172);
	diag_len = *(const uint32_t *)(const void *)(p + 176);

	nvkm_debugf(sc->dev,
	    "NOCAT: flags=0x%x ts=0x%llx recType=%u bugcheck=0x%x "
	    "source=\"%s\" subsystem=0x%x errorCode=0x%llx "
	    "engine=\"%s\" tdrReason=0x%x diagLen=%u\n",
	    flags, (unsigned long long)timestamp, rec_type, bugcheck,
	    source, subsystem, (unsigned long long)error_code,
	    faulting_engine, tdr_reason, diag_len);

	if (repc >= 212) {
		nvkm_debugf(sc->dev,
		    "NOCAT: diag[0..31]= "
		    "%02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x\n",
		    p[180], p[181], p[182], p[183], p[184], p[185],
		    p[186], p[187], p[188], p[189], p[190], p[191],
		    p[192], p[193], p[194], p[195], p[196], p[197],
		    p[198], p[199], p[200], p[201], p[202], p[203],
		    p[204], p[205], p[206], p[207], p[208], p[209],
		    p[210], p[211]);
	}

	return (0);
#else
	(void)priv;
	(void)fn;
	(void)repv;
	(void)repc;
	return (0);
#endif
}

static int
nvkm_gsp_evt_rc_triggered(void *priv, uint32_t fn, void *repv, uint32_t repc)
{
	struct nvkm_softc *sc = priv;
	struct {
		uint32_t nv2080EngineType;
		uint32_t chid;
		uint32_t gfid;
		uint32_t exceptLevel;
		uint32_t exceptType;
		uint32_t scope;
		uint16_t partitionAttributionId;
		uint8_t  _pad[2];
		uint32_t mmuFaultAddrLo;
		uint32_t mmuFaultAddrHi;
		uint32_t mmuFaultType;
		uint8_t  bCallbackNeeded;
		uint8_t  _pad2[3];
		uint32_t rcJournalBufferSize;
	} *rc = repv;
	if (repc < sizeof(*rc)) {
		nvkm_infof(sc->dev, "RC_TRIGGERED: short msg len=%u\n", repc);
		return (0);
	}
	uint64_t mmu_addr = ((uint64_t)rc->mmuFaultAddrHi << 32) | rc->mmuFaultAddrLo;
	nvkm_infof(sc->dev,
	    "RC_TRIGGERED: engineType=%u chid=%u exceptLevel=%u exceptType=0x%x scope=%u "
	    "mmuFaultAddr=0x%llx mmuFaultType=0x%x rcJournalSz=%u\n",
	    rc->nv2080EngineType, rc->chid, rc->exceptLevel, rc->exceptType, rc->scope,
	    (unsigned long long)mmu_addr, rc->mmuFaultType, rc->rcJournalBufferSize);
	{
		uint8_t *journal = (uint8_t *)(rc + 1);
		uint32_t avail = repc > sizeof(*rc) ? repc - sizeof(*rc) : 0;
		uint32_t n = rc->rcJournalBufferSize < avail ?
		    rc->rcJournalBufferSize : avail;
		if (n > 512)
			n = 512;
		for (uint32_t off = 0; off < n; off += 16) {
			uint32_t left = n - off;
			nvkm_debugf(sc->dev,
			    "RC_TRIGGERED journal[%02x]: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    off,
			    left > 0 ? journal[off + 0] : 0,
			    left > 1 ? journal[off + 1] : 0,
			    left > 2 ? journal[off + 2] : 0,
			    left > 3 ? journal[off + 3] : 0,
			    left > 4 ? journal[off + 4] : 0,
			    left > 5 ? journal[off + 5] : 0,
			    left > 6 ? journal[off + 6] : 0,
			    left > 7 ? journal[off + 7] : 0,
			    left > 8 ? journal[off + 8] : 0,
			    left > 9 ? journal[off + 9] : 0,
			    left > 10 ? journal[off + 10] : 0,
			    left > 11 ? journal[off + 11] : 0,
			    left > 12 ? journal[off + 12] : 0,
			    left > 13 ? journal[off + 13] : 0,
			    left > 14 ? journal[off + 14] : 0,
			    left > 15 ? journal[off + 15] : 0);
		}
	}
	return (0);
}

static int
nvkm_gsp_evt_log_only(void *priv, uint32_t fn, void *repv, uint32_t repc)
{
	struct nvkm_softc *sc = priv;
	const uint32_t *words = repv;
	uint32_t count = repc / sizeof(*words);

	nvkm_debugf(sc->dev, "gsp_evt: fn=0x%x len=%u (logged, no action)\n",
	    fn, repc);
	if (fn == 0x1006) {
		const uint8_t *bytes = repv;
		char text[161];
		uint32_t start = repc >= 12 ? 12 : 0;
		uint32_t n = repc > start ? repc - start : 0;

		if (n >= sizeof(text))
			n = sizeof(text) - 1;
		for (uint32_t i = 0; i < n; i++) {
			uint8_t c = bytes[start + i];

			text[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
		}
		text[n] = '\0';
		nvkm_infof(sc->dev, "gsp_evt: OS_ERROR_LOG text=\"%s\"\n",
		    text);
		for (uint32_t i = 0; i < count && i < 32; i += 4) {
			nvkm_debugf(sc->dev,
			    "gsp_evt: 1006[%02u]=%08x %08x %08x %08x\n",
			    i, words[i + 0],
			    i + 1 < count ? words[i + 1] : 0,
			    i + 2 < count ? words[i + 2] : 0,
			    i + 3 < count ? words[i + 3] : 0);
		}
	}
	return (0);
}

static int
nvkm_gsp_on_init_done(void *priv, uint32_t fn, void *repv, uint32_t repc)
{
	struct nvkm_softc *sc = priv;
	(void)fn; (void)repv; (void)repc;
	sc->gsp_running = true;
	nvkm_debugf(sc->dev, "gsp: GSP_INIT_DONE event fired\n");
	return (0);
}

static int
nvkm_pci_attach(device_t dev)
{
	struct nvkm_softc *sc;
	uint32_t boot0;
	int i;

	/* DragonFly amdgpu-style: device_t softc is just drm_softc (one void *).
	 * Our state lives in a heap-alloc'd struct nvkm_softc, parked in
	 * drm_device->dev_private after drm_dev_alloc later in this function. */
	sc = kzalloc(sizeof(*sc), GFP_KERNEL);
	if (sc == NULL) {
		nvkm_infof(dev, "nvkm: kzalloc softc failed\n");
		return (ENOMEM);
	}
	sc->dev = dev;
	sc->fence_context = dma_fence_context_alloc(1);
	sc->fence_seqno = 1;

	/* Phase 5: serialise GSP cmdq writes + msgq drain across ioctl
	 * lwkts and the ithread. Init here, before any RPC is issued. */
	lwkt_token_init(&sc->gsp_tok, "nvkm-gsp");
	nvkm_chid_init(sc);
	LIST_INIT(&sc->gsp_pending);

	nvkm_debugf(dev,
	    "vendor=0x%04x device=0x%04x rev=0x%02x subsys=0x%04x:0x%04x\n",
	    pci_get_vendor(dev), pci_get_device(dev), pci_get_revid(dev),
	    pci_get_subvendor(dev), pci_get_subdevice(dev));

	/*
	 * Try to allocate every possible BAR slot. 64-bit BARs occupy two
	 * consecutive slots and the upper half will fail to allocate, which
	 * is expected.
	 */
	for (i = 0; i < NVKM_NUM_BARS; i++) {
		sc->bar_rid[i] = PCIR_BAR(i);
		sc->bar_res[i] = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
		    &sc->bar_rid[i], RF_ACTIVE);
		if (sc->bar_res[i] != NULL) {
			nvkm_debugf(dev,
			    "  BAR%d: %#jx-%#jx (%ju MiB)\n", i,
			    (uintmax_t)rman_get_start(sc->bar_res[i]),
			    (uintmax_t)rman_get_end(sc->bar_res[i]),
			    (uintmax_t)rman_get_size(sc->bar_res[i]) >> 20);
		}
	}

	if (sc->bar_res[0] == NULL) {
		nvkm_infof(dev, "BAR0 missing; cannot proceed\n");
		nvkm_pci_release_bars(sc);
		return (ENXIO);
	}

	boot0 = nvkm_rd32(sc, NV_PMC_BOOT_0);
	nvkm_debugf(dev, "PMC_BOOT_0 = 0x%08x\n", boot0);

	(void)nvkm_bios_init(sc);
	(void)nvkm_fw_init(sc);
	(void)nvkm_sec2_init(sc);
	(void)nvkm_gsp_init(sc);

	/* Publish VBIOS via sysctl so userspace can dump it for romfile. */
	{
		struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(dev);
		struct sysctl_oid *oid = device_get_sysctl_tree(dev);
		nvkm_bios_publish_sysctl(sc, ctx, oid);
		nvkm_gsp_debug_publish_sysctl(sc, ctx, oid);
	}

	/*
	 * Run FwSec-FRTS to program WPR2 from HS Falcon (PRI is PLM-locked).
	 * NOTE: only FRTS here. FwSec-SB runs at driver SHUTDOWN per
	 * nouveau tu102_gsp_fini (tu102.c:175). Running SB at init time
	 * alters engine state in a way that makes the subsequent booter
	 * fail with mb0=0x1d. Do NOT call SB here.
	 */
	(void)nvkm_fwsec_run_cmd(sc, NVKM_FWSEC_CMD_FRTS, 0, 0);

	/*
	 * Stage minimal GspFwWprMeta in sysmem before the booter runs.
	 * The booter expects its physical address in MAILBOX0/1; without
	 * it the booter halts with mb0 = 0x31. Only magic + revision are
	 * filled at this stage -- this lets us observe a distinct error
	 * code from the booter so we can iterate on the rest of the
	 * fields without flying blind.
	 */
	(void)nvkm_gsp_meta_init(sc);
	(void)nvkm_gsp_boot_prepare(sc);

	/*
	 * Build the libos init args + cmdq/msgq shared memory + RM args
	 * that GSP-RM consumes at startup. Without these, GSP-RM halts on
	 * RISC-V within microseconds of the booter releasing it, because
	 * it cannot find its RMARGS / message queues.
	 *
	 * Order matches nouveau tu102_gsp_oneinit (tu102.c:350-357):
	 *   reset GSP-Falcon -> write libos.addr to MB0/1 -> later run booter.
	 */
	(void)nvkm_gsp_libos_prepare(sc);

	if (sc->gsp != NULL && sc->gsp_libos.kva != NULL) {
		uint64_t lp = sc->gsp_libos.paddr;
		(void)nvkm_falcon_reset_eng(sc->gsp);
		nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x040,
		    (uint32_t)(lp & 0xffffffffu));
		nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x044,
		    (uint32_t)(lp >> 32));
		nvkm_debugf(sc->dev,
		    "gsp: reset + libos args @0x%llx written to MB0/1\n",
		    (unsigned long long)lp);
	}

	/*
	 * Queue an empty SET_REGISTRY RPC (fn 73, NOSEQ) into cmdq before
	 * booter runs. nouveau does this in oneinit. GSP-RM polls cmdq on
	 * startup; no doorbell needed pre-init.
	 */
	if (sc->gsp_shm.kva != NULL) {
		(void)nvkm_gsp_rpc_set_system_info(sc);
		(void)nvkm_gsp_rpc_set_registry(sc);
	}

	/* Run the booter — this is what actually stages GSP-RM in VRAM. */
	if (sc->fw_booter_load != NULL) {
		struct nvkm_booter_info bi;

		if (nvkm_booter_parse(sc, sc->fw_booter_load, &bi) == 0) {
			sc->booter = bi;
			(void)nvkm_booter_load_and_start(sc);
		}
	}

	/*
	 * Post-booter checks (per open-rm kgspBootstrap_TU102:507-520):
	 *   1. Write FALCON_OS = appVersion (informational)
	 *   2. Poll RISCV_STATUS for ACTIVE_STAT
	 *   3. Dump GSP-Falcon + RISC-V state for diagnosis
	 *   4. PRAMIN-peek WPR2 to confirm the booter copied GSP-RM
	 *      (.fwimage at gspFwOffset, BL at bootBinOffset, wpr_meta
	 *      at gspFwWprStart with verified = 0xa0a0a0a0a0a0a0a0).
	 */
	if (sc->gsp != NULL) {
		uint32_t riscv_status;
		int polls;

		nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x080, 0);

		for (polls = 0; polls < 100000; polls++) {
			riscv_status = nvkm_rd32(sc,
			    NVKM_TU102_GSP_RISCV + 0x240);
			if (riscv_status & 1)
				break;
			DELAY(10);
		}
		nvkm_debugf(sc->dev,
		    "gsp: post-booter polled %d us RISCV_STATUS=0x%08x "
		    "(active=%u)\n",
		    polls * 10, riscv_status, riscv_status & 1);

		/*
		 * If RISC-V is active, poll the msgq for the first event
		 * GSP-RM emits at the end of its self-init: GSP_INIT_DONE
		 * (= 0x1001). Layout per nouveau r535/rpc.c:
		 *   each message slot is one 4 KiB page
		 *   msgq region starts with a metadata page; entries follow
		 *   at offset 0x1000 + rptr * 0x1000.
		 *   slot header: r535_gsp_msg (52 B: 16+16+u32+u32+u32+u32)
		 *   then       : nvfw_gsp_rpc (32 B), then payload
		 *   function code = nvfw_gsp_rpc.function at offset 12 of rpc
		 *   (= 44 from start of slot once you add the 52 B mqe hdr)
		 *
		 * GSP writes its writePtr into msgq region offset 0x10
		 * (msgqTxHeader.writePtr). We update our readPtr into the
		 * cmdq region's rx header at offset rxHdrOff (= 32) per the
		 * SWAP_RX layout nouveau uses.
		 */
		if ((riscv_status & 1) && sc->gsp_shm.kva != NULL) {
			int spin;

			/* Register event handlers via the RPC framework. */
			nvkm_gsp_msg_ntfy_init(sc);
			nvkm_gsp_msg_ntfy_add(sc,
			    0x1001 /*GSP_INIT_DONE*/,
			    nvkm_gsp_on_init_done, sc);
			nvkm_gsp_msg_ntfy_add(sc,
			    0x1002 /*RUN_CPU_SEQUENCER*/,
			    nvkm_gsp_seq_msg_handler, sc);
			nvkm_gsp_msg_ntfy_add(sc,
			    0x1020 /*POST_NOCAT_RECORD*/,
			    nvkm_gsp_evt_nocat, sc);
			nvkm_gsp_msg_ntfy_add(sc, 0x1003 /*POST_EVENT*/,
			    nvkm_gsp_evt_post_event, sc);
			nvkm_gsp_msg_ntfy_add(sc, 0x1004 /*RC_TRIGGERED*/,
			    nvkm_gsp_evt_rc_triggered, sc);
			nvkm_gsp_msg_ntfy_add(sc, 0x1005 /*MMU_FAULT_QUEUED*/,
			    nvkm_gsp_evt_log_only, sc);
			nvkm_gsp_msg_ntfy_add(sc, 0x1006 /*OS_ERROR_LOG*/,
			    nvkm_gsp_evt_log_only, sc);
			nvkm_gsp_msg_ntfy_add(sc, 0x100c /*UCODE_LIBOS_PRINT*/,
			    NULL, NULL);
			nvkm_gsp_msg_ntfy_add(sc, 0x100f /*PERF_BRIDGELESS_INFO_UPDATE*/,
			    NULL, NULL);

			/* Drain msgq, dispatching events, until INIT_DONE handler
			 * sets gsp_running or we time out. */
			for (spin = 0; spin < 5000 && !sc->gsp_running; spin++) {
				nvkm_gsp_msg_dispatch_all(sc);
				if (sc->gsp_running)
					break;
				DELAY(1000);
			}
			nvkm_debugf(sc->dev,
			    "gsp: %s after polling msgq for %d ms\n",
			    sc->gsp_running ? "GSP_INIT_DONE received" :
			    "no GSP_INIT_DONE",
			    spin);

			if (sc->gsp_running)
				(void)nvkm_gsp_get_static_info(sc);
			if (sc->gsp_running)
				(void)nvkm_gsp_query_mthdbuf_size(sc);
			if (sc->gsp_running)
				(void)nvkm_gsp_intr_get_kernel_table(sc);

			/* Per nouveau tu102_fifo_init_pbdmas: BAR0+0xb65000 bit 31
			 * "enables doorbell to function". Non-GSP nouveau sets this
			 * explicitly; GSP-RM mode assumes firmware does it -- but our
			 * r570 firmware apparently doesn\'t (doorbells silently dropped). */
			if (sc->gsp_running) {
				uint32_t v = nvkm_rd32(sc, 0xb65000);
				nvkm_wr32(sc, 0xb65000, v | 0x80000000u);
				nvkm_debugf(sc->dev,
				    "gsp_rm: doorbell enable 0xb65000 was 0x%08x, set to 0x%08x\n",
				    v, v | 0x80000000u);
			}

			/* Phase 5 smoke test: allocate the RM client root via RPC.
			 * If this works the rest of the resource tree (device,
			 * vaspace, channel, ...) can be built on top. */
			if (sc->gsp_running) {
				/* Init VRAM bump allocator FIRST so vmm_ctor can
				 * allocate VRAM PT pages (PD3/PD2/PD1). */
				(void)nvkm_gsp_vram_init(sc);
				/* BAR2 first -- gives L2-coherent VRAM access
				 * needed to manipulate BAR1 PT pages. */
				(void)nvkm_gsp_bar2_init(sc);
				/* Bring up BAR1 vmm so the host can write
				 * USERD via L2-coherent paged path. */
				(void)nvkm_gsp_bar1_init(sc);
				sc->gsp_vmm = kzalloc(sizeof(*sc->gsp_vmm),
				    GFP_KERNEL);
				if (sc->gsp_vmm != NULL) {
					if (nvkm_gsp_vmm_ctor(sc, 0xc1d00001,
					    sc->gsp_vmm) != 0) {
						kfree(sc->gsp_vmm);
						sc->gsp_vmm = NULL;
					} else {
						(void)nvkm_gsp_register_nonstall_event(
						    sc->gsp_vmm);
					}
				}
				/* nouveau's GSP-RM path does NOT allocate
				 * KEPLER_CHANNEL_GROUP_A; GSP creates the TSG
				 * implicitly during channel alloc. */
				if (sc->gsp_vmm != NULL) {
					sc->gsp_chan = kzalloc(
					    sizeof(*sc->gsp_chan), GFP_KERNEL);
					if (sc->gsp_chan != NULL) {
						int cerr = nvkm_gsp_chan_ctor(
						    sc->gsp_vmm,
						    NV2080_ENGINE_TYPE_COPY2,  /* runlist 8, pure CE; avoids GRAPHICS RC storm on runlist 0 */
						    sc->gsp_chan);
						if (cerr != 0) {
							kfree(sc->gsp_chan);
							sc->gsp_chan = NULL;
						}
					}
				}
			}

			/* Install IRQ handler + arm GSP doorbell interrupt to host.
			 * Prefer MSI; fall back to INTx. Set INTx Disable bit in CMD
			 * when MSI succeeds (matches Fedora).
			 * After this point, GSP-RM events arrive via ithread; attach
			 * must do no more cmdq writes (no concurrent caller). */
			if (sc->gsp_running) {
				int msi_count = pci_msi_count(dev);
				int want = 1;
				bool used_msi = false;
				nvkm_debugf(dev,
				    "gsp: PCI advertises %d MSI vectors\n", msi_count);
				if (msi_count >= 1 &&
				    pci_alloc_msi(dev, &want, 1, -1) == 0) {
					sc->irq_rid = 1;
					used_msi = true;
					/* Disable INTx now that MSI is owned. */
					uint16_t cmd = pci_read_config(dev, 0x04, 2);
					pci_write_config(dev, 0x04, cmd | 0x0400, 2);
					nvkm_debugf(dev,
					    "gsp: MSI 1 vector acquired (rid=1), INTx disabled\n");
				} else {
					sc->irq_rid = 0;
					nvkm_debugf(dev,
					    "gsp: MSI alloc failed, falling back to INTx (rid=0)\n");
				}
				sc->irq_msi = used_msi;
				sc->irq_res = bus_alloc_resource_any(dev,
				    SYS_RES_IRQ, &sc->irq_rid,
				    used_msi ? RF_ACTIVE : (RF_ACTIVE | RF_SHAREABLE));
				if (sc->irq_res != NULL) {
					lwkt_serialize_init(&sc->irq_serialize);
					int err = bus_setup_intr(dev,
					    sc->irq_res, INTR_MPSAFE,
					    nvkm_gsp_isr, sc,
					    &sc->irq_cookie,
					    &sc->irq_serialize);
					if (err == 0) {
						/* Arm doorbell IRQ in NV_USERMODE. */
						nvkm_wr32(sc, 0x110004, 0x40);
						nvkm_debugf(dev,
						    "gsp: IRQ wired (rid=%d), doorbell intr armed\n",
						    sc->irq_rid);

						/* Register as DRM driver -- creates /dev/dri/{card,renderD}*. */
						(void)nvkm_drm_register(sc);
						nvkm_infof(dev,
						    "ready: GSP-RM running, DRM registered; debug=%d\n",
						    nvkm_debug);
						/* Start msgq drain kthread last - attach is done. */
						sc->gsp_drain_exit = false;
						(void)kthread_create(nvkm_gsp_drain_kthread, sc,
						    &sc->gsp_drain_td, "nvkm-msgq-drain");
#if NVKM_RUN_SUBMIT_TEST
						sc->gsp_test_done = false;
						(void)kthread_create(nvkm_gsp_test_kthread, sc,
						    &sc->gsp_test_td, "nvkm-submit-test");
#endif
					} else {
						nvkm_debugf(dev,
						    "gsp: bus_setup_intr failed (%d)\n", err);
						bus_release_resource(dev, SYS_RES_IRQ,
						    sc->irq_rid, sc->irq_res);
						sc->irq_res = NULL;
					}
				} else {
					nvkm_debugf(dev,
					    "gsp: no IRQ resource available\n");
				}
			}

			/*
			 * Dump LOGINIT / LOGRM "put" pointer (u64 at offset 0)
			 * and the first 64 bytes of the logged data (starting
			 * at offset 8 once the PTE array is past). If GSP-RM
			 * wrote anything to its log buffers we see it here.
			 */
			if (sc->gsp_loginit.kva != NULL) {
				const uint64_t *li = sc->gsp_loginit.kva;
				const uint64_t *lr = sc->gsp_logrm.kva;
				nvkm_debugf(sc->dev,
				    "gsp: LOGINIT put=0x%llx data[0..0x40]: "
				    "%016llx %016llx %016llx %016llx\n",
				    (unsigned long long)li[0],
				    (unsigned long long)li[1],
				    (unsigned long long)li[2],
				    (unsigned long long)li[3],
				    (unsigned long long)li[4]);
				nvkm_debugf(sc->dev,
				    "gsp: LOGRM   put=0x%llx data[0..0x40]: "
				    "%016llx %016llx %016llx %016llx\n",
				    (unsigned long long)lr[0],
				    (unsigned long long)lr[1],
				    (unsigned long long)lr[2],
				    (unsigned long long)lr[3],
				    (unsigned long long)lr[4]);
			}
			/*
			 * Re-read GSP-Falcon MB0/MB1: GSP-RM may write a
			 * progress / panic code there as it dies.
			 */
			{
				uint32_t mb0_late = nvkm_rd32(sc,
				    NVKM_TU102_GSP_BASE + 0x040);
				uint32_t mb1_late = nvkm_rd32(sc,
				    NVKM_TU102_GSP_BASE + 0x044);
				nvkm_debugf(sc->dev,
				    "gsp: late MB0=0x%08x MB1=0x%08x\n",
				    mb0_late, mb1_late);
			}
		}

		{
			uint32_t cpuctl = nvkm_rd32(sc,
			    NVKM_TU102_GSP_BASE + 0x100);
			uint32_t bootvec= nvkm_rd32(sc,
			    NVKM_TU102_GSP_BASE + 0x104);
			uint32_t irqstat= nvkm_rd32(sc,
			    NVKM_TU102_GSP_BASE + 0x008);
			uint32_t mb0    = nvkm_rd32(sc,
			    NVKM_TU102_GSP_BASE + 0x040);
			uint32_t mb1    = nvkm_rd32(sc,
			    NVKM_TU102_GSP_BASE + 0x044);
			uint32_t exci   = nvkm_rd32(sc,
			    NVKM_TU102_GSP_BASE + 0x024);
			uint32_t sctl   = nvkm_rd32(sc,
			    NVKM_TU102_GSP_BASE + 0x240);
			nvkm_debugf(sc->dev,
			    "gsp: F CPUCTL=0x%08x BOOTVEC=0x%08x SCTL=0x%08x "
			    "EXCI=0x%08x IRQSTAT=0x%08x\n",
			    cpuctl, bootvec, sctl, exci, irqstat);
			nvkm_debugf(sc->dev,
			    "gsp: F MB0=0x%08x MB1=0x%08x\n", mb0, mb1);
		}

#ifdef NVKM_DEBUG_WPR_PRAMIN_PEEK
		/*
		 * Non-nouveau diagnostic only.  WPR is protected after GSP-RM
		 * starts, and host PRAMIN reads here show up as BAR2
		 * HUBCLIENT_HOST_CPU REGION_VIOLATION records in LOGRM.
		 */
		if (sc->wpr_meta.kva != NULL) {
			struct nvkm_gsp_wpr_meta *meta =
			    (struct nvkm_gsp_wpr_meta *)sc->wpr_meta.kva;
			uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
			uint64_t addrs[3];
			const char *names[3] = { "wpr_meta", "bootBin",
			    "gspFwImage" };
			addrs[0] = meta->gspFwWprStart;
			addrs[1] = meta->bootBinOffset;
			addrs[2] = meta->gspFwOffset;
			for (int i = 0; i < 3; i++) {
				uint64_t a = addrs[i];
				uint32_t pram_base = (uint32_t)(a >> 16);
				uint32_t pram_off  = (uint32_t)(a & 0xffffu);
				uint32_t w0, w1, w2, w3;
				nvkm_wr32(sc, NV_PBUS_PRAMIN, pram_base);
				w0 = nvkm_rd32(sc, NV_PRAMIN + pram_off + 0x0);
				w1 = nvkm_rd32(sc, NV_PRAMIN + pram_off + 0x4);
				w2 = nvkm_rd32(sc, NV_PRAMIN + pram_off + 0x8);
				w3 = nvkm_rd32(sc, NV_PRAMIN + pram_off + 0xc);
				nvkm_debugf(sc->dev,
				    "gsp: VRAM %s @0x%llx: %08x %08x %08x %08x\n",
				    names[i], (unsigned long long)a,
				    w0, w1, w2, w3);
			}
			/* Also read the 'verified' field of the in-VRAM meta */
			{
				uint64_t a = meta->gspFwWprStart + 0xf8;
				uint32_t pram_base = (uint32_t)(a >> 16);
				uint32_t pram_off  = (uint32_t)(a & 0xffffu);
				uint32_t v_lo, v_hi;
				nvkm_wr32(sc, NV_PBUS_PRAMIN, pram_base);
				v_lo = nvkm_rd32(sc,
				    NV_PRAMIN + pram_off + 0x0);
				v_hi = nvkm_rd32(sc,
				    NV_PRAMIN + pram_off + 0x4);
				nvkm_debugf(sc->dev,
				    "gsp: VRAM meta.verified = 0x%08x%08x "
				    "(expect 0xa0a0a0a0a0a0a0a0 on success)\n",
				    v_hi, v_lo);
			}
			nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
		}
#endif
	}

	return (0);
}

static int
nvkm_pci_detach(device_t dev)
{
	/* device_get_softc returns the small drm_softc shim, not our
	 * nvkm_softc. The real state lives in drm_device->dev_private. */
	struct drm_softc *shim = device_get_softc(dev);
	struct drm_device *ddev = shim ? shim->drm_driver_data : NULL;
	struct nvkm_softc *sc = ddev ? ddev->dev_private : NULL;

	if (sc == NULL)
		return (0);

	/* Order: disarm IRQ → teardown ISR → unregister DRM → fini state.
	 * Disarm first so the doorbell IRQ stops firing into a handler
	 * we are about to remove. */
	if (sc->bar_res[0] != NULL)
		nvkm_wr32(sc, 0x110004, 0x00);

	if (sc->irq_cookie != NULL) {
		if (sc->gsp_test_td != NULL) {
			/* Wake any warmup sleep and wait up to 5s for test to finish. */
			wakeup(&sc->gsp_test_td);
			int w = 0;
			while (!sc->gsp_test_done && w < 50) {
				tsleep(&sc->gsp_test_done, 0, "gsp_tjoin", hz / 10);
				w++;
			}
			sc->gsp_test_td = NULL;
		}
		if (sc->gsp_drain_td != NULL) {
			sc->gsp_drain_exit = true;
			wakeup(&sc->gsp_drain_td);
			tsleep(&sc->gsp_drain_td, 0, "gsp_join", hz);
			sc->gsp_drain_td = NULL;
		}
		bus_teardown_intr(dev, sc->irq_res, sc->irq_cookie);
		sc->irq_cookie = NULL;
	}
	if (sc->irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ,
		    sc->irq_rid, sc->irq_res);
		sc->irq_res = NULL;
		if (sc->irq_msi) {
			pci_release_msi(dev);
			sc->irq_msi = false;
		}
	}

	nvkm_drm_unregister(sc);

	nvkm_booter_release(sc);
	nvkm_gsp_libos_release(sc);
	nvkm_gsp_boot_release(sc);
	nvkm_gsp_meta_fini(sc);
	nvkm_gsp_fini(sc);
	nvkm_sec2_fini(sc);
	nvkm_fw_fini(sc);
	nvkm_bios_fini(sc);
	nvkm_pci_release_bars(sc);

	kfree(sc);
	return (0);
}

static device_method_t nvkm_pci_methods[] = {
	DEVMETHOD(device_probe,		nvkm_pci_probe),
	DEVMETHOD(device_attach,	nvkm_pci_attach),
	DEVMETHOD(device_detach,	nvkm_pci_detach),
	DEVMETHOD_END
};

/*
 * driver_t.name must be "drm" to match the child device that vga_pci_attach()
 * pre-creates via device_add_child(dev, "drm", -1). This is DFly's convention
 * for GPU drivers attaching to vgapci — see amdgpu/i915/radeon, all of which
 * use the same driver name.
 */
static driver_t nvkm_pci_driver = {
	"drm",
	nvkm_pci_methods,
	sizeof(struct drm_softc),     /* amdgpu/i915 convention: drm core writes
	                                 softc->drm_driver_data; real state in
	                                 heap-alloc'd nvkm_softc */
};

static devclass_t nvkm_devclass;

/*
 * Attach on the vgapci bus, not pci directly. DFly's vga_pci driver claims
 * any VGA-class PCI device and exposes it through a pre-allocated "drm"
 * child slot. GPU-specific drivers (amdgpu/i915/radeon/us) bind to that
 * child via the vgapci bus.
 */
DRIVER_MODULE(nvkm, vgapci, nvkm_pci_driver, nvkm_devclass, NULL, NULL);
MODULE_DEPEND(nvkm, drm, 1, 1, 1);
