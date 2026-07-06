/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical GPU lifetime boundary for the native NVIDIA driver.
 *
 * This file owns the KMOD entry.  PCI/newbus attachment, boot LWKT startup,
 * and final device teardown will migrate here in later refactor steps.
 */

#include "nvgpu_device.h"
#include "nvkm_priv.h"

#include <drm/drmP.h>

static int
nvgpu_modevent(module_t mod __unused, int type, void *data __unused)
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

static moduledata_t nvgpu_moddata = {
	"nvgpu",
	nvgpu_modevent,
	NULL
};

DECLARE_MODULE(nvgpu, nvgpu_moddata, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(nvgpu, 1);
/* Pull in the GSP firmware blobs registered by the fw module so
 * firmware_get() works during device attach, regardless of which
 * .ko the kld scanner picks up first at boot. */
MODULE_DEPEND(nvgpu, nvgsp_570_fw, 1, 1, 1);

static device_method_t nvgpu_device_pci_methods[] = {
	DEVMETHOD(device_probe,	nvgpu_device_pci_probe),
	DEVMETHOD(device_attach,	nvgpu_device_pci_attach),
	DEVMETHOD(device_detach,	nvgpu_device_pci_detach),
	DEVMETHOD_END
};

/*
 * driver_t.name must be "drm" to match the child device that vga_pci_attach()
 * pre-creates via device_add_child(dev, "drm", -1). This is DFly's convention
 * for GPU drivers attaching to vgapci; amdgpu, i915, and radeon use the same
 * driver name.
 */
static driver_t nvgpu_device_pci_driver = {
	"drm",
	nvgpu_device_pci_methods,
	sizeof(struct drm_softc),	/* drm core writes drm_driver_data. */
};

static devclass_t nvgpu_device_devclass;

/*
 * Attach on the vgapci bus, not pci directly. DFly's vga_pci driver claims
 * any VGA-class PCI device and exposes it through a pre-allocated "drm" child
 * slot. GPU-specific drivers bind to that child via the vgapci bus.
 */
DRIVER_MODULE(nvgpu, vgapci, nvgpu_device_pci_driver,
    nvgpu_device_devclass, NULL, NULL);
MODULE_DEPEND(nvgpu, drm, 1, 1, 1);
