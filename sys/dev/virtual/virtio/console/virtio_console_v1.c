/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VirtIO console driver for the modern (V1) PCI transport.
 *
 * The queue and protocol layout is derived from FreeBSD's virtio_console
 * driver.  DragonFly's tty implementation uses the older dev_ops interface,
 * so the port is exposed as /dev/vconN rather than as a FreeBSD new-style tty.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/caps.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/serialize.h>
#include <sys/sglist.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/ttydefaults.h>
#include <sys/ucred.h>

#include <dev/virtual/virtio/console/virtio_console.h>
#include <dev/virtual/virtio/v1/virtio.h>
#include <dev/virtual/virtio/v1/virtqueue.h>

#define VCON_BUFSIZE	128
#define VCON_RX_BUFS	32
#define VCON_CTRL_BUFS	8
#define VCON_CTRL_BUFSIZE 64

struct vcon_rxbuf {
	char			data[VCON_BUFSIZE];
	struct sglist		sg;
	struct sglist_seg	seg[2];
};

struct vcon_txbuf {
	char			data[VCON_BUFSIZE];
	struct sglist		sg;
	struct sglist_seg	seg[2];
};

struct vcon_ctrlbuf {
	char			data[VCON_CTRL_BUFSIZE];
	struct sglist		sg;
	struct sglist_seg	seg[2];
};

struct vcon_softc {
	device_t		dev;
	struct lwkt_serialize	serialize;
	struct virtqueue	*rx_vq;
	struct virtqueue	*tx_vq;
	struct virtqueue	*ctrl_rx_vq;
	struct virtqueue	*ctrl_tx_vq;
	struct vcon_rxbuf	rx_buf[VCON_RX_BUFS];
	struct vcon_ctrlbuf	ctrl_buf[VCON_CTRL_BUFS];
	cdev_t			cdev;
	bool			stopping;
	bool			multiport;
};

static int vcon_modevent(module_t, int, void *);
static int vcon_probe(device_t);
static int vcon_attach(device_t);
static int vcon_detach(device_t);
static void vcon_rx_intr(void *);
static void vcon_tx_intr(void *);
static void vcon_ctrl_intr(void *);
static void vcon_ctrl_tx_intr(void *);
static void vcon_tty_start(struct tty *);
static int vcon_tty_param(struct tty *, struct termios *);

static d_open_t vcon_open;
static d_close_t vcon_close;
static d_ioctl_t vcon_ioctl;

static struct dev_ops vcon_ops = {
	{ "vcon", 0, D_TTY },
	.d_open = vcon_open,
	.d_close = vcon_close,
	.d_read = ttyread,
	.d_write = ttywrite,
	.d_ioctl = vcon_ioctl,
	.d_kqfilter = ttykqfilter,
	.d_revoke = ttyrevoke
};

static struct virtio_feature_desc vcon_feature_desc[] = {
	{ VIRTIO_F_VERSION_1, "Version1" },
	{ VIRTIO_CONSOLE_F_MULTIPORT, "Multiport" },
	{ 0, NULL }
};

static device_method_t vcon_methods[] = {
	DEVMETHOD(device_probe, vcon_probe),
	DEVMETHOD(device_attach, vcon_attach),
	DEVMETHOD(device_detach, vcon_detach),
	DEVMETHOD_END
};

static driver_t vcon_driver = {
	"vcon", vcon_methods, sizeof(struct vcon_softc)
};
static devclass_t vcon_devclass;

DRIVER_MODULE(virtio_console, virtio_pci_modern, vcon_driver,
	vcon_devclass, vcon_modevent, NULL);
DRIVER_MODULE(virtio_console, virtio_mmio_modern, vcon_driver,
	vcon_devclass, vcon_modevent, NULL);
MODULE_VERSION(virtio_console, 1);
MODULE_DEPEND(virtio_console, virtio_pci, 1, 1, 1);
MODULE_DEPEND(virtio_console, virtio_mmio, 1, 1, 1);

static int
vcon_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
	case MOD_UNLOAD:
	case MOD_SHUTDOWN:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static int
vcon_requeue_ctrl(struct vcon_softc *sc, struct vcon_ctrlbuf *buf)
{
	int error;

	sglist_init(&buf->sg, nitems(buf->seg), buf->seg);
	error = sglist_append(&buf->sg, buf->data, sizeof(buf->data));
	if (error != 0)
		return (error);
	return (virtio_v1_virtqueue_enqueue(sc->ctrl_rx_vq, buf, &buf->sg,
	    0, buf->sg.sg_nseg));
}

static void
vcon_send_control(struct vcon_softc *sc, uint32_t id, uint16_t event,
    uint16_t value)
{
	struct vcon_ctrlbuf *buf;
	struct virtio_console_control *control;

	if (!sc->multiport)
		return;
	buf = kmalloc(sizeof(*buf), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (buf == NULL)
		return;
	control = (struct virtio_console_control *)buf->data;
	control->id = id;
	control->event = event;
	control->value = value;
	sglist_init(&buf->sg, nitems(buf->seg), buf->seg);
	if (sglist_append(&buf->sg, control, sizeof(*control)) == 0 &&
	    virtio_v1_virtqueue_enqueue(sc->ctrl_tx_vq, buf, &buf->sg,
	    buf->sg.sg_nseg, 0) == 0) {
		virtio_v1_virtqueue_notify(sc->ctrl_tx_vq);
	} else {
		kfree(buf, M_DEVBUF);
	}
}

static int
vcon_probe(device_t dev)
{
	if (virtio_v1_get_device_type(dev) != VIRTIO_ID_CONSOLE)
		return (ENXIO);
	device_set_desc(dev, "VirtIO Console Adapter");
	return (BUS_PROBE_DEFAULT);
}

static int
vcon_requeue_rx(struct vcon_softc *sc, struct vcon_rxbuf *buf)
{
	int error;

	sglist_init(&buf->sg, nitems(buf->seg), buf->seg);
	error = sglist_append(&buf->sg, buf->data, sizeof(buf->data));
	if (error != 0)
		return (error);
	return (virtio_v1_virtqueue_enqueue(sc->rx_vq, buf, &buf->sg, 0,
	    buf->sg.sg_nseg));
}

static void
vcon_rx_process_locked(struct vcon_softc *sc)
{
	struct tty *tp;
	struct vcon_rxbuf *buf;
	uint32_t length;
	int i, received;

	tp = sc->cdev->si_tty;
again:
	received = 0;
	while ((buf = virtio_v1_virtqueue_dequeue(sc->rx_vq, &length)) != NULL) {
		if ((tp->t_state & TS_ISOPEN) != 0) {
			/* l_rint() may synchronously start tty output. */
			lwkt_serialize_exit(&sc->serialize);
			for (i = 0; i < MIN(length, sizeof(buf->data)); ++i)
				(*linesw[tp->t_line].l_rint)(buf->data[i], tp);
			lwkt_serialize_enter(&sc->serialize);
		}
		if (vcon_requeue_rx(sc, buf) != 0)
			device_printf(sc->dev, "could not requeue receive buffer\n");
		received++;
	}
	if (received != 0)
		virtio_v1_virtqueue_notify(sc->rx_vq);
	if (virtio_v1_virtqueue_enable_intr(sc->rx_vq) != 0)
		goto again;
}

static void
vcon_tx_reap_locked(struct vcon_softc *sc)
{
	struct vcon_txbuf *buf;

	while ((buf = virtio_v1_virtqueue_dequeue(sc->tx_vq, NULL)) != NULL)
		kfree(buf, M_DEVBUF);
}

static void
vcon_tx_start_locked(struct vcon_softc *sc, struct tty *tp)
{
	struct vcon_txbuf *buf;
	int copied;

	if (sc->stopping || (tp->t_state & (TS_TIMEOUT | TS_TTSTOP)) != 0)
		return;

	tp->t_state |= TS_BUSY;
	while (!virtio_v1_virtqueue_full(sc->tx_vq) && tp->t_outq.c_cc != 0) {
		buf = kmalloc(sizeof(*buf), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (buf == NULL)
			break;
		copied = clist_qtob(&tp->t_outq, buf->data, sizeof(buf->data));
		if (copied == 0) {
			kfree(buf, M_DEVBUF);
			break;
		}
		sglist_init(&buf->sg, nitems(buf->seg), buf->seg);
		if (sglist_append(&buf->sg, buf->data, copied) != 0 ||
		    virtio_v1_virtqueue_enqueue(sc->tx_vq, buf, &buf->sg,
		    buf->sg.sg_nseg, 0) != 0) {
			kfree(buf, M_DEVBUF);
			break;
		}
	}
	if (!virtio_v1_virtqueue_empty(sc->tx_vq))
		virtio_v1_virtqueue_notify(sc->tx_vq);
	tp->t_state &= ~TS_BUSY;
	ttwwakeup(tp);
}

static void
vcon_rx_intr(void *arg)
{
	struct vcon_softc *sc = arg;
	struct tty *tp;

	if (sc->stopping)
		return;
	tp = sc->cdev->si_tty;
	lwkt_gettoken(&tp->t_token);
	lwkt_serialize_enter(&sc->serialize);
	vcon_rx_process_locked(sc);
	lwkt_serialize_exit(&sc->serialize);
	lwkt_reltoken(&tp->t_token);
}

static void
vcon_tx_intr(void *arg)
{
	struct vcon_softc *sc = arg;
	struct tty *tp;

	if (sc->stopping)
		return;
	tp = sc->cdev->si_tty;
	lwkt_gettoken(&tp->t_token);
	lwkt_serialize_enter(&sc->serialize);
	vcon_tx_reap_locked(sc);
	vcon_tx_start_locked(sc, tp);
	lwkt_serialize_exit(&sc->serialize);
	lwkt_reltoken(&tp->t_token);
}

static void
vcon_ctrl_intr(void *arg)
{
	struct vcon_softc *sc = arg;
	struct vcon_ctrlbuf *buf;
	struct virtio_console_control *control;
	uint32_t length;
	uint16_t event;

	if (sc->stopping)
		return;
	lwkt_serialize_enter(&sc->serialize);
	while ((buf = virtio_v1_virtqueue_dequeue(sc->ctrl_rx_vq, &length)) != NULL) {
		if (length >= sizeof(*control)) {
			control = (struct virtio_console_control *)buf->data;
			event = control->event;
			if (event == VIRTIO_CONSOLE_PORT_ADD)
				vcon_send_control(sc, control->id,
				    VIRTIO_CONSOLE_PORT_READY, 1);
			else if (event == VIRTIO_CONSOLE_CONSOLE_PORT)
				vcon_send_control(sc, control->id,
				    VIRTIO_CONSOLE_PORT_OPEN, 1);
		}
		if (vcon_requeue_ctrl(sc, buf) != 0)
			device_printf(sc->dev, "could not requeue control buffer\n");
	}
	virtio_v1_virtqueue_notify(sc->ctrl_rx_vq);
	(void)virtio_v1_virtqueue_enable_intr(sc->ctrl_rx_vq);
	lwkt_serialize_exit(&sc->serialize);
}

static void
vcon_ctrl_tx_intr(void *arg)
{
	struct vcon_softc *sc = arg;
	struct vcon_ctrlbuf *buf;

	if (sc->stopping)
		return;
	lwkt_serialize_enter(&sc->serialize);
	while ((buf = virtio_v1_virtqueue_dequeue(sc->ctrl_tx_vq, NULL)) != NULL)
		kfree(buf, M_DEVBUF);
	lwkt_serialize_exit(&sc->serialize);
}

static int
vcon_attach(device_t dev)
{
	struct vcon_softc *sc;
	struct vq_alloc_info info[4];
	struct tty *tp;
	int error, i;

	sc = device_get_softc(dev);
	sc->dev = dev;
	lwkt_serialize_init(&sc->serialize);
	virtio_v1_set_feature_desc(dev, vcon_feature_desc);
	sc->multiport = (virtio_v1_negotiate_features(dev,
	    VIRTIO_CONSOLE_F_MULTIPORT) & VIRTIO_CONSOLE_F_MULTIPORT) != 0;
	error = virtio_v1_finalize_features(dev);
	if (error != 0)
		return (error);

	VQ_ALLOC_INFO_INIT(&info[0], 0, vcon_rx_intr, sc, &sc->rx_vq,
	    "%s receive", device_get_nameunit(dev));
	VQ_ALLOC_INFO_INIT(&info[1], 0, vcon_tx_intr, sc, &sc->tx_vq,
	    "%s transmit", device_get_nameunit(dev));
	if (sc->multiport) {
		VQ_ALLOC_INFO_INIT(&info[2], 0, vcon_ctrl_intr, sc,
		    &sc->ctrl_rx_vq, "%s control receive", device_get_nameunit(dev));
		VQ_ALLOC_INFO_INIT(&info[3], 0, vcon_ctrl_tx_intr, sc, &sc->ctrl_tx_vq,
		    "%s control transmit", device_get_nameunit(dev));
	}
	error = virtio_v1_alloc_virtqueues(dev, sc->multiport ? 4 : 2, info);
	if (error != 0)
		return (error);
	error = virtio_v1_setup_intr(dev, 0);
	if (error != 0)
		goto fail;

	sc->cdev = make_dev(&vcon_ops, device_get_unit(dev), UID_ROOT,
	    GID_WHEEL, 0600, "vcon%d", device_get_unit(dev));
	if (sc->cdev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	sc->cdev->si_drv1 = sc;
	tp = ttymalloc(&sc->cdev->si_tty);
	tp->t_oproc = vcon_tty_start;
	tp->t_param = vcon_tty_param;
	tp->t_stop = nottystop;
	tp->t_dev = sc->cdev;

	lwkt_serialize_enter(&sc->serialize);
	for (i = 0; i < VCON_RX_BUFS &&
	    !virtio_v1_virtqueue_full(sc->rx_vq); ++i) {
		error = vcon_requeue_rx(sc, &sc->rx_buf[i]);
		if (error != 0)
			break;
	}
	if (i != 0)
		virtio_v1_virtqueue_notify(sc->rx_vq);
	if (sc->multiport) {
		for (i = 0; i < VCON_CTRL_BUFS &&
		    !virtio_v1_virtqueue_full(sc->ctrl_rx_vq); ++i) {
			error = vcon_requeue_ctrl(sc, &sc->ctrl_buf[i]);
			if (error != 0)
				break;
		}
		if (i != 0)
			virtio_v1_virtqueue_notify(sc->ctrl_rx_vq);
	}
	if (error == 0)
		(void)virtio_v1_virtqueue_enable_intr(sc->rx_vq);
	if (error == 0 && sc->multiport) {
		(void)virtio_v1_virtqueue_enable_intr(sc->ctrl_rx_vq);
		vcon_send_control(sc, UINT32_MAX, VIRTIO_CONSOLE_DEVICE_READY, 1);
	}
	lwkt_serialize_exit(&sc->serialize);
	if (error != 0)
		goto fail;
	return (0);

fail:
	if (sc->cdev != NULL) {
		destroy_dev(sc->cdev);
		sc->cdev = NULL;
	}
	if (sc->rx_vq != NULL) {
		int last = 0;

		virtio_v1_virtqueue_drain(sc->rx_vq, &last);
	}
	if (sc->tx_vq != NULL) {
		int last = 0;

		virtio_v1_virtqueue_drain(sc->tx_vq, &last);
	}
	if (sc->ctrl_rx_vq != NULL) {
		int last = 0;
		virtio_v1_virtqueue_drain(sc->ctrl_rx_vq, &last);
	}
	if (sc->ctrl_tx_vq != NULL) {
		int last = 0;
		virtio_v1_virtqueue_drain(sc->ctrl_tx_vq, &last);
	}
	sc->rx_vq = NULL;
	sc->tx_vq = NULL;
	sc->ctrl_rx_vq = NULL;
	sc->ctrl_tx_vq = NULL;
	sc->ctrl_rx_vq = NULL;
	sc->ctrl_tx_vq = NULL;
	return (error);
}

static int
vcon_detach(device_t dev)
{
	struct vcon_softc *sc;
	struct tty *tp;
	struct vcon_txbuf *txbuf;
	struct vcon_ctrlbuf *ctrlbuf;
	int last;

	sc = device_get_softc(dev);
	sc->stopping = true;
	lwkt_serialize_enter(&sc->serialize);
	lwkt_serialize_exit(&sc->serialize);
	virtio_v1_stop(dev);
	if (sc->cdev != NULL) {
		tp = sc->cdev->si_tty;
		lwkt_gettoken(&tp->t_token);
		if ((tp->t_state & TS_ISOPEN) != 0) {
			(*linesw[tp->t_line].l_close)(tp, 0);
			ttyclose(tp);
		}
		lwkt_reltoken(&tp->t_token);
	}
	if (sc->rx_vq != NULL) {
		last = 0;
		while (virtio_v1_virtqueue_drain(sc->rx_vq, &last) != NULL)
			;
	}
	if (sc->tx_vq != NULL) {
		last = 0;
		while ((txbuf = virtio_v1_virtqueue_drain(sc->tx_vq, &last)) != NULL)
			kfree(txbuf, M_DEVBUF);
	}
	if (sc->ctrl_rx_vq != NULL) {
		last = 0;
		while (virtio_v1_virtqueue_drain(sc->ctrl_rx_vq, &last) != NULL)
			;
	}
	if (sc->ctrl_tx_vq != NULL) {
		last = 0;
		while ((ctrlbuf = virtio_v1_virtqueue_drain(sc->ctrl_tx_vq,
		    &last)) != NULL)
			kfree(ctrlbuf, M_DEVBUF);
	}
	if (sc->cdev != NULL)
		destroy_dev(sc->cdev);
	sc->cdev = NULL;
	sc->rx_vq = NULL;
	sc->tx_vq = NULL;
	return (0);
}

static int
vcon_open(struct dev_open_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct tty *tp = dev->si_tty;
	int error;

	lwkt_gettoken(&tp->t_token);
	if ((tp->t_state & TS_ISOPEN) == 0) {
		tp->t_state |= TS_CARR_ON;
		ttychars(tp);
		tp->t_iflag = TTYDEF_IFLAG;
		tp->t_oflag = TTYDEF_OFLAG;
		tp->t_cflag = TTYDEF_CFLAG | CLOCAL;
		tp->t_lflag = TTYDEF_LFLAG;
		tp->t_ispeed = tp->t_ospeed = TTYDEF_SPEED;
		ttsetwater(tp);
	} else if ((tp->t_state & TS_XCLUDE) != 0 &&
	    caps_priv_check(ap->a_cred, SYSCAP_RESTRICTEDROOT)) {
		lwkt_reltoken(&tp->t_token);
		return (EBUSY);
	}
	error = (*linesw[tp->t_line].l_open)(dev, tp);
	lwkt_reltoken(&tp->t_token);
	return (error);
}

static int
vcon_close(struct dev_close_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct tty *tp = dev->si_tty;

	lwkt_gettoken(&tp->t_token);
	if ((tp->t_state & TS_ISOPEN) != 0) {
		(*linesw[tp->t_line].l_close)(tp, ap->a_fflag);
		ttyclose(tp);
	}
	lwkt_reltoken(&tp->t_token);
	return (0);
}

static int
vcon_ioctl(struct dev_ioctl_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct tty *tp = dev->si_tty;
	int error;

	lwkt_gettoken(&tp->t_token);
	error = (*linesw[tp->t_line].l_ioctl)(tp, ap->a_cmd, ap->a_data,
	    ap->a_fflag, ap->a_cred);
	if (error == ENOIOCTL)
		error = ttioctl(tp, ap->a_cmd, ap->a_data, ap->a_fflag);
	lwkt_reltoken(&tp->t_token);
	return (error == ENOIOCTL ? ENOTTY : error);
}

static int
vcon_tty_param(struct tty *tp, struct termios *termios)
{
	lwkt_gettoken(&tp->t_token);
	tp->t_ispeed = termios->c_ispeed;
	tp->t_ospeed = termios->c_ospeed;
	tp->t_cflag = termios->c_cflag;
	lwkt_reltoken(&tp->t_token);
	return (0);
}

static void
vcon_tty_start(struct tty *tp)
{
	struct vcon_softc *sc;

	lwkt_gettoken(&tp->t_token);
	sc = tp->t_dev == NULL ? NULL : tp->t_dev->si_drv1;
	if (sc != NULL && !sc->stopping) {
		lwkt_serialize_enter(&sc->serialize);
		vcon_tx_reap_locked(sc);
		vcon_tx_start_locked(sc, tp);
		lwkt_serialize_exit(&sc->serialize);
	} else {
		ttwwakeup(tp);
	}
	lwkt_reltoken(&tp->t_token);
}
