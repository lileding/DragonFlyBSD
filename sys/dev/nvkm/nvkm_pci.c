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
#include "nvkm_falcon.h"

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>
#include <sys/sysctl.h>

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

static int
nvkm_pci_attach(device_t dev)
{
	struct nvkm_softc *sc = device_get_softc(dev);
	uint32_t boot0;
	int i;

	sc->dev = dev;

	device_printf(dev,
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
			device_printf(dev,
			    "  BAR%d: %#jx-%#jx (%ju MiB)\n", i,
			    (uintmax_t)rman_get_start(sc->bar_res[i]),
			    (uintmax_t)rman_get_end(sc->bar_res[i]),
			    (uintmax_t)rman_get_size(sc->bar_res[i]) >> 20);
		}
	}

	if (sc->bar_res[0] == NULL) {
		device_printf(dev, "BAR0 missing; cannot proceed\n");
		nvkm_pci_release_bars(sc);
		return (ENXIO);
	}

	boot0 = nvkm_rd32(sc, NV_PMC_BOOT_0);
	device_printf(dev, "PMC_BOOT_0 = 0x%08x\n", boot0);

	(void)nvkm_bios_init(sc);
	(void)nvkm_fw_init(sc);
	(void)nvkm_sec2_init(sc);
	(void)nvkm_gsp_init(sc);

	/* Publish VBIOS via sysctl so userspace can dump it for romfile. */
	{
		struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(dev);
		struct sysctl_oid *oid = device_get_sysctl_tree(dev);
		nvkm_bios_publish_sysctl(sc, ctx, oid);
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
		device_printf(sc->dev,
		    "gsp: reset + libos args @0x%llx written to MB0/1\n",
		    (unsigned long long)lp);
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
		device_printf(sc->dev,
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
			uint8_t *shm = (uint8_t *)sc->gsp_shm.kva;
			uint8_t *msgq = shm + sc->gsp_shm_msgq_off;
			uint8_t *cmdq = shm + sc->gsp_shm_cmdq_off;
			uint32_t rptr = 0;	/* host's read cursor */
			uint32_t wptr_seen_prev = 0;
			int spin;
			bool got_init_done = false;

			for (spin = 0; spin < 5000 && !got_init_done; spin++) {
				uint32_t wptr = *(volatile uint32_t *)(msgq + 0x10);
				if (wptr != wptr_seen_prev) {
					device_printf(sc->dev,
					    "gsp: msgq wptr advanced %u -> %u (rptr=%u)\n",
					    wptr_seen_prev, wptr, rptr);
					wptr_seen_prev = wptr;
				}
				while (rptr != wptr) {
					uint8_t *slot = msgq + 0x1000 +
					    rptr * 0x1000;
					/* r535_gsp_msg header = 52 bytes
					 * (16 auth + 16 aad + 4 chksum + 4 seq
					 *  + 4 elem_count + 4 pad + 4 unused).
					 * Then nvfw_gsp_rpc:
					 *   u32 header_version  off 0
					 *   u32 signature       off 4
					 *   u32 length          off 8
					 *   u32 function        off 12  <-- want this
					 *   u32 rpc_result      off 16
					 *   ... */
					uint32_t func, sig, len, result;
					sig    = *(volatile uint32_t *)(slot + 52 + 4);
					len    = *(volatile uint32_t *)(slot + 52 + 8);
					func   = *(volatile uint32_t *)(slot + 52 + 12);
					result = *(volatile uint32_t *)(slot + 52 + 16);
					device_printf(sc->dev,
					    "gsp: msgq[%u] sig=0x%08x len=%u func=%u(0x%x) result=0x%x\n",
					    rptr, sig, len, func, func, result);
					if (func == 4097 /* GSP_INIT_DONE */)
						got_init_done = true;
					rptr++;
					if (rptr >= 63 /* msgCount */)
						rptr = 0;
				}
				/* publish our rptr back to GSP via cmdq rxHdr */
				*(volatile uint32_t *)(cmdq + 32) = rptr;
				if (got_init_done)
					break;
				DELAY(1000);	/* 1 ms */
			}
			device_printf(sc->dev,
			    "gsp: %s after polling msgq for %d ms (final wptr=%u rptr=%u)\n",
			    got_init_done ? "GSP_INIT_DONE received" :
			    "no GSP_INIT_DONE",
			    spin, wptr_seen_prev, rptr);

			/*
			 * Dump LOGINIT / LOGRM "put" pointer (u64 at offset 0)
			 * and the first 64 bytes of the logged data (starting
			 * at offset 8 once the PTE array is past). If GSP-RM
			 * wrote anything to its log buffers we see it here.
			 */
			if (sc->gsp_loginit.kva != NULL) {
				const uint64_t *li = sc->gsp_loginit.kva;
				const uint64_t *lr = sc->gsp_logrm.kva;
				device_printf(sc->dev,
				    "gsp: LOGINIT put=0x%llx data[0..0x40]: "
				    "%016llx %016llx %016llx %016llx\n",
				    (unsigned long long)li[0],
				    (unsigned long long)li[1],
				    (unsigned long long)li[2],
				    (unsigned long long)li[3],
				    (unsigned long long)li[4]);
				device_printf(sc->dev,
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
				device_printf(sc->dev,
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
			device_printf(sc->dev,
			    "gsp: F CPUCTL=0x%08x BOOTVEC=0x%08x SCTL=0x%08x "
			    "EXCI=0x%08x IRQSTAT=0x%08x\n",
			    cpuctl, bootvec, sctl, exci, irqstat);
			device_printf(sc->dev,
			    "gsp: F MB0=0x%08x MB1=0x%08x\n", mb0, mb1);
		}

		/*
		 * PRAMIN-peek WPR2 to verify the booter copied content
		 * into VRAM. PRAMIN window is BAR0 + 0x700000 (1 MiB);
		 * the window's VRAM base is set via 0x001700 (value =
		 * vram_addr >> 16). Save+restore.
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
				device_printf(sc->dev,
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
				device_printf(sc->dev,
				    "gsp: VRAM meta.verified = 0x%08x%08x "
				    "(expect 0xa0a0a0a0a0a0a0a0 on success)\n",
				    v_hi, v_lo);
			}
			nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
		}
	}

	return (0);
}

static int
nvkm_pci_detach(device_t dev)
{
	struct nvkm_softc *sc = device_get_softc(dev);

	nvkm_booter_release(sc);
	nvkm_gsp_libos_release(sc);
	nvkm_gsp_boot_release(sc);
	nvkm_gsp_meta_fini(sc);
	nvkm_gsp_fini(sc);
	nvkm_sec2_fini(sc);
	nvkm_fw_fini(sc);
	nvkm_bios_fini(sc);
	nvkm_pci_release_bars(sc);
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
	sizeof(struct nvkm_softc),
};

static devclass_t nvkm_devclass;

/*
 * Attach on the vgapci bus, not pci directly. DFly's vga_pci driver claims
 * any VGA-class PCI device and exposes it through a pre-allocated "drm"
 * child slot. GPU-specific drivers (amdgpu/i915/radeon/us) bind to that
 * child via the vgapci bus.
 */
DRIVER_MODULE(nvkm, vgapci, nvkm_pci_driver, nvkm_devclass, NULL, NULL);
