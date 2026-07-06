/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical GPU lifetime boundary for the native NVIDIA driver.
 */

#include "nvgpu_device.h"
#include "nvgpu_chip.h"
#include "nvgpu_debug.h"
#include "nvdrm_drv.h"
#include "nvgpu_intr.h"
#include "nvgpu_unload.h"
#include "nvgsp_bar.h"
#include "nvgsp_boot.h"
#include "nvgsp_channel.h"
#include "nvgsp_disp.h"
#include "nvgsp_event.h"
#include "nvgsp_rpc.h"
#include "nvgsp_state.h"
#include "nvgsp_vmm.h"
#include "nvgsp_vram.h"

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
 * Physical GPU root.  Owned by PCI attach and borrowed by DRM, GSP, display,
 * interrupt, and per-open process state while detach is excluded.
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
static int nvgpu_device_boot(struct nvgpu_device *gpu);
static int nvgpu_device_boot_identify(struct nvgpu_device *gpu);
static void nvgpu_device_boot_done(struct nvgpu_device *gpu);
static int nvgpu_device_stop(struct nvgpu_device *gpu);
static void nvgpu_device_fini(struct nvgpu_device *gpu);

/* Handle module load and unload notifications. */
static int
nvgpu_device_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		nvgpu_log(NVGPU_LOG_INFO, "loaded (target GSP firmware 570.144)\n");
		return (0);
	case MOD_UNLOAD:
		nvgpu_log(NVGPU_LOG_INFO, "unloaded\n");
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
static struct nvgpu_device *nvgpu_default_gpu;

DRIVER_MODULE(nvgpu, vgapci, nvgpu_device_pci_driver,
    nvgpu_device_devclass, NULL, NULL);
MODULE_DEPEND(nvgpu, drm, 1, 1, 1);

/* Look up static chip metadata for a PCI device. */
static const struct nvgpu_pci_device *
nvgpu_device_pci_match(device_t dev)
{
	if (pci_get_vendor(dev) != NVGPU_PCI_VENDOR_NVIDIA)
		return (NULL);
	return (nvgpu_chip_pci_lookup(pci_get_device(dev)));
}

/* Tell newbus whether this NVIDIA PCI function is supported. */
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

/* Recover the GPU object stored behind the drm child softc. */
static struct nvgpu_device *
nvgpu_device_from_newbus(device_t dev)
{
	struct drm_softc *shim;

	shim = device_get_softc(dev);
	if (shim == NULL)
		return (NULL);
	return (shim->drm_driver_data);
}

/* Publish or clear the GPU object in the drm child softc. */
static void
nvgpu_device_store_newbus(device_t dev, struct nvgpu_device *gpu)
{
	struct drm_softc *shim;

	shim = device_get_softc(dev);
	if (shim != NULL)
		shim->drm_driver_data = (void *)gpu;
}

/* Map the PCI BAR resources required for early MMIO. */
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

		nvgpu_log(NVGPU_LOG_DEBUG, "BAR%d: %#jx-%#jx (%ju MiB)\n", i,
		    (uintmax_t)rman_get_start(gpu->bar_res[i]),
		    (uintmax_t)rman_get_end(gpu->bar_res[i]),
		    (uintmax_t)rman_get_size(gpu->bar_res[i]) >> 20);
	}

	if (gpu->bar_res[0] == NULL) {
		nvgpu_log(NVGPU_LOG_INFO, "BAR0 missing; cannot proceed\n");
		nvgpu_device_release_bars(gpu);
		return (ENXIO);
	}

	return (0);
}

/* Release all PCI BAR resources owned by the GPU. */
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

/* Return the DragonFly device for a GPU or the default GPU. */
/* Return gpu's borrowed device_t.  NULL gpu means the current default GPU, if any. */
device_t
nvgpu_device_dev(struct nvgpu_device *gpu)
{
	if (gpu == NULL)
		gpu = nvgpu_default_gpu;
	if (gpu == NULL)
		return (NULL);
	return (gpu->dev);
}

static uint32_t
nvgpu_device_rd32(struct nvgpu_device *gpu, uint32_t offset)
{
	return (bus_read_4(gpu->bar_res[0], offset));
}

/* Create the boot LWKT that performs long GSP bring-up. */
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

/* Request boot LWKT stop and wait for its exit. */
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

/* Run the device boot sequence inside the boot LWKT. */
static void
nvgpu_device_boot_run(void *arg)
{
	struct nvgpu_device *gpu = arg;
	int error;

	error = nvgpu_device_boot(gpu);
	gpu->boot_result = error;
	gpu->boot_done = true;
	wakeup(&gpu->boot_done);
	kthread_exit();
}

/* Read basic GPU identity registers and log the attachment. */
static int
nvgpu_device_boot_identify(struct nvgpu_device *gpu)
{
	if (gpu->boot_stop_requested)
		return (EINTR);

	gpu->boot0 = nvgpu_device_rd32(gpu, NVGPU_PMC_BOOT_0);
	nvgpu_log(NVGPU_LOG_INFO, "%s attached, PMC_BOOT_0=0x%08x\n",
	    gpu->pci_device->name, gpu->boot0);
	return (0);
}

/* Execute the ordered device boot chain before DRM publication. */
static int
nvgpu_device_boot(struct nvgpu_device *gpu)
{
	int error;

	error = nvgpu_device_boot_identify(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_state_init(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_rpc_init(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_event_init(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_boot(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_vram_init(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_bar2_init(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_bar1_init(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_vmm_init_kernel(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_channel_create_bootstrap(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_channel_create_golden(gpu);
	if (error != 0)
		return (error);
	error = nvgsp_disp_init(gpu);
	if (error != 0)
		return (error);
	error = nvgpu_intr_init(gpu);
	if (error != 0)
		return (error);
	error = nvgpu_intr_enable(gpu);
	if (error != 0)
		return (error);
	error = nvdrm_register(gpu);
	if (error != 0)
		return (error);
	nvgpu_device_boot_done(gpu);
	return (0);
}

static void
nvgpu_device_boot_done(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot completed\n");
}

/* Run the reverse device teardown chain. */
static int
nvgpu_device_stop(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "stop device\n");
	nvgpu_device_boot_stop(gpu);
	nvdrm_unregister(gpu);
	nvgsp_disp_fini(gpu);
	nvgsp_channel_destroy_bootstrap(gpu);
	nvgsp_channel_destroy_golden(gpu);
	nvgsp_vmm_fini_kernel(gpu);
	nvgpu_intr_disable(gpu);
	nvgpu_intr_fini(gpu);
	nvgsp_bar1_fini(gpu);
	nvgsp_bar2_fini(gpu);
	nvgsp_vram_fini(gpu);
	nvgsp_shutdown(gpu);
	nvgsp_state_fini(gpu);
	return (0);
}

/* Release final device resources and free the GPU object. */
static void
nvgpu_device_fini(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "finish device\n");
	nvgpu_device_release_bars(gpu);
	nvgpu_log(NVGPU_LOG_INFO, "detached\n");
	if (nvgpu_default_gpu == gpu)
		nvgpu_default_gpu = NULL;
	kfree(gpu);
}

/* Allocate the GPU object and start asynchronous device boot. */
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
		nvgpu_log(NVGPU_LOG_INFO, "nvgpu: device allocation failed\n");
		return (ENOMEM);
	}

	gpu->dev = dev;
	nvgpu_default_gpu = gpu;
	gpu->pci_device = id;
	gpu->chip = id->chip;
	nvgpu_device_store_newbus(dev, gpu);

	nvgpu_log(NVGPU_LOG_DEBUG,
	    "vendor=0x%04x device=0x%04x rev=0x%02x subsys=0x%04x:0x%04x\n",
	    pci_get_vendor(dev), pci_get_device(dev), pci_get_revid(dev),
	    pci_get_subvendor(dev), pci_get_subdevice(dev));

	error = nvgpu_device_alloc_bars(gpu);
	if (error != 0)
		goto fail;

	error = nvgpu_unload_init(gpu);
	if (error != 0)
		goto fail_bars;

	error = nvgpu_device_boot_start(gpu);
	if (error != 0)
		goto fail_bars;

	return (0);

fail_bars:
	nvgpu_device_release_bars(gpu);
fail:
	nvgpu_device_store_newbus(dev, NULL);
	if (nvgpu_default_gpu == gpu)
		nvgpu_default_gpu = NULL;
	kfree(gpu);
	return (error);
}

/* Admit unload and tear down the GPU object if idle. */
static int
nvgpu_device_pci_detach(device_t dev)
{
	struct nvgpu_device *gpu;
	int error;

	gpu = nvgpu_device_from_newbus(dev);
	if (gpu == NULL)
		return (0);

	error = nvgpu_unload_begin(gpu);
	if (error != 0)
		return (error);

	nvgpu_device_store_newbus(dev, NULL);
	error = nvgpu_device_stop(gpu);
	if (error != 0) {
		nvgpu_device_store_newbus(dev, gpu);
		return (error);
	}
	nvgpu_device_fini(gpu);
	return (0);
}
