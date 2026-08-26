/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2017, Bryan Venteicher <bryanv@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Driver for the modern VirtIO PCI interface. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/module.h>

#include <machine/cpu.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <bus/pci/pcivar.h>
#include <bus/pci/pcireg.h>

#include <dev/virtual/virtio/virtio/virtio_v1.h>
#include <dev/virtual/virtio/virtio/virtqueue_v1.h>
#include <dev/virtual/virtio/pci/virtio_pci_v1.h>
#include <dev/virtual/virtio/pci/virtio_pci_v1_modern_var.h>

#include "virtio_v1_bus_if.h"
#include "virtio_pci_v1_if.h"
#include "virtio_v1_if.h"

#define MPASS(_expr) KASSERT((_expr), ("virtio-pci modern assertion"))

struct vtpci_v1_modern_resource_map {
	struct resource		*vtrm_res;
	int			vtrm_cap_offset;
	int			vtrm_bar;
	int			vtrm_offset;
	int			vtrm_length;
	int			vtrm_type;	/* SYS_RES_{MEMORY, IOPORT} */
};

struct vtpci_v1_modern_bar_resource {
	struct resource		*vtbr_res;
	int			 vtbr_type;
};

struct vtpci_v1_modern_softc {
	device_t			 vtpci_v1_dev;
	struct vtpci_v1_common		 vtpci_v1_common;
	uint32_t			 vtpci_v1_notify_offset_multiplier;
	uint16_t			 vtpci_v1_devid;
	int				 vtpci_v1_msix_bar;
	struct resource			*vtpci_v1_msix_res;

	struct vtpci_v1_modern_resource_map vtpci_v1_common_res_map;
	struct vtpci_v1_modern_resource_map vtpci_v1_notify_res_map;
	struct vtpci_v1_modern_resource_map vtpci_v1_isr_res_map;
	struct vtpci_v1_modern_resource_map vtpci_v1_device_res_map;

#define VTPCI_MODERN_MAX_BARS		6
	struct vtpci_v1_modern_bar_resource vtpci_v1_bar_res[VTPCI_MODERN_MAX_BARS];
};

static int	vtpci_v1_modern_probe(device_t);
static int	vtpci_v1_modern_attach(device_t);
static int	vtpci_v1_modern_detach(device_t);
static int	vtpci_v1_modern_suspend(device_t);
static int	vtpci_v1_modern_resume(device_t);
static int	vtpci_v1_modern_shutdown(device_t);

static void	vtpci_v1_modern_driver_added(device_t, driver_t *);
static void	vtpci_v1_modern_child_detached(device_t, device_t);
static int	vtpci_v1_modern_read_ivar(device_t, device_t, int, uintptr_t *);
static int	vtpci_v1_modern_write_ivar(device_t, device_t, int, uintptr_t);

static uint8_t	vtpci_v1_modern_read_isr(device_t);
static uint16_t	vtpci_v1_modern_get_vq_size(device_t, int);
static bus_size_t vtpci_v1_modern_get_vq_notify_off(device_t, int);
static void	vtpci_v1_modern_set_vq(device_t, struct virtqueue *);
static void	vtpci_v1_modern_disable_vq(device_t, int);
static int	vtpci_v1_modern_register_msix(struct vtpci_v1_modern_softc *, int,
		    struct vtpci_interrupt *);
static int	vtpci_v1_modern_register_cfg_msix(device_t,
		    struct vtpci_interrupt *);
static int	vtpci_v1_modern_register_vq_msix(device_t, int idx,
		    struct vtpci_interrupt *);

static uint64_t	vtpci_v1_modern_negotiate_features(device_t, uint64_t);
static int	vtpci_v1_modern_finalize_features(device_t);
static bool	vtpci_v1_modern_with_feature(device_t, uint64_t);
static int	vtpci_v1_modern_alloc_virtqueues(device_t, int,
		    struct vq_alloc_info *);
static int	vtpci_v1_modern_setup_interrupts(device_t, int);
static void	vtpci_v1_modern_stop(device_t);
static int	vtpci_v1_modern_reinit(device_t, uint64_t);
static void	vtpci_v1_modern_reinit_complete(device_t);
static void	vtpci_v1_modern_notify_vq(device_t, uint16_t, bus_size_t);
static int	vtpci_v1_modern_config_generation(device_t);
static void	vtpci_v1_modern_read_dev_config(device_t, bus_size_t, void *, int);
static void	vtpci_v1_modern_write_dev_config(device_t, bus_size_t, const void *, int);

static int	vtpci_v1_modern_probe_configs(device_t);
static int	vtpci_v1_modern_find_cap(device_t, uint8_t, int *);
static int	vtpci_v1_modern_map_configs(struct vtpci_v1_modern_softc *);
static void	vtpci_v1_modern_unmap_configs(struct vtpci_v1_modern_softc *);
static int	vtpci_v1_modern_find_cap_resource(struct vtpci_v1_modern_softc *,
		     uint8_t, int, int, struct vtpci_v1_modern_resource_map *);
static int	vtpci_v1_modern_bar_type(struct vtpci_v1_modern_softc *, int);
static struct resource *vtpci_v1_modern_get_bar_resource(
		    struct vtpci_v1_modern_softc *, int, int);
static struct resource *vtpci_v1_modern_alloc_bar_resource(
		    struct vtpci_v1_modern_softc *, int, int);
static void	vtpci_v1_modern_free_bar_resources(struct vtpci_v1_modern_softc *);
static int	vtpci_v1_modern_alloc_resource_map(struct vtpci_v1_modern_softc *,
		    struct vtpci_v1_modern_resource_map *);
static void	vtpci_v1_modern_free_resource_map(struct vtpci_v1_modern_softc *,
		    struct vtpci_v1_modern_resource_map *);
static void	vtpci_v1_modern_alloc_msix_resource(struct vtpci_v1_modern_softc *);
static void	vtpci_v1_modern_free_msix_resource(struct vtpci_v1_modern_softc *);

static void	vtpci_v1_modern_probe_and_attach_child(struct vtpci_v1_modern_softc *);

static uint64_t vtpci_v1_modern_read_features(struct vtpci_v1_modern_softc *);
static void	vtpci_v1_modern_write_features(struct vtpci_v1_modern_softc *,
		    uint64_t);
static void	vtpci_v1_modern_select_virtqueue(struct vtpci_v1_modern_softc *, int);
static uint8_t	vtpci_v1_modern_get_status(struct vtpci_v1_modern_softc *);
static void	vtpci_v1_modern_set_status(struct vtpci_v1_modern_softc *, uint8_t);
static void	vtpci_v1_modern_reset(struct vtpci_v1_modern_softc *);
static void	vtpci_v1_modern_enable_virtqueues(struct vtpci_v1_modern_softc *);

static uint8_t	vtpci_v1_modern_read_common_1(struct vtpci_v1_modern_softc *,
		    bus_size_t);
static uint16_t vtpci_v1_modern_read_common_2(struct vtpci_v1_modern_softc *,
		    bus_size_t);
static uint32_t vtpci_v1_modern_read_common_4(struct vtpci_v1_modern_softc *,
		    bus_size_t);
static void	vtpci_v1_modern_write_common_1(struct vtpci_v1_modern_softc *,
		     bus_size_t, uint8_t);
static void	vtpci_v1_modern_write_common_2(struct vtpci_v1_modern_softc *,
		     bus_size_t, uint16_t);
static void	vtpci_v1_modern_write_common_4(struct vtpci_v1_modern_softc *,
		    bus_size_t, uint32_t);
static void	vtpci_v1_modern_write_common_8(struct vtpci_v1_modern_softc *,
		    bus_size_t, uint64_t);
static void	vtpci_v1_modern_write_notify_2(struct vtpci_v1_modern_softc *,
		    bus_size_t, uint16_t);
static uint8_t  vtpci_v1_modern_read_isr_1(struct vtpci_v1_modern_softc *,
		    bus_size_t);
static uint8_t	vtpci_v1_modern_read_device_1(struct vtpci_v1_modern_softc *,
		    bus_size_t);
static uint16_t vtpci_v1_modern_read_device_2(struct vtpci_v1_modern_softc *,
		    bus_size_t);
static uint32_t vtpci_v1_modern_read_device_4(struct vtpci_v1_modern_softc *,
		    bus_size_t);
static uint64_t vtpci_v1_modern_read_device_8(struct vtpci_v1_modern_softc *,
		    bus_size_t);
static void	vtpci_v1_modern_write_device_1(struct vtpci_v1_modern_softc *,
		    bus_size_t, uint8_t);
static void	vtpci_v1_modern_write_device_2(struct vtpci_v1_modern_softc *,
		    bus_size_t, uint16_t);
static void	vtpci_v1_modern_write_device_4(struct vtpci_v1_modern_softc *,
		    bus_size_t, uint32_t);
static void	vtpci_v1_modern_write_device_8(struct vtpci_v1_modern_softc *,
		    bus_size_t, uint64_t);

/* This parent intentionally accepts modern PCI IDs only. */

static device_method_t vtpci_v1_modern_methods[] = {
	/* Device interface. */
	DEVMETHOD(device_probe,			vtpci_v1_modern_probe),
	DEVMETHOD(device_attach,		vtpci_v1_modern_attach),
	DEVMETHOD(device_detach,		vtpci_v1_modern_detach),
	DEVMETHOD(device_suspend,		vtpci_v1_modern_suspend),
	DEVMETHOD(device_resume,		vtpci_v1_modern_resume),
	DEVMETHOD(device_shutdown,		vtpci_v1_modern_shutdown),

	/* Bus interface. */
	DEVMETHOD(bus_driver_added,		vtpci_v1_modern_driver_added),
	DEVMETHOD(bus_child_detached,		vtpci_v1_modern_child_detached),
	DEVMETHOD(bus_read_ivar,		vtpci_v1_modern_read_ivar),
	DEVMETHOD(bus_write_ivar,		vtpci_v1_modern_write_ivar),

	/* VirtIO PCI interface. */
	DEVMETHOD(virtio_pci_v1_read_isr,		 vtpci_v1_modern_read_isr),
	DEVMETHOD(virtio_pci_v1_get_vq_size,	 vtpci_v1_modern_get_vq_size),
	DEVMETHOD(virtio_pci_v1_get_vq_notify_off,	 vtpci_v1_modern_get_vq_notify_off),
	DEVMETHOD(virtio_pci_v1_set_vq,		 vtpci_v1_modern_set_vq),
	DEVMETHOD(virtio_pci_v1_disable_vq,	 vtpci_v1_modern_disable_vq),
	DEVMETHOD(virtio_pci_v1_register_cfg_msix,	 vtpci_v1_modern_register_cfg_msix),
	DEVMETHOD(virtio_pci_v1_register_vq_msix,	 vtpci_v1_modern_register_vq_msix),

	/* VirtIO bus interface. */
	DEVMETHOD(virtio_v1_bus_negotiate_features,  vtpci_v1_modern_negotiate_features),
	DEVMETHOD(virtio_v1_bus_finalize_features,	  vtpci_v1_modern_finalize_features),
	DEVMETHOD(virtio_v1_bus_with_feature,	  vtpci_v1_modern_with_feature),
	DEVMETHOD(virtio_v1_bus_alloc_virtqueues,	  vtpci_v1_modern_alloc_virtqueues),
	DEVMETHOD(virtio_v1_bus_setup_intr,	  vtpci_v1_modern_setup_interrupts),
	DEVMETHOD(virtio_v1_bus_stop,		  vtpci_v1_modern_stop),
	DEVMETHOD(virtio_v1_bus_reinit,		  vtpci_v1_modern_reinit),
	DEVMETHOD(virtio_v1_bus_reinit_complete,	  vtpci_v1_modern_reinit_complete),
	DEVMETHOD(virtio_v1_bus_notify_vq,		  vtpci_v1_modern_notify_vq),
	DEVMETHOD(virtio_v1_bus_config_generation,	  vtpci_v1_modern_config_generation),
	DEVMETHOD(virtio_v1_bus_read_device_config,  vtpci_v1_modern_read_dev_config),
	DEVMETHOD(virtio_v1_bus_write_device_config, vtpci_v1_modern_write_dev_config),

	DEVMETHOD_END
};

static driver_t vtpci_v1_modern_driver = {
	.name = "virtio_pci_modern",
	.methods = vtpci_v1_modern_methods,
	.size = sizeof(struct vtpci_v1_modern_softc)
};

static devclass_t virtio_pci_modern_modern_devclass;
DRIVER_MODULE(virtio_pci_modern, pci, vtpci_v1_modern_driver,
    virtio_pci_modern_modern_devclass, 0, 0);

static int
vtpci_v1_modern_probe(device_t dev)
{
	const char *name;
	uint16_t devid;

	if (pci_get_vendor(dev) != VIRTIO_PCI_VENDORID)
		return (ENXIO);

	if (pci_get_device(dev) < VIRTIO_PCI_DEVICEID_MIN ||
	    pci_get_device(dev) > VIRTIO_PCI_DEVICEID_MODERN_MAX)
		return (ENXIO);

	if (pci_get_device(dev) < VIRTIO_PCI_DEVICEID_MODERN_MIN)
		return (ENXIO);
	devid = pci_get_device(dev) - VIRTIO_PCI_DEVICEID_MODERN_MIN;

	if (vtpci_v1_modern_probe_configs(dev) != 0)
		return (ENXIO);

	name = virtio_v1_device_name(devid);
	if (name == NULL)
		name = "Unknown";

	device_set_desc(dev, "VirtIO PCI (modern) adapter");

	return (BUS_PROBE_DEFAULT);
}

static int
vtpci_v1_modern_attach(device_t dev)
{
	struct vtpci_v1_modern_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->vtpci_v1_dev = dev;
	vtpci_v1_init(&sc->vtpci_v1_common, dev, true);

	sc->vtpci_v1_devid = pci_get_device(dev) -
	    VIRTIO_PCI_DEVICEID_MODERN_MIN;

	error = vtpci_v1_modern_map_configs(sc);
	if (error) {
		device_printf(dev, "cannot map configs\n");
		vtpci_v1_modern_unmap_configs(sc);
		return (error);
	}

	vtpci_v1_modern_reset(sc);

	/* Tell the host we've noticed this device. */
	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_ACK);

	error = vtpci_v1_add_child(&sc->vtpci_v1_common);
	if (error)
		goto fail;

	vtpci_v1_modern_probe_and_attach_child(sc);

	return (0);

fail:
	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_FAILED);
	vtpci_v1_modern_detach(dev);

	return (error);
}

static int
vtpci_v1_modern_detach(device_t dev)
{
	struct vtpci_v1_modern_softc *sc;
	int error;

	sc = device_get_softc(dev);

	error = vtpci_v1_delete_child(&sc->vtpci_v1_common);
	if (error)
		return (error);

	vtpci_v1_modern_reset(sc);
	vtpci_v1_modern_unmap_configs(sc);

	return (0);
}

static int
vtpci_v1_modern_suspend(device_t dev)
{
	return (bus_generic_suspend(dev));
}

static int
vtpci_v1_modern_resume(device_t dev)
{
	return (bus_generic_resume(dev));
}

static int
vtpci_v1_modern_shutdown(device_t dev)
{
	(void) bus_generic_shutdown(dev);
	/* Forcibly stop the host device. */
	vtpci_v1_modern_stop(dev);

	return (0);
}

static void
vtpci_v1_modern_driver_added(device_t dev, driver_t *driver)
{
	vtpci_v1_modern_probe_and_attach_child(device_get_softc(dev));
}

static void
vtpci_v1_modern_child_detached(device_t dev, device_t child)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	vtpci_v1_modern_reset(sc);
	vtpci_v1_child_detached(&sc->vtpci_v1_common);

	/* After the reset, retell the host we've noticed this device. */
	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_ACK);
}

static int
vtpci_v1_modern_read_ivar(device_t dev, device_t child, int index,
    uintptr_t *result)
{
	struct vtpci_v1_modern_softc *sc;
	struct vtpci_v1_common *cn;

	sc = device_get_softc(dev);
	cn = &sc->vtpci_v1_common;

	if (vtpci_v1_child_device(cn) != child)
		return (ENOENT);

	switch (index) {
	case VIRTIO_IVAR_DEVTYPE:
		*result = sc->vtpci_v1_devid;
		break;
	default:
		return (vtpci_v1_read_ivar(cn, index, result));
	}

	return (0);
}

static int
vtpci_v1_modern_write_ivar(device_t dev, device_t child, int index,
    uintptr_t value)
{
	struct vtpci_v1_modern_softc *sc;
	struct vtpci_v1_common *cn;

	sc = device_get_softc(dev);
	cn = &sc->vtpci_v1_common;

	if (vtpci_v1_child_device(cn) != child)
		return (ENOENT);

	switch (index) {
	default:
		return (vtpci_v1_write_ivar(cn, index, value));
	}

	return (0);
}

static uint64_t
vtpci_v1_modern_negotiate_features(device_t dev, uint64_t child_features)
{
	struct vtpci_v1_modern_softc *sc;
	uint64_t host_features, features;

	sc = device_get_softc(dev);
	host_features = vtpci_v1_modern_read_features(sc);

	/*
	 * Since the driver was added as a child of the modern PCI bus,
	 * always add the V1 flag.
	 */
	child_features |= VIRTIO_F_VERSION_1;

	/*
	 * Accept per-virtqueue reset if the device offers it: negotiating
	 * the feature carries no obligation for a driver that never uses
	 * it, while declining capability-only transport features can make
	 * strict devices refuse the entire feature set.
	 */
	child_features |= host_features & VIRTIO_F_RING_RESET;

	features = vtpci_v1_negotiate_features(&sc->vtpci_v1_common,
	    child_features, host_features);
	vtpci_v1_modern_write_features(sc, features);

	return (features);
}

static int
vtpci_v1_modern_finalize_features(device_t dev)
{
	struct vtpci_v1_modern_softc *sc;
	uint8_t status;

	sc = device_get_softc(dev);

	/*
	 * Must re-read the status after setting it to verify the negotiated
	 * features were accepted by the device.
	 */
	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_S_FEATURES_OK);

	status = vtpci_v1_modern_get_status(sc);
	if ((status & VIRTIO_CONFIG_S_FEATURES_OK) == 0) {
		device_printf(dev, "desired features were not accepted "
		    "(host %#jx, written %#jx)\n",
		    (uintmax_t)sc->vtpci_v1_common.vtpci_v1_host_features,
		    (uintmax_t)sc->vtpci_v1_common.vtpci_v1_features);
		return (ENOTSUP);
	}

	return (0);
}

static bool
vtpci_v1_modern_with_feature(device_t dev, uint64_t feature)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	return (vtpci_v1_with_feature(&sc->vtpci_v1_common, feature));
}

static uint64_t
vtpci_v1_modern_read_features(struct vtpci_v1_modern_softc *sc)
{
	uint32_t features0, features1;

	vtpci_v1_modern_write_common_4(sc, VIRTIO_PCI_COMMON_DFSELECT, 0);
	features0 = vtpci_v1_modern_read_common_4(sc, VIRTIO_PCI_COMMON_DF);
	vtpci_v1_modern_write_common_4(sc, VIRTIO_PCI_COMMON_DFSELECT, 1);
	features1 = vtpci_v1_modern_read_common_4(sc, VIRTIO_PCI_COMMON_DF);

	return (((uint64_t) features1 << 32) | features0);
}

static void
vtpci_v1_modern_write_features(struct vtpci_v1_modern_softc *sc, uint64_t features)
{
	uint32_t features0, features1;

	features0 = features;
	features1 = features >> 32;

	vtpci_v1_modern_write_common_4(sc, VIRTIO_PCI_COMMON_GFSELECT, 0);
	vtpci_v1_modern_write_common_4(sc, VIRTIO_PCI_COMMON_GF, features0);
	vtpci_v1_modern_write_common_4(sc, VIRTIO_PCI_COMMON_GFSELECT, 1);
	vtpci_v1_modern_write_common_4(sc, VIRTIO_PCI_COMMON_GF, features1);
}

static int
vtpci_v1_modern_alloc_virtqueues(device_t dev, int nvqs,
    struct vq_alloc_info *vq_info)
{
	struct vtpci_v1_modern_softc *sc;
	struct vtpci_v1_common *cn;
	uint16_t max_nvqs;

	sc = device_get_softc(dev);
	cn = &sc->vtpci_v1_common;

	max_nvqs = vtpci_v1_modern_read_common_2(sc, VIRTIO_PCI_COMMON_NUMQ);
	if (nvqs > max_nvqs) {
		device_printf(sc->vtpci_v1_dev, "requested virtqueue count %d "
		    "exceeds max %d\n", nvqs, max_nvqs);
		return (E2BIG);
	}

	return (vtpci_v1_alloc_virtqueues(cn, nvqs, vq_info));
}

static int
vtpci_v1_modern_setup_interrupts(device_t dev, int type)
{
	struct vtpci_v1_modern_softc *sc;
	int error;

	sc = device_get_softc(dev);

	error = vtpci_v1_setup_interrupts(&sc->vtpci_v1_common, type);
	if (error == 0)
		vtpci_v1_modern_enable_virtqueues(sc);

	return (error);
}

static void
vtpci_v1_modern_stop(device_t dev)
{
	vtpci_v1_modern_reset(device_get_softc(dev));
}

static int
vtpci_v1_modern_reinit(device_t dev, uint64_t features)
{
	struct vtpci_v1_modern_softc *sc;
	struct vtpci_v1_common *cn;
	int error;

	sc = device_get_softc(dev);
	cn = &sc->vtpci_v1_common;

	/*
	 * Redrive the device initialization. This is a bit of an abuse of
	 * the specification, but VirtualBox, QEMU/KVM, and BHyVe seem to
	 * play nice.
	 *
	 * We do not allow the host device to change from what was originally
	 * negotiated beyond what the guest driver changed. MSIX state should
	 * not change, number of virtqueues and their size remain the same, etc.
	 * This will need to be rethought when we want to support migration.
	 */

	if (vtpci_v1_modern_get_status(sc) != VIRTIO_CONFIG_STATUS_RESET)
		vtpci_v1_modern_stop(dev);

	/*
	 * Quickly drive the status through ACK and DRIVER. The device does
	 * not become usable again until DRIVER_OK in reinit complete.
	 */
	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_ACK);
	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_DRIVER);

	/*
	 * TODO: Check that features are not added as to what was
	 * originally negotiated.
	 */
	vtpci_v1_modern_negotiate_features(dev, features);
	error = vtpci_v1_modern_finalize_features(dev);
	if (error) {
		device_printf(dev, "cannot finalize features during reinit\n");
		return (error);
	}

	error = vtpci_v1_reinit(cn);
	if (error)
		return (error);

	return (0);
}

static void
vtpci_v1_modern_reinit_complete(device_t dev)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	vtpci_v1_modern_enable_virtqueues(sc);
	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_DRIVER_OK);
}

static void
vtpci_v1_modern_notify_vq(device_t dev, uint16_t queue, bus_size_t offset)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	vtpci_v1_modern_write_notify_2(sc, offset, queue);
}

static uint8_t
vtpci_v1_modern_get_status(struct vtpci_v1_modern_softc *sc)
{
	return (vtpci_v1_modern_read_common_1(sc, VIRTIO_PCI_COMMON_STATUS));
}

static void
vtpci_v1_modern_set_status(struct vtpci_v1_modern_softc *sc, uint8_t status)
{
	if (status != VIRTIO_CONFIG_STATUS_RESET)
		status |= vtpci_v1_modern_get_status(sc);

	vtpci_v1_modern_write_common_1(sc, VIRTIO_PCI_COMMON_STATUS, status);
}

static int
vtpci_v1_modern_config_generation(device_t dev)
{
	struct vtpci_v1_modern_softc *sc;
	uint8_t gen;

	sc = device_get_softc(dev);
	gen = vtpci_v1_modern_read_common_1(sc, VIRTIO_PCI_COMMON_CFGGENERATION);

	return (gen);
}

static void
vtpci_v1_modern_read_dev_config(device_t dev, bus_size_t offset, void *dst,
    int length)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	if (sc->vtpci_v1_device_res_map.vtrm_res == NULL) {
		panic("%s: attempt to read dev config but not present",
		    __func__);
	}

	switch (length) {
	case 1:
		*(uint8_t *) dst = vtpci_v1_modern_read_device_1(sc, offset);
		break;
	case 2:
		*(uint16_t *) dst = vtpci_v1_modern_read_device_2(sc, offset);
		break;
	case 4:
		*(uint32_t *) dst = vtpci_v1_modern_read_device_4(sc, offset);
		break;
	case 8:
		*(uint64_t *) dst = vtpci_v1_modern_read_device_8(sc, offset);
		break;
	default:
		panic("%s: device %s invalid device read length %d offset %d",
		    __func__, device_get_nameunit(dev), length, (int) offset);
	}
}

static void
vtpci_v1_modern_write_dev_config(device_t dev, bus_size_t offset, const void *src,
    int length)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	if (sc->vtpci_v1_device_res_map.vtrm_res == NULL) {
		panic("%s: attempt to write dev config but not present",
		    __func__);
	}

	switch (length) {
	case 1:
		vtpci_v1_modern_write_device_1(sc, offset, *(const uint8_t *) src);
		break;
	case 2: {
		uint16_t val = *(const uint16_t *) src;
		vtpci_v1_modern_write_device_2(sc, offset, val);
		break;
	}
	case 4: {
		uint32_t val = *(const uint32_t *) src;
		vtpci_v1_modern_write_device_4(sc, offset, val);
		break;
	}
	case 8: {
		uint64_t val = *(const uint64_t *) src;
		vtpci_v1_modern_write_device_8(sc, offset, val);
		break;
	}
	default:
		panic("%s: device %s invalid device write length %d offset %d",
		    __func__, device_get_nameunit(dev), length, (int) offset);
	}
}

static int
vtpci_v1_modern_probe_configs(device_t dev)
{
	int error;

	/*
	 * These config capabilities must be present. The DEVICE_CFG
	 * capability is only present if the device requires it.
	 */

	error = vtpci_v1_modern_find_cap(dev, VIRTIO_PCI_CAP_COMMON_CFG, NULL);
	if (error) {
		device_printf(dev, "cannot find COMMON_CFG capability\n");
		return (error);
	}

	error = vtpci_v1_modern_find_cap(dev, VIRTIO_PCI_CAP_NOTIFY_CFG, NULL);
	if (error) {
		device_printf(dev, "cannot find NOTIFY_CFG capability\n");
		return (error);
	}

	error = vtpci_v1_modern_find_cap(dev, VIRTIO_PCI_CAP_ISR_CFG, NULL);
	if (error) {
		device_printf(dev, "cannot find ISR_CFG capability\n");
		return (error);
	}

	return (0);
}

static int
vtpci_v1_modern_find_cap(device_t dev, uint8_t cfg_type, int *cap_offset)
{
	uint32_t type, bar;
	int capreg, next, i;

	capreg = pci_read_config(dev, PCIR_CAP_PTR, 1);
	for (i = 0; capreg >= 0x40 && i < 48; i++) {
		type = pci_read_config(dev, capreg + PCICAP_ID, 1);
		next = pci_read_config(dev, capreg + PCICAP_NEXTPTR, 1);

		if (type == PCIY_VENDOR) {
			bar = pci_read_config(dev, capreg + offsetof(struct virtio_pci_cap, bar), 1);
			if (bar < VTPCI_MODERN_MAX_BARS &&
			    pci_read_config(dev, capreg + offsetof(struct virtio_pci_cap, cfg_type), 1) == cfg_type) {
				if (cap_offset != NULL)
					*cap_offset = capreg;
				return (0);
			}
		}

		if (next == 0 || next == capreg)
			break;
		capreg = next;
	}

	return (ENOENT);
}

static int
vtpci_v1_modern_map_common_config(struct vtpci_v1_modern_softc *sc)
{
	device_t dev;
	int error;

	dev = sc->vtpci_v1_dev;

	error = vtpci_v1_modern_find_cap_resource(sc, VIRTIO_PCI_CAP_COMMON_CFG,
	    sizeof(struct virtio_pci_common_cfg), 4, &sc->vtpci_v1_common_res_map);
	if (error) {
		device_printf(dev, "cannot find cap COMMON_CFG resource\n");
		return (error);
	}

	error = vtpci_v1_modern_alloc_resource_map(sc, &sc->vtpci_v1_common_res_map);
	if (error) {
		device_printf(dev, "cannot alloc resource for COMMON_CFG\n");
		return (error);
	}

	return (0);
}

static int
vtpci_v1_modern_map_notify_config(struct vtpci_v1_modern_softc *sc)
{
	device_t dev;
	int cap_offset, error;

	dev = sc->vtpci_v1_dev;

	error = vtpci_v1_modern_find_cap_resource(sc, VIRTIO_PCI_CAP_NOTIFY_CFG,
	    -1, 2, &sc->vtpci_v1_notify_res_map);
	if (error) {
		device_printf(dev, "cannot find cap NOTIFY_CFG resource\n");
		return (error);
	}

	cap_offset = sc->vtpci_v1_notify_res_map.vtrm_cap_offset;

	sc->vtpci_v1_notify_offset_multiplier = pci_read_config(dev, cap_offset +
	    offsetof(struct virtio_pci_notify_cap, notify_off_multiplier), 4);

	error = vtpci_v1_modern_alloc_resource_map(sc, &sc->vtpci_v1_notify_res_map);
	if (error) {
		device_printf(dev, "cannot alloc resource for NOTIFY_CFG\n");
		return (error);
	}

	return (0);
}

static int
vtpci_v1_modern_map_isr_config(struct vtpci_v1_modern_softc *sc)
{
	device_t dev;
	int error;

	dev = sc->vtpci_v1_dev;

	error = vtpci_v1_modern_find_cap_resource(sc, VIRTIO_PCI_CAP_ISR_CFG,
	    sizeof(uint8_t), 1, &sc->vtpci_v1_isr_res_map);
	if (error) {
		device_printf(dev, "cannot find cap ISR_CFG resource\n");
		return (error);
	}

	error = vtpci_v1_modern_alloc_resource_map(sc, &sc->vtpci_v1_isr_res_map);
	if (error) {
		device_printf(dev, "cannot alloc resource for ISR_CFG\n");
		return (error);
	}

	return (0);
}

static int
vtpci_v1_modern_map_device_config(struct vtpci_v1_modern_softc *sc)
{
	device_t dev;
	int error;

	dev = sc->vtpci_v1_dev;

	error = vtpci_v1_modern_find_cap_resource(sc, VIRTIO_PCI_CAP_DEVICE_CFG,
	    -1, 4, &sc->vtpci_v1_device_res_map);
	if (error == ENOENT) {
		/* Device configuration is optional depending on device. */
		return (0);
	} else if (error) {
		device_printf(dev, "cannot find cap DEVICE_CFG resource\n");
		return (error);
	}

	error = vtpci_v1_modern_alloc_resource_map(sc, &sc->vtpci_v1_device_res_map);
	if (error) {
		device_printf(dev, "cannot alloc resource for DEVICE_CFG\n");
		return (error);
	}

	return (0);
}

static int
vtpci_v1_modern_map_configs(struct vtpci_v1_modern_softc *sc)
{
	int error;

	error = vtpci_v1_modern_map_common_config(sc);
	if (error)
		return (error);

	error = vtpci_v1_modern_map_notify_config(sc);
	if (error)
		return (error);

	error = vtpci_v1_modern_map_isr_config(sc);
	if (error)
		return (error);

	error = vtpci_v1_modern_map_device_config(sc);
	if (error)
		return (error);

	vtpci_v1_modern_alloc_msix_resource(sc);

	return (0);
}

static void
vtpci_v1_modern_unmap_configs(struct vtpci_v1_modern_softc *sc)
{

	vtpci_v1_modern_free_resource_map(sc, &sc->vtpci_v1_common_res_map);
	vtpci_v1_modern_free_resource_map(sc, &sc->vtpci_v1_notify_res_map);
	vtpci_v1_modern_free_resource_map(sc, &sc->vtpci_v1_isr_res_map);
	vtpci_v1_modern_free_resource_map(sc, &sc->vtpci_v1_device_res_map);

	vtpci_v1_modern_free_bar_resources(sc);
	vtpci_v1_modern_free_msix_resource(sc);

	sc->vtpci_v1_notify_offset_multiplier = 0;
}

static int
vtpci_v1_modern_find_cap_resource(struct vtpci_v1_modern_softc *sc, uint8_t cfg_type,
    int min_size, int alignment, struct vtpci_v1_modern_resource_map *res)
{
	device_t dev;
	int cap_offset, offset, length, error;
	uint8_t bar, cap_length;

	dev = sc->vtpci_v1_dev;

	error = vtpci_v1_modern_find_cap(dev, cfg_type, &cap_offset);
	if (error)
		return (error);

	cap_length = pci_read_config(dev,
	    cap_offset + offsetof(struct virtio_pci_cap, cap_len), 1);

	if (cap_length < sizeof(struct virtio_pci_cap)) {
		device_printf(dev, "cap %u length %d less than expected\n",
		    cfg_type, cap_length);
		return (ENXIO);
	}

	bar = pci_read_config(dev,
	    cap_offset + offsetof(struct virtio_pci_cap, bar), 1);
	offset = pci_read_config(dev,
	    cap_offset + offsetof(struct virtio_pci_cap, offset), 4);
	length = pci_read_config(dev,
	    cap_offset + offsetof(struct virtio_pci_cap, length), 4);

	if (min_size != -1 && length < min_size) {
		device_printf(dev, "cap %u struct length %d less than min %d\n",
		    cfg_type, length, min_size);
		return (ENXIO);
	}

	if (offset % alignment) {
		device_printf(dev, "cap %u struct offset %d not aligned to %d\n",
		    cfg_type, offset, alignment);
		return (ENXIO);
	}

	/* BMV: TODO Can we determine the size of the BAR here? */

	res->vtrm_cap_offset = cap_offset;
	res->vtrm_bar = bar;
	res->vtrm_offset = offset;
	res->vtrm_length = length;
	res->vtrm_type = vtpci_v1_modern_bar_type(sc, bar);

	return (0);
}

static int
vtpci_v1_modern_bar_type(struct vtpci_v1_modern_softc *sc, int bar)
{
	uint32_t val;

	/*
	 * The BAR described by a config capability may be either an IOPORT or
	 * MEM, but we must know the type when calling bus_alloc_resource().
	 */
	val = pci_read_config(sc->vtpci_v1_dev, PCIR_BAR(bar), 4);
	if (PCI_BAR_IO(val))
		return (SYS_RES_IOPORT);
	else
		return (SYS_RES_MEMORY);
}

static struct resource *
vtpci_v1_modern_get_bar_resource(struct vtpci_v1_modern_softc *sc, int bar, int type)
{
	struct resource *res;

	MPASS(bar >= 0 && bar < VTPCI_MODERN_MAX_BARS);
	res = sc->vtpci_v1_bar_res[bar].vtbr_res;
	MPASS(res == NULL || sc->vtpci_v1_bar_res[bar].vtbr_type == type);

	return (res);
}

static struct resource *
vtpci_v1_modern_alloc_bar_resource(struct vtpci_v1_modern_softc *sc, int bar,
    int type)
{
	struct resource *res;
	int rid;

	MPASS(bar >= 0 && bar < VTPCI_MODERN_MAX_BARS);
	MPASS(type == SYS_RES_MEMORY || type == SYS_RES_IOPORT);

	res = sc->vtpci_v1_bar_res[bar].vtbr_res;
	if (res != NULL) {
		MPASS(sc->vtpci_v1_bar_res[bar].vtbr_type == type);
		return (res);
	}

	rid = PCIR_BAR(bar);
	res = bus_alloc_resource_any(sc->vtpci_v1_dev, type, &rid,
	    RF_ACTIVE);
	if (res != NULL) {
		sc->vtpci_v1_bar_res[bar].vtbr_res = res;
		sc->vtpci_v1_bar_res[bar].vtbr_type = type;
	}

	return (res);
}

static void
vtpci_v1_modern_free_bar_resources(struct vtpci_v1_modern_softc *sc)
{
	device_t dev;
	struct resource *res;
	int bar, rid, type;

	dev = sc->vtpci_v1_dev;

	for (bar = 0; bar < VTPCI_MODERN_MAX_BARS; bar++) {
		res = sc->vtpci_v1_bar_res[bar].vtbr_res;
		type = sc->vtpci_v1_bar_res[bar].vtbr_type;

		if (res != NULL) {
			rid = PCIR_BAR(bar);
			bus_release_resource(dev, type, rid, res);
			sc->vtpci_v1_bar_res[bar].vtbr_res = NULL;
			sc->vtpci_v1_bar_res[bar].vtbr_type = 0;
		}
	}
}

static int
vtpci_v1_modern_alloc_resource_map(struct vtpci_v1_modern_softc *sc,
    struct vtpci_v1_modern_resource_map *map)
{
	struct resource *res;
	int type;

	type = map->vtrm_type;

	res = vtpci_v1_modern_alloc_bar_resource(sc, map->vtrm_bar, type);
	if (res == NULL)
		return (ENXIO);

	map->vtrm_res = res;
	return (0);
}

static void
vtpci_v1_modern_free_resource_map(struct vtpci_v1_modern_softc *sc,
    struct vtpci_v1_modern_resource_map *map)
{
	struct resource *res;
	int type;

	type = map->vtrm_type;
	res = vtpci_v1_modern_get_bar_resource(sc, map->vtrm_bar, type);

	if (res != NULL && map->vtrm_res != NULL)
		map->vtrm_res = NULL;
}

static void
vtpci_v1_modern_alloc_msix_resource(struct vtpci_v1_modern_softc *sc)
{
	(void)sc;
}

static void
vtpci_v1_modern_free_msix_resource(struct vtpci_v1_modern_softc *sc)
{
	device_t dev;

	dev = sc->vtpci_v1_dev;

	if (sc->vtpci_v1_msix_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->vtpci_v1_msix_bar,
		    sc->vtpci_v1_msix_res);
		sc->vtpci_v1_msix_bar = 0;
		sc->vtpci_v1_msix_res = NULL;
	}
}

static void
vtpci_v1_modern_probe_and_attach_child(struct vtpci_v1_modern_softc *sc)
{
	device_t dev, child;

	dev = sc->vtpci_v1_dev;
	child = vtpci_v1_child_device(&sc->vtpci_v1_common);

	if (child == NULL || device_get_state(child) != DS_NOTPRESENT)
		return;

	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_DRIVER);

	if (device_probe_and_attach(child) != 0) {
		vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_FAILED);
		/* Reset state for later attempt. */
		vtpci_v1_modern_child_detached(dev, child);
	} else {
		vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_DRIVER_OK);
		VIRTIO_V1_ATTACH_COMPLETED(child);
	}
}

static int
vtpci_v1_modern_register_msix(struct vtpci_v1_modern_softc *sc, int offset,
    struct vtpci_interrupt *intr)
{
	uint16_t vector;

	if (intr != NULL) {
		/* Map from guest rid to host vector. */
		vector = intr->vti_rid - 1;
	} else
		vector = VIRTIO_MSI_NO_VECTOR;

	vtpci_v1_modern_write_common_2(sc, offset, vector);
	return (vtpci_v1_modern_read_common_2(sc, offset) == vector ? 0 : ENODEV);
}

static int
vtpci_v1_modern_register_cfg_msix(device_t dev, struct vtpci_interrupt *intr)
{
	struct vtpci_v1_modern_softc *sc;
	int error;

	sc = device_get_softc(dev);

	error = vtpci_v1_modern_register_msix(sc, VIRTIO_PCI_COMMON_MSIX, intr);
	if (error) {
		device_printf(dev,
		    "unable to register config MSIX interrupt\n");
		return (error);
	}

	return (0);
}

static int
vtpci_v1_modern_register_vq_msix(device_t dev, int idx,
    struct vtpci_interrupt *intr)
{
	struct vtpci_v1_modern_softc *sc;
	int error;

	sc = device_get_softc(dev);

	vtpci_v1_modern_select_virtqueue(sc, idx);
	error = vtpci_v1_modern_register_msix(sc, VIRTIO_PCI_COMMON_Q_MSIX, intr);
	if (error) {
		device_printf(dev,
		    "unable to register virtqueue MSIX interrupt\n");
		return (error);
	}

	return (0);
}

static void
vtpci_v1_modern_reset(struct vtpci_v1_modern_softc *sc)
{
	/*
	 * Setting the status to RESET sets the host device to the
	 * original, uninitialized state. Must poll the status until
	 * the reset is complete.
	 */
	vtpci_v1_modern_set_status(sc, VIRTIO_CONFIG_STATUS_RESET);

	while (vtpci_v1_modern_get_status(sc) != VIRTIO_CONFIG_STATUS_RESET)
		cpu_pause();
}

static void
vtpci_v1_modern_select_virtqueue(struct vtpci_v1_modern_softc *sc, int idx)
{
	vtpci_v1_modern_write_common_2(sc, VIRTIO_PCI_COMMON_Q_SELECT, idx);
}

static uint8_t
vtpci_v1_modern_read_isr(device_t dev)
{
	return (vtpci_v1_modern_read_isr_1(device_get_softc(dev), 0));
}

static uint16_t
vtpci_v1_modern_get_vq_size(device_t dev, int idx)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	vtpci_v1_modern_select_virtqueue(sc, idx);
	return (vtpci_v1_modern_read_common_2(sc, VIRTIO_PCI_COMMON_Q_SIZE));
}

static bus_size_t
vtpci_v1_modern_get_vq_notify_off(device_t dev, int idx)
{
	struct vtpci_v1_modern_softc *sc;
	uint16_t q_notify_off;

	sc = device_get_softc(dev);

	vtpci_v1_modern_select_virtqueue(sc, idx);
	q_notify_off = vtpci_v1_modern_read_common_2(sc, VIRTIO_PCI_COMMON_Q_NOFF);

	return (q_notify_off * sc->vtpci_v1_notify_offset_multiplier);
}

static void
vtpci_v1_modern_set_vq(device_t dev, struct virtqueue *vq)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	vtpci_v1_modern_select_virtqueue(sc, virtio_v1_virtqueue_index(vq));

	/* BMV: Currently we never adjust the device's proposed VQ size. */
	vtpci_v1_modern_write_common_2(sc,
	    VIRTIO_PCI_COMMON_Q_SIZE, virtio_v1_virtqueue_size(vq));

	vtpci_v1_modern_write_common_8(sc,
	    VIRTIO_PCI_COMMON_Q_DESCLO, virtio_v1_virtqueue_desc_paddr(vq));
	vtpci_v1_modern_write_common_8(sc,
	    VIRTIO_PCI_COMMON_Q_AVAILLO, virtio_v1_virtqueue_avail_paddr(vq));
        vtpci_v1_modern_write_common_8(sc,
	    VIRTIO_PCI_COMMON_Q_USEDLO, virtio_v1_virtqueue_used_paddr(vq));
}

static void
vtpci_v1_modern_disable_vq(device_t dev, int idx)
{
	struct vtpci_v1_modern_softc *sc;

	sc = device_get_softc(dev);

	vtpci_v1_modern_select_virtqueue(sc, idx);
	vtpci_v1_modern_write_common_8(sc, VIRTIO_PCI_COMMON_Q_DESCLO, 0ULL);
	vtpci_v1_modern_write_common_8(sc, VIRTIO_PCI_COMMON_Q_AVAILLO, 0ULL);
        vtpci_v1_modern_write_common_8(sc, VIRTIO_PCI_COMMON_Q_USEDLO, 0ULL);
}

static void
vtpci_v1_modern_enable_virtqueues(struct vtpci_v1_modern_softc *sc)
{
	int idx;

	for (idx = 0; idx < sc->vtpci_v1_common.vtpci_v1_nvqs; idx++) {
		vtpci_v1_modern_select_virtqueue(sc, idx);
		vtpci_v1_modern_write_common_2(sc, VIRTIO_PCI_COMMON_Q_ENABLE, 1);
	}
}

static uint8_t
vtpci_v1_modern_read_common_1(struct vtpci_v1_modern_softc *sc, bus_size_t off)
{
	return (bus_read_1(sc->vtpci_v1_common_res_map.vtrm_res,
	    sc->vtpci_v1_common_res_map.vtrm_offset + off));
}

static uint16_t
vtpci_v1_modern_read_common_2(struct vtpci_v1_modern_softc *sc, bus_size_t off)
{
	return bus_read_2(sc->vtpci_v1_common_res_map.vtrm_res,
	    sc->vtpci_v1_common_res_map.vtrm_offset + off);
}

static uint32_t
vtpci_v1_modern_read_common_4(struct vtpci_v1_modern_softc *sc, bus_size_t off)
{
	return bus_read_4(sc->vtpci_v1_common_res_map.vtrm_res,
	    sc->vtpci_v1_common_res_map.vtrm_offset + off);
}

static void
vtpci_v1_modern_write_common_1(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint8_t val)
{
	bus_write_1(sc->vtpci_v1_common_res_map.vtrm_res,
	    sc->vtpci_v1_common_res_map.vtrm_offset + off, val);
}

static void
vtpci_v1_modern_write_common_2(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint16_t val)
{
	bus_write_2(sc->vtpci_v1_common_res_map.vtrm_res,
	    sc->vtpci_v1_common_res_map.vtrm_offset + off, val);
}

static void
vtpci_v1_modern_write_common_4(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint32_t val)
{
	bus_write_4(sc->vtpci_v1_common_res_map.vtrm_res,
	    sc->vtpci_v1_common_res_map.vtrm_offset + off, val);
}

static void
vtpci_v1_modern_write_common_8(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint64_t val)
{
	uint32_t val0, val1;

	val0 = (uint32_t) val;
	val1 = val >> 32;

	vtpci_v1_modern_write_common_4(sc, off, val0);
	vtpci_v1_modern_write_common_4(sc, off + 4, val1);
}

static void
vtpci_v1_modern_write_notify_2(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint16_t val)
{
	bus_write_2(sc->vtpci_v1_notify_res_map.vtrm_res,
	    sc->vtpci_v1_notify_res_map.vtrm_offset + off, val);
}

static uint8_t
vtpci_v1_modern_read_isr_1(struct vtpci_v1_modern_softc *sc, bus_size_t off)
{
	return (bus_read_1(sc->vtpci_v1_isr_res_map.vtrm_res,
	    sc->vtpci_v1_isr_res_map.vtrm_offset + off));
}

static uint8_t
vtpci_v1_modern_read_device_1(struct vtpci_v1_modern_softc *sc, bus_size_t off)
{
	return (bus_read_1(sc->vtpci_v1_device_res_map.vtrm_res,
	    sc->vtpci_v1_device_res_map.vtrm_offset + off));
}

static uint16_t
vtpci_v1_modern_read_device_2(struct vtpci_v1_modern_softc *sc, bus_size_t off)
{
	return (bus_read_2(sc->vtpci_v1_device_res_map.vtrm_res,
	    sc->vtpci_v1_device_res_map.vtrm_offset + off));
}

static uint32_t
vtpci_v1_modern_read_device_4(struct vtpci_v1_modern_softc *sc, bus_size_t off)
{
	return (bus_read_4(sc->vtpci_v1_device_res_map.vtrm_res,
	    sc->vtpci_v1_device_res_map.vtrm_offset + off));
}

static uint64_t
vtpci_v1_modern_read_device_8(struct vtpci_v1_modern_softc *sc, bus_size_t off)
{
	device_t dev;
	int gen;
	uint32_t val0, val1;

	dev = sc->vtpci_v1_dev;

	/*
	 * Treat the 64-bit field as two 32-bit fields. Use the generation
	 * to ensure a consistent read.
	 */
	do {
		gen = vtpci_v1_modern_config_generation(dev);
		val0 = vtpci_v1_modern_read_device_4(sc, off);
		val1 = vtpci_v1_modern_read_device_4(sc, off + 4);
	} while (gen != vtpci_v1_modern_config_generation(dev));

	return (((uint64_t) val1 << 32) | val0);
}

static void
vtpci_v1_modern_write_device_1(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint8_t val)
{
	bus_write_1(sc->vtpci_v1_device_res_map.vtrm_res,
	    sc->vtpci_v1_device_res_map.vtrm_offset + off, val);
}

static void
vtpci_v1_modern_write_device_2(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint16_t val)
{
	bus_write_2(sc->vtpci_v1_device_res_map.vtrm_res,
	    sc->vtpci_v1_device_res_map.vtrm_offset + off, val);
}

static void
vtpci_v1_modern_write_device_4(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint32_t val)
{
	bus_write_4(sc->vtpci_v1_device_res_map.vtrm_res,
	    sc->vtpci_v1_device_res_map.vtrm_offset + off, val);
}

static void
vtpci_v1_modern_write_device_8(struct vtpci_v1_modern_softc *sc, bus_size_t off,
    uint64_t val)
{
	uint32_t val0, val1;

	val0 = (uint32_t) val;
	val1 = val >> 32;

	vtpci_v1_modern_write_device_4(sc, off, val0);
	vtpci_v1_modern_write_device_4(sc, off + 4, val1);
}
