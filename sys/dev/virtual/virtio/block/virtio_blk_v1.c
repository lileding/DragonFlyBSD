/*-
 * Copyright (c) 2011, Bryan Venteicher <bryanv@FreeBSD.org>
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
 * $FreeBSD: head/sys/dev/virtio/block/virtio_blk.c 252707 2013-07-04 17:57:26Z bryanv $
 */

/* Driver for VirtIO block devices. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bio.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sglist.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <sys/serialize.h>
#include <sys/buf2.h>
#include <sys/rman.h>
#include <sys/disk.h>
#include <sys/devicestat.h>

#include <dev/virtual/virtio/v1/virtio.h>
#include <dev/virtual/virtio/v1/virtqueue.h>
#include "virtio_blk.h"

struct vtblk_v1_request {
	struct virtio_blk_outhdr	 vbr_hdr __aligned(16);
	struct bio			*vbr_bio;
	uint8_t				 vbr_ack;

	SLIST_ENTRY(vtblk_v1_request)	 vbr_link;
};

enum vtblk_v1_cache_mode {
	VTBLK_CACHE_WRITETHROUGH,
	VTBLK_CACHE_WRITEBACK,
	VTBLK_CACHE_MAX
};

struct vtblk_v1_queue {
	struct vtblk_v1_softc	*vtblk_v1_sc;
	struct virtqueue	*vtblk_v1_vq;
	struct sglist		*vtblk_v1_sglist;
	struct bio_queue_head	 vtblk_v1_bioq;
	SLIST_HEAD(, vtblk_v1_request)
				 vtblk_v1_req_free;
	struct lwkt_serialize	 vtblk_v1_slz;
};

struct vtblk_v1_softc {
	device_t		 vtblk_v1_dev;
	uint64_t		 vtblk_v1_features;
	uint32_t		 vtblk_v1_flags;
#define VTBLK_FLAG_INDIRECT	0x0001
#define VTBLK_FLAG_READONLY	0x0002
#define VTBLK_FLAG_DETACH	0x0004
#define VTBLK_FLAG_SUSPEND	0x0008
#define VTBLK_FLAG_DUMPING	0x0010
#define VTBLK_FLAG_WC_CONFIG	0x0020

	struct disk		 vtblk_v1_disk;
	cdev_t			 cdev;
	struct devstat		 stats;

	struct vtblk_v1_queue	 vtblk_v1_queues[SMP_MAXCPU];
	u_int			 vtblk_v1_nqs;
	u_int			 vtblk_v1_nintrs;
	int			 vtblk_v1_vqmap[SMP_MAXCPU];

	int			 vtblk_v1_sector_size;
	int			 vtblk_v1_max_nsegs;
	int			 vtblk_v1_request_count;
	enum vtblk_v1_cache_mode	 vtblk_v1_write_cache;

	struct vtblk_v1_request	 vtblk_v1_dump_request;
};

static struct virtio_feature_desc vtblk_v1_feature_desc[] = {
	{ VIRTIO_BLK_F_BARRIER,		"HostBarrier"	},
	{ VIRTIO_BLK_F_SIZE_MAX,	"MaxSegSize"	},
	{ VIRTIO_BLK_F_SEG_MAX,		"MaxNumSegs"	},
	{ VIRTIO_BLK_F_GEOMETRY,	"DiskGeometry"	},
	{ VIRTIO_BLK_F_RO,		"ReadOnly"	},
	{ VIRTIO_BLK_F_BLK_SIZE,	"BlockSize"	},
	{ VIRTIO_BLK_F_SCSI,		"SCSICmds"	},
	{ VIRTIO_BLK_F_FLUSH,		"FlushCommand"	},
	{ VIRTIO_BLK_F_TOPOLOGY,	"Topology"	},
	{ VIRTIO_BLK_F_CONFIG_WCE,	"ConfigWCE"	},
	{ VIRTIO_BLK_F_MQ,		"MultiQueue"	},
	{ VIRTIO_BLK_F_DISCARD,		"Discard"	},
	{ VIRTIO_BLK_F_WRITE_ZEROES,	"WriteZeroes"	},

	{ 0, NULL }
};

static int	vtblk_v1_probe(device_t);
static int	vtblk_v1_attach(device_t);
static int	vtblk_v1_detach(device_t);
static int	vtblk_v1_suspend(device_t);
static int	vtblk_v1_resume(device_t);
static int	vtblk_v1_shutdown(device_t);

static void	vtblk_v1_negotiate_features(struct vtblk_v1_softc *);
static int	vtblk_v1_maximum_segments(struct vtblk_v1_softc *,
		    struct virtio_blk_config *);
static int	vtblk_v1_alloc_virtqueues(struct vtblk_v1_softc *);
static void	vtblk_v1_set_write_cache(struct vtblk_v1_softc *, int);
static int	vtblk_v1_write_cache_enabled(struct vtblk_v1_softc *,
		    struct virtio_blk_config *);
static int	vtblk_v1_write_cache_sysctl(SYSCTL_HANDLER_ARGS);
static void	vtblk_v1_alloc_disk(struct vtblk_v1_softc *,
		    struct virtio_blk_config *);
/*
 * Interface to the device switch.
 */
static d_open_t		vtblk_v1_open;
static d_strategy_t	vtblk_v1_strategy;
static d_dump_t		vtblk_v1_dump;

static struct dev_ops vbd_disk_ops = {
	{ "vbd", 200, D_DISK | D_MPSAFE | D_KVABIO },
	.d_open		= vtblk_v1_open,
	.d_close	= nullclose,
	.d_read		= physread,
	.d_write	= physwrite,
	.d_strategy	= vtblk_v1_strategy,
	.d_dump		= vtblk_v1_dump,
};

static void	vtblk_v1_vq_startio(struct vtblk_v1_queue *);
static struct vtblk_v1_request * vtblk_v1_bio_request(struct vtblk_v1_queue *);
static int	vtblk_v1_execute_request(struct vtblk_v1_queue *,
		    struct vtblk_v1_request *);
static void	vtblk_v1_vq_intr(void *);
static void	vtblk_v1_vq_intr_locked(void *);

static void	vtblk_v1_prepare_dump(struct vtblk_v1_softc *);
static int	vtblk_v1_write_dump(struct vtblk_v1_softc *, void *, off_t, size_t);
static int	vtblk_v1_flush_dump(struct vtblk_v1_softc *);
static int	vtblk_v1_poll_request(struct vtblk_v1_softc *,
		    struct vtblk_v1_request *);

static void	vtblk_v1_drain_vq(struct vtblk_v1_queue *, int);
static void	vtblk_v1_drain(struct vtblk_v1_softc *);

static int	vtblk_v1_alloc_requests(struct vtblk_v1_queue *);
static void	vtblk_v1_free_requests(struct vtblk_v1_softc *);
static struct vtblk_v1_request * vtblk_v1_dequeue_request(struct vtblk_v1_queue *);
static void	vtblk_v1_enqueue_request(struct vtblk_v1_queue *,
		    struct vtblk_v1_request *);

static int	vtblk_v1_request_error(struct vtblk_v1_request *);
static void	vtblk_v1_finish_bio(struct bio *, int);

static void	vtblk_v1_setup_sysctl(struct vtblk_v1_softc *);
static int	vtblk_v1_tunable_int(struct vtblk_v1_softc *, const char *, int);

/* Tunables. */
static int vtblk_v1_writecache_mode = -1;
TUNABLE_INT("hw.vtblk.writecache_mode", &vtblk_v1_writecache_mode);
static int vtblk_v1_max_queues = SMP_MAXCPU;
TUNABLE_INT("hw.vtblk.max_queues", &vtblk_v1_max_queues);

/* Features desired/implemented by this driver. */
#define VTBLK_FEATURES \
    (VIRTIO_BLK_F_SIZE_MAX		| \
     VIRTIO_BLK_F_SEG_MAX		| \
     VIRTIO_BLK_F_GEOMETRY		| \
     VIRTIO_BLK_F_RO			| \
     VIRTIO_BLK_F_BLK_SIZE		| \
     VIRTIO_BLK_F_FLUSH			| \
     VIRTIO_BLK_F_CONFIG_WCE		| \
     VIRTIO_BLK_F_MQ			| \
     VIRTIO_RING_F_INDIRECT_DESC)

/*
 * Each block request uses at least two segments - one for the header
 * and one for the status.
 */
#define VTBLK_MIN_SEGMENTS	2

static device_method_t vtblk_v1_methods[] = {
	/* Device methods. */
	DEVMETHOD(device_probe,		vtblk_v1_probe),
	DEVMETHOD(device_attach,	vtblk_v1_attach),
	DEVMETHOD(device_detach,	vtblk_v1_detach),
	DEVMETHOD(device_suspend,	vtblk_v1_suspend),
	DEVMETHOD(device_resume,	vtblk_v1_resume),
	DEVMETHOD(device_shutdown,	vtblk_v1_shutdown),

	DEVMETHOD_END
};

static driver_t vtblk_v1_driver = {
	"vtblk",
	vtblk_v1_methods,
	sizeof(struct vtblk_v1_softc)
};
static devclass_t vtblk_v1_devclass;

DRIVER_MODULE(virtio_blk_v1, virtio_pci_modern, vtblk_v1_driver, vtblk_v1_devclass, NULL, NULL);
DRIVER_MODULE(virtio_blk_v1, virtio_mmio_modern, vtblk_v1_driver, vtblk_v1_devclass, NULL, NULL);
MODULE_VERSION(virtio_blk_v1, 1);
MODULE_DEPEND(virtio_blk_v1, virtio_pci, 1, 1, 1);
MODULE_DEPEND(virtio_blk_v1, virtio_mmio, 1, 1, 1);

static int
vtblk_v1_probe(device_t dev)
{

	if (virtio_v1_get_device_type(dev) != VIRTIO_ID_BLOCK)
		return (ENXIO);

	device_set_desc(dev, "VirtIO Block Adapter");

	return (BUS_PROBE_DEFAULT);
}

static int
vtblk_v1_attach(device_t dev)
{
	struct vtblk_v1_softc *sc;
	struct virtio_blk_config blkcfg;
	int error;
	int i;

	sc = device_get_softc(dev);
	sc->vtblk_v1_dev = dev;

	virtio_v1_set_feature_desc(dev, vtblk_v1_feature_desc);
	vtblk_v1_negotiate_features(sc);

	if (virtio_v1_with_feature(dev, VIRTIO_RING_F_INDIRECT_DESC))
		sc->vtblk_v1_flags |= VTBLK_FLAG_INDIRECT;
	if (virtio_v1_with_feature(dev, VIRTIO_BLK_F_RO))
		sc->vtblk_v1_flags |= VTBLK_FLAG_READONLY;
	if (virtio_v1_with_feature(dev, VIRTIO_BLK_F_CONFIG_WCE))
		sc->vtblk_v1_flags |= VTBLK_FLAG_WC_CONFIG;

	/* Get local copy of config. */
	virtio_v1_read_device_config_array(dev, 0, &blkcfg, sizeof(uint32_t),
	    sizeof(blkcfg) / sizeof(uint32_t));

	/*
	 * With the current sglist(9) implementation, it is not easy
	 * for us to support a maximum segment size as adjacent
	 * segments are coalesced. For now, just make sure it's larger
	 * than the maximum supported transfer size.
	 */
	if (virtio_v1_with_feature(dev, VIRTIO_BLK_F_SIZE_MAX)) {
		if (blkcfg.size_max < MAXPHYS) {
			error = ENOTSUP;
			device_printf(dev, "host requires unsupported "
			    "maximum segment size feature\n");
			return error;
		}
	}

	sc->vtblk_v1_max_nsegs = vtblk_v1_maximum_segments(sc, &blkcfg);
	if (sc->vtblk_v1_max_nsegs <= VTBLK_MIN_SEGMENTS) {
		error = EINVAL;
		device_printf(dev, "fewer than minimum number of segments "
		    "allowed: %d\n", sc->vtblk_v1_max_nsegs);
		return error;
	}


	/* V1 transport currently uses one shared interrupt/queue. */
	sc->vtblk_v1_nqs = 1;
	sc->vtblk_v1_nintrs = 1;

	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		struct vtblk_v1_queue *vq = &sc->vtblk_v1_queues[i];

		vq->vtblk_v1_sc = sc;
		lwkt_serialize_init(&vq->vtblk_v1_slz);
		bioq_init(&vq->vtblk_v1_bioq);
		SLIST_INIT(&vq->vtblk_v1_req_free);
		/*
		 * Allocate working sglist. The number of segments may be too
		 * large to safely store on the stack.
		 */
		vq->vtblk_v1_sglist = sglist_alloc(sc->vtblk_v1_max_nsegs, M_INTWAIT);
		if (vq->vtblk_v1_sglist == NULL) {
			error = ENOMEM;
			device_printf(dev, "cannot allocate sglist\n");
			goto fail;
		}
	}

	for (i = 0; i < ncpus; i++) {
		// Could be improved to take the CPU topology into account.
		sc->vtblk_v1_vqmap[i] = i % sc->vtblk_v1_nqs;
	}

	error = vtblk_v1_alloc_virtqueues(sc);
	if (error) {
		device_printf(dev, "cannot allocate virtqueues\n");
		goto fail;
	}

	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		error = vtblk_v1_alloc_requests(&sc->vtblk_v1_queues[i]);
		if (error) {
			device_printf(dev, "cannot preallocate requests\n");
			goto fail;
		}
	}

	for (i = 0; i < sc->vtblk_v1_nintrs; i++) {
		error = virtio_v1_setup_intr(dev, 0);
		if (error) {
			device_printf(dev,
				      "cannot setup virtqueue interrupt\n");
			goto fail;
		}
	}

	for (i = 0; i < sc->vtblk_v1_nqs; i++)
		virtio_v1_virtqueue_enable_intr(sc->vtblk_v1_queues[i].vtblk_v1_vq);

	vtblk_v1_alloc_disk(sc, &blkcfg);
	vtblk_v1_setup_sysctl(sc);

fail:
	if (error)
		vtblk_v1_detach(dev);

	return (error);
}

static int
vtblk_v1_detach(device_t dev)
{
	struct vtblk_v1_softc *sc;
	int i;

	sc = device_get_softc(dev);


	sc->vtblk_v1_flags |= VTBLK_FLAG_DETACH;
	// Once VTBLK_FLAG_DETACH is set, we just need to take the virtqueue
	// serializers once, to make sure that any pending d_strategy call is
	// finished, or will return ENXIO.
	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		struct vtblk_v1_queue *vq = &sc->vtblk_v1_queues[i];

		lwkt_serialize_enter(&vq->vtblk_v1_slz);
		lwkt_serialize_exit(&vq->vtblk_v1_slz);
	}
	virtio_v1_stop(sc->vtblk_v1_dev);

	// Now the device is fully stopped, and we can clean up everything
	// safely.
	vtblk_v1_drain(sc);
	if (sc->cdev != NULL) {
		disk_destroy(&sc->vtblk_v1_disk);
		sc->cdev = NULL;
	}

	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		struct vtblk_v1_queue *vq = &sc->vtblk_v1_queues[i];

		if (vq->vtblk_v1_sglist != NULL) {
			sglist_free(vq->vtblk_v1_sglist);
			vq->vtblk_v1_sglist = NULL;
		}
	}

	return (0);
}

static int
vtblk_v1_suspend(device_t dev)
{
	struct vtblk_v1_softc *sc = device_get_softc(dev);
	int i;

	sc->vtblk_v1_flags |= VTBLK_FLAG_SUSPEND;
	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		lwkt_serialize_enter(&sc->vtblk_v1_queues[i].vtblk_v1_slz);
		/* XXX BMV: virtio_v1_stop(), etc needed here? */
		lwkt_serialize_exit(&sc->vtblk_v1_queues[i].vtblk_v1_slz);
	}

	return (0);
}

static int
vtblk_v1_resume(device_t dev)
{
	struct vtblk_v1_softc *sc = device_get_softc(dev);
	int i;

	sc->vtblk_v1_flags &= ~VTBLK_FLAG_SUSPEND;
	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		lwkt_serialize_enter(&sc->vtblk_v1_queues[i].vtblk_v1_slz);
		/* XXX BMV: virtio_v1_reinit(), etc needed here? */
#if 0 /* XXX Resume IO? */
		vtblk_v1_vq_startio(&sc->vtblk_v1_queues[i]);
#endif
		lwkt_serialize_exit(&sc->vtblk_v1_queues[i].vtblk_v1_slz);
	}

	return (0);
}

static int
vtblk_v1_shutdown(device_t dev)
{

	return (0);
}

static int
vtblk_v1_open(struct dev_open_args *ap)
{
	struct vtblk_v1_softc *sc;
	cdev_t dev = ap->a_head.a_dev;
	sc = dev->si_drv1;
	if (sc == NULL)
		return (ENXIO);

	if ((ap->a_oflags & FWRITE) && (sc->vtblk_v1_flags & VTBLK_FLAG_READONLY))
		return (EACCES);

	return (sc->vtblk_v1_flags & VTBLK_FLAG_DETACH ? ENXIO : 0);
}

static int
vtblk_v1_dump(struct dev_dump_args *ap)
{
	struct vtblk_v1_softc *sc;
	cdev_t dev = ap->a_head.a_dev;
        uint64_t buf_start, buf_len;
        int error;

	sc = dev->si_drv1;
	if (sc == NULL)
		return (ENXIO);

        buf_start = ap->a_offset;
        buf_len = ap->a_length;

//	lwkt_serialize_enter(&sc->vtblk_v1_queues[0].vtblk_v1_slz);

	if ((sc->vtblk_v1_flags & VTBLK_FLAG_DUMPING) == 0) {
		vtblk_v1_prepare_dump(sc);
		sc->vtblk_v1_flags |= VTBLK_FLAG_DUMPING;
	}

	if (buf_len > 0)
		error = vtblk_v1_write_dump(sc, ap->a_virtual, buf_start,
		    buf_len);
	else if (buf_len == 0)
		error = vtblk_v1_flush_dump(sc);
	else {
		error = EINVAL;
		sc->vtblk_v1_flags &= ~VTBLK_FLAG_DUMPING;
	}

//	lwkt_serialize_exit(&sc->vtblk_v1_queues[0].vtblk_v1_slz);

	return (error);
}

/*
 * WARNING! We are using the KVABIO API and must not access memory
 *          through bp->b_data without first calling bkvasync(bp).
 */
static int
vtblk_v1_strategy(struct dev_strategy_args *ap)
{
	struct vtblk_v1_softc *sc;
	cdev_t dev = ap->a_head.a_dev;
	sc = dev->si_drv1;
	struct bio *bio = ap->a_bio;
	struct vtblk_v1_queue *q = &sc->vtblk_v1_queues[sc->vtblk_v1_vqmap[mycpuid]];

	if (sc == NULL) {
		vtblk_v1_finish_bio(bio, EINVAL);
		return EINVAL;
	}

	lwkt_serialize_enter(&q->vtblk_v1_slz);
	if ((sc->vtblk_v1_flags & VTBLK_FLAG_DETACH) == 0) {
		bioqdisksort(&q->vtblk_v1_bioq, bio);
		vtblk_v1_vq_startio(q);
		lwkt_serialize_exit(&q->vtblk_v1_slz);
	} else {
		lwkt_serialize_exit(&q->vtblk_v1_slz);
		vtblk_v1_finish_bio(bio, ENXIO);
	}
	return 0;
}

static void
vtblk_v1_negotiate_features(struct vtblk_v1_softc *sc)
{
	device_t dev;
	uint64_t features;

	dev = sc->vtblk_v1_dev;
	features = VTBLK_FEATURES & ~VIRTIO_BLK_F_MQ;

	sc->vtblk_v1_features = virtio_v1_negotiate_features(dev, features);
}

/*
 * Calculate the maximum number of DMA segment supported.  Note
 * that the in/out header is encoded in the segment list.  We
 * assume that VTBLK_MIN_SEGMENTS covers that part of it so
 * we add it into the desired total.  If the SEG_MAX feature
 * is not specified we have to just assume that the host can
 * handle the maximum number of segments required for a MAXPHYS
 * sized request.
 *
 * The additional + 1 is in case a MAXPHYS-sized buffer crosses
 * a page boundary.
 */
static int
vtblk_v1_maximum_segments(struct vtblk_v1_softc *sc,
    struct virtio_blk_config *blkcfg)
{
	device_t dev;
	int nsegs;

	dev = sc->vtblk_v1_dev;
	nsegs = VTBLK_MIN_SEGMENTS;

	if (virtio_v1_with_feature(dev, VIRTIO_BLK_F_SEG_MAX)) {
		nsegs = MIN(blkcfg->seg_max, MAXPHYS / PAGE_SIZE + 1 + nsegs);
	} else {
		nsegs = MAXPHYS / PAGE_SIZE + 1 + nsegs;
	}
	if (sc->vtblk_v1_flags & VTBLK_FLAG_INDIRECT)
		nsegs = MIN(nsegs, VIRTIO_MAX_INDIRECT);

	return (nsegs);
}

static int
vtblk_v1_alloc_virtqueues(struct vtblk_v1_softc *sc)
{
	device_t dev = sc->vtblk_v1_dev;
	struct vq_alloc_info *vq_info =
	    kmalloc(sc->vtblk_v1_nqs * sizeof(struct vq_alloc_info),
	    M_TEMP, M_WAITOK | M_ZERO);
	int i;
	int error;

	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		VQ_ALLOC_INFO_INIT(&vq_info[i], sc->vtblk_v1_max_nsegs,
		    vtblk_v1_vq_intr, &sc->vtblk_v1_queues[i],
		    &sc->vtblk_v1_queues[i].vtblk_v1_vq, "%s request %d",
		    device_get_nameunit(dev), i);
	}

	error = virtio_v1_alloc_virtqueues(dev, sc->vtblk_v1_nqs, vq_info);
	kfree(vq_info, M_TEMP);
	return error;
}

static void
vtblk_v1_set_write_cache(struct vtblk_v1_softc *sc, int wc)
{

	/* Set either writeback (1) or writethrough (0) mode. */
	virtio_v1_write_dev_config_1(sc->vtblk_v1_dev,
	    offsetof(struct virtio_blk_config, writeback), wc);
}

static int
vtblk_v1_write_cache_enabled(struct vtblk_v1_softc *sc,
    struct virtio_blk_config *blkcfg)
{
	int wc;

	if (sc->vtblk_v1_flags & VTBLK_FLAG_WC_CONFIG) {
		wc = vtblk_v1_tunable_int(sc, "writecache_mode",
		    vtblk_v1_writecache_mode);
		if (wc >= 0 && wc < VTBLK_CACHE_MAX)
			vtblk_v1_set_write_cache(sc, wc);
		else
			wc = blkcfg->writeback;
	} else
		wc = virtio_v1_with_feature(sc->vtblk_v1_dev, VIRTIO_BLK_F_FLUSH);

	return (wc);
}

static int
vtblk_v1_write_cache_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct vtblk_v1_softc *sc;
	int oldwc, wc, error;

	sc = oidp->oid_arg1;
	oldwc = wc = sc->vtblk_v1_write_cache;

	error = sysctl_handle_int(oidp, &wc, 0, req);
	if (error || req->newptr == NULL)
		return (error);
	if ((sc->vtblk_v1_flags & VTBLK_FLAG_WC_CONFIG) == 0)
		return (EPERM);
	if (wc < 0 || wc >= VTBLK_CACHE_MAX)
		return (EINVAL);

	if (oldwc == wc)
		return (0);

	// Not used outside this SYSCTL right now, so no locking needed.
	//lwkt_serialize_enter(&sc->vtblk_v1_queues[0].vtblk_v1_slz);
	sc->vtblk_v1_write_cache = wc;
	vtblk_v1_set_write_cache(sc, sc->vtblk_v1_write_cache);
	//lwkt_serialize_exit(&sc->vtblk_v1_queues[0].vtblk_v1_slz);

	return (0);
}

static void
vtblk_v1_alloc_disk(struct vtblk_v1_softc *sc, struct virtio_blk_config *blkcfg)
{
	struct disk_info info;

	/* construct the disk_info */
	bzero(&info, sizeof(info));

	if (virtio_v1_with_feature(sc->vtblk_v1_dev, VIRTIO_BLK_F_BLK_SIZE))
		sc->vtblk_v1_sector_size = blkcfg->blk_size;
	else
		sc->vtblk_v1_sector_size = 512;

	/* blkcfg->capacity is always expressed in 512 byte sectors. */
	info.d_media_blksize = sc->vtblk_v1_sector_size;
	info.d_media_blocks = blkcfg->capacity * 512 / info.d_media_blksize;

	if (virtio_v1_with_feature(sc->vtblk_v1_dev, VIRTIO_BLK_F_GEOMETRY)) {
		info.d_ncylinders = blkcfg->geometry.cylinders;
		info.d_nheads = blkcfg->geometry.heads;
		info.d_secpertrack = blkcfg->geometry.sectors;
		info.d_secpercyl = info.d_secpertrack * info.d_nheads;

		/*
		 * If the virtio device is reporting a legacy cylinder count,
		 * recalculate ncylinders based on the media size
		 */
		if (info.d_ncylinders == 16383) {
			info.d_ncylinders = info.d_media_blocks /
			    (info.d_nheads * info.d_secpertrack);
			device_printf(sc->vtblk_v1_dev,
				      "Virtio: ncylinders at legacy maximum "
				      "(16383), recalculating to %d\n",
				      info.d_ncylinders);
		}
	} else {
		/* Fabricate a geometry */
		info.d_secpertrack = 1024;
		info.d_nheads = 1;
		info.d_secpercyl = info.d_secpertrack * info.d_nheads;
		info.d_ncylinders =
		    (u_int)(info.d_media_blocks / info.d_secpercyl);
	}

	if (vtblk_v1_write_cache_enabled(sc, blkcfg) != 0)
		sc->vtblk_v1_write_cache = VTBLK_CACHE_WRITEBACK;
	else
		sc->vtblk_v1_write_cache = VTBLK_CACHE_WRITETHROUGH;

	devstat_add_entry(&sc->stats, "vbd", device_get_unit(sc->vtblk_v1_dev),
			  DEV_BSIZE, DEVSTAT_ALL_SUPPORTED,
			  DEVSTAT_TYPE_DIRECT | DEVSTAT_TYPE_IF_OTHER,
			  DEVSTAT_PRIORITY_DISK);

	/* attach a generic disk device to ourselves */
	sc->cdev = disk_create(device_get_unit(sc->vtblk_v1_dev), &sc->vtblk_v1_disk,
			       &vbd_disk_ops);

	sc->cdev->si_drv1 = sc;
	sc->cdev->si_iosize_max = MAXPHYS;
	disk_setdiskinfo(&sc->vtblk_v1_disk, &info);
	if (virtio_v1_with_feature(sc->vtblk_v1_dev, VIRTIO_BLK_F_BLK_SIZE)) {
		device_printf(sc->vtblk_v1_dev, "Block size: %u\n",
		    sc->vtblk_v1_sector_size);
	}
	device_printf(sc->vtblk_v1_dev,
	    "%juMB (%ju %d byte sectors: %dH %dS/T %dC)\n",
	    ((uintmax_t)info.d_media_blocks * info.d_media_blksize) >> 20,
	    (uintmax_t)info.d_media_blocks, info.d_media_blksize,
	    blkcfg->geometry.heads, blkcfg->geometry.sectors,
	    blkcfg->geometry.cylinders);
}

static void
vtblk_v1_vq_startio(struct vtblk_v1_queue *q)
{
	struct vtblk_v1_softc *sc = q->vtblk_v1_sc;
	struct virtqueue *vq;
	struct vtblk_v1_request *req;
	int enq;

	vq = q->vtblk_v1_vq;
	enq = 0;

	ASSERT_SERIALIZED(&q->vtblk_v1_slz);

	if (sc->vtblk_v1_flags & VTBLK_FLAG_SUSPEND)
		return;

	while (!virtio_v1_virtqueue_full(vq)) {
		req = vtblk_v1_bio_request(q);
		if (req == NULL)
			break;

		if (vtblk_v1_execute_request(q, req) != 0) {
			bioqdisksort(&q->vtblk_v1_bioq, req->vbr_bio);
			vtblk_v1_enqueue_request(q, req);
			break;
		}
		devstat_start_transaction(&sc->stats);

		enq++;
	}

	if (enq > 0)
		virtio_v1_virtqueue_notify(vq);
}

static struct vtblk_v1_request *
vtblk_v1_bio_request(struct vtblk_v1_queue *q)
{
	struct bio_queue_head *bioq;
	struct vtblk_v1_request *req;
	struct bio *bio;
	struct buf *bp;

	bioq = &q->vtblk_v1_bioq;

	if (bioq_first(bioq) == NULL)
		return (NULL);

	req = vtblk_v1_dequeue_request(q);
	if (req == NULL)
		return (NULL);

	bio = bioq_takefirst(bioq);
	req->vbr_bio = bio;
	req->vbr_ack = -1;
	req->vbr_hdr.ioprio = 1;
	bp = bio->bio_buf;

	switch (bp->b_cmd) {
	case BUF_CMD_FLUSH:
		req->vbr_hdr.type = VIRTIO_BLK_T_FLUSH;
		break;
	case BUF_CMD_READ:
		req->vbr_hdr.type = VIRTIO_BLK_T_IN;
		req->vbr_hdr.sector = bio->bio_offset / DEV_BSIZE;
		break;
	case BUF_CMD_WRITE:
		req->vbr_hdr.type = VIRTIO_BLK_T_OUT;
		req->vbr_hdr.sector = bio->bio_offset / DEV_BSIZE;
		break;
	default:
		KASSERT(0, ("bio with unhandled cmd: %d", bp->b_cmd));
		req->vbr_hdr.type = -1;
		break;
	}

	return (req);
}

static int
vtblk_v1_execute_request(struct vtblk_v1_queue *q, struct vtblk_v1_request *req)
{
	struct sglist *sg;
	struct bio *bio;
	struct buf *bp;
	int writable, error;

	sg = q->vtblk_v1_sglist;
	bio = req->vbr_bio;
	bp = bio->bio_buf;
	writable = 0;

	/*
	 * sglist is live throughout this subroutine.
	 */
	error = sglist_append(sg, &req->vbr_hdr,
			      sizeof(struct virtio_blk_outhdr));
	KASSERT(error == 0, ("error adding header to sglist"));
	KASSERT(sg->sg_nseg == 1,
	    ("header spanned multiple segments: %d", sg->sg_nseg));

	if (bp->b_cmd == BUF_CMD_READ || bp->b_cmd == BUF_CMD_WRITE) {
		error = sglist_append(sg, bp->b_data, bp->b_bcount);
		KASSERT(error == 0, ("error adding buffer to sglist"));

		/* BUF_CMD_READ means the host writes into our buffer. */
		if (bp->b_cmd == BUF_CMD_READ)
			writable += sg->sg_nseg - 1;
	}

	error = sglist_append(sg, &req->vbr_ack, sizeof(uint8_t));
	KASSERT(error == 0, ("error adding ack to sglist"));
	writable++;

	KASSERT(sg->sg_nseg >= VTBLK_MIN_SEGMENTS,
	    ("fewer than min segments: %d", sg->sg_nseg));

	error = virtio_v1_virtqueue_enqueue(q->vtblk_v1_vq, req, sg,
				  sg->sg_nseg - writable, writable);

	sglist_reset(sg);

	return (error);
}

static void
vtblk_v1_vq_intr(void *arg)
{
	struct vtblk_v1_queue *q = arg;

	lwkt_serialize_enter(&q->vtblk_v1_slz);
	vtblk_v1_vq_intr_locked(arg);
	lwkt_serialize_exit(&q->vtblk_v1_slz);
}

static void
vtblk_v1_vq_intr_locked(void *arg)
{
	struct vtblk_v1_queue *q = arg;
	struct vtblk_v1_softc *sc = q->vtblk_v1_sc;
	struct virtqueue *vq = q->vtblk_v1_vq;
	struct vtblk_v1_request *req;
	struct bio *bio;
	struct buf *bp;

	ASSERT_SERIALIZED(&q->vtblk_v1_slz);

	if (!virtio_v1_virtqueue_nused(vq))
		return;

	lwkt_serialize_handler_disable(&q->vtblk_v1_slz);
	virtio_v1_virtqueue_disable_intr(q->vtblk_v1_vq);

retry:
	if (sc->vtblk_v1_flags & VTBLK_FLAG_DETACH)
		return;

	while ((req = virtio_v1_virtqueue_dequeue(vq, NULL)) != NULL) {
		bio = req->vbr_bio;
		bp = bio->bio_buf;

		if (req->vbr_ack == VIRTIO_BLK_S_OK) {
			bp->b_resid = 0;
		} else {
			bp->b_flags |= B_ERROR;
			if (req->vbr_ack == VIRTIO_BLK_S_UNSUPP) {
				bp->b_error = ENOTSUP;
			} else {
				bp->b_error = EIO;
			}
		}

		devstat_end_transaction_buf(&sc->stats, bio->bio_buf);

		lwkt_serialize_exit(&q->vtblk_v1_slz);
		/*
		 * Unlocking the controller around biodone() does not allow
		 * processing further device interrupts; when we queued
		 * vtblk_v1_vq_intr, we disabled interrupts. It will allow
		 * concurrent vtblk_v1_strategy/_startio command dispatches.
		 */
		biodone(bio);
		lwkt_serialize_enter(&q->vtblk_v1_slz);

		vtblk_v1_enqueue_request(q, req);
	}

	vtblk_v1_vq_startio(q);

	if (virtio_v1_virtqueue_enable_intr(vq) != 0) {
		/*
		 * If new virtqueue entries appeared immediately after
		 * enabling interrupts, process them now. Release and
		 * retake softcontroller lock to try to avoid blocking
		 * I/O dispatch for too long.
		 */
		virtio_v1_virtqueue_disable_intr(vq);
		goto retry;
	}
	lwkt_serialize_handler_enable(&q->vtblk_v1_slz);
}

static void
vtblk_v1_prepare_dump(struct vtblk_v1_softc *sc)
{
	device_t dev;
	int i;

	dev = sc->vtblk_v1_dev;

	for (i = 0 ; i < sc->vtblk_v1_nqs; i++)
		virtio_v1_virtqueue_disable_intr(sc->vtblk_v1_queues[i].vtblk_v1_vq);

	virtio_v1_stop(sc->vtblk_v1_dev);

	/*
	 * Drain all requests caught in-flight in the virtqueues,
	 * skipping biodone(). When dumping, only one request is
	 * outstanding at a time, and we just poll the virtqueue
	 * for the response.
	 */
	for (i = 0 ; i < sc->vtblk_v1_nqs; i++)
		vtblk_v1_drain_vq(&sc->vtblk_v1_queues[i], 1);

	if (virtio_v1_reinit(dev, sc->vtblk_v1_features) != 0) {
		panic("%s: cannot reinit VirtIO block device during dump",
		    device_get_nameunit(dev));
	}

	for (i = 0 ; i < sc->vtblk_v1_nqs; i++)
		virtio_v1_virtqueue_disable_intr(sc->vtblk_v1_queues[i].vtblk_v1_vq);
	virtio_v1_reinit_complete(dev);
}

static int
vtblk_v1_write_dump(struct vtblk_v1_softc *sc, void *virtual, off_t offset,
    size_t length)
{
	struct bio bio;
	struct buf bp;
	struct vtblk_v1_request *req;

	req = &sc->vtblk_v1_dump_request;
	req->vbr_ack = -1;
	req->vbr_hdr.type = VIRTIO_BLK_T_OUT;
	req->vbr_hdr.ioprio = 1;
	req->vbr_hdr.sector = offset / 512;

	req->vbr_bio = &bio;
	bzero(&bio, sizeof(struct bio));
	bzero(&bp, sizeof(struct buf));

	bio.bio_buf = &bp;
	bp.b_cmd = BUF_CMD_WRITE;
	bp.b_data = virtual;
	bp.b_bcount = length;

	return (vtblk_v1_poll_request(sc, req));
}

static int
vtblk_v1_flush_dump(struct vtblk_v1_softc *sc)
{
	struct bio bio;
	struct buf bp;
	struct vtblk_v1_request *req;

	req = &sc->vtblk_v1_dump_request;
	req->vbr_ack = -1;
	req->vbr_hdr.type = VIRTIO_BLK_T_FLUSH;
	req->vbr_hdr.ioprio = 1;
	req->vbr_hdr.sector = 0;

	req->vbr_bio = &bio;
	bzero(&bio, sizeof(struct bio));
	bzero(&bp, sizeof(struct buf));

	bio.bio_buf = &bp;
	bp.b_cmd = BUF_CMD_FLUSH;

	return (vtblk_v1_poll_request(sc, req));
}

static int
vtblk_v1_poll_request(struct vtblk_v1_softc *sc, struct vtblk_v1_request *req)
{
	struct virtqueue *vq;
	int error;

	vq = sc->vtblk_v1_queues[0].vtblk_v1_vq;

	if (!virtio_v1_virtqueue_empty(vq))
		return (EBUSY);

	error = vtblk_v1_execute_request(&sc->vtblk_v1_queues[0], req);
	if (error)
		return (error);

	virtio_v1_virtqueue_notify(vq);
	virtio_v1_virtqueue_poll(vq, NULL);

	error = vtblk_v1_request_error(req);
	if (error && bootverbose) {
		device_printf(sc->vtblk_v1_dev,
		    "%s: IO error: %d\n", __func__, error);
	}

	return (error);
}

static void
vtblk_v1_drain_vq(struct vtblk_v1_queue *q, int skip_done)
{
	struct virtqueue *vq = q->vtblk_v1_vq;
	struct vtblk_v1_request *req;
	int last;

	last = 0;

	while ((req = virtio_v1_virtqueue_drain(vq, &last)) != NULL) {
		if (!skip_done)
			vtblk_v1_finish_bio(req->vbr_bio, ENXIO);

		vtblk_v1_enqueue_request(q, req);
	}

	KASSERT(virtio_v1_virtqueue_empty(vq), ("virtqueue not empty"));
}

static void
vtblk_v1_drain(struct vtblk_v1_softc *sc)
{
	struct vtblk_v1_queue *q;
	struct bio_queue_head *bioq;
	struct bio *bio;
	int i;

	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		q = &sc->vtblk_v1_queues[i];
		bioq = &q->vtblk_v1_bioq;

		if (q->vtblk_v1_vq != NULL)
			vtblk_v1_drain_vq(q, 0);

		while (bioq_first(bioq) != NULL) {
			bio = bioq_takefirst(bioq);
			vtblk_v1_finish_bio(bio, ENXIO);
		}
	}

	vtblk_v1_free_requests(sc);
}

static int
vtblk_v1_alloc_requests(struct vtblk_v1_queue *vq)
{
	struct vtblk_v1_softc *sc = vq->vtblk_v1_sc;
	struct vtblk_v1_request *req;
	int i, nreqs;

	nreqs = virtio_v1_virtqueue_size(vq->vtblk_v1_vq);

	/*
	 * Preallocate sufficient requests to keep the virtqueue full. Each
	 * request consumes VTBLK_MIN_SEGMENTS or more descriptors so reduce
	 * the number allocated when indirect descriptors are not available.
	 */
	if ((sc->vtblk_v1_flags & VTBLK_FLAG_INDIRECT) == 0)
		nreqs /= VTBLK_MIN_SEGMENTS;

	for (i = 0; i < nreqs; i++) {
		req = contigmalloc(sizeof(struct vtblk_v1_request), M_DEVBUF,
		    M_WAITOK, 0, BUS_SPACE_MAXADDR, 16, 0);
		if (req == NULL)
			return (ENOMEM);

		KKASSERT(sglist_count(&req->vbr_hdr, sizeof(req->vbr_hdr))
		    == 1);
		KKASSERT(sglist_count(&req->vbr_ack, sizeof(req->vbr_ack))
		    == 1);

		sc->vtblk_v1_request_count++;
		vtblk_v1_enqueue_request(vq, req);
	}

	return (0);
}

static void
vtblk_v1_free_requests(struct vtblk_v1_softc *sc)
{
	struct vtblk_v1_request *req;
	int i;

	for (i = 0; i < sc->vtblk_v1_nqs; i++) {
		struct vtblk_v1_queue *q = &sc->vtblk_v1_queues[i];
		while ((req = vtblk_v1_dequeue_request(q)) != NULL) {
			sc->vtblk_v1_request_count--;
			contigfree(req, sizeof(struct vtblk_v1_request), M_DEVBUF);
		}
	}

	KASSERT(sc->vtblk_v1_request_count == 0, ("leaked requests"));
}

static struct vtblk_v1_request *
vtblk_v1_dequeue_request(struct vtblk_v1_queue *q)
{
	struct vtblk_v1_request *req;

	req = SLIST_FIRST(&q->vtblk_v1_req_free);
	if (req != NULL)
		SLIST_REMOVE_HEAD(&q->vtblk_v1_req_free, vbr_link);

	return (req);
}

static void
vtblk_v1_enqueue_request(struct vtblk_v1_queue *vq, struct vtblk_v1_request *req)
{

	bzero(req, sizeof(struct vtblk_v1_request));
	SLIST_INSERT_HEAD(&vq->vtblk_v1_req_free, req, vbr_link);
}

static int
vtblk_v1_request_error(struct vtblk_v1_request *req)
{
	int error;

	switch (req->vbr_ack) {
	case VIRTIO_BLK_S_OK:
		error = 0;
		break;
	case VIRTIO_BLK_S_UNSUPP:
		error = ENOTSUP;
		break;
	default:
		error = EIO;
		break;
	}

	return (error);
}

static void
vtblk_v1_finish_bio(struct bio *bio, int error)
{

	biodone(bio);
}

static void
vtblk_v1_setup_sysctl(struct vtblk_v1_softc *sc)
{
	device_t dev;
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;
	struct sysctl_oid_list *child;

	dev = sc->vtblk_v1_dev;
	ctx = device_get_sysctl_ctx(dev);
	tree = device_get_sysctl_tree(dev);
	child = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "writecache_mode",
	    CTLTYPE_INT | CTLFLAG_RW, sc, 0, vtblk_v1_write_cache_sysctl,
	    "I", "Write cache mode (writethrough (0) or writeback (1))");
}

static int
vtblk_v1_tunable_int(struct vtblk_v1_softc *sc, const char *knob, int def)
{
	char path[64];

	ksnprintf(path, sizeof(path),
	    "hw.vtblk.%d.%s", device_get_unit(sc->vtblk_v1_dev), knob);
	TUNABLE_INT_FETCH(path, &def);

	return (def);
}
