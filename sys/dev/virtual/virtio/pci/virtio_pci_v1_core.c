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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/module.h>
#include <sys/malloc.h>

#include <sys/bus.h>
#include <sys/rman.h>

#include <bus/pci/pcivar.h>
#include <bus/pci/pcireg.h>

#include <dev/virtual/virtio/virtio/virtio_v1.h>
#include <dev/virtual/virtio/virtio/virtqueue_v1.h>
#include <dev/virtual/virtio/pci/virtio_pci_v1.h>
#include <dev/virtual/virtio/pci/virtio_pci_v1_var.h>

#include "virtio_pci_if.h"
#include "virtio_if.h"

static void	vtpci_v1_describe_features(struct vtpci_v1_common *, const char *,
		    uint64_t);
static int	vtpci_v1_alloc_msix(struct vtpci_v1_common *, int);
static int	vtpci_v1_alloc_msi(struct vtpci_v1_common *);
static int	vtpci_v1_alloc_intr_msix_pervq(struct vtpci_v1_common *);
static int	vtpci_v1_alloc_intr_msix_shared(struct vtpci_v1_common *);
static int	vtpci_v1_alloc_intr_msi(struct vtpci_v1_common *);
static int	vtpci_v1_alloc_intr_intx(struct vtpci_v1_common *);
static int	vtpci_v1_alloc_interrupt(struct vtpci_v1_common *, int, int,
		    struct vtpci_interrupt *);
static void	vtpci_v1_free_interrupt(struct vtpci_v1_common *,
		    struct vtpci_interrupt *);

static void	vtpci_v1_free_interrupts(struct vtpci_v1_common *);
static void	vtpci_v1_free_virtqueues(struct vtpci_v1_common *);
static void	vtpci_v1_cleanup_setup_intr_attempt(struct vtpci_v1_common *);
static int	vtpci_v1_alloc_intr_resources(struct vtpci_v1_common *);
static int	vtpci_v1_setup_intx_interrupt(struct vtpci_v1_common *,
		    int);
static int	vtpci_v1_setup_pervq_msix_interrupts(struct vtpci_v1_common *,
		    int);
static int	vtpci_v1_set_host_msix_vectors(struct vtpci_v1_common *);
static int	vtpci_v1_setup_msix_interrupts(struct vtpci_v1_common *,
		    int);
static int	vtpci_v1_setup_intrs(struct vtpci_v1_common *, int);
static int	vtpci_v1_reinit_virtqueue(struct vtpci_v1_common *, int);
static void	vtpci_v1_intx_intr(void *);
static int	vtpci_v1_vq_shared_intr_filter(void *);
static void	vtpci_v1_vq_shared_intr(void *);
static int	vtpci_v1_vq_intr_filter(void *);
static void	vtpci_v1_vq_intr(void *);
static void	vtpci_v1_config_intr(void *);

static void	vtpci_v1_setup_sysctl(struct vtpci_v1_common *);

#define vtpci_v1_setup_msi_interrupt vtpci_v1_setup_intx_interrupt

/*
 * This module contains two drivers:
 *   - virtio_pci_legacy for pre-V1 support
 *   - virtio_pci_modern for V1 support
 */
MODULE_VERSION(virtio_pci, 1);
MODULE_DEPEND(virtio_pci, pci, 1, 1, 1);
MODULE_DEPEND(virtio_pci, virtio, 1, 1, 1);

SYSCTL_DECL(_hw_virtio);
SYSCTL_NODE(_hw_virtio, OID_AUTO, pci, CTLFLAG_RD, 0,
    "VirtIO PCI driver parameters");

int vtpci_v1_disable_msix = 0;
SYSCTL_INT(_hw_virtio_pci, OID_AUTO, disable_msix, CTLFLAG_RW,
    &vtpci_v1_disable_msix, 0, "If set to 1, disables MSI-X.");

static uint8_t
vtpci_v1_read_isr(struct vtpci_v1_common *cn)
{
	return (VIRTIO_PCI_READ_ISR(cn->vtpci_v1_dev));
}

static uint16_t
vtpci_v1_get_vq_size(struct vtpci_v1_common *cn, int idx)
{
	return (VIRTIO_PCI_GET_VQ_SIZE(cn->vtpci_v1_dev, idx));
}

static bus_size_t
vtpci_v1_get_vq_notify_off(struct vtpci_v1_common *cn, int idx)
{
	return (VIRTIO_PCI_GET_VQ_NOTIFY_OFF(cn->vtpci_v1_dev, idx));
}

static void
vtpci_v1_set_vq(struct vtpci_v1_common *cn, struct virtqueue *vq)
{
	VIRTIO_PCI_SET_VQ(cn->vtpci_v1_dev, vq);
}

static void
vtpci_v1_disable_vq(struct vtpci_v1_common *cn, int idx)
{
	VIRTIO_PCI_DISABLE_VQ(cn->vtpci_v1_dev, idx);
}

static int
vtpci_v1_register_cfg_msix(struct vtpci_v1_common *cn, struct vtpci_interrupt *intr)
{
	return (VIRTIO_PCI_REGISTER_CFG_MSIX(cn->vtpci_v1_dev, intr));
}

static int
vtpci_v1_register_vq_msix(struct vtpci_v1_common *cn, int idx,
    struct vtpci_interrupt *intr)
{
	return (VIRTIO_PCI_REGISTER_VQ_MSIX(cn->vtpci_v1_dev, idx, intr));
}

void
vtpci_v1_init(struct vtpci_v1_common *cn, device_t dev, bool modern)
{

	cn->vtpci_v1_dev = dev;

	pci_enable_busmaster(dev);

	if (modern)
		cn->vtpci_v1_flags |= VTPCI_FLAG_MODERN;
	if (pci_msi_count(dev) == 0)
		cn->vtpci_v1_flags |= VTPCI_FLAG_NO_MSI;
	if (pci_msix_count(dev) == 0)
		cn->vtpci_v1_flags |= VTPCI_FLAG_NO_MSIX;

	vtpci_v1_setup_sysctl(cn);
}

int
vtpci_v1_add_child(struct vtpci_v1_common *cn)
{
	device_t dev, child;

	dev = cn->vtpci_v1_dev;

	child = device_add_child(dev, NULL, -1);
	if (child == NULL) {
		device_printf(dev, "cannot create child device\n");
		return (ENOMEM);
	}

	cn->vtpci_v1_child_dev = child;

	return (0);
}

int
vtpci_v1_delete_child(struct vtpci_v1_common *cn)
{
	device_t dev;
	int error;

	dev = cn->vtpci_v1_dev;

	error = bus_generic_detach(dev);
	if (error)
		return (error);

	return (0);
}

void
vtpci_v1_child_detached(struct vtpci_v1_common *cn)
{

	vtpci_v1_release_child_resources(cn);

	cn->vtpci_v1_child_feat_desc = NULL;
	cn->vtpci_v1_host_features = 0;
	cn->vtpci_v1_features = 0;
}

int
vtpci_v1_reinit(struct vtpci_v1_common *cn)
{
	int idx, error;

	for (idx = 0; idx < cn->vtpci_v1_nvqs; idx++) {
		error = vtpci_v1_reinit_virtqueue(cn, idx);
		if (error)
			return (error);
	}

	if (vtpci_v1_is_msix_enabled(cn)) {
		error = vtpci_v1_set_host_msix_vectors(cn);
		if (error)
			return (error);
	}

	return (0);
}

static void
vtpci_v1_describe_features(struct vtpci_v1_common *cn, const char *msg,
    uint64_t features)
{
	device_t dev, child;

	dev = cn->vtpci_v1_dev;
	child = cn->vtpci_v1_child_dev;

	if (device_is_attached(child) || bootverbose == 0)
		return;

	virtio_v1_describe(dev, msg, features, cn->vtpci_v1_child_feat_desc);
}

uint64_t
vtpci_v1_negotiate_features(struct vtpci_v1_common *cn,
    uint64_t child_features, uint64_t host_features)
{
	uint64_t features;

	cn->vtpci_v1_host_features = host_features;
	vtpci_v1_describe_features(cn, "host", host_features);

	/*
	 * Limit negotiated features to what the driver, virtqueue, and
	 * host all support.
	 */
	features = host_features & child_features;
	features = virtio_v1_filter_transport_features(features);

	cn->vtpci_v1_features = features;
	vtpci_v1_describe_features(cn, "negotiated", features);

	return (features);
}

bool
vtpci_v1_with_feature(struct vtpci_v1_common *cn, uint64_t feature)
{
	return ((cn->vtpci_v1_features & feature) != 0);
}

int
vtpci_v1_read_ivar(struct vtpci_v1_common *cn, int index, uintptr_t *result)
{
	device_t dev;
	int error;

	dev = cn->vtpci_v1_dev;
	error = 0;

	switch (index) {
	case VIRTIO_IVAR_SUBDEVICE:
		*result = pci_get_subdevice(dev);
		break;
	case VIRTIO_IVAR_VENDOR:
		*result = pci_get_vendor(dev);
		break;
	case VIRTIO_IVAR_DEVICE:
		*result = pci_get_device(dev);
		break;
	case VIRTIO_IVAR_SUBVENDOR:
		*result = pci_get_subvendor(dev);
		break;
	case VIRTIO_IVAR_MODERN:
		*result = vtpci_v1_is_modern(cn);
		break;
	default:
		error = ENOENT;
	}

	return (error);
}

int
vtpci_v1_write_ivar(struct vtpci_v1_common *cn, int index, uintptr_t value)
{
	int error;

	error = 0;

	switch (index) {
	case VIRTIO_IVAR_FEATURE_DESC:
		cn->vtpci_v1_child_feat_desc = (void *) value;
		break;
	default:
		error = ENOENT;
	}

	return (error);
}

int
vtpci_v1_alloc_virtqueues(struct vtpci_v1_common *cn, int nvqs,
    struct vq_alloc_info *vq_info)
{
	device_t dev;
	int idx, align, error;

	dev = cn->vtpci_v1_dev;

	/*
	 * This is VIRTIO_PCI_VRING_ALIGN from legacy VirtIO. In modern VirtIO,
	 * the tables do not have to be allocated contiguously, but we do so
	 * anyways.
	 */
	align = 4096;

	if (cn->vtpci_v1_nvqs != 0)
		return (EALREADY);
	if (nvqs <= 0)
		return (EINVAL);

	cn->vtpci_v1_vqs = kmalloc(nvqs * sizeof(struct vtpci_v1_virtqueue),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (cn->vtpci_v1_vqs == NULL)
		return (ENOMEM);

	for (idx = 0; idx < nvqs; idx++) {
		struct vtpci_v1_virtqueue *vqx;
		struct vq_alloc_info *info;
		struct virtqueue *vq;
		bus_size_t notify_offset;
		uint16_t size;

		vqx = &cn->vtpci_v1_vqs[idx];
		info = &vq_info[idx];

		size = vtpci_v1_get_vq_size(cn, idx);
		notify_offset = vtpci_v1_get_vq_notify_off(cn, idx);

		error = virtio_v1_virtqueue_alloc(dev, idx, size, notify_offset, align,
		    ~(vm_paddr_t)0, info, &vq);
		if (error) {
			device_printf(dev,
			    "cannot allocate virtqueue %d: %d\n", idx, error);
			break;
		}

		vtpci_v1_set_vq(cn, vq);

		vqx->vtv_vq = *info->vqai_vq = vq;
		vqx->vtv_no_intr = info->vqai_intr == NULL;

		cn->vtpci_v1_nvqs++;
	}

	if (error)
		vtpci_v1_free_virtqueues(cn);

	return (error);
}

static int
vtpci_v1_alloc_msix(struct vtpci_v1_common *cn, int nvectors)
{
	device_t dev = cn->vtpci_v1_dev;
	int error, i, required;

	required = nvectors + 1;
	if (pci_msix_count(dev) < required)
		return (ENXIO);
	if ((error = pci_setup_msix(dev)) != 0) {
		device_printf(dev, "pci_setup_msix failed: %d\n", error);
		return (error);
	}
	cn->vtpci_v1_msix_rids = kmalloc(required * sizeof(int),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (cn->vtpci_v1_msix_rids == NULL) {
		pci_teardown_msix(dev);
		return (ENOMEM);
	}
	for (i = 0; i < required; ++i) {
		error = pci_alloc_msix_vector(dev, i,
		    &cn->vtpci_v1_msix_rids[i], (device_get_unit(dev) + i) % ncpus);
		if (error != 0) {
			device_printf(dev, "MSI-X vector %d/%d allocation failed: %d\n",
			    i + 1, required, error);
			while (--i >= 0)
				pci_release_msix_vector(dev, cn->vtpci_v1_msix_rids[i]);
			kfree(cn->vtpci_v1_msix_rids, M_DEVBUF);
			cn->vtpci_v1_msix_rids = NULL;
			pci_teardown_msix(dev);
			return (error);
		}
	}
	cn->vtpci_v1_nmsix_resources = required;
	cn->vtpci_v1_irq_flags = RF_ACTIVE;
	return (0);
}

static int
vtpci_v1_alloc_msi(struct vtpci_v1_common *cn)
{
	int flags, rid;

	rid = 0;
	if (pci_alloc_1intr(cn->vtpci_v1_dev, 1, &rid, &flags) != PCI_INTR_TYPE_MSI)
		return (ENXIO);
	cn->vtpci_v1_msi_rid = rid;
	cn->vtpci_v1_irq_flags = flags;
	return (0);
}

static int
vtpci_v1_alloc_intr_msix_pervq(struct vtpci_v1_common *cn)
{
	int i, nvectors, error;

	if (vtpci_v1_disable_msix != 0 || cn->vtpci_v1_flags & VTPCI_FLAG_NO_MSIX)
		return (ENOTSUP);

	for (nvectors = 0, i = 0; i < cn->vtpci_v1_nvqs; i++) {
		if (cn->vtpci_v1_vqs[i].vtv_no_intr == 0)
			nvectors++;
	}

	error = vtpci_v1_alloc_msix(cn, nvectors);
	if (error)
		return (error);

	cn->vtpci_v1_flags |= VTPCI_FLAG_MSIX;

	return (0);
}

static int
vtpci_v1_alloc_intr_msix_shared(struct vtpci_v1_common *cn)
{
	int error;

	if (vtpci_v1_disable_msix != 0 || cn->vtpci_v1_flags & VTPCI_FLAG_NO_MSIX)
		return (ENOTSUP);

	error = vtpci_v1_alloc_msix(cn, 1);
	if (error)
		return (error);

	cn->vtpci_v1_flags |= VTPCI_FLAG_MSIX | VTPCI_FLAG_SHARED_MSIX;

	return (0);
}

static int
vtpci_v1_alloc_intr_msi(struct vtpci_v1_common *cn)
{
	int error;

	/* Only BHyVe supports MSI. */
	if (cn->vtpci_v1_flags & VTPCI_FLAG_NO_MSI)
		return (ENOTSUP);

	error = vtpci_v1_alloc_msi(cn);
	if (error)
		return (error);

	cn->vtpci_v1_flags |= VTPCI_FLAG_MSI;

	return (0);
}

static int
vtpci_v1_alloc_intr_intx(struct vtpci_v1_common *cn)
{

	cn->vtpci_v1_flags |= VTPCI_FLAG_INTX;

	return (0);
}

static int
vtpci_v1_alloc_interrupt(struct vtpci_v1_common *cn, int rid, int flags,
    struct vtpci_interrupt *intr)
{
	struct resource *irq;

	irq = bus_alloc_resource_any(cn->vtpci_v1_dev, SYS_RES_IRQ, &rid, flags);
	if (irq == NULL)
		return (ENXIO);

	intr->vti_irq = irq;
	intr->vti_rid = rid;

	return (0);
}

static void
vtpci_v1_free_interrupt(struct vtpci_v1_common *cn, struct vtpci_interrupt *intr)
{
	device_t dev;

	dev = cn->vtpci_v1_dev;

	if (intr->vti_handler != NULL) {
		bus_teardown_intr(dev, intr->vti_irq, intr->vti_handler);
		intr->vti_handler = NULL;
	}

	if (intr->vti_irq != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, intr->vti_rid,
		    intr->vti_irq);
		intr->vti_irq = NULL;
		intr->vti_rid = -1;
	}
}

static void
vtpci_v1_free_interrupts(struct vtpci_v1_common *cn)
{
	struct vtpci_interrupt *intr;
	int i, nvq_intrs;

	nvq_intrs = cn->vtpci_v1_nmsix_resources - 1;
	vtpci_v1_free_interrupt(cn, &cn->vtpci_v1_device_interrupt);
	if ((intr = cn->vtpci_v1_msix_vq_interrupts) != NULL) {
		for (i = 0; i < nvq_intrs; ++i)
			vtpci_v1_free_interrupt(cn, &intr[i]);
		kfree(intr, M_DEVBUF);
		cn->vtpci_v1_msix_vq_interrupts = NULL;
	}
	if (cn->vtpci_v1_flags & VTPCI_FLAG_MSIX) {
		for (i = 0; i < cn->vtpci_v1_nmsix_resources; ++i)
			pci_release_msix_vector(cn->vtpci_v1_dev, cn->vtpci_v1_msix_rids[i]);
		kfree(cn->vtpci_v1_msix_rids, M_DEVBUF);
		cn->vtpci_v1_msix_rids = NULL;
		pci_teardown_msix(cn->vtpci_v1_dev);
	} else if (cn->vtpci_v1_flags & VTPCI_FLAG_MSI) {
		pci_release_msi(cn->vtpci_v1_dev);
	}
	cn->vtpci_v1_nmsix_resources = 0;
	cn->vtpci_v1_flags &= ~VTPCI_FLAG_ITYPE_MASK;
}

static void
vtpci_v1_free_virtqueues(struct vtpci_v1_common *cn)
{
	struct vtpci_v1_virtqueue *vqx;
	int idx;

	for (idx = 0; idx < cn->vtpci_v1_nvqs; idx++) {
		vtpci_v1_disable_vq(cn, idx);

		vqx = &cn->vtpci_v1_vqs[idx];
		virtio_v1_virtqueue_free(vqx->vtv_vq);
		vqx->vtv_vq = NULL;
	}

	kfree(cn->vtpci_v1_vqs, M_DEVBUF);
	cn->vtpci_v1_vqs = NULL;
	cn->vtpci_v1_nvqs = 0;
}

void
vtpci_v1_release_child_resources(struct vtpci_v1_common *cn)
{

	vtpci_v1_free_interrupts(cn);
	vtpci_v1_free_virtqueues(cn);
}

static void
vtpci_v1_cleanup_setup_intr_attempt(struct vtpci_v1_common *cn)
{
	int idx;

	if (cn->vtpci_v1_flags & VTPCI_FLAG_MSIX) {
		vtpci_v1_register_cfg_msix(cn, NULL);

		for (idx = 0; idx < cn->vtpci_v1_nvqs; idx++)
			vtpci_v1_register_vq_msix(cn, idx, NULL);
	}

	vtpci_v1_free_interrupts(cn);
}

static int
vtpci_v1_alloc_intr_resources(struct vtpci_v1_common *cn)
{
	struct vtpci_interrupt *intr;
	int i, rid, flags, nvq_intrs, error;

	if (cn->vtpci_v1_flags & VTPCI_FLAG_INTX) {
		rid = 0;
		flags = RF_ACTIVE | RF_SHAREABLE;
	} else if (cn->vtpci_v1_flags & VTPCI_FLAG_MSI) {
		rid = cn->vtpci_v1_msi_rid;
		flags = cn->vtpci_v1_irq_flags;
	} else {
		rid = cn->vtpci_v1_msix_rids[0];
		flags = cn->vtpci_v1_irq_flags;
	}
	intr = &cn->vtpci_v1_device_interrupt;
	error = vtpci_v1_alloc_interrupt(cn, rid, flags, intr);
	if (error || cn->vtpci_v1_flags & (VTPCI_FLAG_INTX | VTPCI_FLAG_MSI))
		return (error);
	nvq_intrs = cn->vtpci_v1_nmsix_resources - 1;
	cn->vtpci_v1_msix_vq_interrupts = kmalloc(nvq_intrs *
	    sizeof(struct vtpci_interrupt), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (cn->vtpci_v1_msix_vq_interrupts == NULL)
		return (ENOMEM);
	for (i = 0, intr = cn->vtpci_v1_msix_vq_interrupts; i < nvq_intrs; ++i, ++intr) {
		error = vtpci_v1_alloc_interrupt(cn, cn->vtpci_v1_msix_rids[i + 1],
		    flags, intr);
		if (error)
			return (error);
	}
	return (0);
}

static int
vtpci_v1_setup_intx_interrupt(struct vtpci_v1_common *cn, int type)
{
	struct vtpci_interrupt *intr;
	int error;

	intr = &cn->vtpci_v1_device_interrupt;

	error = bus_setup_intr(cn->vtpci_v1_dev, intr->vti_irq, type, vtpci_v1_intx_intr, cn, &intr->vti_handler, NULL);

	return (error);
}

static int
vtpci_v1_setup_pervq_msix_interrupts(struct vtpci_v1_common *cn, int type)
{
	struct vtpci_v1_virtqueue *vqx;
	struct vtpci_interrupt *intr;
	int i, error;

	intr = cn->vtpci_v1_msix_vq_interrupts;

	for (i = 0; i < cn->vtpci_v1_nvqs; i++) {
		vqx = &cn->vtpci_v1_vqs[i];

		if (vqx->vtv_no_intr)
			continue;

		error = bus_setup_intr(cn->vtpci_v1_dev, intr->vti_irq, type, vtpci_v1_vq_intr, vqx->vtv_vq, &intr->vti_handler, NULL);
		if (error)
			return (error);

		intr++;
	}

	return (0);
}

static int
vtpci_v1_set_host_msix_vectors(struct vtpci_v1_common *cn)
{
	struct vtpci_interrupt *intr, *tintr;
	int idx, error;

	intr = &cn->vtpci_v1_device_interrupt;
	error = vtpci_v1_register_cfg_msix(cn, intr);
	if (error)
		return (error);

	intr = cn->vtpci_v1_msix_vq_interrupts;
	for (idx = 0; idx < cn->vtpci_v1_nvqs; idx++) {
		if (cn->vtpci_v1_vqs[idx].vtv_no_intr)
			tintr = NULL;
		else
			tintr = intr;

		error = vtpci_v1_register_vq_msix(cn, idx, tintr);
		if (error)
			break;

		/*
		 * For shared MSIX, all the virtqueues share the first
		 * interrupt.
		 */
		if (!cn->vtpci_v1_vqs[idx].vtv_no_intr &&
		    (cn->vtpci_v1_flags & VTPCI_FLAG_SHARED_MSIX) == 0)
			intr++;
	}

	return (error);
}

static int
vtpci_v1_setup_msix_interrupts(struct vtpci_v1_common *cn, int type)
{
	struct vtpci_interrupt *intr;
	int error;

	intr = &cn->vtpci_v1_device_interrupt;

	error = bus_setup_intr(cn->vtpci_v1_dev, intr->vti_irq, type, vtpci_v1_config_intr, cn, &intr->vti_handler, NULL);
	if (error)
		return (error);

	if (cn->vtpci_v1_flags & VTPCI_FLAG_SHARED_MSIX) {
		intr = &cn->vtpci_v1_msix_vq_interrupts[0];

		error = bus_setup_intr(cn->vtpci_v1_dev, intr->vti_irq, type, vtpci_v1_vq_shared_intr, cn, &intr->vti_handler, NULL);
	} else
		error = vtpci_v1_setup_pervq_msix_interrupts(cn, type);

	return (error ? error : vtpci_v1_set_host_msix_vectors(cn));
}

static int
vtpci_v1_setup_intrs(struct vtpci_v1_common *cn, int type)
{
	int error;

	type |= INTR_MPSAFE;
	KASSERT(cn->vtpci_v1_flags & VTPCI_FLAG_ITYPE_MASK,
	    ("%s: no interrupt type selected %#x", __func__, cn->vtpci_v1_flags));

	error = vtpci_v1_alloc_intr_resources(cn);
	if (error)
		return (error);

	if (cn->vtpci_v1_flags & VTPCI_FLAG_INTX)
		error = vtpci_v1_setup_intx_interrupt(cn, type);
	else if (cn->vtpci_v1_flags & VTPCI_FLAG_MSI)
		error = vtpci_v1_setup_msi_interrupt(cn, type);
	else
		error = vtpci_v1_setup_msix_interrupts(cn, type);

	return (error);
}

int
vtpci_v1_setup_interrupts(struct vtpci_v1_common *cn, int type)
{
	device_t dev;
	int attempt, error;

	dev = cn->vtpci_v1_dev;

	for (attempt = 0; attempt < 5; attempt++) {
		/*
		 * Start with the most desirable interrupt configuration and
		 * fallback towards less desirable ones.
		 */
		switch (attempt) {
		case 0:
			error = vtpci_v1_alloc_intr_msix_pervq(cn);
			break;
		case 1:
			error = vtpci_v1_alloc_intr_msix_shared(cn);
			break;
		case 2:
			error = vtpci_v1_alloc_intr_msi(cn);
			break;
		case 3:
			error = vtpci_v1_alloc_intr_intx(cn);
			break;
		default:
			device_printf(dev,
			    "exhausted all interrupt allocation attempts\n");
			return (ENXIO);
		}

		if (error == 0 && vtpci_v1_setup_intrs(cn, type) == 0)
			break;

		vtpci_v1_cleanup_setup_intr_attempt(cn);
	}

	if (bootverbose) {
		if (cn->vtpci_v1_flags & VTPCI_FLAG_INTX)
			device_printf(dev, "using legacy interrupt\n");
		else if (cn->vtpci_v1_flags & VTPCI_FLAG_MSI)
			device_printf(dev, "using MSI interrupt\n");
		else if (cn->vtpci_v1_flags & VTPCI_FLAG_SHARED_MSIX)
			device_printf(dev, "using shared MSIX interrupts\n");
		else
			device_printf(dev, "using per VQ MSIX interrupts\n");
	}

	return (0);
}

static int
vtpci_v1_reinit_virtqueue(struct vtpci_v1_common *cn, int idx)
{
	struct vtpci_v1_virtqueue *vqx;
	struct virtqueue *vq;
	int error;

	vqx = &cn->vtpci_v1_vqs[idx];
	vq = vqx->vtv_vq;

	KASSERT(vq != NULL, ("%s: vq %d not allocated", __func__, idx));

	error = virtio_v1_virtqueue_reinit(vq, vtpci_v1_get_vq_size(cn, idx));
	if (error == 0)
		vtpci_v1_set_vq(cn, vq);

	return (error);
}

static void
vtpci_v1_intx_intr(void *xcn)
{
	struct vtpci_v1_common *cn;
	struct vtpci_v1_virtqueue *vqx;
	int i;
	uint8_t isr;

	cn = xcn;
	isr = vtpci_v1_read_isr(cn);

	if (isr & VIRTIO_PCI_ISR_CONFIG)
		vtpci_v1_config_intr(cn);

	if (isr & VIRTIO_PCI_ISR_INTR) {
		vqx = &cn->vtpci_v1_vqs[0];
		for (i = 0; i < cn->vtpci_v1_nvqs; i++, vqx++) {
			if (vqx->vtv_no_intr == 0)
				virtio_v1_virtqueue_intr(vqx->vtv_vq);
		}
	}
}

static int __unused
vtpci_v1_vq_shared_intr_filter(void *xcn)
{
	struct vtpci_v1_common *cn;
	struct vtpci_v1_virtqueue *vqx;
	int i, rc;

	cn = xcn;
	vqx = &cn->vtpci_v1_vqs[0];
	rc = 0;

	for (i = 0; i < cn->vtpci_v1_nvqs; i++, vqx++) {
		if (vqx->vtv_no_intr == 0)
			rc |= virtio_v1_virtqueue_intr_filter(vqx->vtv_vq);
	}

	return (rc);
}

static void
vtpci_v1_vq_shared_intr(void *xcn)
{
	struct vtpci_v1_common *cn;
	struct vtpci_v1_virtqueue *vqx;
	int i;

	cn = xcn;
	vqx = &cn->vtpci_v1_vqs[0];

	for (i = 0; i < cn->vtpci_v1_nvqs; i++, vqx++) {
		if (vqx->vtv_no_intr == 0)
			virtio_v1_virtqueue_intr(vqx->vtv_vq);
	}
}

static int __unused
vtpci_v1_vq_intr_filter(void *xvq)
{
	struct virtqueue *vq;
	int rc;

	vq = xvq;
	rc = virtio_v1_virtqueue_intr_filter(vq);

	return (rc);
}

static void
vtpci_v1_vq_intr(void *xvq)
{
	struct virtqueue *vq;

	vq = xvq;
	virtio_v1_virtqueue_intr(vq);
}

static void
vtpci_v1_config_intr(void *xcn)
{
	struct vtpci_v1_common *cn;
	device_t child;

	cn = xcn;
	child = cn->vtpci_v1_child_dev;

	if (child != NULL)
		VIRTIO_CONFIG_CHANGE(child);
}

static int
vtpci_v1_feature_sysctl(struct sysctl_req *req, struct vtpci_v1_common *cn,
    uint64_t features)
{
	struct sbuf *sb;
	int error;

	sb = sbuf_new_for_sysctl(NULL, NULL, 256, req);
	if (sb == NULL)
		return (ENOMEM);

	error = virtio_v1_describe_sbuf(sb, features, cn->vtpci_v1_child_feat_desc);
	sbuf_delete(sb);

	return (error);
}

static int
vtpci_v1_host_features_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct vtpci_v1_common *cn;

	cn = arg1;

	return (vtpci_v1_feature_sysctl(req, cn, cn->vtpci_v1_host_features));
}

static int
vtpci_v1_negotiated_features_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct vtpci_v1_common *cn;

	cn = arg1;

	return (vtpci_v1_feature_sysctl(req, cn, cn->vtpci_v1_features));
}

static void
vtpci_v1_setup_sysctl(struct vtpci_v1_common *cn)
{
	device_t dev;
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;
	struct sysctl_oid_list *child;

	dev = cn->vtpci_v1_dev;
	ctx = device_get_sysctl_ctx(dev);
	tree = device_get_sysctl_tree(dev);
	child = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "nvqs",
	    CTLFLAG_RD, &cn->vtpci_v1_nvqs, 0, "Number of virtqueues");

	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "host_features",
	    CTLTYPE_STRING | CTLFLAG_RD, cn, 0,
	    vtpci_v1_host_features_sysctl, "A", "Features supported by the host");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "negotiated_features",
	    CTLTYPE_STRING | CTLFLAG_RD, cn, 0,
	    vtpci_v1_negotiated_features_sysctl, "A", "Features negotiated");
}
