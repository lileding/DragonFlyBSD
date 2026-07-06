/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical GPU lifetime boundary for the native NVIDIA driver.
 */

#include "nvgpu_device.h"
#include "nvgpu_chip.h"
#include "nvgpu_debug.h"

#include <drm/drmP.h>

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/module.h>
#include <sys/rman.h>

#define NVGPU_PMC_BOOT_0	0x00000000u

/*
 * struct nvgpu_device
 *
 * Ownership:
 *   Allocated by PCI attach and stored as the private pointer behind the small
 *   DragonFly drm_softc shim. Subsystems borrow it; none of them owns it.
 *
 * Lifetime:
 *   Created after PCI probe succeeds. Destroyed by PCI detach after userspace,
 *   boot work, interrupts, display, and BAR resources have been stopped. This
 *   early refactor stage only owns PCI identity and BAR resources.
 *
 * Threading:
 *   PCI attach/detach are serialized by newbus. Runtime subsystems must add
 *   their own tokens or locks before they publish mutable state reachable from
 *   ioctl, interrupt, or worker contexts.
 */
struct nvgpu_device {
	device_t dev;
	const struct nvgpu_pci_device *pci_device;
	const struct nvgpu_chip_config *chip;
	int bar_rid[NVGPU_NUM_BARS];
	struct resource *bar_res[NVGPU_NUM_BARS];
	struct thread *boot_td;
	bool boot_stop_requested;
	bool boot_done;
	int boot_result;
	uint32_t boot0;
};

static int nvgpu_device_pci_probe(device_t dev);
static int nvgpu_device_pci_attach(device_t dev);
static int nvgpu_device_pci_detach(device_t dev);
static const struct nvgpu_pci_device *nvgpu_device_pci_match(device_t dev);
static struct nvgpu_device *nvgpu_device_from_newbus(device_t dev);
static void nvgpu_device_store_newbus(device_t dev, struct nvgpu_device *gpu);
static int nvgpu_device_alloc_bars(struct nvgpu_device *gpu);
static void nvgpu_device_release_bars(struct nvgpu_device *gpu);
static uint32_t nvgpu_device_rd32(struct nvgpu_device *gpu, uint32_t offset);
static int nvgpu_device_boot_start(struct nvgpu_device *gpu);
static void nvgpu_device_boot_stop(struct nvgpu_device *gpu);
static void nvgpu_device_boot_run(void *arg);
static int nvgpu_device_boot_identify(struct nvgpu_device *gpu);

static int
nvgpu_device_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		kprintf("nvgpu: loaded (target GSP firmware 570.144)\n");
		return (0);
	case MOD_UNLOAD:
		kprintf("nvgpu: unloaded\n");
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t nvgpu_device_moddata = {
	"nvgpu",
	nvgpu_device_modevent,
	NULL
};

DECLARE_MODULE(nvgpu, nvgpu_device_moddata, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(nvgpu, 1);
MODULE_DEPEND(nvgpu, nvgsp_570_fw, 1, 1, 1);

static device_method_t nvgpu_device_pci_methods[] = {
	DEVMETHOD(device_probe,	nvgpu_device_pci_probe),
	DEVMETHOD(device_attach,	nvgpu_device_pci_attach),
	DEVMETHOD(device_detach,	nvgpu_device_pci_detach),
	DEVMETHOD_END
};

/*
 * DragonFly's vga_pci parent pre-creates a child named "drm". The driver name
 * must stay "drm" so the native GPU driver binds to that child, matching
 * amdgpu, i915, and radeon.
 */
static driver_t nvgpu_device_pci_driver = {
	"drm",
	nvgpu_device_pci_methods,
	sizeof(struct drm_softc),
};

static devclass_t nvgpu_device_devclass;

DRIVER_MODULE(nvgpu, vgapci, nvgpu_device_pci_driver,
    nvgpu_device_devclass, NULL, NULL);
MODULE_DEPEND(nvgpu, drm, 1, 1, 1);

static const struct nvgpu_pci_device *
nvgpu_device_pci_match(device_t dev)
{
	if (pci_get_vendor(dev) != NVGPU_PCI_VENDOR_NVIDIA)
		return (NULL);
	return (nvgpu_chip_pci_lookup(pci_get_device(dev)));
}

static int
nvgpu_device_pci_probe(device_t dev)
{
	const struct nvgpu_pci_device *id;

	id = nvgpu_device_pci_match(dev);
	if (id == NULL || id->chip == NULL)
		return (ENXIO);

	device_set_desc(dev, id->name);
	return (BUS_PROBE_DEFAULT);
}

static struct nvgpu_device *
nvgpu_device_from_newbus(device_t dev)
{
	struct drm_softc *shim;

	shim = device_get_softc(dev);
	if (shim == NULL)
		return (NULL);
	return (shim->drm_driver_data);
}

static void
nvgpu_device_store_newbus(device_t dev, struct nvgpu_device *gpu)
{
	struct drm_softc *shim;

	shim = device_get_softc(dev);
	if (shim != NULL)
		shim->drm_driver_data = (void *)gpu;
}

static int
nvgpu_device_alloc_bars(struct nvgpu_device *gpu)
{
	int i;

	for (i = 0; i < NVGPU_NUM_BARS; i++) {
		gpu->bar_rid[i] = PCIR_BAR(i);
		gpu->bar_res[i] = bus_alloc_resource_any(gpu->dev,
		    SYS_RES_MEMORY, &gpu->bar_rid[i], RF_ACTIVE);
		if (gpu->bar_res[i] == NULL)
			continue;

		nvgpu_debugf(gpu->dev, "BAR%d: %#jx-%#jx (%ju MiB)\n", i,
		    (uintmax_t)rman_get_start(gpu->bar_res[i]),
		    (uintmax_t)rman_get_end(gpu->bar_res[i]),
		    (uintmax_t)rman_get_size(gpu->bar_res[i]) >> 20);
	}

	if (gpu->bar_res[0] == NULL) {
		nvgpu_infof(gpu->dev, "BAR0 missing; cannot proceed\n");
		nvgpu_device_release_bars(gpu);
		return (ENXIO);
	}

	return (0);
}

static void
nvgpu_device_release_bars(struct nvgpu_device *gpu)
{
	int i;

	for (i = 0; i < NVGPU_NUM_BARS; i++) {
		if (gpu->bar_res[i] == NULL)
			continue;
		bus_release_resource(gpu->dev, SYS_RES_MEMORY, gpu->bar_rid[i],
		    gpu->bar_res[i]);
		gpu->bar_res[i] = NULL;
	}
}

static uint32_t
nvgpu_device_rd32(struct nvgpu_device *gpu, uint32_t offset)
{
	return (bus_read_4(gpu->bar_res[0], offset));
}

static int
nvgpu_device_boot_start(struct nvgpu_device *gpu)
{
	int error;

	gpu->boot_stop_requested = false;
	gpu->boot_done = false;
	gpu->boot_result = 0;
	error = kthread_create(nvgpu_device_boot_run, gpu, &gpu->boot_td,
	    "nvgpu-boot");
	if (error != 0) {
		gpu->boot_td = NULL;
		gpu->boot_done = true;
		gpu->boot_result = error;
	}
	return (error);
}

static void
nvgpu_device_boot_stop(struct nvgpu_device *gpu)
{
	if (gpu->boot_td == NULL)
		return;

	gpu->boot_stop_requested = true;
	wakeup(&gpu->boot_stop_requested);
	while (!gpu->boot_done)
		tsleep(&gpu->boot_done, 0, "nvgpubt", hz);
	gpu->boot_td = NULL;
}

static void
nvgpu_device_boot_run(void *arg)
{
	struct nvgpu_device *gpu = arg;
	int error;

	error = nvgpu_device_boot_identify(gpu);
	gpu->boot_result = error;
	gpu->boot_done = true;
	wakeup(&gpu->boot_done);
	kthread_exit();
}

static int
nvgpu_device_boot_identify(struct nvgpu_device *gpu)
{
	if (gpu->boot_stop_requested)
		return (EINTR);

	gpu->boot0 = nvgpu_device_rd32(gpu, NVGPU_PMC_BOOT_0);
	nvgpu_infof(gpu->dev, "%s attached, PMC_BOOT_0=0x%08x\n",
	    gpu->pci_device->name, gpu->boot0);
	return (0);
}

static int
nvgpu_device_pci_attach(device_t dev)
{
	const struct nvgpu_pci_device *id;
	struct nvgpu_device *gpu;
	int error;

	id = nvgpu_device_pci_match(dev);
	if (id == NULL || id->chip == NULL)
		return (ENXIO);

	gpu = kzalloc(sizeof(*gpu), GFP_KERNEL);
	if (gpu == NULL) {
		nvgpu_infof(dev, "nvgpu: device allocation failed\n");
		return (ENOMEM);
	}

	gpu->dev = dev;
	gpu->pci_device = id;
	gpu->chip = id->chip;
	nvgpu_device_store_newbus(dev, gpu);

	nvgpu_debugf(dev,
	    "vendor=0x%04x device=0x%04x rev=0x%02x subsys=0x%04x:0x%04x\n",
	    pci_get_vendor(dev), pci_get_device(dev), pci_get_revid(dev),
	    pci_get_subvendor(dev), pci_get_subdevice(dev));

	error = nvgpu_device_alloc_bars(gpu);
	if (error != 0)
		goto fail;

	error = nvgpu_device_boot_start(gpu);
	if (error != 0)
		goto fail_bars;

	return (0);

fail_bars:
	nvgpu_device_release_bars(gpu);
fail:
	nvgpu_device_store_newbus(dev, NULL);
	kfree(gpu);
	return (error);
}

static int
nvgpu_device_pci_detach(device_t dev)
{
	struct nvgpu_device *gpu;

	gpu = nvgpu_device_from_newbus(dev);
	if (gpu == NULL)
		return (0);

	nvgpu_device_store_newbus(dev, NULL);
	nvgpu_device_boot_stop(gpu);
	nvgpu_device_release_bars(gpu);
	nvgpu_infof(dev, "detached\n");
	kfree(gpu);
	return (0);
}
