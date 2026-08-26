/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2013, Bryan Venteicher <bryanv@FreeBSD.org>
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
 *
 * $FreeBSD: head/sys/dev/virtio/random/virtio_v1_random.c 338324 2018-08-26 12:51:46Z markm $
 */

/* Driver for VirtIO entropy device. */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/thread.h>
#include <sys/sglist.h>
#include <sys/callout.h>
#include <sys/random.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <dev/virtual/virtio/virtio/virtio_v1.h>
#include <dev/virtual/virtio/virtio/virtqueue_v1.h>

struct vtrnd_v1_softc {
	device_t		 vtrnd_v1_dev;
	uint64_t		 vtrnd_v1_features;
	struct callout		 vtrnd_v1_callout;
	struct virtqueue	*vtrnd_v1_vq;
};

static int	vtrnd_v1_modevent(module_t, int, void *);

static int	vtrnd_v1_probe(device_t);
static int	vtrnd_v1_attach(device_t);
static int	vtrnd_v1_detach(device_t);

static void	vtrnd_v1_negotiate_features(struct vtrnd_v1_softc *);
static int	vtrnd_v1_alloc_virtqueue(struct vtrnd_v1_softc *);
static void	vtrnd_v1_harvest(struct vtrnd_v1_softc *);
static void	vtrnd_v1_timer(void *);

#define VTRND_FEATURES	0

static struct virtio_feature_desc vtrnd_v1_feature_desc[] = {
	{ 0, NULL }
};

static device_method_t vtrnd_v1_methods[] = {
	/* Device methods. */
	DEVMETHOD(device_probe,		vtrnd_v1_probe),
	DEVMETHOD(device_attach,	vtrnd_v1_attach),
	DEVMETHOD(device_detach,	vtrnd_v1_detach),

	DEVMETHOD_END
};

static driver_t vtrnd_v1_driver = {
	"vtrnd_v1",
	vtrnd_v1_methods,
	sizeof(struct vtrnd_v1_softc)
};
static devclass_t vtrnd_v1_devclass;

DRIVER_MODULE(virtio_random_v1, virtio_pci_modern, vtrnd_v1_driver, vtrnd_v1_devclass,
    vtrnd_v1_modevent, NULL);
DRIVER_MODULE(virtio_random_v1, virtio_mmio_modern, vtrnd_v1_driver, vtrnd_v1_devclass,
    vtrnd_v1_modevent, NULL);
MODULE_VERSION(virtio_random_v1, 1);
MODULE_DEPEND(virtio_random_v1, virtio, 1, 1, 1);
MODULE_DEPEND(virtio_random_v1, virtio_pci, 1, 1, 1);
MODULE_DEPEND(virtio_random_v1, virtio_mmio, 1, 1, 1);

static int
vtrnd_v1_modevent(module_t mod, int type, void *unused)
{
	int error;

	switch (type) {
	case MOD_LOAD:
	case MOD_UNLOAD:
	case MOD_SHUTDOWN:
		error = 0;
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static int
vtrnd_v1_probe(device_t dev)
{

	if (virtio_v1_get_device_type(dev) != VIRTIO_ID_ENTROPY)
		return (ENXIO);

	device_set_desc(dev, "VirtIO Entropy Adapter");

	return (BUS_PROBE_DEFAULT);
}

static int
vtrnd_v1_attach(device_t dev)
{
	struct vtrnd_v1_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->vtrnd_v1_dev = dev;

	callout_init_mp(&sc->vtrnd_v1_callout);

	virtio_v1_set_feature_desc(dev, vtrnd_v1_feature_desc);
	vtrnd_v1_negotiate_features(sc);

	error = vtrnd_v1_alloc_virtqueue(sc);
	if (error) {
		device_printf(dev, "cannot allocate virtqueue\n");
		goto fail;
	}

	callout_reset(&sc->vtrnd_v1_callout, 5 * hz, vtrnd_v1_timer, sc);

fail:
	if (error)
		vtrnd_v1_detach(dev);

	return (error);
}

static int
vtrnd_v1_detach(device_t dev)
{
	struct vtrnd_v1_softc *sc;

	sc = device_get_softc(dev);

	callout_terminate(&sc->vtrnd_v1_callout);

	return (0);
}

static void
vtrnd_v1_negotiate_features(struct vtrnd_v1_softc *sc)
{
	device_t dev;
	uint64_t features;

	dev = sc->vtrnd_v1_dev;
	features = VTRND_FEATURES;

	sc->vtrnd_v1_features = virtio_v1_negotiate_features(dev, features);
	(void)virtio_v1_finalize_features(dev);
}

static int
vtrnd_v1_alloc_virtqueue(struct vtrnd_v1_softc *sc)
{
	device_t dev;
	struct vq_alloc_info vq_info;

	dev = sc->vtrnd_v1_dev;

	VQ_ALLOC_INFO_INIT(&vq_info, 0, NULL, sc, &sc->vtrnd_v1_vq,
	    "%s request", device_get_nameunit(dev));

	return (virtio_v1_alloc_virtqueues(dev, 1, &vq_info));
}

static void
vtrnd_v1_harvest(struct vtrnd_v1_softc *sc)
{
	struct sglist_seg segs[1];
	struct sglist sg;
	struct virtqueue *vq;
	uint32_t value;
	int error;

	vq = sc->vtrnd_v1_vq;

	sglist_init(&sg, 1, segs);
	error = sglist_append(&sg, &value, sizeof(value));
	KASSERT(error == 0 && sg.sg_nseg == 1,
	    ("%s: error %d adding buffer to sglist", __func__, error));

	if (!virtio_v1_virtqueue_empty(vq))
		return;
	if (virtio_v1_virtqueue_enqueue(vq, &value, &sg, 0, 1) != 0)
		return;

	/*
	 * Poll for the response, but the command is likely already
	 * done when we return from the notify.
	 */
	virtio_v1_virtqueue_notify(vq);
	virtio_v1_virtqueue_poll(vq, NULL);

	add_buffer_randomness_src((const char *)&value, sizeof(value),
	    RAND_SRC_VIRTIO);
}

static void
vtrnd_v1_timer(void *xsc)
{
	struct vtrnd_v1_softc *sc;

	sc = xsc;

	vtrnd_v1_harvest(sc);
	callout_reset(&sc->vtrnd_v1_callout, 5 * hz, vtrnd_v1_timer, sc);
}
