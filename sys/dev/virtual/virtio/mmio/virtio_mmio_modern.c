/* Driver for the VirtIO MMIO version 2 (modern) interface. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <dev/virtual/virtio/v1/virtio.h>
#include <dev/virtual/virtio/v1/virtqueue.h>
#include "virtio_mmio.h"
#include "virtio_v1_bus_if.h"

struct vtmmio_v1_softc {
        device_t        dev;
        device_t        child;
        struct resource *res;
        struct resource *irq;
        void           *intrhand;
        uint64_t        features;
        struct virtio_feature_desc *desc;
        struct virtqueue **vqs;
        int             nvqs;
};

#define VTMMIO_V1_READ_1(sc, offset) \
	bus_read_1((sc)->res, (offset))
#define VTMMIO_V1_READ_4(sc, offset) \
	bus_read_4((sc)->res, (offset))
#define VTMMIO_V1_WRITE_1(sc, offset, value) \
	bus_write_1((sc)->res, (offset), (value))
#define VTMMIO_V1_WRITE_4(sc, offset, value) \
	bus_write_4((sc)->res, (offset), (value))

static int      vtmmio_v1_probe(device_t);
static int      vtmmio_v1_attach(device_t);
static int      vtmmio_v1_detach(device_t);
static int      vtmmio_v1_suspend(device_t);
static int      vtmmio_v1_resume(device_t);
static int      vtmmio_v1_shutdown(device_t);
static void     vtmmio_v1_driver_added(device_t, driver_t *);
static void     vtmmio_v1_child_detached(device_t, device_t);
static int      vtmmio_v1_read_ivar(device_t, device_t, int, uintptr_t *);
static int      vtmmio_v1_write_ivar(device_t, device_t, int, uintptr_t);
static uint64_t vtmmio_v1_negotiate_features(device_t, uint64_t);
static int      vtmmio_v1_finalize_features(device_t);
static bool     vtmmio_v1_with_feature(device_t, uint64_t);
static int
vtmmio_v1_alloc_virtqueues(device_t, int,
                           struct vq_alloc_info *);
static int      vtmmio_v1_setup_intr(device_t, int);
static void     vtmmio_v1_stop(device_t);
static int      vtmmio_v1_reinit(device_t, uint64_t);
static void     vtmmio_v1_reinit_complete(device_t);
static void     vtmmio_v1_notify_virtqueue(device_t, uint16_t, bus_size_t);
static void     vtmmio_v1_read_dev_config(device_t, bus_size_t, void *, int);
static void     vtmmio_v1_write_dev_config(device_t, bus_size_t, const void *, int);
static void     vtmmio_v1_reset(struct vtmmio_v1_softc *);
static void     vtmmio_v1_free_virtqueues(struct vtmmio_v1_softc *);
static void
vtmmio_v1_set_virtqueue(struct vtmmio_v1_softc *,
                        struct virtqueue *);
static void     vtmmio_v1_intr(void *);
static void     vtmmio_v1_probe_and_attach_child(struct vtmmio_v1_softc *);

static device_method_t vtmmio_v1_methods[] = {
	DEVMETHOD(device_probe,		  vtmmio_v1_probe),
	DEVMETHOD(device_attach,		  vtmmio_v1_attach),
	DEVMETHOD(device_detach,		  vtmmio_v1_detach),
	DEVMETHOD(device_suspend,		  vtmmio_v1_suspend),
	DEVMETHOD(device_resume,		  vtmmio_v1_resume),
	DEVMETHOD(device_shutdown,		  vtmmio_v1_shutdown),
	DEVMETHOD(bus_driver_added,		  vtmmio_v1_driver_added),
	DEVMETHOD(bus_child_detached,		  vtmmio_v1_child_detached),
	DEVMETHOD(bus_read_ivar,		  vtmmio_v1_read_ivar),
	DEVMETHOD(bus_write_ivar,		  vtmmio_v1_write_ivar),
	DEVMETHOD(virtio_v1_bus_negotiate_features, vtmmio_v1_negotiate_features),
	DEVMETHOD(virtio_v1_bus_finalize_features, vtmmio_v1_finalize_features),
	DEVMETHOD(virtio_v1_bus_with_feature,	  vtmmio_v1_with_feature),
	DEVMETHOD(virtio_v1_bus_alloc_virtqueues,  vtmmio_v1_alloc_virtqueues),
	DEVMETHOD(virtio_v1_bus_setup_intr,	  vtmmio_v1_setup_intr),
	DEVMETHOD(virtio_v1_bus_stop,		  vtmmio_v1_stop),
	DEVMETHOD(virtio_v1_bus_reinit,		  vtmmio_v1_reinit),
	DEVMETHOD(virtio_v1_bus_reinit_complete,   vtmmio_v1_reinit_complete),
	DEVMETHOD(virtio_v1_bus_notify_vq,	  vtmmio_v1_notify_virtqueue),
	DEVMETHOD(virtio_v1_bus_read_device_config, vtmmio_v1_read_dev_config),
	DEVMETHOD(virtio_v1_bus_write_device_config, vtmmio_v1_write_dev_config),
	DEVMETHOD_END
};

DEFINE_CLASS_0(virtio_mmio_modern, vtmmio_v1_driver, vtmmio_v1_methods,
	    sizeof(struct vtmmio_v1_softc));

driver_t vtmmio_v1_root_driver = {
	"virtio_mmio_modern",
	vtmmio_v1_methods,
	sizeof(struct vtmmio_v1_softc)
};
static devclass_t vtmmio_v1_devclass;
DRIVER_MODULE(virtio_mmio_modern, nexus, vtmmio_v1_root_driver,
	    vtmmio_v1_devclass, NULL, NULL);
MODULE_DEPEND(virtio_mmio, virtio, 1, 1, 1);

static int
vtmmio_v1_probe(device_t dev)
{
        struct resource *r;
        int             rid = 0;
        r = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
        if (r == NULL)
                return ENXIO;
	if (bus_read_4(r, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC_VIRT ||
	    bus_read_4(r, VIRTIO_MMIO_VERSION) != 2 ||
	    bus_read_4(r, VIRTIO_MMIO_DEVICE_ID) == 0) {
		bus_release_resource(dev, SYS_RES_MEMORY, rid, r);
		return ENXIO;
	}
	bus_release_resource(dev, SYS_RES_MEMORY, rid, r);
        device_set_desc(dev, "VirtIO MMIO (modern) adapter");
        return BUS_PROBE_DEFAULT;
}
static int
vtmmio_v1_attach(device_t dev)
{
        struct vtmmio_v1_softc *s = device_get_softc(dev);
        int             rid = 0;
        s->dev = dev;
        s->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
        if (!s->res)
                return ENXIO;
        vtmmio_v1_reset(s);
	VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_STATUS_ACK);
        if (!(s->child = device_add_child(dev, NULL, -1))) {
                vtmmio_v1_detach(dev);
                return ENOMEM;
	}
	vtmmio_v1_probe_and_attach_child(s);
	return 0;
}

static int
vtmmio_v1_suspend(device_t dev)
{
	return (bus_generic_suspend(dev));
}

static int
vtmmio_v1_resume(device_t dev)
{
	return (bus_generic_resume(dev));
}

static int
vtmmio_v1_shutdown(device_t dev)
{
	(void)bus_generic_shutdown(dev);
	vtmmio_v1_stop(dev);
	return (0);
}
static int
vtmmio_v1_detach(device_t dev)
{
        struct vtmmio_v1_softc *s = device_get_softc(dev);
        int             e;
        if (s->child) {
                if ((e = device_delete_child(dev, s->child)))
                        return e;
                s->child = NULL;
	}
	if (s->intrhand) {
		bus_teardown_intr(dev, s->irq, s->intrhand);
		s->intrhand = NULL;
	}
	if (s->irq) {
		bus_release_resource(dev, SYS_RES_IRQ, 0, s->irq);
		s->irq = NULL;
	}
        vtmmio_v1_free_virtqueues(s);
        vtmmio_v1_reset(s);
        if (s->res)
                bus_release_resource(dev, SYS_RES_MEMORY, 0, s->res);
        s->res = NULL;
        return 0;
}
static void
vtmmio_v1_driver_added(device_t d, driver_t * x __unused)
{
        vtmmio_v1_probe_and_attach_child(device_get_softc(d));
}
static void
vtmmio_v1_child_detached(device_t d, device_t c __unused)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        vtmmio_v1_free_virtqueues(s);
        vtmmio_v1_reset(s);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_STATUS_ACK);
}
static int
vtmmio_v1_read_ivar(device_t d, device_t c, int i, uintptr_t * r)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        if (c != s->child || i != VIRTIO_IVAR_DEVTYPE)
                return ENOENT;
        *r = VTMMIO_V1_READ_4(s, VIRTIO_MMIO_DEVICE_ID);
        return 0;
}
static int
vtmmio_v1_write_ivar(device_t d, device_t c, int i, uintptr_t v)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        if (c != s->child)
                return ENOENT;
	if (i == VIRTIO_IVAR_FEATURE_DESC) {
		s->desc = (void *)v;
		return 0;
	}
	return ENOENT;
}

static uint64_t
vtmmio_v1_negotiate_features(device_t d, uint64_t child)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        uint64_t        host;
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_HOST_FEATURES_SEL, 0);
        host = VTMMIO_V1_READ_4(s, VIRTIO_MMIO_HOST_FEATURES);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_HOST_FEATURES_SEL, 1);
        host |= (uint64_t) VTMMIO_V1_READ_4(s, VIRTIO_MMIO_HOST_FEATURES) << 32;
	s->features = virtio_v1_filter_transport_features(host & (child | VIRTIO_F_VERSION_1));
        virtio_v1_describe(d, "host", host, s->desc);
        virtio_v1_describe(d, "negotiated", s->features, s->desc);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_GUEST_FEATURES_SEL, 0);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_GUEST_FEATURES, s->features);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_GUEST_FEATURES_SEL, 1);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_GUEST_FEATURES, s->features >> 32);
	return (s->features);
}
static int
vtmmio_v1_finalize_features(device_t d)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_STATUS, VTMMIO_V1_READ_4(s, VIRTIO_MMIO_STATUS) | VIRTIO_CONFIG_S_FEATURES_OK);
	return ((VTMMIO_V1_READ_4(s, VIRTIO_MMIO_STATUS) &
	    VIRTIO_CONFIG_S_FEATURES_OK) != 0 ? 0 : ENOTSUP);
}
static bool
vtmmio_v1_with_feature(device_t d, uint64_t f)
{
	return ((((struct vtmmio_v1_softc *)device_get_softc(d))->features & f) != 0);
}
static void
vtmmio_v1_set_virtqueue(struct vtmmio_v1_softc *s, struct virtqueue *v)
{
        uint16_t        i = virtio_v1_virtqueue_index(v);
        vm_paddr_t      p;
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_SEL, i);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_NUM, virtio_v1_virtqueue_size(v));
        p = virtio_v1_virtqueue_desc_paddr(v);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_DESC_LOW, p);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_DESC_HIGH, p >> 32);
        p = virtio_v1_virtqueue_avail_paddr(v);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_AVAIL_LOW, p);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, p >> 32);
        p = virtio_v1_virtqueue_used_paddr(v);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_USED_LOW, p);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_USED_HIGH, p >> 32);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_READY, 1);
}
static int
vtmmio_v1_alloc_virtqueues(device_t d, int n, struct vq_alloc_info *a)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        int             i, e = 0;
	if (s->nvqs != 0 || n <= 0 || n > 256)
                return EINVAL;
        s->vqs = kmalloc(n * sizeof(*s->vqs), M_DEVBUF, M_WAITOK | M_ZERO);
        for (i = 0; i < n; i++) {
                VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_SEL, i);
                e = virtio_v1_virtqueue_alloc(d, i, VTMMIO_V1_READ_4(s, VIRTIO_MMIO_QUEUE_NUM_MAX), VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_MMIO_VRING_ALIGN, ~(vm_paddr_t) 0, &a[i], &s->vqs[i]);
                if (e)
                        break;
                *a[i].vqai_vq = s->vqs[i];
                vtmmio_v1_set_virtqueue(s, s->vqs[i]);
                s->nvqs++;
	}
	if (e)
                vtmmio_v1_free_virtqueues(s);
        return e;
}
static int
vtmmio_v1_setup_intr(device_t d, int t __unused)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        int             rid = 0;
        if (s->intrhand)
                return EALREADY;
        s->irq = bus_alloc_resource_any(d, SYS_RES_IRQ, &rid, RF_ACTIVE);
        if (!s->irq)
                return ENXIO;
	if (bus_setup_intr(d, s->irq, INTR_MPSAFE, vtmmio_v1_intr, s,
	    &s->intrhand, NULL) != 0) {
		bus_release_resource(d, SYS_RES_IRQ, rid, s->irq);
		s->irq = NULL;
		return (ENXIO);
	}

	return (0);
}
static void
vtmmio_v1_intr(void *x)
{
        struct vtmmio_v1_softc *s = x;
        uint32_t        st = VTMMIO_V1_READ_4(s, VIRTIO_MMIO_INTERRUPT_STATUS);
        int             i;
        if (!st)
                return;
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_INTERRUPT_ACK, st);
	if ((st & VIRTIO_MMIO_INT_VRING) != 0)
                for (i = 0; i < s->nvqs; i++)
                        virtio_v1_virtqueue_intr(s->vqs[i]);
}
static void
vtmmio_v1_stop(device_t d)
{
        vtmmio_v1_reset(device_get_softc(d));
}
static int
vtmmio_v1_reinit(device_t d, uint64_t f)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        int             i, e;
        vtmmio_v1_reset(s);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_STATUS_ACK | VIRTIO_CONFIG_STATUS_DRIVER);
        vtmmio_v1_negotiate_features(d, f);
        if ((e = vtmmio_v1_finalize_features(d)))
                return e;
        for (i = 0; i < s->nvqs; i++) {
                if ((e = virtio_v1_virtqueue_reinit(s->vqs[i], virtio_v1_virtqueue_size(s->vqs[i]))))
                        return e;
                vtmmio_v1_set_virtqueue(s, s->vqs[i]);
	}
	return (0);
}
static void
vtmmio_v1_reinit_complete(device_t d)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_STATUS, VTMMIO_V1_READ_4(s, VIRTIO_MMIO_STATUS) | VIRTIO_CONFIG_STATUS_DRIVER_OK);
}
static void
vtmmio_v1_notify_virtqueue(device_t d, uint16_t q, bus_size_t o __unused)
{
        VTMMIO_V1_WRITE_4((struct vtmmio_v1_softc *)device_get_softc(d), VIRTIO_MMIO_QUEUE_NOTIFY, q);
}
static void
vtmmio_v1_read_dev_config(device_t d, bus_size_t o, void *p, int n)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        uint8_t        *x = p;
        while (n--)
                *x++ = VTMMIO_V1_READ_1(s, VIRTIO_MMIO_CONFIG + o++);
}
static void
vtmmio_v1_write_dev_config(device_t d, bus_size_t o, const void *p, int n)
{
        struct vtmmio_v1_softc *s = device_get_softc(d);
        const           uint8_t *x = p;
        while (n--)
                VTMMIO_V1_WRITE_1(s, VIRTIO_MMIO_CONFIG + o++, *x++);
}
static void
vtmmio_v1_reset(struct vtmmio_v1_softc *s)
{
        if (s->res)
                VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_STATUS_RESET);
}
static void
vtmmio_v1_free_virtqueues(struct vtmmio_v1_softc *s)
{
        int             i;
        for (i = 0; i < s->nvqs; i++) {
                VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_SEL, i);
                VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_QUEUE_READY, 0);
		if (s->vqs[i] != NULL)
			virtio_v1_virtqueue_free(s->vqs[i]);
	}
	if (s->vqs)
                kfree(s->vqs, M_DEVBUF);
        s->vqs = NULL;
        s->nvqs = 0;
}
static void
vtmmio_v1_probe_and_attach_child(struct vtmmio_v1_softc *s)
{
        if (!s->child || device_get_state(s->child) != DS_NOTPRESENT)
                return;
        VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_STATUS_ACK | VIRTIO_CONFIG_STATUS_DRIVER);
	if (device_probe_and_attach(s->child) != 0 ||
	    device_get_state(s->child) == DS_NOTPRESENT) {
                vtmmio_v1_reset(s);
                VTMMIO_V1_WRITE_4(s, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_STATUS_ACK);
	} else {
		vtmmio_v1_reinit_complete(s->dev);
	}
}
