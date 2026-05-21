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

	/* FwSec FRTS: program WPR2 from HS Falcon (PRI is PLM-locked). */
	/*
	 * Run the two FwSec invocations the way nouveau does on TU102:
	 *   1. FRTS (cmd 0x15) sets up the FRTS region in VRAM
	 *   2. SB   (cmd 0x19) does sub-boot, programs WPR2 PLMs, etc.
	 * The frts_addr/size arguments are only used for FRTS; they're
	 * computed inside nvkm_fwsec_run_cmd from the actual FB layout.
	 * Pass 0/0 to make that explicit.
	 */
	(void)nvkm_fwsec_run_cmd(sc, NVKM_FWSEC_CMD_FRTS, 0, 0);
	(void)nvkm_fwsec_run_cmd(sc, NVKM_FWSEC_CMD_SB,   0, 0);

	/*
	 * Stage minimal GspFwWprMeta in sysmem before the booter runs.
	 * The booter expects its physical address in MAILBOX0/1; without
	 * it the booter halts with mb0 = 0x31. Only magic + revision are
	 * filled at this stage -- this lets us observe a distinct error
	 * code from the booter so we can iterate on the rest of the
	 * fields without flying blind.
	 */
	(void)nvkm_gsp_meta_init(sc);

	if (sc->fw_booter_load != NULL) {
		struct nvkm_booter_info bi;

		if (nvkm_booter_parse(sc, sc->fw_booter_load, &bi) == 0) {
			sc->booter = bi;
			(void)nvkm_booter_load_and_start(sc);
		}
	}

	return (0);
}

static int
nvkm_pci_detach(device_t dev)
{
	struct nvkm_softc *sc = device_get_softc(dev);

	nvkm_booter_release(sc);
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
