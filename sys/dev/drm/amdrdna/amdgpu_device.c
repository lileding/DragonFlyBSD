/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Phase A0 skeleton: loadable PCI driver with RDNA+ ID table.
 * No hardware bring-up yet — attach only logs and holds softc.
 */

#include "amdgpu_chip.h"
#include "amdgpu_debug.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>

MALLOC_DEFINE(M_AMDRDNA, "amdrdna", "RDNA+ AMD GPU driver");

struct amdrdna_softc {
	device_t			dev;
	const struct amdgpu_chip_info	*chip;
	const char			*desc;
	/* BAR handles reserved for later phases; unused in A0. */
	struct resource			*bar0;
	int				bar0_rid;
};

static int amdrdna_probe(device_t dev);
static int amdrdna_attach(device_t dev);
static int amdrdna_detach(device_t dev);

static device_method_t amdrdna_methods[] = {
	DEVMETHOD(device_probe,		amdrdna_probe),
	DEVMETHOD(device_attach,	amdrdna_attach),
	DEVMETHOD(device_detach,	amdrdna_detach),
	DEVMETHOD_END
};

/*
 * Parent vgapci creates a child named "drm". Keep the driver name "drm"
 * so we bind like nvgpu / legacy drm drivers when a matching GPU appears.
 */
static driver_t amdrdna_driver = {
	"drm",
	amdrdna_methods,
	sizeof(struct amdrdna_softc),
};

static devclass_t amdrdna_devclass;

static int
amdrdna_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		if (amdgpu_debug_init() != 0)
			return (ENOMEM);
		amdgpu_log(AMDGPU_LOG_INFO,
		    "loaded (Phase A0 skeleton, RDNA+ PCI table)\n");
		return (0);
	case MOD_UNLOAD:
		amdgpu_log(AMDGPU_LOG_INFO, "unloaded\n");
		amdgpu_debug_fini();
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t amdrdna_moddata = {
	"amdrdna",
	amdrdna_modevent,
	NULL
};

DECLARE_MODULE(amdrdna, amdrdna_moddata, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(amdrdna, 1);

DRIVER_MODULE(amdrdna, vgapci, amdrdna_driver, amdrdna_devclass, NULL, NULL);

static int
amdrdna_probe(device_t dev)
{
	const struct amdgpu_chip_info *chip;
	const struct amdgpu_pci_id *id;
	uint16_t vendor, device;
	uint8_t classb, subclass;
	char desc[128];

	vendor = pci_get_vendor(dev);
	device = pci_get_device(dev);
	classb = pci_get_class(dev);
	subclass = pci_get_subclass(dev);

	chip = amdgpu_chip_resolve(vendor, device, classb, subclass);
	if (chip == NULL)
		return (ENXIO);

	id = amdgpu_chip_lookup_device(device);
	if (id != NULL && id->name != NULL)
		device_set_desc(dev, id->name);
	else {
		ksnprintf(desc, sizeof(desc),
		    "AMD RDNA+ GPU (0x%04x, %s/%s)", device,
		    chip->family_name, chip->codename);
		device_set_desc_copy(dev, desc);
	}

	return (BUS_PROBE_DEFAULT);
}

static int
amdrdna_attach(device_t dev)
{
	struct amdrdna_softc *sc;
	const struct amdgpu_chip_info *chip;
	uint16_t device;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->bar0 = NULL;
	sc->bar0_rid = PCIR_BAR(0);

	device = pci_get_device(dev);
	chip = amdgpu_chip_resolve(pci_get_vendor(dev), device,
	    pci_get_class(dev), pci_get_subclass(dev));
	if (chip == NULL)
		return (ENXIO);

	sc->chip = chip;
	sc->desc = device_get_desc(dev);

	amdgpu_log(AMDGPU_LOG_INFO,
	    "attach %s at pci%d:%d:%d device=0x%04x chip=%s family=%s%s "
	    "(skeleton: no hardware init yet)\n",
	    sc->desc != NULL ? sc->desc : "AMD GPU",
	    pci_get_bus(dev), pci_get_slot(dev), pci_get_function(dev),
	    device, chip->codename, chip->family_name,
	    chip->is_apu ? " apu" : "");

	/*
	 * Map BAR0 read-only identity later. A0 intentionally does not
	 * touch MMIO so load/unload stays safe without firmware.
	 */
	return (0);
}

static int
amdrdna_detach(device_t dev)
{
	struct amdrdna_softc *sc = device_get_softc(dev);

	amdgpu_log(AMDGPU_LOG_INFO, "detach %s\n",
	    sc->desc != NULL ? sc->desc : "AMD GPU");

	if (sc->bar0 != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->bar0_rid,
		    sc->bar0);
		sc->bar0 = NULL;
	}

	return (0);
}
