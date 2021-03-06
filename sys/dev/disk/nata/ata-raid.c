/*-
 * Copyright (c) 2000 - 2008 Søren Schmidt <sos@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
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
 * $FreeBSD: src/sys/dev/ata/ata-raid.c,v 1.120 2006/04/15 10:27:41 maxim Exp $
 */

#include "opt_ata.h"

#include <sys/param.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/buf2.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/devicestat.h>
#include <sys/disk.h>
#include <sys/endian.h>
#include <sys/libkern.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/nata.h>
#include <sys/systm.h>

#include <vm/pmap.h>

#include <machine/md_var.h>

#include <bus/pci/pcivar.h>

#include "ata-all.h"
#include "ata-disk.h"
#include "ata-raid.h"
#include "ata-pci.h"
#include "ata_if.h"

/* local implementation, to trigger a warning */
static inline void
biofinish(struct bio *bp, struct bio *x __unused, int error)
{
	struct buf *bbp = bp->bio_buf;

	bbp->b_flags |= B_ERROR;
	bbp->b_error = error;
	biodone(bp);
}

/* device structure */
static	d_strategy_t	ata_raid_strategy;
static	d_dump_t	ata_raid_dump;
static struct dev_ops ar_ops = {
	{ "ar", 0, D_DISK },
	.d_open =	nullopen,
	.d_close =	nullclose,
	.d_read =	physread,
	.d_write =	physwrite,
	.d_strategy =	ata_raid_strategy,
	.d_dump =	ata_raid_dump,
};

/* prototypes */
static void ata_raid_done(struct ata_request *request);
static void ata_raid_config_changed(struct ar_softc *rdp, int writeback);
static int ata_raid_status(struct ata_ioc_raid_status *status);
static int ata_raid_create(struct ata_ioc_raid_config *config);
static int ata_raid_delete(int array);
static int ata_raid_addspare(struct ata_ioc_raid_config *config);
static int ata_raid_rebuild(int array);
static int ata_raid_read_metadata(device_t subdisk);
static int ata_raid_write_metadata(struct ar_softc *rdp);
static int ata_raid_wipe_metadata(struct ar_softc *rdp);
static int ata_raid_adaptec_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_hptv2_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_hptv2_write_meta(struct ar_softc *rdp);
static int ata_raid_hptv3_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_intel_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_intel_write_meta(struct ar_softc *rdp);
static int ata_raid_ite_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_jmicron_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_jmicron_write_meta(struct ar_softc *rdp);
static int ata_raid_lsiv2_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_lsiv3_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_nvidia_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_promise_read_meta(device_t dev, struct ar_softc **raidp, int native);
static int ata_raid_promise_write_meta(struct ar_softc *rdp);
static int ata_raid_sii_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_sis_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_sis_write_meta(struct ar_softc *rdp);
static int ata_raid_via_read_meta(device_t dev, struct ar_softc **raidp);
static int ata_raid_via_write_meta(struct ar_softc *rdp);
static struct ata_request *ata_raid_init_request(struct ar_softc *rdp, struct bio *bio);
static int ata_raid_send_request(struct ata_request *request);
static int ata_raid_rw(device_t dev, u_int64_t lba, void *data, u_int bcount, int flags);
static char * ata_raid_format(struct ar_softc *rdp);
static char * ata_raid_type(struct ar_softc *rdp);
static char * ata_raid_flags(struct ar_softc *rdp);

/* debugging only */
static void ata_raid_print_meta(struct ar_softc *meta);
static void ata_raid_adaptec_print_meta(struct adaptec_raid_conf *meta);
static void ata_raid_hptv2_print_meta(struct hptv2_raid_conf *meta);
static void ata_raid_hptv3_print_meta(struct hptv3_raid_conf *meta);
static void ata_raid_intel_print_meta(struct intel_raid_conf *meta);
static void ata_raid_ite_print_meta(struct ite_raid_conf *meta);
static void ata_raid_jmicron_print_meta(struct jmicron_raid_conf *meta);
static void ata_raid_lsiv2_print_meta(struct lsiv2_raid_conf *meta);
static void ata_raid_lsiv3_print_meta(struct lsiv3_raid_conf *meta);
static void ata_raid_nvidia_print_meta(struct nvidia_raid_conf *meta);
static void ata_raid_promise_print_meta(struct promise_raid_conf *meta);
static void ata_raid_sii_print_meta(struct sii_raid_conf *meta);
static void ata_raid_sis_print_meta(struct sis_raid_conf *meta);
static void ata_raid_via_print_meta(struct via_raid_conf *meta);

/* internal vars */
static struct ar_softc *ata_raid_arrays[MAX_ARRAYS];
static MALLOC_DEFINE(M_AR, "ar_driver", "ATA PseudoRAID driver");
static devclass_t ata_raid_sub_devclass;
static int testing = 0;

static void
ata_raid_attach(struct ar_softc *rdp, int writeback)
{
    struct disk_info info;
    cdev_t cdev;
    char buffer[32];
    int disk;

    lockinit(&rdp->lock, "ataraidattach", 0, 0);
    ata_raid_config_changed(rdp, writeback);

    /* sanitize arrays total_size % (width * interleave) == 0 */
    if (rdp->type == AR_T_RAID0 || rdp->type == AR_T_RAID01 ||
	rdp->type == AR_T_RAID5) {
	rdp->total_sectors = rounddown(rdp->total_sectors,
	    rdp->interleave * rdp->width);
	ksprintf(buffer, " (stripe %d KB)",
		(rdp->interleave * DEV_BSIZE) / 1024);
    }
    else
	buffer[0] = '\0';

    devstat_add_entry(&rdp->devstat, "ar", rdp->lun,
	DEV_BSIZE, DEVSTAT_NO_ORDERED_TAGS,
	DEVSTAT_TYPE_STORARRAY | DEVSTAT_TYPE_IF_OTHER,
	DEVSTAT_PRIORITY_ARRAY);

    cdev = disk_create(rdp->lun, &rdp->disk, &ar_ops);
    cdev->si_drv1 = rdp;
    cdev->si_iosize_max = 128 * DEV_BSIZE;
    rdp->cdev = cdev;

    bzero(&info, sizeof(info));
    info.d_media_blksize = DEV_BSIZE;		/* mandatory */
    info.d_media_blocks = rdp->total_sectors;

    info.d_secpertrack = rdp->sectors;		/* optional */
    info.d_nheads = rdp->heads;
    info.d_ncylinders = rdp->total_sectors/(rdp->heads*rdp->sectors);
    info.d_secpercyl = rdp->sectors * rdp->heads;

    kprintf("ar%d: %juMB <%s %s%s> status: %s\n", rdp->lun,
	   rdp->total_sectors / ((1024L * 1024L) / DEV_BSIZE),
	   ata_raid_format(rdp), ata_raid_type(rdp),
	   buffer, ata_raid_flags(rdp));

    if (testing || bootverbose)
	kprintf("ar%d: %ju sectors [%dC/%dH/%dS] <%s> subdisks defined as:\n",
	       rdp->lun, rdp->total_sectors,
	       rdp->cylinders, rdp->heads, rdp->sectors, rdp->name);

    for (disk = 0; disk < rdp->total_disks; disk++) {
	kprintf("ar%d: disk%d ", rdp->lun, disk);
	if (rdp->disks[disk].dev) {
	    if (rdp->disks[disk].flags & AR_DF_PRESENT) {
		/* status of this disk in the array */
		if (rdp->disks[disk].flags & AR_DF_ONLINE)
		    kprintf("READY ");
		else if (rdp->disks[disk].flags & AR_DF_SPARE)
		    kprintf("SPARE ");
		else
		    kprintf("FREE  ");

		/* what type of disk is this in the array */
		switch (rdp->type) {
		case AR_T_RAID1:
		case AR_T_RAID01:
		    if (disk < rdp->width)
			kprintf("(master) ");
		    else
			kprintf("(mirror) ");
		}
		
		/* which physical disk is used */
		kprintf("using %s at ata%d-%s\n",
		       device_get_nameunit(rdp->disks[disk].dev),
		       device_get_unit(device_get_parent(rdp->disks[disk].dev)),
		       (((struct ata_device *)
			 device_get_softc(rdp->disks[disk].dev))->unit == 
			 ATA_MASTER) ? "master" : "slave");
	    }
	    else if (rdp->disks[disk].flags & AR_DF_ASSIGNED)
		kprintf("DOWN\n");
	    else
		kprintf("INVALID no RAID config on this subdisk\n");
	}
	else
	    kprintf("DOWN no device found for this subdisk\n");
    }

    disk_setdiskinfo(&rdp->disk, &info);
}

/*
 * ATA PseudoRAID ioctl function. Note that this does not need to be adjusted
 * to the dev_ops way, because it's just chained from the generic ata ioctl.
 */
static int
ata_raid_ioctl(u_long cmd, caddr_t data)
{
    struct ata_ioc_raid_status *status = (struct ata_ioc_raid_status *)data;
    struct ata_ioc_raid_config *config = (struct ata_ioc_raid_config *)data;
    int *lun = (int *)data;
    int error = EOPNOTSUPP;

    switch (cmd) {
    case IOCATARAIDSTATUS:
	error = ata_raid_status(status);
	break;
			
    case IOCATARAIDCREATE:
	error = ata_raid_create(config);
	break;
	 
    case IOCATARAIDDELETE:
	error = ata_raid_delete(*lun);
	break;
     
    case IOCATARAIDADDSPARE:
	error = ata_raid_addspare(config);
	break;
			    
    case IOCATARAIDREBUILD:
	error = ata_raid_rebuild(*lun);
	break;
    }
    return error;
}

static int
ata_raid_flush(struct ar_softc *rdp, struct bio *bp)
{
    struct ata_request *request;
    device_t dev;
    int disk;

    bp->bio_driver_info = NULL;

    for (disk = 0; disk < rdp->total_disks; disk++) {
	if ((dev = rdp->disks[disk].dev) != NULL)
	    bp->bio_driver_info = (void *)((intptr_t)bp->bio_driver_info + 1);
    }
    for (disk = 0; disk < rdp->total_disks; disk++) {
	if ((dev = rdp->disks[disk].dev) == NULL)
	    continue;
	if (!(request = ata_raid_init_request(rdp, bp)))
	    return ENOMEM;
	request->dev = dev;
	request->u.ata.command = ATA_FLUSHCACHE;
	request->u.ata.lba = 0;
	request->u.ata.count = 0;
	request->u.ata.feature = 0;
	request->timeout = 1;	/* ATA_DEFAULT_TIMEOUT */
	request->retries = 0;
	request->flags |= ATA_R_ORDERED | ATA_R_DIRECT;
	ata_queue_request(request);
    }
    return 0;
}

/*
 * XXX TGEN there are a lot of offset -> block number conversions going on
 * here, which is suboptimal.
 */
static int
ata_raid_strategy(struct dev_strategy_args *ap)
{
    struct ar_softc *rdp = ap->a_head.a_dev->si_drv1;
    struct bio *bp = ap->a_bio;
    struct buf *bbp = bp->bio_buf;
    struct ata_request *request;
    caddr_t data;
    u_int64_t blkno, lba, blk = 0;
    int count, chunk, drv, par = 0, change = 0;

    if (bbp->b_cmd == BUF_CMD_FLUSH) {
	int error;

	error = ata_raid_flush(rdp, bp);
	if (error != 0)
		biofinish(bp, NULL, error);
	return(0);
    }

    if (!(rdp->status & AR_S_READY) ||
	(bbp->b_cmd != BUF_CMD_READ && bbp->b_cmd != BUF_CMD_WRITE)) {
	biofinish(bp, NULL, EIO);
	return(0);
    }

    bbp->b_resid = bbp->b_bcount;
    for (count = howmany(bbp->b_bcount, DEV_BSIZE),
	 /* bio_offset is byte granularity, convert */
	 blkno = (u_int64_t)(bp->bio_offset >> DEV_BSHIFT),
	 data = bbp->b_data;
	 count > 0; 
	 count -= chunk, blkno += chunk, data += (chunk * DEV_BSIZE)) {

	switch (rdp->type) {
	case AR_T_RAID1:
	    drv = 0;
	    lba = blkno;
	    chunk = count;
	    break;
	
	case AR_T_JBOD:
	case AR_T_SPAN:
	    drv = 0;
	    lba = blkno;
	    while (lba >= rdp->disks[drv].sectors)
		lba -= rdp->disks[drv++].sectors;
	    chunk = min(rdp->disks[drv].sectors - lba, count);
	    break;
	
	case AR_T_RAID0:
	case AR_T_RAID01:
	    chunk = blkno % rdp->interleave;
	    drv = (blkno / rdp->interleave) % rdp->width;
	    lba = (((blkno/rdp->interleave)/rdp->width)*rdp->interleave)+chunk;
	    chunk = min(count, rdp->interleave - chunk);
	    break;

	case AR_T_RAID5:
	    drv = (blkno / rdp->interleave) % (rdp->width - 1);
	    par = rdp->width - 1 - 
		  (blkno / (rdp->interleave * (rdp->width - 1))) % rdp->width;
	    if (drv >= par)
		drv++;
	    lba = ((blkno/rdp->interleave)/(rdp->width-1))*(rdp->interleave) +
		  ((blkno%(rdp->interleave*(rdp->width-1)))%rdp->interleave);
	    chunk = min(count, rdp->interleave - (lba % rdp->interleave));
	    break;

	default:
	    kprintf("ar%d: unknown array type in ata_raid_strategy\n", rdp->lun);
	    biofinish(bp, NULL, EIO);
	    return(0);
	}
	 
	/* offset on all but "first on HPTv2" */
	if (!(drv == 0 && rdp->format == AR_F_HPTV2_RAID))
	    lba += rdp->offset_sectors;

	if (!(request = ata_raid_init_request(rdp, bp))) {
	    biofinish(bp, NULL, EIO);
	    return(0);
	}
	request->data = data;
	request->bytecount = chunk * DEV_BSIZE;
	request->u.ata.lba = lba;
	request->u.ata.count = request->bytecount / DEV_BSIZE;
	    
	devstat_start_transaction(&rdp->devstat);
	switch (rdp->type) {
	case AR_T_JBOD:
	case AR_T_SPAN:
	case AR_T_RAID0:
	    if (((rdp->disks[drv].flags & (AR_DF_PRESENT|AR_DF_ONLINE)) ==
		 (AR_DF_PRESENT|AR_DF_ONLINE) && !rdp->disks[drv].dev)) {
		rdp->disks[drv].flags &= ~AR_DF_ONLINE;
		ata_raid_config_changed(rdp, 1);
		ata_free_request(request);
		biofinish(bp, NULL, EIO);
		return(0);
	    }
	    request->this = drv;
	    request->dev = rdp->disks[request->this].dev;
	    ata_raid_send_request(request);
	    break;

	case AR_T_RAID1:
	case AR_T_RAID01:
	    if ((rdp->disks[drv].flags &
		 (AR_DF_PRESENT|AR_DF_ONLINE))==(AR_DF_PRESENT|AR_DF_ONLINE) &&
		!rdp->disks[drv].dev) {
		rdp->disks[drv].flags &= ~AR_DF_ONLINE;
		change = 1;
	    }
	    if ((rdp->disks[drv + rdp->width].flags &
		 (AR_DF_PRESENT|AR_DF_ONLINE))==(AR_DF_PRESENT|AR_DF_ONLINE) &&
		!rdp->disks[drv + rdp->width].dev) {
		rdp->disks[drv + rdp->width].flags &= ~AR_DF_ONLINE;
		change = 1;
	    }
	    if (change)
		ata_raid_config_changed(rdp, 1);
	    if (!(rdp->status & AR_S_READY)) {
		ata_free_request(request);
		biofinish(bp, NULL, EIO);
		return(0);
	    }

	    if (rdp->status & AR_S_REBUILDING)
		blk = ((lba / rdp->interleave) * rdp->width) * rdp->interleave +
		      (rdp->interleave * (drv % rdp->width)) +
		      lba % rdp->interleave;

	    if (bbp->b_cmd == BUF_CMD_READ) {
		int src_online =
		    (rdp->disks[drv].flags & AR_DF_ONLINE);
		int mir_online =
		    (rdp->disks[drv+rdp->width].flags & AR_DF_ONLINE);

		/* if mirror gone or close to last access on source */
		if (!mir_online || 
		    ((src_online) &&
		     ((u_int64_t)(bp->bio_offset >> DEV_BSHIFT)) >=
			(rdp->disks[drv].last_lba - AR_PROXIMITY) &&
		     ((u_int64_t)(bp->bio_offset >> DEV_BSHIFT)) <=
			(rdp->disks[drv].last_lba + AR_PROXIMITY))) {
		    rdp->toggle = 0;
		} 
		/* if source gone or close to last access on mirror */
		else if (!src_online ||
			 ((mir_online) &&
			  ((u_int64_t)(bp->bio_offset >> DEV_BSHIFT)) >=
			  (rdp->disks[drv+rdp->width].last_lba-AR_PROXIMITY) &&
			  ((u_int64_t)(bp->bio_offset >> DEV_BSHIFT)) <=
			  (rdp->disks[drv+rdp->width].last_lba+AR_PROXIMITY))) {
		    drv += rdp->width;
		    rdp->toggle = 1;
		}
		/* not close to any previous access, toggle */
		else {
		    if (rdp->toggle)
			rdp->toggle = 0;
		    else {
			drv += rdp->width;
			rdp->toggle = 1;
		    }
		}

		if ((rdp->status & AR_S_REBUILDING) &&
		    (blk <= rdp->rebuild_lba) &&
		    ((blk + chunk) > rdp->rebuild_lba)) {
		    struct ata_composite *composite;
		    struct ata_request *rebuild;
		    int this;

		    /* figure out what part to rebuild */
		    if (drv < rdp->width)
			this = drv + rdp->width;
		    else
			this = drv - rdp->width;

		    /* do we have a spare to rebuild on ? */
		    if (rdp->disks[this].flags & AR_DF_SPARE) {
			if ((composite = ata_alloc_composite())) {
			    if ((rebuild = ata_alloc_request())) {
				rdp->rebuild_lba = blk + chunk;
				bcopy(request, rebuild,
				      sizeof(struct ata_request));
				rebuild->this = this;
				rebuild->dev = rdp->disks[this].dev;
				rebuild->flags &= ~ATA_R_READ;
				rebuild->flags |= ATA_R_WRITE;
				lockinit(&composite->lock, "ardfspare", 0, 0);
				composite->residual = request->bytecount;
				composite->rd_needed |= (1 << drv);
				composite->wr_depend |= (1 << drv);
				composite->wr_needed |= (1 << this);
				composite->request[drv] = request;
				composite->request[this] = rebuild;
				request->composite = composite;
				rebuild->composite = composite;
				ata_raid_send_request(rebuild);
			    }
			    else {
				ata_free_composite(composite);
				kprintf("DOH! ata_alloc_request failed!\n");
			    }
			}
			else {
			    kprintf("DOH! ata_alloc_composite failed!\n");
			}
		    }
		    else if (rdp->disks[this].flags & AR_DF_ONLINE) {
			/*
			 * if we got here we are a chunk of a RAID01 that 
			 * does not need a rebuild, but we need to increment
			 * the rebuild_lba address to get the rebuild to
			 * move to the next chunk correctly
			 */
			rdp->rebuild_lba = blk + chunk;
		    }
		    else
			kprintf("DOH! we didn't find the rebuild part\n");
		}
	    }
	    if (bbp->b_cmd == BUF_CMD_WRITE) {
		if ((rdp->disks[drv+rdp->width].flags & AR_DF_ONLINE) ||
		    ((rdp->status & AR_S_REBUILDING) &&
		     (rdp->disks[drv+rdp->width].flags & AR_DF_SPARE) &&
		     ((blk < rdp->rebuild_lba) ||
		      ((blk <= rdp->rebuild_lba) &&
		       ((blk + chunk) > rdp->rebuild_lba))))) {
		    if ((rdp->disks[drv].flags & AR_DF_ONLINE) ||
			((rdp->status & AR_S_REBUILDING) &&
			 (rdp->disks[drv].flags & AR_DF_SPARE) &&
			 ((blk < rdp->rebuild_lba) ||
			  ((blk <= rdp->rebuild_lba) &&
			   ((blk + chunk) > rdp->rebuild_lba))))) {
			struct ata_request *mirror;
			struct ata_composite *composite;
			int this = drv + rdp->width;

			if ((composite = ata_alloc_composite())) {
			    if ((mirror = ata_alloc_request())) {
				if ((blk <= rdp->rebuild_lba) &&
				    ((blk + chunk) > rdp->rebuild_lba))
				    rdp->rebuild_lba = blk + chunk;
				bcopy(request, mirror,
				      sizeof(struct ata_request));
				mirror->this = this;
				mirror->dev = rdp->disks[this].dev;
				lockinit(&composite->lock, "ardfonline", 0, 0);
				composite->residual = request->bytecount;
				composite->wr_needed |= (1 << drv);
				composite->wr_needed |= (1 << this);
				composite->request[drv] = request;
				composite->request[this] = mirror;
				request->composite = composite;
				mirror->composite = composite;
				ata_raid_send_request(mirror);
				rdp->disks[this].last_lba =
				    (u_int64_t)(bp->bio_offset >> DEV_BSHIFT) +
				    chunk;
			    }
			    else {
				ata_free_composite(composite);
				kprintf("DOH! ata_alloc_request failed!\n");
			    }
			}
			else {
			    kprintf("DOH! ata_alloc_composite failed!\n");
			}
		    }
		    else
			drv += rdp->width;
		}
	    }
	    request->this = drv;
	    request->dev = rdp->disks[request->this].dev;
	    ata_raid_send_request(request);
	    rdp->disks[request->this].last_lba =
	       ((u_int64_t)(bp->bio_offset) >> DEV_BSHIFT) + chunk;
	    break;

	case AR_T_RAID5:
	    if (((rdp->disks[drv].flags & (AR_DF_PRESENT|AR_DF_ONLINE)) ==
		 (AR_DF_PRESENT|AR_DF_ONLINE) && !rdp->disks[drv].dev)) {
		rdp->disks[drv].flags &= ~AR_DF_ONLINE;
		change = 1;
	    }
	    if (((rdp->disks[par].flags & (AR_DF_PRESENT|AR_DF_ONLINE)) ==
		 (AR_DF_PRESENT|AR_DF_ONLINE) && !rdp->disks[par].dev)) {
		rdp->disks[par].flags &= ~AR_DF_ONLINE;
		change = 1;
	    }
	    if (change)
		ata_raid_config_changed(rdp, 1);
	    if (!(rdp->status & AR_S_READY)) {
		ata_free_request(request);
		biofinish(bp, NULL, EIO);
		return(0);
	    }
	    if (rdp->status & AR_S_DEGRADED) {
		/* do the XOR game if possible */
	    }
	    else {
		request->this = drv;
		request->dev = rdp->disks[request->this].dev;
		if (bbp->b_cmd == BUF_CMD_READ) {
		    ata_raid_send_request(request);
		}
		if (bbp->b_cmd == BUF_CMD_WRITE) {
		    ata_raid_send_request(request);
		    /*
		     * ensure that read-modify-write to each disk is atomic.
		     * couple of copies of request
		     * read old data data from drv
		     * write new data to drv
		     * read smth-smth data from pairs
		     * write old data xor smth-smth data xor data to pairs
		     */
		}
	    }
	    break;

	default:
	    kprintf("ar%d: unknown array type in ata_raid_strategy\n", rdp->lun);
	}
    }

    return(0);
}

static void
ata_raid_done(struct ata_request *request)
{
    struct ar_softc *rdp = request->driver;
    struct ata_composite *composite = NULL;
    struct bio *bp = request->bio;
    struct buf *bbp = bp->bio_buf;
    int i, mirror, finished = 0;

    if (bbp->b_cmd == BUF_CMD_FLUSH) {
	if (bbp->b_error == 0)
		bbp->b_error = request->result;
	ata_free_request(request);
	bp->bio_driver_info = (void *)((intptr_t)bp->bio_driver_info - 1);
	if ((intptr_t)bp->bio_driver_info == 0) {
		if (bbp->b_error)
			bbp->b_flags |= B_ERROR;
		biodone(bp);
	}
	return;
    }

    switch (rdp->type) {
    case AR_T_JBOD:
    case AR_T_SPAN:
    case AR_T_RAID0:
	if (request->result) {
	    rdp->disks[request->this].flags &= ~AR_DF_ONLINE;
	    ata_raid_config_changed(rdp, 1);
	    bbp->b_error = request->result;
	    finished = 1;
	}
	else {
	    bbp->b_resid -= request->donecount;
	    if (!bbp->b_resid)
		finished = 1;
	}
	break;

    case AR_T_RAID1:
    case AR_T_RAID01:
	if (request->this < rdp->width)
	    mirror = request->this + rdp->width;
	else
	    mirror = request->this - rdp->width;
	if (request->result) {
	    rdp->disks[request->this].flags &= ~AR_DF_ONLINE;
	    ata_raid_config_changed(rdp, 1);
	}
	if (rdp->status & AR_S_READY) {
	    u_int64_t blk = 0;

	    if (rdp->status & AR_S_REBUILDING) 
		blk = ((request->u.ata.lba / rdp->interleave) * rdp->width) *
		      rdp->interleave + (rdp->interleave * 
		      (request->this % rdp->width)) +
		      request->u.ata.lba % rdp->interleave;

	    if (bbp->b_cmd == BUF_CMD_READ) {

		/* is this a rebuild composite */
		if ((composite = request->composite)) {
		    lockmgr(&composite->lock, LK_EXCLUSIVE);
		
		    /* handle the read part of a rebuild composite */
		    if (request->flags & ATA_R_READ) {

			/* if read failed array is now broken */
			if (request->result) {
			    rdp->disks[request->this].flags &= ~AR_DF_ONLINE;
			    ata_raid_config_changed(rdp, 1);
			    bbp->b_error = request->result;
			    rdp->rebuild_lba = blk;
			    finished = 1;
			}

			/* good data, update how far we've gotten */
			else {
			    bbp->b_resid -= request->donecount;
			    composite->residual -= request->donecount;
			    if (!composite->residual) {
				if (composite->wr_done & (1 << mirror))
				    finished = 1;
			    }
			}
		    }

		    /* handle the write part of a rebuild composite */
		    else if (request->flags & ATA_R_WRITE) {
			if (composite->rd_done & (1 << mirror)) {
			    if (request->result) {
				kprintf("DOH! rebuild failed\n"); /* XXX SOS */
				rdp->rebuild_lba = blk;
			    }
			    if (!composite->residual)
				finished = 1;
			}
		    }
		    lockmgr(&composite->lock, LK_RELEASE);
		}

		/* if read failed retry on the mirror */
		else if (request->result) {
		    request->dev = rdp->disks[mirror].dev;
		    request->flags &= ~ATA_R_TIMEOUT;
		    ata_raid_send_request(request);
		    return;
		}

		/* we have good data */
		else {
		    bbp->b_resid -= request->donecount;
		    if (!bbp->b_resid)
			finished = 1;
		}
	    }
	    else if (bbp->b_cmd == BUF_CMD_WRITE) {
		/* do we have a mirror or rebuild to deal with ? */
		if ((composite = request->composite)) {
		    lockmgr(&composite->lock, LK_EXCLUSIVE);
		    if (composite->wr_done & (1 << mirror)) {
			if (request->result) {
			    if (composite->request[mirror]->result) {
				kprintf("DOH! all disks failed and got here\n");
				bbp->b_error = EIO;
			    }
			    if (rdp->status & AR_S_REBUILDING) {
				rdp->rebuild_lba = blk;
				kprintf("DOH! rebuild failed\n"); /* XXX SOS */
			    }
			    bbp->b_resid -=
				composite->request[mirror]->donecount;
			    composite->residual -=
				composite->request[mirror]->donecount;
			}
			else {
			    bbp->b_resid -= request->donecount;
			    composite->residual -= request->donecount;
			}
			if (!composite->residual)
			    finished = 1;
		    }
		    lockmgr(&composite->lock, LK_RELEASE);
		}
		/* no mirror we are done */
		else {
		    bbp->b_resid -= request->donecount;
		    if (!bbp->b_resid)
			finished = 1;
		}
	    }
	}
	else {
	    /* XXX TGEN bbp->b_flags |= B_ERROR; */
	    bbp->b_error = request->result;
	    biodone(bp);
	}
	break;

    case AR_T_RAID5:
	if (request->result) {
	    rdp->disks[request->this].flags &= ~AR_DF_ONLINE;
	    ata_raid_config_changed(rdp, 1);
	    if (rdp->status & AR_S_READY) {
		if (bbp->b_cmd == BUF_CMD_READ) {
		    /* do the XOR game to recover data */
		}
		if (bbp->b_cmd == BUF_CMD_WRITE) {
		    /* if the parity failed we're OK sortof */
		    /* otherwise wee need to do the XOR long dance */
		}
		finished = 1;
	    }
	    else {
		/* XXX TGEN bbp->b_flags |= B_ERROR; */
		bbp->b_error = request->result;
		biodone(bp);
	    }
	}
	else {
	    /* did we have an XOR game going ?? */
	    bbp->b_resid -= request->donecount;
	    if (!bbp->b_resid)
		finished = 1;
	}
	break;

    default:
	kprintf("ar%d: unknown array type in ata_raid_done\n", rdp->lun);
    }

    if (finished) {
	if ((rdp->status & AR_S_REBUILDING) && 
	    rdp->rebuild_lba >= rdp->total_sectors) {
	    int disk;

	    for (disk = 0; disk < rdp->total_disks; disk++) {
		if ((rdp->disks[disk].flags &
		     (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_SPARE)) ==
		    (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_SPARE)) {
		    rdp->disks[disk].flags &= ~AR_DF_SPARE;
		    rdp->disks[disk].flags |= AR_DF_ONLINE;
		}
	    }
	    rdp->status &= ~AR_S_REBUILDING;
	    ata_raid_config_changed(rdp, 1);
	}
	devstat_end_transaction_buf(&rdp->devstat, bbp);
	if (!bbp->b_resid)
	    biodone(bp);
    }
		 
    if (composite) {
	if (finished) {
	    /* we are done with this composite, free all resources */
	    for (i = 0; i < 32; i++) {
		if (composite->rd_needed & (1 << i) ||
		    composite->wr_needed & (1 << i)) {
		    ata_free_request(composite->request[i]);
		}
	    }
	    lockuninit(&composite->lock);
	    ata_free_composite(composite);
	}
    }
    else
	ata_free_request(request);
}

static int
ata_raid_dump(struct dev_dump_args *ap)
{
	struct ar_softc *rdp = ap->a_head.a_dev->si_drv1;
	struct buf dbuf;
	int error = 0;
	int disk;

	if (ap->a_length == 0) {
		/* flush subdisk buffers to media */
		for (disk = 0, error = 0; disk < rdp->total_disks; disk++) {
			if (rdp->disks[disk].dev) {
				error |= ata_controlcmd(rdp->disks[disk].dev,
						ATA_FLUSHCACHE, 0, 0, 0);
			}
		}
		return (error ? EIO : 0);
	}

	bzero(&dbuf, sizeof(struct buf));
	initbufbio(&dbuf);
	BUF_LOCK(&dbuf, LK_EXCLUSIVE);
	/* bio_offset is byte granularity, convert block granularity a_blkno */
	dbuf.b_bio1.bio_offset = ap->a_offset;
	dbuf.b_bio1.bio_caller_info1.ptr = (void *)rdp;
	dbuf.b_bio1.bio_flags |= BIO_SYNC;
	dbuf.b_bio1.bio_done = biodone_sync;
	dbuf.b_bcount = ap->a_length;
	dbuf.b_data = ap->a_virtual;
	dbuf.b_cmd = BUF_CMD_WRITE;
	dev_dstrategy(rdp->cdev, &dbuf.b_bio1);
	/* wait for completion, unlock the buffer, check status */
	if (biowait(&dbuf.b_bio1, "dumpw")) {
	    BUF_UNLOCK(&dbuf);
	    return(dbuf.b_error ? dbuf.b_error : EIO);
	}
	BUF_UNLOCK(&dbuf);
	uninitbufbio(&dbuf);

	return 0;
}

static void
ata_raid_config_changed(struct ar_softc *rdp, int writeback)
{
    int disk, count, status;

    lockmgr(&rdp->lock, LK_EXCLUSIVE);

    /* set default all working mode */
    status = rdp->status;
    rdp->status &= ~AR_S_DEGRADED;
    rdp->status |= AR_S_READY;

    /* make sure all lost drives are accounted for */
    for (disk = 0; disk < rdp->total_disks; disk++) {
	if (!(rdp->disks[disk].flags & AR_DF_PRESENT))
	    rdp->disks[disk].flags &= ~AR_DF_ONLINE;
    }

    /* depending on RAID type figure out our health status */
    switch (rdp->type) {
    case AR_T_JBOD:
    case AR_T_SPAN:
    case AR_T_RAID0:
	for (disk = 0; disk < rdp->total_disks; disk++) 
	    if (!(rdp->disks[disk].flags & AR_DF_ONLINE))
		rdp->status &= ~AR_S_READY; 
	break;

    case AR_T_RAID1:
    case AR_T_RAID01:
	for (disk = 0; disk < rdp->width; disk++) {
	    if (!(rdp->disks[disk].flags & AR_DF_ONLINE) &&
		!(rdp->disks[disk + rdp->width].flags & AR_DF_ONLINE)) {
		rdp->status &= ~AR_S_READY;
	    }
	    else if (((rdp->disks[disk].flags & AR_DF_ONLINE) &&
		      !(rdp->disks[disk + rdp->width].flags & AR_DF_ONLINE)) ||
		     (!(rdp->disks[disk].flags & AR_DF_ONLINE) &&
		      (rdp->disks [disk + rdp->width].flags & AR_DF_ONLINE))) {
		rdp->status |= AR_S_DEGRADED;
	    }
	}
	break;

    case AR_T_RAID5:
	for (count = 0, disk = 0; disk < rdp->total_disks; disk++) {
	    if (!(rdp->disks[disk].flags & AR_DF_ONLINE))
		count++;
	}
	if (count) {
	    if (count > 1)
		rdp->status &= ~AR_S_READY;
	    else
		rdp->status |= AR_S_DEGRADED;
	}
	break;
    default:
	rdp->status &= ~AR_S_READY;
    }

    /*
     * Note that when the array breaks so comes up broken we
     * force a write of the array config to the remaining
     * drives so that the generation will be incremented past
     * those of the missing or failed drives (in all cases).
     */
    if (rdp->status != status) {

	/* raid status has changed, update metadata */
	writeback = 1;

	/* announce we have trouble ahead */
	if (!(rdp->status & AR_S_READY)) {
	    kprintf("ar%d: FAILURE - %s array broken\n",
		   rdp->lun, ata_raid_type(rdp));
	}
	else if (rdp->status & AR_S_DEGRADED) {
	    if (rdp->type & (AR_T_RAID1 | AR_T_RAID01))
		kprintf("ar%d: WARNING - mirror", rdp->lun);
	    else
		kprintf("ar%d: WARNING - parity", rdp->lun);
	    kprintf(" protection lost. %s array in DEGRADED mode\n",
		   ata_raid_type(rdp));
	}
    }
    lockmgr(&rdp->lock, LK_RELEASE);
    if (writeback)
	ata_raid_write_metadata(rdp);

}

static int
ata_raid_status(struct ata_ioc_raid_status *status)
{
    struct ar_softc *rdp;
    int i;
	
    if (!(rdp = ata_raid_arrays[status->lun]))
	return ENXIO;
	
    status->type = rdp->type;
    status->total_disks = rdp->total_disks;
    for (i = 0; i < rdp->total_disks; i++ ) {
	status->disks[i].state = 0;
	if ((rdp->disks[i].flags & AR_DF_PRESENT) && rdp->disks[i].dev) {
	    status->disks[i].lun = device_get_unit(rdp->disks[i].dev);
	    if (rdp->disks[i].flags & AR_DF_PRESENT)
		status->disks[i].state |= AR_DISK_PRESENT;
	    if (rdp->disks[i].flags & AR_DF_ONLINE)
		status->disks[i].state |= AR_DISK_ONLINE;
	    if (rdp->disks[i].flags & AR_DF_SPARE)
		status->disks[i].state |= AR_DISK_SPARE;
	} else
	    status->disks[i].lun = -1;
    }
    status->interleave = rdp->interleave;
    status->status = rdp->status;
    status->progress = 100 * rdp->rebuild_lba / rdp->total_sectors;
    return 0;
}

static int
ata_raid_create(struct ata_ioc_raid_config *config)
{
    struct ar_softc *rdp;
    device_t subdisk;
    int array, disk;
    int ctlr = 0, total_disks = 0;
    u_int disk_size = 0;

    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!ata_raid_arrays[array])
	    break;
    }
    if (array >= MAX_ARRAYS)
	return ENOSPC;

    rdp = (struct ar_softc*)kmalloc(sizeof(struct ar_softc), M_AR,
	M_WAITOK | M_ZERO);

    for (disk = 0; disk < config->total_disks; disk++) {
	if ((subdisk = devclass_get_device(ata_raid_sub_devclass,
					   config->disks[disk]))) {
	    struct ata_raid_subdisk *ars = device_get_softc(subdisk);

	    /* is device already assigned to another array ? */
	    if (ars->raid[rdp->volume]) {
		config->disks[disk] = -1;
		kfree(rdp, M_AR);
		return EBUSY;
	    }
	    rdp->disks[disk].dev = device_get_parent(subdisk);

	    switch (pci_get_vendor(GRANDPARENT(rdp->disks[disk].dev))) {
	    case ATA_HIGHPOINT_ID:
		/* 
		 * we need some way to decide if it should be v2 or v3
		 * for now just use v2 since the v3 BIOS knows how to 
		 * handle that as well.
		 */
		ctlr = AR_F_HPTV2_RAID;
		rdp->disks[disk].sectors = HPTV3_LBA(rdp->disks[disk].dev);
		break;

	    case ATA_INTEL_ID:
		ctlr = AR_F_INTEL_RAID;
		rdp->disks[disk].sectors = INTEL_LBA(rdp->disks[disk].dev);
		break;

	    case ATA_ITE_ID:
		ctlr = AR_F_ITE_RAID;
		rdp->disks[disk].sectors = ITE_LBA(rdp->disks[disk].dev);
		break;

	    case ATA_JMICRON_ID:
		ctlr = AR_F_JMICRON_RAID;
		rdp->disks[disk].sectors = JMICRON_LBA(rdp->disks[disk].dev);
		break;

	    case 0:     /* XXX SOS cover up for bug in our PCI code */
	    case ATA_PROMISE_ID:        
		ctlr = AR_F_PROMISE_RAID;
		rdp->disks[disk].sectors = PROMISE_LBA(rdp->disks[disk].dev);
		break;

	    case ATA_SIS_ID:        
		ctlr = AR_F_SIS_RAID;
		rdp->disks[disk].sectors = SIS_LBA(rdp->disks[disk].dev);
		break;

	    case ATA_ATI_ID:        
	    case ATA_VIA_ID:        
		ctlr = AR_F_VIA_RAID;
		rdp->disks[disk].sectors = VIA_LBA(rdp->disks[disk].dev);
		break;

	    default:
		/* XXX SOS
		 * right, so here we are, we have an ATA chip and we want
		 * to create a RAID and store the metadata.
		 * we need to find a way to tell what kind of metadata this
		 * hardware's BIOS might be using (good ideas are welcomed)
		 * for now we just use our own native FreeBSD format.
		 * the only way to get support for the BIOS format is to
		 * setup the RAID from there, in that case we pickup the
		 * metadata format from the disks (if we support it).
		 */
		kprintf("WARNING!! - not able to determine metadata format\n"
		       "WARNING!! - Using FreeBSD PseudoRAID metadata\n"
		       "If that is not what you want, use the BIOS to "
		       "create the array\n");
		ctlr = AR_F_FREEBSD_RAID;
		rdp->disks[disk].sectors = PROMISE_LBA(rdp->disks[disk].dev);
		break;
	    }

	    /* we need all disks to be of the same format */
	    if ((rdp->format & AR_F_FORMAT_MASK) &&
		(rdp->format & AR_F_FORMAT_MASK) != (ctlr & AR_F_FORMAT_MASK)) {
		kfree(rdp, M_AR);
		return EXDEV;
	    }
	    else
		rdp->format = ctlr;
	    
	    /* use the smallest disk of the lots size */
	    /* gigabyte boundry ??? XXX SOS */
	    if (disk_size)
		disk_size = min(rdp->disks[disk].sectors, disk_size);
	    else
		disk_size = rdp->disks[disk].sectors;
	    rdp->disks[disk].flags = 
		(AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_ONLINE);

	    total_disks++;
	}
	else {
	    config->disks[disk] = -1;
	    kfree(rdp, M_AR);
	    return ENXIO;
	}
    }

    if (total_disks != config->total_disks) {
	kfree(rdp, M_AR);
	return ENODEV;
    }

    switch (config->type) {
    case AR_T_JBOD:
    case AR_T_SPAN:
    case AR_T_RAID0:
	break;

    case AR_T_RAID1:
	if (total_disks != 2) {
	    kfree(rdp, M_AR);
	    return EPERM;
	}
	break;

    case AR_T_RAID01:
	if (total_disks % 2 != 0) {
	    kfree(rdp, M_AR);
	    return EPERM;
	}
	break;

    case AR_T_RAID5:
	if (total_disks < 3) {
	    kfree(rdp, M_AR);
	    return EPERM;
	}
	break;

    default:
	kfree(rdp, M_AR);
	return EOPNOTSUPP;
    }
    rdp->type = config->type;
    rdp->lun = array;
    if (rdp->type == AR_T_RAID0 || rdp->type == AR_T_RAID01 ||
	rdp->type == AR_T_RAID5) {
	int bit = 0;

	while (config->interleave >>= 1)
	    bit++;
	rdp->interleave = 1 << bit;
    }
    rdp->offset_sectors = 0;

    /* values that depend on metadata format */
    switch (rdp->format) {
    case AR_F_ADAPTEC_RAID:
	rdp->interleave = min(max(32, rdp->interleave), 128); /*+*/
	break;

    case AR_F_HPTV2_RAID:
	rdp->interleave = min(max(8, rdp->interleave), 128); /*+*/
	rdp->offset_sectors = HPTV2_LBA(x) + 1;
	break;

    case AR_F_HPTV3_RAID:
	rdp->interleave = min(max(32, rdp->interleave), 4096); /*+*/
	break;

    case AR_F_INTEL_RAID:
	rdp->interleave = min(max(8, rdp->interleave), 256); /*+*/
	break;

    case AR_F_ITE_RAID:
	rdp->interleave = min(max(2, rdp->interleave), 128); /*+*/
	break;

    case AR_F_JMICRON_RAID:
	rdp->interleave = min(max(8, rdp->interleave), 256); /*+*/
	break;

    case AR_F_LSIV2_RAID:
	rdp->interleave = min(max(2, rdp->interleave), 4096);
	break;

    case AR_F_LSIV3_RAID:
	rdp->interleave = min(max(2, rdp->interleave), 256);
	break;

    case AR_F_PROMISE_RAID:
	rdp->interleave = min(max(2, rdp->interleave), 2048); /*+*/
	break;

    case AR_F_SII_RAID:
	rdp->interleave = min(max(8, rdp->interleave), 256); /*+*/
	break;

    case AR_F_SIS_RAID:
	rdp->interleave = min(max(32, rdp->interleave), 512); /*+*/
	break;

    case AR_F_VIA_RAID:
	rdp->interleave = min(max(8, rdp->interleave), 128); /*+*/
	break;
    }

    rdp->total_disks = total_disks;
    rdp->width = total_disks / (rdp->type & (AR_RAID1 | AR_T_RAID01) ? 2 : 1);
    rdp->total_sectors =
	(uint64_t)disk_size * (rdp->width - (rdp->type == AR_RAID5));
    rdp->heads = 255;
    rdp->sectors = 63;
    rdp->cylinders = rdp->total_sectors / (255 * 63);
    rdp->rebuild_lba = 0;
    rdp->status |= AR_S_READY;

    /* we are committed to this array, grap the subdisks */
    for (disk = 0; disk < config->total_disks; disk++) {
	if ((subdisk = devclass_get_device(ata_raid_sub_devclass,
					   config->disks[disk]))) {
	    struct ata_raid_subdisk *ars = device_get_softc(subdisk);

	    ars->raid[rdp->volume] = rdp;
	    ars->disk_number[rdp->volume] = disk;
	}
    }
    ata_raid_attach(rdp, 1);
    ata_raid_arrays[array] = rdp;
    config->lun = array;
    return 0;
}

static int
ata_raid_delete(int array)
{
    struct ar_softc *rdp;    
    device_t subdisk;
    int disk;

    if (!(rdp = ata_raid_arrays[array]))
	return ENXIO;
 
    rdp->status &= ~AR_S_READY;
    disk_destroy(&rdp->disk);
    devstat_remove_entry(&rdp->devstat);

    for (disk = 0; disk < rdp->total_disks; disk++) {
	if ((rdp->disks[disk].flags & AR_DF_PRESENT) && rdp->disks[disk].dev) {
	    if ((subdisk = devclass_get_device(ata_raid_sub_devclass,
		     device_get_unit(rdp->disks[disk].dev)))) {
		struct ata_raid_subdisk *ars = device_get_softc(subdisk);

		if (ars->raid[rdp->volume] != rdp)           /* XXX SOS */
		    device_printf(subdisk, "DOH! this disk doesn't belong\n");
		if (ars->disk_number[rdp->volume] != disk)   /* XXX SOS */
		    device_printf(subdisk, "DOH! this disk number is wrong\n");
		ars->raid[rdp->volume] = NULL;
		ars->disk_number[rdp->volume] = -1;
	    }
	    rdp->disks[disk].flags = 0;
	}
    }
    ata_raid_wipe_metadata(rdp);
    ata_raid_arrays[array] = NULL;
    kfree(rdp, M_AR);
    return 0;
}

static int
ata_raid_addspare(struct ata_ioc_raid_config *config)
{
    struct ar_softc *rdp;    
    device_t subdisk;
    int disk;

    if (!(rdp = ata_raid_arrays[config->lun]))
	return ENXIO;
    if (!(rdp->status & AR_S_DEGRADED) || !(rdp->status & AR_S_READY))
	return ENXIO;
    if (rdp->status & AR_S_REBUILDING)
	return EBUSY; 
    switch (rdp->type) {
    case AR_T_RAID1:
    case AR_T_RAID01:
    case AR_T_RAID5:
	for (disk = 0; disk < rdp->total_disks; disk++ ) {

	    if (((rdp->disks[disk].flags & (AR_DF_PRESENT | AR_DF_ONLINE)) ==
		 (AR_DF_PRESENT | AR_DF_ONLINE)) && rdp->disks[disk].dev)
		continue;

	    if ((subdisk = devclass_get_device(ata_raid_sub_devclass,
					       config->disks[0] ))) {
		struct ata_raid_subdisk *ars = device_get_softc(subdisk);

		if (ars->raid[rdp->volume]) 
		    return EBUSY;
    
		/* XXX SOS validate size etc etc */
		ars->raid[rdp->volume] = rdp;
		ars->disk_number[rdp->volume] = disk;
		rdp->disks[disk].dev = device_get_parent(subdisk);
		rdp->disks[disk].flags =
		    (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_SPARE);

		device_printf(rdp->disks[disk].dev,
			      "inserted into ar%d disk%d as spare\n",
			      rdp->lun, disk);
		ata_raid_config_changed(rdp, 1);
		return 0;
	    }
	}
	return ENXIO;

    default:
	return EPERM;
    }
}
 
static int
ata_raid_rebuild(int array)
{
    struct ar_softc *rdp;    
    int disk, count;

    if (!(rdp = ata_raid_arrays[array]))
	return ENXIO;
    /* XXX SOS we should lock the rdp softc here */
    if (!(rdp->status & AR_S_DEGRADED) || !(rdp->status & AR_S_READY))
	return ENXIO;
    if (rdp->status & AR_S_REBUILDING)
	return EBUSY; 

    switch (rdp->type) {
    case AR_T_RAID1:
    case AR_T_RAID01:
    case AR_T_RAID5:
	for (count = 0, disk = 0; disk < rdp->total_disks; disk++ ) {
	    if (((rdp->disks[disk].flags &
		  (AR_DF_PRESENT|AR_DF_ASSIGNED|AR_DF_ONLINE|AR_DF_SPARE)) ==
		 (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_SPARE)) &&
		rdp->disks[disk].dev) {
		count++;
	    }
	}

	if (count) {
	    rdp->rebuild_lba = 0;
	    rdp->status |= AR_S_REBUILDING;
	    return 0;
	}
	return EIO;

    default:
	return EPERM;
    }
}

static int
ata_raid_read_metadata(device_t subdisk)
{
    devclass_t pci_devclass = devclass_find("pci");
    devclass_t atapci_devclass = devclass_find("atapci");
    devclass_t devclass=device_get_devclass(GRANDPARENT(GRANDPARENT(subdisk)));

    /* prioritize vendor native metadata layout if possible */
    if (devclass == pci_devclass || devclass == atapci_devclass) {
	switch (pci_get_vendor(GRANDPARENT(device_get_parent(subdisk)))) {
	case ATA_HIGHPOINT_ID: 
	    if (ata_raid_hptv3_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    if (ata_raid_hptv2_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    break;

	case ATA_INTEL_ID:
	    if (ata_raid_intel_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    break;

	case ATA_ITE_ID:
	    if (ata_raid_ite_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    break;

	case ATA_JMICRON_ID:
	    if (ata_raid_jmicron_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    break;

	case ATA_NVIDIA_ID:
	    if (ata_raid_nvidia_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    break;

	case 0:         /* XXX SOS cover up for bug in our PCI code */
	case ATA_PROMISE_ID: 
	    if (ata_raid_promise_read_meta(subdisk, ata_raid_arrays, 0))
		return 0;
	    break;

	case ATA_ATI_ID:
	case ATA_SILICON_IMAGE_ID:
	    if (ata_raid_sii_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    break;

	case ATA_SIS_ID:
	    if (ata_raid_sis_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    break;

	case ATA_VIA_ID:
	    if (ata_raid_via_read_meta(subdisk, ata_raid_arrays))
		return 0;
	    break;
	}
    }
    
    /* handle controllers that have multiple layout possibilities */
    /* NOTE: the order of these are not insignificant */

    /* Adaptec HostRAID */
    if (ata_raid_adaptec_read_meta(subdisk, ata_raid_arrays))
	return 0;

    /* LSILogic v3 and v2 */
    if (ata_raid_lsiv3_read_meta(subdisk, ata_raid_arrays))
	return 0;
    if (ata_raid_lsiv2_read_meta(subdisk, ata_raid_arrays))
	return 0;

    /* if none of the above matched, try FreeBSD native format */
    return ata_raid_promise_read_meta(subdisk, ata_raid_arrays, 1);
}

static int
ata_raid_write_metadata(struct ar_softc *rdp)
{
    switch (rdp->format) {
    case AR_F_FREEBSD_RAID:
    case AR_F_PROMISE_RAID: 
	return ata_raid_promise_write_meta(rdp);

    case AR_F_HPTV3_RAID:
    case AR_F_HPTV2_RAID:
	/*
	 * always write HPT v2 metadata, the v3 BIOS knows it as well.
	 * this is handy since we cannot know what version BIOS is on there
	 */
	return ata_raid_hptv2_write_meta(rdp);

    case AR_F_INTEL_RAID:
	return ata_raid_intel_write_meta(rdp);

    case AR_F_JMICRON_RAID:
	return ata_raid_jmicron_write_meta(rdp);

    case AR_F_SIS_RAID:
	return ata_raid_sis_write_meta(rdp);

    case AR_F_VIA_RAID:
	return ata_raid_via_write_meta(rdp);
#if 0
    case AR_F_HPTV3_RAID:
	return ata_raid_hptv3_write_meta(rdp);

    case AR_F_ADAPTEC_RAID:
	return ata_raid_adaptec_write_meta(rdp);

    case AR_F_ITE_RAID:
	return ata_raid_ite_write_meta(rdp);

    case AR_F_LSIV2_RAID:
	return ata_raid_lsiv2_write_meta(rdp);

    case AR_F_LSIV3_RAID:
	return ata_raid_lsiv3_write_meta(rdp);

    case AR_F_NVIDIA_RAID:
	return ata_raid_nvidia_write_meta(rdp);

    case AR_F_SII_RAID:
	return ata_raid_sii_write_meta(rdp);

#endif
    default:
	kprintf("ar%d: writing of %s metadata is NOT supported yet\n",
	       rdp->lun, ata_raid_format(rdp));
    }
    return -1;
}

static int
ata_raid_wipe_metadata(struct ar_softc *rdp)
{
    int disk, error = 0;
    u_int64_t lba;
    u_int32_t size;
    u_int8_t *meta;

    for (disk = 0; disk < rdp->total_disks; disk++) {
	if (rdp->disks[disk].dev) {
	    switch (rdp->format) {
	    case AR_F_ADAPTEC_RAID:
		lba = ADP_LBA(rdp->disks[disk].dev);
		size = sizeof(struct adaptec_raid_conf);
		break;

	    case AR_F_HPTV2_RAID:
		lba = HPTV2_LBA(rdp->disks[disk].dev);
		size = sizeof(struct hptv2_raid_conf);
		break;
		
	    case AR_F_HPTV3_RAID:
		lba = HPTV3_LBA(rdp->disks[disk].dev);
		size = sizeof(struct hptv3_raid_conf);
		break;

	    case AR_F_INTEL_RAID:
		lba = INTEL_LBA(rdp->disks[disk].dev);
		size = 3 * 512;         /* XXX SOS */
		break;

	    case AR_F_ITE_RAID:
		lba = ITE_LBA(rdp->disks[disk].dev);
		size = sizeof(struct ite_raid_conf);
		break;

	    case AR_F_JMICRON_RAID:
		lba = JMICRON_LBA(rdp->disks[disk].dev);
		size = sizeof(struct jmicron_raid_conf);
		break;

	    case AR_F_LSIV2_RAID:
		lba = LSIV2_LBA(rdp->disks[disk].dev);
		size = sizeof(struct lsiv2_raid_conf);
		break;

	    case AR_F_LSIV3_RAID:
		lba = LSIV3_LBA(rdp->disks[disk].dev);
		size = sizeof(struct lsiv3_raid_conf);
		break;

	    case AR_F_NVIDIA_RAID:
		lba = NVIDIA_LBA(rdp->disks[disk].dev);
		size = sizeof(struct nvidia_raid_conf);
		break;

	    case AR_F_FREEBSD_RAID:
	    case AR_F_PROMISE_RAID: 
		lba = PROMISE_LBA(rdp->disks[disk].dev);
		size = sizeof(struct promise_raid_conf);
		break;

	    case AR_F_SII_RAID:
		lba = SII_LBA(rdp->disks[disk].dev);
		size = sizeof(struct sii_raid_conf);
		break;

	    case AR_F_SIS_RAID:
		lba = SIS_LBA(rdp->disks[disk].dev);
		size = sizeof(struct sis_raid_conf);
		break;

	    case AR_F_VIA_RAID:
		lba = VIA_LBA(rdp->disks[disk].dev);
		size = sizeof(struct via_raid_conf);
		break;

	    default:
		kprintf("ar%d: wiping of %s metadata is NOT supported yet\n",
		       rdp->lun, ata_raid_format(rdp));
		return ENXIO;
	    }
	    meta = kmalloc(size, M_AR, M_WAITOK | M_ZERO);
	    if (ata_raid_rw(rdp->disks[disk].dev, lba, meta, size,
			    ATA_R_WRITE | ATA_R_DIRECT)) {
		device_printf(rdp->disks[disk].dev, "wipe metadata failed\n");
		error = EIO;
	    }
	    kfree(meta, M_AR);
	}
    }
    return error;
}

/* Adaptec HostRAID Metadata */
static int
ata_raid_adaptec_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct adaptec_raid_conf *meta;
    struct ar_softc *raid;
    int array, disk, retval = 0; 

    meta = (struct adaptec_raid_conf *)
	    kmalloc(sizeof(struct adaptec_raid_conf), M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, ADP_LBA(parent),
		    meta, sizeof(struct adaptec_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "Adaptec read metadata failed\n");
	goto adaptec_out;
    }

    /* check if this is a Adaptec RAID struct */
    if (meta->magic_0 != ADP_MAGIC_0 || meta->magic_3 != ADP_MAGIC_3) {
	if (testing || bootverbose)
	    device_printf(parent, "Adaptec check1 failed\n");
	goto adaptec_out;
    }

    if (testing || bootverbose)
	ata_raid_adaptec_print_meta(meta);

    /* now convert Adaptec metadata into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_ADAPTEC_RAID))
	    continue;

	if (raid->magic_0 && raid->magic_0 != meta->configs[0].magic_0)
	    continue;

	if (!meta->generation || be32toh(meta->generation) > raid->generation) {
	    switch (meta->configs[0].type) {
	    case ADP_T_RAID0:
		raid->magic_0 = meta->configs[0].magic_0;
		raid->type = AR_T_RAID0;
		raid->interleave = 1 << (meta->configs[0].stripe_shift >> 1);
		raid->width = be16toh(meta->configs[0].total_disks);
		break;
	    
	    case ADP_T_RAID1:
		raid->magic_0 = meta->configs[0].magic_0;
		raid->type = AR_T_RAID1;
		raid->width = be16toh(meta->configs[0].total_disks) / 2;
		break;

	    default:
		device_printf(parent, "Adaptec unknown RAID type 0x%02x\n",
			      meta->configs[0].type);
		kfree(raidp[array], M_AR);
		raidp[array] = NULL;
		goto adaptec_out;
	    }

	    raid->format = AR_F_ADAPTEC_RAID;
	    raid->generation = be32toh(meta->generation);
	    raid->total_disks = be16toh(meta->configs[0].total_disks);
	    raid->total_sectors = be32toh(meta->configs[0].sectors);
	    raid->heads = 255;
	    raid->sectors = 63;
	    raid->cylinders = raid->total_sectors / (63 * 255);
	    raid->offset_sectors = 0;
	    raid->rebuild_lba = 0;
	    raid->lun = array;
	    strncpy(raid->name, meta->configs[0].name,
		    min(sizeof(raid->name), sizeof(meta->configs[0].name)));

	    /* clear out any old info */
	    if (raid->generation) {
		for (disk = 0; disk < raid->total_disks; disk++) {
		    raid->disks[disk].dev = NULL;
		    raid->disks[disk].flags = 0;
		}
	    }
	}
	if (be32toh(meta->generation) >= raid->generation) {
	    struct ata_device *atadev = device_get_softc(parent);
	    struct ata_channel *ch = device_get_softc(GRANDPARENT(dev));
	    int disk_number =
		(ch->unit << !(ch->flags & ATA_NO_SLAVE)) + atadev->unit;

	    raid->disks[disk_number].dev = parent;
	    raid->disks[disk_number].sectors = 
		be32toh(meta->configs[disk_number + 1].sectors);
	    raid->disks[disk_number].flags =
		(AR_DF_ONLINE | AR_DF_PRESENT | AR_DF_ASSIGNED);
	    ars->raid[raid->volume] = raid;
	    ars->disk_number[raid->volume] = disk_number;
	    retval = 1;
	}
	break;
    }

adaptec_out:
    kfree(meta, M_AR);
    return retval;
}

/* Highpoint V2 RocketRAID Metadata */
static int
ata_raid_hptv2_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct hptv2_raid_conf *meta;
    struct ar_softc *raid = NULL;
    int array, disk_number = 0, retval = 0;

    meta = (struct hptv2_raid_conf *)kmalloc(sizeof(struct hptv2_raid_conf),
	M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, HPTV2_LBA(parent),
		    meta, sizeof(struct hptv2_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "HighPoint (v2) read metadata failed\n");
	goto hptv2_out;
    }

    /* check if this is a HighPoint v2 RAID struct */
    if (meta->magic != HPTV2_MAGIC_OK && meta->magic != HPTV2_MAGIC_BAD) {
	if (testing || bootverbose)
	    device_printf(parent, "HighPoint (v2) check1 failed\n");
	goto hptv2_out;
    }

    /* is this disk defined, or an old leftover/spare ? */
    if (!meta->magic_0) {
	if (testing || bootverbose)
	    device_printf(parent, "HighPoint (v2) check2 failed\n");
	goto hptv2_out;
    }

    if (testing || bootverbose)
	ata_raid_hptv2_print_meta(meta);

    /* now convert HighPoint (v2) metadata into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_HPTV2_RAID))
	    continue;

	switch (meta->type) {
	case HPTV2_T_RAID0:
	    if ((meta->order & (HPTV2_O_RAID0|HPTV2_O_OK)) ==
		(HPTV2_O_RAID0|HPTV2_O_OK))
		goto highpoint_raid1;
	    if (meta->order & (HPTV2_O_RAID0 | HPTV2_O_RAID1))
		goto highpoint_raid01;
	    if (raid->magic_0 && raid->magic_0 != meta->magic_0)
		continue;
	    raid->magic_0 = meta->magic_0;
	    raid->type = AR_T_RAID0;
	    raid->interleave = 1 << meta->stripe_shift;
	    disk_number = meta->disk_number;
	    if (!(meta->order & HPTV2_O_OK))
		meta->magic = 0;        /* mark bad */
	    break;

	case HPTV2_T_RAID1:
highpoint_raid1:
	    if (raid->magic_0 && raid->magic_0 != meta->magic_0)
		continue;
	    raid->magic_0 = meta->magic_0;
	    raid->type = AR_T_RAID1;
	    disk_number = (meta->disk_number > 0);
	    break;

	case HPTV2_T_RAID01_RAID0:
highpoint_raid01:
	    if (meta->order & HPTV2_O_RAID0) {
		if ((raid->magic_0 && raid->magic_0 != meta->magic_0) ||
		    (raid->magic_1 && raid->magic_1 != meta->magic_1))
		    continue;
		raid->magic_0 = meta->magic_0;
		raid->magic_1 = meta->magic_1;
		raid->type = AR_T_RAID01;
		raid->interleave = 1 << meta->stripe_shift;
		disk_number = meta->disk_number;
	    }
	    else {
		if (raid->magic_1 && raid->magic_1 != meta->magic_1)
		    continue;
		raid->magic_1 = meta->magic_1;
		raid->type = AR_T_RAID01;
		raid->interleave = 1 << meta->stripe_shift;
		disk_number = meta->disk_number + meta->array_width;
		if (!(meta->order & HPTV2_O_RAID1))
		    meta->magic = 0;    /* mark bad */
	    }
	    break;

	case HPTV2_T_SPAN:
	    if (raid->magic_0 && raid->magic_0 != meta->magic_0)
		continue;
	    raid->magic_0 = meta->magic_0;
	    raid->type = AR_T_SPAN;
	    disk_number = meta->disk_number;
	    break;

	default:
	    device_printf(parent, "Highpoint (v2) unknown RAID type 0x%02x\n",
			  meta->type);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    goto hptv2_out;
	}

	raid->format |= AR_F_HPTV2_RAID;
	raid->disks[disk_number].dev = parent;
	raid->disks[disk_number].flags = (AR_DF_PRESENT | AR_DF_ASSIGNED);
	raid->lun = array;
	strncpy(raid->name, meta->name_1,
		min(sizeof(raid->name), sizeof(meta->name_1)));
	if (meta->magic == HPTV2_MAGIC_OK) {
	    raid->disks[disk_number].flags |= AR_DF_ONLINE;
	    raid->width = meta->array_width;
	    raid->total_sectors = meta->total_sectors;
	    raid->heads = 255;
	    raid->sectors = 63;
	    raid->cylinders = raid->total_sectors / (63 * 255);
	    raid->offset_sectors = HPTV2_LBA(parent) + 1;
	    raid->rebuild_lba = meta->rebuild_lba;
	    raid->disks[disk_number].sectors =
		raid->total_sectors / raid->width;
	}
	else
	    raid->disks[disk_number].flags &= ~AR_DF_ONLINE;

	if ((raid->type & AR_T_RAID0) && (raid->total_disks < raid->width))
	    raid->total_disks = raid->width;
	if (disk_number >= raid->total_disks)
	    raid->total_disks = disk_number + 1;
	ars->raid[raid->volume] = raid;
	ars->disk_number[raid->volume] = disk_number;
	retval = 1;
	break;
    }

hptv2_out:
    kfree(meta, M_AR);
    return retval;
}

static int
ata_raid_hptv2_write_meta(struct ar_softc *rdp)
{
    struct hptv2_raid_conf *meta;
    struct timeval timestamp;
    int disk, error = 0;

    meta = (struct hptv2_raid_conf *)kmalloc(sizeof(struct hptv2_raid_conf),
	M_AR, M_WAITOK | M_ZERO);

    microtime(&timestamp);
    rdp->magic_0 = timestamp.tv_sec + 2;
    rdp->magic_1 = timestamp.tv_sec;
   
    for (disk = 0; disk < rdp->total_disks; disk++) {
	if ((rdp->disks[disk].flags & (AR_DF_PRESENT | AR_DF_ONLINE)) ==
	    (AR_DF_PRESENT | AR_DF_ONLINE))
	    meta->magic = HPTV2_MAGIC_OK;
	if (rdp->disks[disk].flags & AR_DF_ASSIGNED) {
	    meta->magic_0 = rdp->magic_0;
	    if (strlen(rdp->name))
		strncpy(meta->name_1, rdp->name, sizeof(meta->name_1));
	    else
		strcpy(meta->name_1, "FreeBSD");
	}
	meta->disk_number = disk;

	switch (rdp->type) {
	case AR_T_RAID0:
	    meta->type = HPTV2_T_RAID0;
	    strcpy(meta->name_2, "RAID 0");
	    if (rdp->disks[disk].flags & AR_DF_ONLINE)
		meta->order = HPTV2_O_OK;
	    break;

	case AR_T_RAID1:
	    meta->type = HPTV2_T_RAID0;
	    strcpy(meta->name_2, "RAID 1");
	    meta->disk_number = (disk < rdp->width) ? disk : disk + 5;
	    meta->order = HPTV2_O_RAID0 | HPTV2_O_OK;
	    break;

	case AR_T_RAID01:
	    meta->type = HPTV2_T_RAID01_RAID0;
	    strcpy(meta->name_2, "RAID 0+1");
	    if (rdp->disks[disk].flags & AR_DF_ONLINE) {
		if (disk < rdp->width) {
		    meta->order = (HPTV2_O_RAID0 | HPTV2_O_RAID1);
		    meta->magic_0 = rdp->magic_0 - 1;
		}
		else {
		    meta->order = HPTV2_O_RAID1;
		    meta->disk_number -= rdp->width;
		}
	    }
	    else
		meta->magic_0 = rdp->magic_0 - 1;
	    meta->magic_1 = rdp->magic_1;
	    break;

	case AR_T_SPAN:
	    meta->type = HPTV2_T_SPAN;
	    strcpy(meta->name_2, "SPAN");
	    break;
	default:
	    kfree(meta, M_AR);
	    return ENODEV;
	}

	meta->array_width = rdp->width;
	meta->stripe_shift = (rdp->width > 1) ? (ffs(rdp->interleave)-1) : 0;
	meta->total_sectors = rdp->total_sectors;
	meta->rebuild_lba = rdp->rebuild_lba;
	if (testing || bootverbose)
	    ata_raid_hptv2_print_meta(meta);
	if (rdp->disks[disk].dev) {
	    if (ata_raid_rw(rdp->disks[disk].dev,
			    HPTV2_LBA(rdp->disks[disk].dev), meta,
			    sizeof(struct promise_raid_conf),
			    ATA_R_WRITE | ATA_R_DIRECT)) {
		device_printf(rdp->disks[disk].dev, "write metadata failed\n");
		error = EIO;
	    }
	}
    }
    kfree(meta, M_AR);
    return error;
}

/* Highpoint V3 RocketRAID Metadata */
static int
ata_raid_hptv3_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct hptv3_raid_conf *meta;
    struct ar_softc *raid = NULL;
    int array, disk_number, retval = 0;

    meta = (struct hptv3_raid_conf *)kmalloc(sizeof(struct hptv3_raid_conf),
	M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, HPTV3_LBA(parent),
		    meta, sizeof(struct hptv3_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "HighPoint (v3) read metadata failed\n");
	goto hptv3_out;
    }

    /* check if this is a HighPoint v3 RAID struct */
    if (meta->magic != HPTV3_MAGIC) {
	if (testing || bootverbose)
	    device_printf(parent, "HighPoint (v3) check1 failed\n");
	goto hptv3_out;
    }

    /* check if there are any config_entries */
    if (meta->config_entries < 1) {
	if (testing || bootverbose)
	    device_printf(parent, "HighPoint (v3) check2 failed\n");
	goto hptv3_out;
    }

    if (testing || bootverbose)
	ata_raid_hptv3_print_meta(meta);

    /* now convert HighPoint (v3) metadata into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_HPTV3_RAID))
	    continue;

	if ((raid->format & AR_F_HPTV3_RAID) && raid->magic_0 != meta->magic_0)
	    continue;
	
	switch (meta->configs[0].type) {
	case HPTV3_T_RAID0:
	    raid->type = AR_T_RAID0;
	    raid->width = meta->configs[0].total_disks;
	    disk_number = meta->configs[0].disk_number;
	    break;

	case HPTV3_T_RAID1:
	    raid->type = AR_T_RAID1;
	    raid->width = meta->configs[0].total_disks / 2;
	    disk_number = meta->configs[0].disk_number;
	    break;

	case HPTV3_T_RAID5:
	    raid->type = AR_T_RAID5;
	    raid->width = meta->configs[0].total_disks;
	    disk_number = meta->configs[0].disk_number;
	    break;

	case HPTV3_T_SPAN:
	    raid->type = AR_T_SPAN;
	    raid->width = meta->configs[0].total_disks;
	    disk_number = meta->configs[0].disk_number;
	    break;

	default:
	    device_printf(parent, "Highpoint (v3) unknown RAID type 0x%02x\n",
			  meta->configs[0].type);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    goto hptv3_out;
	}
	if (meta->config_entries == 2) {
	    switch (meta->configs[1].type) {
	    case HPTV3_T_RAID1:
		if (raid->type == AR_T_RAID0) {
		    raid->type = AR_T_RAID01;
		    disk_number = meta->configs[1].disk_number +
				  (meta->configs[0].disk_number << 1);
		    break;
		}
	    default:
		device_printf(parent, "Highpoint (v3) unknown level 2 0x%02x\n",
			      meta->configs[1].type);
		kfree(raidp[array], M_AR);
		raidp[array] = NULL;
		goto hptv3_out;
	    }
	}

	raid->magic_0 = meta->magic_0;
	raid->format = AR_F_HPTV3_RAID;
	raid->generation = meta->timestamp;
	raid->interleave = 1 << meta->configs[0].stripe_shift;
	raid->total_disks = meta->configs[0].total_disks +
	    meta->configs[1].total_disks;
	raid->total_sectors = meta->configs[0].total_sectors +
	    ((u_int64_t)meta->configs_high[0].total_sectors << 32);
	raid->heads = 255;
	raid->sectors = 63;
	raid->cylinders = raid->total_sectors / (63 * 255);
	raid->offset_sectors = 0;
	raid->rebuild_lba = meta->configs[0].rebuild_lba +
	    ((u_int64_t)meta->configs_high[0].rebuild_lba << 32);
	raid->lun = array;
	strncpy(raid->name, meta->name,
		min(sizeof(raid->name), sizeof(meta->name)));
	raid->disks[disk_number].sectors = raid->total_sectors /
	    (raid->type == AR_T_RAID5 ? raid->width - 1 : raid->width);
	raid->disks[disk_number].dev = parent;
	raid->disks[disk_number].flags = 
	    (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_ONLINE);
	ars->raid[raid->volume] = raid;
	ars->disk_number[raid->volume] = disk_number;
	retval = 1;
	break;
    }

hptv3_out:
    kfree(meta, M_AR);
    return retval;
}

/* Intel MatrixRAID Metadata */
static int
ata_raid_intel_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct intel_raid_conf *meta;
    struct intel_raid_mapping *map;
    struct ar_softc *raid = NULL;
    u_int32_t checksum, *ptr;
    int array, count, disk, volume = 1, retval = 0;
    char *tmp;

    meta = (struct intel_raid_conf *)kmalloc(1536, M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, INTEL_LBA(parent), meta, 1024, ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "Intel read metadata failed\n");
	goto intel_out;
    }
    tmp = (char *)meta;
    bcopy(tmp, tmp+1024, 512);
    bcopy(tmp+512, tmp, 1024);
    bzero(tmp+1024, 512);

    /* check if this is a Intel RAID struct */
    if (strncmp(meta->intel_id, INTEL_MAGIC, strlen(INTEL_MAGIC))) {
	if (testing || bootverbose)
	    device_printf(parent, "Intel check1 failed\n");
	goto intel_out;
    }

    for (checksum = 0, ptr = (u_int32_t *)meta, count = 0;
	 count < (meta->config_size / sizeof(u_int32_t)); count++) {
	checksum += *ptr++;
    }
    checksum -= meta->checksum;
    if (checksum != meta->checksum) {  
	if (testing || bootverbose)
	    device_printf(parent, "Intel check2 failed\n");          
	goto intel_out;
    }

    if (testing || bootverbose)
	ata_raid_intel_print_meta(meta);

    map = (struct intel_raid_mapping *)&meta->disk[meta->total_disks];

    /* now convert Intel metadata into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_INTEL_RAID))
	    continue;

	if ((raid->format & AR_F_INTEL_RAID) &&
	    (raid->magic_0 != meta->config_id))
	    continue;

	/*
	 * update our knowledge about the array config based on generation
	 * NOTE: there can be multiple volumes on a disk set
	 */
	if (!meta->generation || meta->generation > raid->generation) {
	    switch (map->type) {
	    case INTEL_T_RAID0:
		raid->type = AR_T_RAID0;
		raid->width = map->total_disks;
		break;

	    case INTEL_T_RAID1:
		if (map->total_disks == 4)
		    raid->type = AR_T_RAID01;
		else
		    raid->type = AR_T_RAID1;
		raid->width = map->total_disks / 2;
		break;

	    case INTEL_T_RAID5:
		raid->type = AR_T_RAID5;
		raid->width = map->total_disks;
		break;

	    default:
		device_printf(parent, "Intel unknown RAID type 0x%02x\n",
			      map->type);
		kfree(raidp[array], M_AR);
		raidp[array] = NULL;
		goto intel_out;
	    }

	    switch (map->status) {
	    case INTEL_S_READY:
		raid->status = AR_S_READY;
		break;
	    case INTEL_S_DEGRADED:
		raid->status |= AR_S_DEGRADED;
		break;
	    case INTEL_S_DISABLED:
	    case INTEL_S_FAILURE:
		raid->status = 0;
	    }

	    raid->magic_0 = meta->config_id;
	    raid->format = AR_F_INTEL_RAID;
	    raid->generation = meta->generation;
	    raid->interleave = map->stripe_sectors;
	    raid->total_disks = map->total_disks;
	    raid->total_sectors = map->total_sectors;
	    raid->heads = 255;
	    raid->sectors = 63;
	    raid->cylinders = raid->total_sectors / (63 * 255);
	    raid->offset_sectors = map->offset;         
	    raid->rebuild_lba = 0;
	    raid->lun = array;
	    raid->volume = volume - 1;
	    strncpy(raid->name, map->name,
		    min(sizeof(raid->name), sizeof(map->name)));

	    /* clear out any old info */
	    for (disk = 0; disk < raid->total_disks; disk++) {
		u_int disk_idx = map->disk_idx[disk] & 0xffff;

		raid->disks[disk].dev = NULL;
		bcopy(meta->disk[disk_idx].serial,
		      raid->disks[disk].serial,
		      sizeof(raid->disks[disk].serial));
		raid->disks[disk].sectors =
		    meta->disk[disk_idx].sectors;
		raid->disks[disk].flags = 0;
		if (meta->disk[disk_idx].flags & INTEL_F_ONLINE)
		    raid->disks[disk].flags |= AR_DF_ONLINE;
		if (meta->disk[disk_idx].flags & INTEL_F_ASSIGNED)
		    raid->disks[disk].flags |= AR_DF_ASSIGNED;
		if (meta->disk[disk_idx].flags & INTEL_F_SPARE) {
		    raid->disks[disk].flags &= ~(AR_DF_ONLINE | AR_DF_ASSIGNED);
		    raid->disks[disk].flags |= AR_DF_SPARE;
		}
		if (meta->disk[disk_idx].flags & INTEL_F_DOWN)
		    raid->disks[disk].flags &= ~AR_DF_ONLINE;
	    }
	}
	if (meta->generation >= raid->generation) {
	    for (disk = 0; disk < raid->total_disks; disk++) {
		struct ata_device *atadev = device_get_softc(parent);
		int len;

		for (len = 0; len < sizeof(atadev->param.serial); len++) {
		    if (atadev->param.serial[len] < 0x20)
			break;
		}
		len = (len > sizeof(raid->disks[disk].serial)) ?
		    len - sizeof(raid->disks[disk].serial) : 0;
		if (!strncmp(raid->disks[disk].serial, atadev->param.serial + len,
		    sizeof(raid->disks[disk].serial))) {
		    raid->disks[disk].dev = parent;
		    raid->disks[disk].flags |= (AR_DF_PRESENT | AR_DF_ONLINE);
		    ars->raid[raid->volume] = raid;
		    ars->disk_number[raid->volume] = disk;
		    retval = 1;
		}
	    }
	}
	else
	    goto intel_out;

	if (retval) {
	    if (volume < meta->total_volumes) {
		map = (struct intel_raid_mapping *)
		      &map->disk_idx[map->total_disks];
		volume++;
		retval = 0;
		continue;
	    }
	    break;
	}
	else {
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    if (volume == 2)
		retval = 1;
	}
    }

intel_out:
    kfree(meta, M_AR);
    return retval;
}

static int
ata_raid_intel_write_meta(struct ar_softc *rdp)
{
    struct intel_raid_conf *meta;
    struct intel_raid_mapping *map;
    struct timeval timestamp;
    u_int32_t checksum, *ptr;
    int count, disk, error = 0;
    char *tmp;

    meta = (struct intel_raid_conf *)kmalloc(1536, M_AR, M_WAITOK | M_ZERO);

    rdp->generation++;

    /* Generate a new config_id if none exists */
    if (!rdp->magic_0) {
	microtime(&timestamp);
	rdp->magic_0 = timestamp.tv_sec ^ timestamp.tv_usec;
    }

    bcopy(INTEL_MAGIC, meta->intel_id, sizeof(meta->intel_id));
    bcopy(INTEL_VERSION_1100, meta->version, sizeof(meta->version));
    meta->config_id = rdp->magic_0;
    meta->generation = rdp->generation;
    meta->total_disks = rdp->total_disks;
    meta->total_volumes = 1;                                    /* XXX SOS */
    for (disk = 0; disk < rdp->total_disks; disk++) {
	if (rdp->disks[disk].dev) {
	    struct ata_channel *ch =
		device_get_softc(device_get_parent(rdp->disks[disk].dev));
	    struct ata_device *atadev =
		device_get_softc(rdp->disks[disk].dev);
	    int len;

	    for (len = 0; len < sizeof(atadev->param.serial); len++) {
		if (atadev->param.serial[len] < 0x20)
		    break;
	    }
	    len = (len > sizeof(rdp->disks[disk].serial)) ?
	        len - sizeof(rdp->disks[disk].serial) : 0;
	    bcopy(atadev->param.serial + len, meta->disk[disk].serial,
		  sizeof(rdp->disks[disk].serial));
	    meta->disk[disk].sectors = rdp->disks[disk].sectors;
	    meta->disk[disk].id = (ch->unit << 16) | atadev->unit;
	}
	else
	    meta->disk[disk].sectors = rdp->total_sectors / rdp->width;
	meta->disk[disk].flags = 0;
	if (rdp->disks[disk].flags & AR_DF_SPARE)
	    meta->disk[disk].flags  |= INTEL_F_SPARE;
	else {
	    if (rdp->disks[disk].flags & AR_DF_ONLINE)
		meta->disk[disk].flags |= INTEL_F_ONLINE;
	    else
		meta->disk[disk].flags |= INTEL_F_DOWN;
	    if (rdp->disks[disk].flags & AR_DF_ASSIGNED)
		meta->disk[disk].flags  |= INTEL_F_ASSIGNED;
	}
    }
    map = (struct intel_raid_mapping *)&meta->disk[meta->total_disks];

    bcopy(rdp->name, map->name, sizeof(rdp->name));
    map->total_sectors = rdp->total_sectors;
    map->state = 12;                                            /* XXX SOS */
    map->offset = rdp->offset_sectors;
    map->stripe_count = rdp->total_sectors / (rdp->interleave*rdp->total_disks);
    map->stripe_sectors =  rdp->interleave;
    map->disk_sectors = rdp->total_sectors / rdp->width;
    map->status = INTEL_S_READY;                                /* XXX SOS */
    switch (rdp->type) {
    case AR_T_RAID0:
	map->type = INTEL_T_RAID0;
	break;
    case AR_T_RAID1:
	map->type = INTEL_T_RAID1;
	break;
    case AR_T_RAID01:
	map->type = INTEL_T_RAID1;
	break;
    case AR_T_RAID5:
	map->type = INTEL_T_RAID5;
	break;
    default:
	kfree(meta, M_AR);
	return ENODEV;
    }
    map->total_disks = rdp->total_disks;
    map->magic[0] = 0x02;
    map->magic[1] = 0xff;
    map->magic[2] = 0x01;
    for (disk = 0; disk < rdp->total_disks; disk++)
	map->disk_idx[disk] = disk;

    meta->config_size = (char *)&map->disk_idx[disk] - (char *)meta;
    for (checksum = 0, ptr = (u_int32_t *)meta, count = 0;
	 count < (meta->config_size / sizeof(u_int32_t)); count++) {
	checksum += *ptr++;
    }
    meta->checksum = checksum;

    if (testing || bootverbose)
	ata_raid_intel_print_meta(meta);

    tmp = (char *)meta;
    bcopy(tmp, tmp+1024, 512);
    bcopy(tmp+512, tmp, 1024);
    bzero(tmp+1024, 512);

    for (disk = 0; disk < rdp->total_disks; disk++) {
	if (rdp->disks[disk].dev) {
	    if (ata_raid_rw(rdp->disks[disk].dev,
			    INTEL_LBA(rdp->disks[disk].dev),
			    meta, 1024, ATA_R_WRITE | ATA_R_DIRECT)) {
		device_printf(rdp->disks[disk].dev, "write metadata failed\n");
		error = EIO;
	    }
	}
    }
    kfree(meta, M_AR);
    return error;
}


/* Integrated Technology Express Metadata */
static int
ata_raid_ite_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct ite_raid_conf *meta;
    struct ar_softc *raid = NULL;
    int array, disk_number, count, retval = 0;
    u_int16_t *ptr;

    meta = (struct ite_raid_conf *)kmalloc(sizeof(struct ite_raid_conf), M_AR,
	M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, ITE_LBA(parent),
		    meta, sizeof(struct ite_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "ITE read metadata failed\n");
	goto ite_out;
    }

    /* check if this is a ITE RAID struct */
    for (ptr = (u_int16_t *)meta->ite_id, count = 0;
	 count < sizeof(meta->ite_id)/sizeof(uint16_t); count++)
	ptr[count] = be16toh(ptr[count]);

    if (strncmp(meta->ite_id, ITE_MAGIC, strlen(ITE_MAGIC))) {
	if (testing || bootverbose)
	    device_printf(parent, "ITE check1 failed\n");
	goto ite_out;
    }

    if (testing || bootverbose)
	ata_raid_ite_print_meta(meta);

    /* now convert ITE metadata into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if ((raid = raidp[array])) {
	    if (raid->format != AR_F_ITE_RAID)
		continue;
	    if (raid->magic_0 != *((u_int64_t *)meta->timestamp_0))
		continue;
	}

	/* if we dont have a disks timestamp the RAID is invalidated */
	if (*((u_int64_t *)meta->timestamp_1) == 0)
	    goto ite_out;

	if (!raid) {
	    raidp[array] = (struct ar_softc *)kmalloc(sizeof(struct ar_softc),
						     M_AR, M_WAITOK | M_ZERO);
	}

	switch (meta->type) {
	case ITE_T_RAID0:
	    raid->type = AR_T_RAID0;
	    raid->width = meta->array_width;
	    raid->total_disks = meta->array_width;
	    disk_number = meta->disk_number;
	    break;

	case ITE_T_RAID1:
	    raid->type = AR_T_RAID1;
	    raid->width = 1;
	    raid->total_disks = 2;
	    disk_number = meta->disk_number;
	    break;

	case ITE_T_RAID01:
	    raid->type = AR_T_RAID01;
	    raid->width = meta->array_width;
	    raid->total_disks = 4;
	    disk_number = ((meta->disk_number & 0x02) >> 1) |
			  ((meta->disk_number & 0x01) << 1);
	    break;

	case ITE_T_SPAN:
	    raid->type = AR_T_SPAN;
	    raid->width = 1;
	    raid->total_disks = meta->array_width;
	    disk_number = meta->disk_number;
	    break;

	default:
	    device_printf(parent, "ITE unknown RAID type 0x%02x\n", meta->type);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    goto ite_out;
	}

	raid->magic_0 = *((u_int64_t *)meta->timestamp_0);
	raid->format = AR_F_ITE_RAID;
	raid->generation = 0;
	raid->interleave = meta->stripe_sectors;
	raid->total_sectors = meta->total_sectors;
	raid->heads = 255;
	raid->sectors = 63;
	raid->cylinders = raid->total_sectors / (63 * 255);
	raid->offset_sectors = 0;
	raid->rebuild_lba = 0;
	raid->lun = array;

	raid->disks[disk_number].dev = parent;
	raid->disks[disk_number].sectors = raid->total_sectors / raid->width;
	raid->disks[disk_number].flags = 
	    (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_ONLINE);
	ars->raid[raid->volume] = raid;
	ars->disk_number[raid->volume] = disk_number;
	retval = 1;
	break;
    }
ite_out:
    kfree(meta, M_AR);
    return retval;
}

/* JMicron Technology Corp Metadata */
static int
ata_raid_jmicron_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct jmicron_raid_conf *meta;
    struct ar_softc *raid = NULL;
    u_int16_t checksum, *ptr;
    u_int64_t disk_size;
    int count, array, disk, total_disks, retval = 0;

    meta = (struct jmicron_raid_conf *)
	kmalloc(sizeof(struct jmicron_raid_conf), M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, JMICRON_LBA(parent),
		    meta, sizeof(struct jmicron_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent,
			  "JMicron read metadata failed\n");
    }

    /* check for JMicron signature */
    if (strncmp(meta->signature, JMICRON_MAGIC, 2)) {
	if (testing || bootverbose)
	    device_printf(parent, "JMicron check1 failed\n");
	goto jmicron_out;
    }

    /* calculate checksum and compare for valid */
    for (checksum = 0, ptr = (u_int16_t *)meta, count = 0; count < 64; count++)
	checksum += *ptr++;
    if (checksum) {  
	if (testing || bootverbose)
	    device_printf(parent, "JMicron check2 failed\n");
	goto jmicron_out;
    }

    if (testing || bootverbose)
	ata_raid_jmicron_print_meta(meta);

    /* now convert JMicron meta into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
jmicron_next:
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_JMICRON_RAID))
	    continue;

	for (total_disks = 0, disk = 0; disk < JM_MAX_DISKS; disk++) {
	    if (meta->disks[disk]) {
		if (raid->format == AR_F_JMICRON_RAID) {
		    if (bcmp(&meta->disks[disk], 
			raid->disks[disk].serial, sizeof(u_int32_t))) {
			array++;
			goto jmicron_next;
		    }
		}
		else 
		    bcopy(&meta->disks[disk],
			  raid->disks[disk].serial, sizeof(u_int32_t));
		total_disks++;
	    }
	}
	/* handle spares XXX SOS */

	switch (meta->type) {
	case JM_T_RAID0:
	    raid->type = AR_T_RAID0;
	    raid->width = total_disks;
	    break;

	case JM_T_RAID1:
	    raid->type = AR_T_RAID1;
	    raid->width = 1;
	    break;

	case JM_T_RAID01:
	    raid->type = AR_T_RAID01;
	    raid->width = total_disks / 2;
	    break;

	case JM_T_RAID5:
	    raid->type = AR_T_RAID5;
	    raid->width = total_disks;
	    break;

	case JM_T_JBOD:
	    raid->type = AR_T_SPAN;
	    raid->width = 1;
	    break;

	default:
	    device_printf(parent,
			  "JMicron unknown RAID type 0x%02x\n", meta->type);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    goto jmicron_out;
	}
	disk_size = (meta->disk_sectors_high << 16) + meta->disk_sectors_low;
	raid->format = AR_F_JMICRON_RAID;
	strncpy(raid->name, meta->name, sizeof(meta->name));
	raid->generation = 0;
	raid->interleave = 2 << meta->stripe_shift;
	raid->total_disks = total_disks;
	raid->total_sectors = disk_size * (raid->width-(raid->type==AR_RAID5));
	raid->heads = 255;
	raid->sectors = 63;
	raid->cylinders = raid->total_sectors / (63 * 255);
	raid->offset_sectors = meta->offset * 16;
	raid->rebuild_lba = 0;
	raid->lun = array;

	for (disk = 0; disk < raid->total_disks; disk++) {
	    if (meta->disks[disk] == meta->disk_id) {
		raid->disks[disk].dev = parent;
		raid->disks[disk].sectors = disk_size;
		raid->disks[disk].flags =
		    (AR_DF_ONLINE | AR_DF_PRESENT | AR_DF_ASSIGNED);
		ars->raid[raid->volume] = raid;
		ars->disk_number[raid->volume] = disk;
		retval = 1;
		break;
	    }
	}
	break;
    }
jmicron_out:
    kfree(meta, M_AR);
    return retval;
}

static int
ata_raid_jmicron_write_meta(struct ar_softc *rdp)
{
    struct jmicron_raid_conf *meta;
    u_int64_t disk_sectors;
    int disk, error = 0;

    meta = (struct jmicron_raid_conf *)
	kmalloc(sizeof(struct jmicron_raid_conf), M_AR, M_WAITOK | M_ZERO);

    rdp->generation++;
    switch (rdp->type) {
    case AR_T_JBOD:
	meta->type = JM_T_JBOD;
	break;

    case AR_T_RAID0:
	meta->type = JM_T_RAID0;
	break;

    case AR_T_RAID1:
	meta->type = JM_T_RAID1;
	break;

    case AR_T_RAID5:
	meta->type = JM_T_RAID5;
	break;

    case AR_T_RAID01:
	meta->type = JM_T_RAID01;
	break;

    default:
	kfree(meta, M_AR);
	return ENODEV;
    }
    bcopy(JMICRON_MAGIC, meta->signature, sizeof(JMICRON_MAGIC));
    meta->version = JMICRON_VERSION;
    meta->offset = rdp->offset_sectors / 16;
    disk_sectors = rdp->total_sectors / (rdp->width - (rdp->type == AR_RAID5));
    meta->disk_sectors_low = disk_sectors & 0xffff;
    meta->disk_sectors_high = disk_sectors >> 16;
    strncpy(meta->name, rdp->name, sizeof(meta->name));
    meta->stripe_shift = ffs(rdp->interleave) - 2;

    for (disk = 0; disk < rdp->total_disks && disk < JM_MAX_DISKS; disk++) {
	if (rdp->disks[disk].serial[0])
	    bcopy(rdp->disks[disk].serial,&meta->disks[disk],sizeof(u_int32_t));
	else
	    meta->disks[disk] = (u_int32_t)(uintptr_t)rdp->disks[disk].dev;
    }

    for (disk = 0; disk < rdp->total_disks; disk++) {
	if (rdp->disks[disk].dev) {
	    u_int16_t checksum = 0, *ptr;
	    int count;

	    meta->disk_id = meta->disks[disk];
	    meta->checksum = 0;
	    for (ptr = (u_int16_t *)meta, count = 0; count < 64; count++)
		checksum += *ptr++;
	    meta->checksum -= checksum;

	    if (testing || bootverbose)
		ata_raid_jmicron_print_meta(meta);

	    if (ata_raid_rw(rdp->disks[disk].dev,
			    JMICRON_LBA(rdp->disks[disk].dev),
			    meta, sizeof(struct jmicron_raid_conf),
			    ATA_R_WRITE | ATA_R_DIRECT)) {
		device_printf(rdp->disks[disk].dev, "write metadata failed\n");
		error = EIO;
	    }
	}
    }
    /* handle spares XXX SOS */

    kfree(meta, M_AR);
    return error;
}

/* LSILogic V2 MegaRAID Metadata */
static int
ata_raid_lsiv2_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct lsiv2_raid_conf *meta;
    struct ar_softc *raid = NULL;
    int array, retval = 0;

    meta = (struct lsiv2_raid_conf *)kmalloc(sizeof(struct lsiv2_raid_conf),
	M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, LSIV2_LBA(parent),
		    meta, sizeof(struct lsiv2_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "LSI (v2) read metadata failed\n");
	goto lsiv2_out;
    }

    /* check if this is a LSI RAID struct */
    if (strncmp(meta->lsi_id, LSIV2_MAGIC, strlen(LSIV2_MAGIC))) {
	if (testing || bootverbose)
	    device_printf(parent, "LSI (v2) check1 failed\n");
	goto lsiv2_out;
    }

    if (testing || bootverbose)
	ata_raid_lsiv2_print_meta(meta);

    /* now convert LSI (v2) config meta into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	int raid_entry, conf_entry;

	if (!raidp[array + meta->raid_number]) {
	    raidp[array + meta->raid_number] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array + meta->raid_number];
	if (raid->format && (raid->format != AR_F_LSIV2_RAID))
	    continue;

	if (raid->magic_0 && 
	    ((raid->magic_0 != meta->timestamp) ||
	     (raid->magic_1 != meta->raid_number)))
	    continue;

	array += meta->raid_number;

	raid_entry = meta->raid_number;
	conf_entry = (meta->configs[raid_entry].raid.config_offset >> 4) +
		     meta->disk_number - 1;

	switch (meta->configs[raid_entry].raid.type) {
	case LSIV2_T_RAID0:
	    raid->magic_0 = meta->timestamp;
	    raid->magic_1 = meta->raid_number;
	    raid->type = AR_T_RAID0;
	    raid->interleave = meta->configs[raid_entry].raid.stripe_sectors;
	    raid->width = meta->configs[raid_entry].raid.array_width; 
	    break;

	case LSIV2_T_RAID1:
	    raid->magic_0 = meta->timestamp;
	    raid->magic_1 = meta->raid_number;
	    raid->type = AR_T_RAID1;
	    raid->width = meta->configs[raid_entry].raid.array_width; 
	    break;
	    
	case LSIV2_T_RAID0 | LSIV2_T_RAID1:
	    raid->magic_0 = meta->timestamp;
	    raid->magic_1 = meta->raid_number;
	    raid->type = AR_T_RAID01;
	    raid->interleave = meta->configs[raid_entry].raid.stripe_sectors;
	    raid->width = meta->configs[raid_entry].raid.array_width; 
	    break;

	default:
	    device_printf(parent, "LSI v2 unknown RAID type 0x%02x\n",
			  meta->configs[raid_entry].raid.type);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    goto lsiv2_out;
	}

	raid->format = AR_F_LSIV2_RAID;
	raid->generation = 0;
	raid->total_disks = meta->configs[raid_entry].raid.disk_count;
	raid->total_sectors = meta->configs[raid_entry].raid.total_sectors;
	raid->heads = 255;
	raid->sectors = 63;
	raid->cylinders = raid->total_sectors / (63 * 255);
	raid->offset_sectors = 0;
	raid->rebuild_lba = 0;
	raid->lun = array;

	if (meta->configs[conf_entry].disk.device != LSIV2_D_NONE) {
	    raid->disks[meta->disk_number].dev = parent;
	    raid->disks[meta->disk_number].sectors = 
		meta->configs[conf_entry].disk.disk_sectors;
	    raid->disks[meta->disk_number].flags = 
		(AR_DF_ONLINE | AR_DF_PRESENT | AR_DF_ASSIGNED);
	    ars->raid[raid->volume] = raid;
	    ars->disk_number[raid->volume] = meta->disk_number;
	    retval = 1;
	}
	else
	    raid->disks[meta->disk_number].flags &= ~AR_DF_ONLINE;

	break;
    }

lsiv2_out:
    kfree(meta, M_AR);
    return retval;
}

/* LSILogic V3 MegaRAID Metadata */
static int
ata_raid_lsiv3_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct lsiv3_raid_conf *meta;
    struct ar_softc *raid = NULL;
    u_int8_t checksum, *ptr;
    int array, entry, count, disk_number, retval = 0;

    meta = (struct lsiv3_raid_conf *)kmalloc(sizeof(struct lsiv3_raid_conf),
	M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, LSIV3_LBA(parent),
		    meta, sizeof(struct lsiv3_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "LSI (v3) read metadata failed\n");
	goto lsiv3_out;
    }

    /* check if this is a LSI RAID struct */
    if (strncmp(meta->lsi_id, LSIV3_MAGIC, strlen(LSIV3_MAGIC))) {
	if (testing || bootverbose)
	    device_printf(parent, "LSI (v3) check1 failed\n");
	goto lsiv3_out;
    }

    /* check if the checksum is OK */
    for (checksum = 0, ptr = meta->lsi_id, count = 0; count < 512; count++)
	checksum += *ptr++;
    if (checksum) {  
	if (testing || bootverbose)
	    device_printf(parent, "LSI (v3) check2 failed\n");
	goto lsiv3_out;
    }

    if (testing || bootverbose)
	ata_raid_lsiv3_print_meta(meta);

    /* now convert LSI (v3) config meta into our generic form */
    for (array = 0, entry = 0; array < MAX_ARRAYS && entry < 8;) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_LSIV3_RAID)) {
	    array++;
	    continue;
	}

	if ((raid->format == AR_F_LSIV3_RAID) &&
	    (raid->magic_0 != meta->timestamp)) {
	    array++;
	    continue;
	}

	switch (meta->raid[entry].total_disks) {
	case 0:
	    entry++;
	    continue;
	case 1:
	    if (meta->raid[entry].device == meta->device) {
		disk_number = 0;
		break;
	    }
	    if (raid->format)
		array++;
	    entry++;
	    continue;
	case 2:
	    disk_number = (meta->device & (LSIV3_D_DEVICE|LSIV3_D_CHANNEL))?1:0;
	    break;
	default:
	    device_printf(parent, "lsiv3 > 2 disk support untested!!\n");
	    disk_number = (meta->device & LSIV3_D_DEVICE ? 1 : 0) +
			  (meta->device & LSIV3_D_CHANNEL ? 2 : 0);
	    break;
	}

	switch (meta->raid[entry].type) {
	case LSIV3_T_RAID0:
	    raid->type = AR_T_RAID0;
	    raid->width = meta->raid[entry].total_disks;
	    break;

	case LSIV3_T_RAID1:
	    raid->type = AR_T_RAID1;
	    raid->width = meta->raid[entry].array_width;
	    break;

	default:
	    device_printf(parent, "LSI v3 unknown RAID type 0x%02x\n",
			  meta->raid[entry].type);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    entry++;
	    continue;
	}

	raid->magic_0 = meta->timestamp;
	raid->format = AR_F_LSIV3_RAID;
	raid->generation = 0;
	raid->interleave = meta->raid[entry].stripe_pages * 8;
	raid->total_disks = meta->raid[entry].total_disks;
	raid->total_sectors = raid->width * meta->raid[entry].sectors;
	raid->heads = 255;
	raid->sectors = 63;
	raid->cylinders = raid->total_sectors / (63 * 255);
	raid->offset_sectors = meta->raid[entry].offset;
	raid->rebuild_lba = 0;
	raid->lun = array;

	raid->disks[disk_number].dev = parent;
	raid->disks[disk_number].sectors = raid->total_sectors / raid->width;
	raid->disks[disk_number].flags = 
	    (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_ONLINE);
	ars->raid[raid->volume] = raid;
	ars->disk_number[raid->volume] = disk_number;
	retval = 1;
	entry++;
	array++;
    }

lsiv3_out:
    kfree(meta, M_AR);
    return retval;
}

/* nVidia MediaShield Metadata */
static int
ata_raid_nvidia_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct nvidia_raid_conf *meta;
    struct ar_softc *raid = NULL;
    u_int32_t checksum, *ptr;
    int array, count, retval = 0;

    meta = (struct nvidia_raid_conf *)kmalloc(sizeof(struct nvidia_raid_conf),
	M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, NVIDIA_LBA(parent),
		    meta, sizeof(struct nvidia_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "nVidia read metadata failed\n");
	goto nvidia_out;
    }

    /* check if this is a nVidia RAID struct */
    if (strncmp(meta->nvidia_id, NV_MAGIC, strlen(NV_MAGIC))) {
	if (testing || bootverbose)
	    device_printf(parent, "nVidia check1 failed\n");
	goto nvidia_out;
    }

    /* check if the checksum is OK */
    for (checksum = 0, ptr = (u_int32_t*)meta, count = 0; 
	 count < meta->config_size; count++)
	checksum += *ptr++;
    if (checksum) {  
	if (testing || bootverbose)
	    device_printf(parent, "nVidia check2 failed\n");
	goto nvidia_out;
    }

    if (testing || bootverbose)
	ata_raid_nvidia_print_meta(meta);

    /* now convert nVidia meta into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] =
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_NVIDIA_RAID))
	    continue;

	if (raid->format == AR_F_NVIDIA_RAID &&
	    ((raid->magic_0 != meta->magic_1) ||
	     (raid->magic_1 != meta->magic_2))) {
	    continue;
	}

	switch (meta->type) {
	case NV_T_SPAN:
	    raid->type = AR_T_SPAN;
	    break;

	case NV_T_RAID0: 
	    raid->type = AR_T_RAID0;
	    break;

	case NV_T_RAID1:
	    raid->type = AR_T_RAID1;
	    break;

	case NV_T_RAID5:
	    raid->type = AR_T_RAID5;
	    break;

	case NV_T_RAID01:
	    raid->type = AR_T_RAID01;
	    break;

	default:
	    device_printf(parent, "nVidia unknown RAID type 0x%02x\n",
			  meta->type);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    goto nvidia_out;
	}
	raid->magic_0 = meta->magic_1;
	raid->magic_1 = meta->magic_2;
	raid->format = AR_F_NVIDIA_RAID;
	raid->generation = 0;
	raid->interleave = meta->stripe_sectors;
	raid->width = meta->array_width;
	raid->total_disks = meta->total_disks;
	raid->total_sectors = meta->total_sectors;
	raid->heads = 255;
	raid->sectors = 63;
	raid->cylinders = raid->total_sectors / (63 * 255);
	raid->offset_sectors = 0;
	raid->rebuild_lba = meta->rebuild_lba;
	raid->lun = array;
	raid->status = AR_S_READY;
	if (meta->status & NV_S_DEGRADED)
	    raid->status |= AR_S_DEGRADED;

	raid->disks[meta->disk_number].dev = parent;
	raid->disks[meta->disk_number].sectors =
	    raid->total_sectors / raid->width;
	raid->disks[meta->disk_number].flags =
	    (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_ONLINE);
	ars->raid[raid->volume] = raid;
	ars->disk_number[raid->volume] = meta->disk_number;
	retval = 1;
	break;
    }

nvidia_out:
    kfree(meta, M_AR);
    return retval;
}

/* Promise FastTrak Metadata */
static int
ata_raid_promise_read_meta(device_t dev, struct ar_softc **raidp, int native)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct promise_raid_conf *meta;
    struct ar_softc *raid;
    u_int32_t checksum, *ptr;
    int array, count, disk, disksum = 0, retval = 0; 

    meta = (struct promise_raid_conf *)
	kmalloc(sizeof(struct promise_raid_conf), M_AR, M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, PROMISE_LBA(parent),
		    meta, sizeof(struct promise_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "%s read metadata failed\n",
			  native ? "FreeBSD" : "Promise");
	goto promise_out;
    }

    /* check the signature */
    if (native) {
	if (strncmp(meta->promise_id, ATA_MAGIC, strlen(ATA_MAGIC))) {
	    if (testing || bootverbose)
		device_printf(parent, "FreeBSD check1 failed\n");
	    goto promise_out;
	}
    }
    else {
	if (strncmp(meta->promise_id, PR_MAGIC, strlen(PR_MAGIC))) {
	    if (testing || bootverbose)
		device_printf(parent, "Promise check1 failed\n");
	    goto promise_out;
	}
    }

    /* check if the checksum is OK */
    for (checksum = 0, ptr = (u_int32_t *)meta, count = 0; count < 511; count++)
	checksum += *ptr++;
    if (checksum != *ptr) {  
	if (testing || bootverbose)
	    device_printf(parent, "%s check2 failed\n",
			  native ? "FreeBSD" : "Promise");           
	goto promise_out;
    }

    /* check on disk integrity status */
    if (meta->raid.integrity != PR_I_VALID) {
	if (testing || bootverbose)
	    device_printf(parent, "%s check3 failed\n",
			  native ? "FreeBSD" : "Promise");           
	goto promise_out;
    }

    if (testing || bootverbose)
	ata_raid_promise_print_meta(meta);

    /* now convert Promise metadata into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format &&
	    (raid->format != (native ? AR_F_FREEBSD_RAID : AR_F_PROMISE_RAID)))
	    continue;

	if ((raid->format == (native ? AR_F_FREEBSD_RAID : AR_F_PROMISE_RAID))&&
	    !(meta->raid.magic_1 == (raid->magic_1)))
	    continue;

	/* update our knowledge about the array config based on generation */
	if (!meta->raid.generation || meta->raid.generation > raid->generation){
	    switch (meta->raid.type) {
	    case PR_T_SPAN:
		raid->type = AR_T_SPAN;
		break;

	    case PR_T_JBOD:
		raid->type = AR_T_JBOD;
		break;

	    case PR_T_RAID0:
		raid->type = AR_T_RAID0;
		break;

	    case PR_T_RAID1:
		raid->type = AR_T_RAID1;
		if (meta->raid.array_width > 1)
		    raid->type = AR_T_RAID01;
		break;

	    case PR_T_RAID5:
		raid->type = AR_T_RAID5;
		break;

	    default:
		device_printf(parent, "%s unknown RAID type 0x%02x\n",
			      native ? "FreeBSD" : "Promise", meta->raid.type);
		kfree(raidp[array], M_AR);
		raidp[array] = NULL;
		goto promise_out;
	    }
	    raid->magic_1 = meta->raid.magic_1;
	    raid->format = (native ? AR_F_FREEBSD_RAID : AR_F_PROMISE_RAID);
	    raid->generation = meta->raid.generation;
	    raid->interleave = 1 << meta->raid.stripe_shift;
	    raid->width = meta->raid.array_width;
	    raid->total_disks = meta->raid.total_disks;
	    raid->heads = meta->raid.heads + 1;
	    raid->sectors = meta->raid.sectors;
	    raid->cylinders = meta->raid.cylinders + 1;
	    raid->total_sectors = meta->raid.total_sectors;
	    raid->offset_sectors = 0;
	    raid->rebuild_lba = meta->raid.rebuild_lba;
	    raid->lun = array;
	    if ((meta->raid.status &
		 (PR_S_VALID | PR_S_ONLINE | PR_S_INITED | PR_S_READY)) ==
		(PR_S_VALID | PR_S_ONLINE | PR_S_INITED | PR_S_READY)) {
		raid->status |= AR_S_READY;
		if (meta->raid.status & PR_S_DEGRADED)
		    raid->status |= AR_S_DEGRADED;
	    }
	    else
		raid->status &= ~AR_S_READY;

	    /* convert disk flags to our internal types */
	    for (disk = 0; disk < meta->raid.total_disks; disk++) {
		raid->disks[disk].dev = NULL;
		raid->disks[disk].flags = 0;
		*((u_int64_t *)(raid->disks[disk].serial)) = 
		    meta->raid.disk[disk].magic_0;
		disksum += meta->raid.disk[disk].flags;
		if (meta->raid.disk[disk].flags & PR_F_ONLINE)
		    raid->disks[disk].flags |= AR_DF_ONLINE;
		if (meta->raid.disk[disk].flags & PR_F_ASSIGNED)
		    raid->disks[disk].flags |= AR_DF_ASSIGNED;
		if (meta->raid.disk[disk].flags & PR_F_SPARE) {
		    raid->disks[disk].flags &= ~(AR_DF_ONLINE | AR_DF_ASSIGNED);
		    raid->disks[disk].flags |= AR_DF_SPARE;
		}
		if (meta->raid.disk[disk].flags & (PR_F_REDIR | PR_F_DOWN))
		    raid->disks[disk].flags &= ~AR_DF_ONLINE;
	    }
	    if (!disksum) {
		device_printf(parent, "%s subdisks has no flags\n",
			      native ? "FreeBSD" : "Promise");
		kfree(raidp[array], M_AR);
		raidp[array] = NULL;
		goto promise_out;
	    }
	}
	if (meta->raid.generation >= raid->generation) {
	    int disk_number = meta->raid.disk_number;

	    if (raid->disks[disk_number].flags && (meta->magic_0 ==
		*((u_int64_t *)(raid->disks[disk_number].serial)))) {
		raid->disks[disk_number].dev = parent;
		raid->disks[disk_number].flags |= AR_DF_PRESENT;
		raid->disks[disk_number].sectors = meta->raid.disk_sectors;
		if ((raid->disks[disk_number].flags &
		    (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_ONLINE)) ==
		    (AR_DF_PRESENT | AR_DF_ASSIGNED | AR_DF_ONLINE)) {
		    ars->raid[raid->volume] = raid;
		    ars->disk_number[raid->volume] = disk_number;
		    retval = 1;
		}
	    }
	}
	break;
    }

promise_out:
    kfree(meta, M_AR);
    return retval;
}

static int
ata_raid_promise_write_meta(struct ar_softc *rdp)
{
    struct promise_raid_conf *meta;
    struct timeval timestamp;
    u_int32_t *ckptr;
    int count, disk, drive, error = 0;

    meta = (struct promise_raid_conf *)
	kmalloc(sizeof(struct promise_raid_conf), M_AR, M_WAITOK);

    rdp->generation++;
    microtime(&timestamp);

    for (disk = 0; disk < rdp->total_disks; disk++) {
	for (count = 0; count < sizeof(struct promise_raid_conf); count++)
	    *(((u_int8_t *)meta) + count) = 255 - (count % 256);
	meta->dummy_0 = 0x00020000;
	meta->raid.disk_number = disk;

	if (rdp->disks[disk].dev) {
	    struct ata_device *atadev = device_get_softc(rdp->disks[disk].dev);
	    struct ata_channel *ch = 
		device_get_softc(device_get_parent(rdp->disks[disk].dev));

	    meta->raid.channel = ch->unit;
	    meta->raid.device = atadev->unit;
	    meta->raid.disk_sectors = rdp->disks[disk].sectors;
	    meta->raid.disk_offset = rdp->offset_sectors;
	}
	else {
	    meta->raid.channel = 0;
	    meta->raid.device = 0;
	    meta->raid.disk_sectors = 0;
	    meta->raid.disk_offset = 0;
	}
	meta->magic_0 = PR_MAGIC0(meta->raid) | timestamp.tv_sec;
	meta->magic_1 = timestamp.tv_sec >> 16;
	meta->magic_2 = timestamp.tv_sec;
	meta->raid.integrity = PR_I_VALID;
	meta->raid.magic_0 = meta->magic_0;
	meta->raid.rebuild_lba = rdp->rebuild_lba;
	meta->raid.generation = rdp->generation;

	if (rdp->status & AR_S_READY) {
	    meta->raid.flags = (PR_F_VALID | PR_F_ASSIGNED | PR_F_ONLINE);
	    meta->raid.status = 
		(PR_S_VALID | PR_S_ONLINE | PR_S_INITED | PR_S_READY);
	    if (rdp->status & AR_S_DEGRADED)
		meta->raid.status |= PR_S_DEGRADED;
	    else
		meta->raid.status |= PR_S_FUNCTIONAL;
	}
	else {
	    meta->raid.flags = PR_F_DOWN;
	    meta->raid.status = 0;
	}

	switch (rdp->type) {
	case AR_T_RAID0:
	    meta->raid.type = PR_T_RAID0;
	    break;
	case AR_T_RAID1:
	    meta->raid.type = PR_T_RAID1;
	    break;
	case AR_T_RAID01:
	    meta->raid.type = PR_T_RAID1;
	    break;
	case AR_T_RAID5:
	    meta->raid.type = PR_T_RAID5;
	    break;
	case AR_T_SPAN:
	    meta->raid.type = PR_T_SPAN;
	    break;
	case AR_T_JBOD:
	    meta->raid.type = PR_T_JBOD;
	    break;
	default:
	    kfree(meta, M_AR);
	    return ENODEV;
	}

	meta->raid.total_disks = rdp->total_disks;
	meta->raid.stripe_shift = ffs(rdp->interleave) - 1;
	meta->raid.array_width = rdp->width;
	meta->raid.array_number = rdp->lun;
	meta->raid.total_sectors = rdp->total_sectors;
	meta->raid.cylinders = rdp->cylinders - 1;
	meta->raid.heads = rdp->heads - 1;
	meta->raid.sectors = rdp->sectors;
	meta->raid.magic_1 = (u_int64_t)meta->magic_2<<16 | meta->magic_1;

	bzero(&meta->raid.disk, 8 * 12);
	for (drive = 0; drive < rdp->total_disks; drive++) {
	    meta->raid.disk[drive].flags = 0;
	    if (rdp->disks[drive].flags & AR_DF_PRESENT)
		meta->raid.disk[drive].flags |= PR_F_VALID;
	    if (rdp->disks[drive].flags & AR_DF_ASSIGNED)
		meta->raid.disk[drive].flags |= PR_F_ASSIGNED;
	    if (rdp->disks[drive].flags & AR_DF_ONLINE)
		meta->raid.disk[drive].flags |= PR_F_ONLINE;
	    else
		if (rdp->disks[drive].flags & AR_DF_PRESENT)
		    meta->raid.disk[drive].flags = (PR_F_REDIR | PR_F_DOWN);
	    if (rdp->disks[drive].flags & AR_DF_SPARE)
		meta->raid.disk[drive].flags |= PR_F_SPARE;
	    meta->raid.disk[drive].dummy_0 = 0x0;
	    if (rdp->disks[drive].dev) {
		struct ata_channel *ch = 
		    device_get_softc(device_get_parent(rdp->disks[drive].dev));
		struct ata_device *atadev =
		    device_get_softc(rdp->disks[drive].dev);

		meta->raid.disk[drive].channel = ch->unit;
		meta->raid.disk[drive].device = atadev->unit;
	    }
	    meta->raid.disk[drive].magic_0 =
		PR_MAGIC0(meta->raid.disk[drive]) | timestamp.tv_sec;
	}

	if (rdp->disks[disk].dev) {
	    if ((rdp->disks[disk].flags & (AR_DF_PRESENT | AR_DF_ONLINE)) ==
		(AR_DF_PRESENT | AR_DF_ONLINE)) {
		if (rdp->format == AR_F_FREEBSD_RAID)
		    bcopy(ATA_MAGIC, meta->promise_id, sizeof(ATA_MAGIC));
		else
		    bcopy(PR_MAGIC, meta->promise_id, sizeof(PR_MAGIC));
	    }
	    else
		bzero(meta->promise_id, sizeof(meta->promise_id));
	    meta->checksum = 0;
	    for (ckptr = (int32_t *)meta, count = 0; count < 511; count++)
		meta->checksum += *ckptr++;
	    if (testing || bootverbose)
		ata_raid_promise_print_meta(meta);
	    if (ata_raid_rw(rdp->disks[disk].dev,
			    PROMISE_LBA(rdp->disks[disk].dev),
			    meta, sizeof(struct promise_raid_conf),
			    ATA_R_WRITE | ATA_R_DIRECT)) {
		device_printf(rdp->disks[disk].dev, "write metadata failed\n");
		error = EIO;
	    }
	}
    }
    kfree(meta, M_AR);
    return error;
}

/* Silicon Image Medley Metadata */
static int
ata_raid_sii_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct sii_raid_conf *meta;
    struct ar_softc *raid = NULL;
    u_int16_t checksum, *ptr;
    int array, count, disk, retval = 0;

    meta = (struct sii_raid_conf *)kmalloc(sizeof(struct sii_raid_conf), M_AR,
	M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, SII_LBA(parent),
		    meta, sizeof(struct sii_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "Silicon Image read metadata failed\n");
	goto sii_out;
    }

    /* check if this is a Silicon Image (Medley) RAID struct */
    for (checksum = 0, ptr = (u_int16_t *)meta, count = 0; count < 160; count++)
	checksum += *ptr++;
    if (checksum) {  
	if (testing || bootverbose)
	    device_printf(parent, "Silicon Image check1 failed\n");
	goto sii_out;
    }

    for (checksum = 0, ptr = (u_int16_t *)meta, count = 0; count < 256; count++)
	checksum += *ptr++;
    if (checksum != meta->checksum_1) {  
	if (testing || bootverbose)
	    device_printf(parent, "Silicon Image check2 failed\n");          
	goto sii_out;
    }

    /* check verison */
    if (meta->version_major != 0x0002 ||
	(meta->version_minor != 0x0000 && meta->version_minor != 0x0001)) {
	if (testing || bootverbose)
	    device_printf(parent, "Silicon Image check3 failed\n");          
	goto sii_out;
    }

    if (testing || bootverbose)
	ata_raid_sii_print_meta(meta);

    /* now convert Silicon Image meta into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_SII_RAID))
	    continue;

	if (raid->format == AR_F_SII_RAID &&
	    (raid->magic_0 != *((u_int64_t *)meta->timestamp))) {
	    continue;
	}

	/* update our knowledge about the array config based on generation */
	if (!meta->generation || meta->generation > raid->generation) {
	    switch (meta->type) {
	    case SII_T_RAID0:
		raid->type = AR_T_RAID0;
		break;

	    case SII_T_RAID1:
		raid->type = AR_T_RAID1;
		break;

	    case SII_T_RAID01:
		raid->type = AR_T_RAID01;
		break;

	    case SII_T_SPARE:
		device_printf(parent, "Silicon Image SPARE disk\n");
		kfree(raidp[array], M_AR);
		raidp[array] = NULL;
		goto sii_out;

	    default:
		device_printf(parent,"Silicon Image unknown RAID type 0x%02x\n",
			      meta->type);
		kfree(raidp[array], M_AR);
		raidp[array] = NULL;
		goto sii_out;
	    }
	    raid->magic_0 = *((u_int64_t *)meta->timestamp);
	    raid->format = AR_F_SII_RAID;
	    raid->generation = meta->generation;
	    raid->interleave = meta->stripe_sectors;
	    raid->width = (meta->raid0_disks != 0xff) ? meta->raid0_disks : 1;
	    raid->total_disks = 
		((meta->raid0_disks != 0xff) ? meta->raid0_disks : 0) +
		((meta->raid1_disks != 0xff) ? meta->raid1_disks : 0);
	    raid->total_sectors = meta->total_sectors;
	    raid->heads = 255;
	    raid->sectors = 63;
	    raid->cylinders = raid->total_sectors / (63 * 255);
	    raid->offset_sectors = 0;
	    raid->rebuild_lba = meta->rebuild_lba;
	    raid->lun = array;
	    strncpy(raid->name, meta->name,
		    min(sizeof(raid->name), sizeof(meta->name)));

	    /* clear out any old info */
	    if (raid->generation) {
		for (disk = 0; disk < raid->total_disks; disk++) {
		    raid->disks[disk].dev = NULL;
		    raid->disks[disk].flags = 0;
		}
	    }
	}
	if (meta->generation >= raid->generation) {
	    /* XXX SOS add check for the right physical disk by serial# */
	    if (meta->status & SII_S_READY) {
		int disk_number = (raid->type == AR_T_RAID01) ?
		    meta->raid1_ident + (meta->raid0_ident << 1) :
		    meta->disk_number;

		raid->disks[disk_number].dev = parent;
		raid->disks[disk_number].sectors = 
		    raid->total_sectors / raid->width;
		raid->disks[disk_number].flags =
		    (AR_DF_ONLINE | AR_DF_PRESENT | AR_DF_ASSIGNED);
		ars->raid[raid->volume] = raid;
		ars->disk_number[raid->volume] = disk_number;
		retval = 1;
	    }
	}
	break;
    }

sii_out:
    kfree(meta, M_AR);
    return retval;
}

/* Silicon Integrated Systems Metadata */
static int
ata_raid_sis_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct sis_raid_conf *meta;
    struct ar_softc *raid = NULL;
    int array, disk_number, drive, retval = 0;

    meta = (struct sis_raid_conf *)kmalloc(sizeof(struct sis_raid_conf), M_AR,
	M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, SIS_LBA(parent),
		    meta, sizeof(struct sis_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent,
			  "Silicon Integrated Systems read metadata failed\n");
    }

    /* check for SiS magic */
    if (meta->magic != SIS_MAGIC) {
	if (testing || bootverbose)
	    device_printf(parent,
			  "Silicon Integrated Systems check1 failed\n");
	goto sis_out;
    }

    if (testing || bootverbose)
	ata_raid_sis_print_meta(meta);

    /* now convert SiS meta into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}

	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_SIS_RAID))
	    continue;

	if ((raid->format == AR_F_SIS_RAID) &&
	    ((raid->magic_0 != meta->controller_pci_id) ||
	     (raid->magic_1 != meta->timestamp))) {
	    continue;
	}

	switch (meta->type_total_disks & SIS_T_MASK) {
	case SIS_T_JBOD:
	    raid->type = AR_T_JBOD;
	    raid->width = (meta->type_total_disks & SIS_D_MASK);
	    raid->total_sectors += SIS_LBA(parent);
	    break;

	case SIS_T_RAID0:
	    raid->type = AR_T_RAID0;
	    raid->width = (meta->type_total_disks & SIS_D_MASK);
	    if (!raid->total_sectors || 
		(raid->total_sectors > (raid->width * SIS_LBA(parent))))
		raid->total_sectors = raid->width * SIS_LBA(parent);
	    break;

	case SIS_T_RAID1:
	    raid->type = AR_T_RAID1;
	    raid->width = 1;
	    if (!raid->total_sectors || (raid->total_sectors > SIS_LBA(parent)))
		raid->total_sectors = SIS_LBA(parent);
	    break;

	default:
	    device_printf(parent, "Silicon Integrated Systems "
			  "unknown RAID type 0x%08x\n", meta->magic);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    goto sis_out;
	}
	raid->magic_0 = meta->controller_pci_id;
	raid->magic_1 = meta->timestamp;
	raid->format = AR_F_SIS_RAID;
	raid->generation = 0;
	raid->interleave = meta->stripe_sectors;
	raid->total_disks = (meta->type_total_disks & SIS_D_MASK);
	raid->heads = 255;
	raid->sectors = 63;
	raid->cylinders = raid->total_sectors / (63 * 255);
	raid->offset_sectors = 0;
	raid->rebuild_lba = 0;
	raid->lun = array;
	/* XXX SOS if total_disks > 2 this doesn't float */
	if (((meta->disks & SIS_D_MASTER) >> 4) == meta->disk_number)
	    disk_number = 0;
	else 
	    disk_number = 1;

	for (drive = 0; drive < raid->total_disks; drive++) {
	    raid->disks[drive].sectors = raid->total_sectors/raid->width;
	    if (drive == disk_number) {
		raid->disks[disk_number].dev = parent;
		raid->disks[disk_number].flags =
		    (AR_DF_ONLINE | AR_DF_PRESENT | AR_DF_ASSIGNED);
		ars->raid[raid->volume] = raid;
		ars->disk_number[raid->volume] = disk_number;
	    }
	}
	retval = 1;
	break;
    }

sis_out:
    kfree(meta, M_AR);
    return retval;
}

static int
ata_raid_sis_write_meta(struct ar_softc *rdp)
{
    struct sis_raid_conf *meta;
    struct timeval timestamp;
    int disk, error = 0;

    meta = (struct sis_raid_conf *)kmalloc(sizeof(struct sis_raid_conf), M_AR,
	M_WAITOK | M_ZERO);

    rdp->generation++;
    microtime(&timestamp);

    meta->magic = SIS_MAGIC;
    /* XXX SOS if total_disks > 2 this doesn't float */
    for (disk = 0; disk < rdp->total_disks; disk++) {
	if (rdp->disks[disk].dev) {
	    struct ata_channel *ch = 
		device_get_softc(device_get_parent(rdp->disks[disk].dev));
	    struct ata_device *atadev = device_get_softc(rdp->disks[disk].dev);
	    int disk_number = 1 + atadev->unit + (ch->unit << 1);

	    meta->disks |= disk_number << ((1 - disk) << 2);
	}
    }
    switch (rdp->type) {
    case AR_T_JBOD:
	meta->type_total_disks = SIS_T_JBOD;
	break;

    case AR_T_RAID0:
	meta->type_total_disks = SIS_T_RAID0;
	break;

    case AR_T_RAID1:
	meta->type_total_disks = SIS_T_RAID1;
	break;

    default:
	kfree(meta, M_AR);
	return ENODEV;
    }
    meta->type_total_disks |= (rdp->total_disks & SIS_D_MASK);
    meta->stripe_sectors = rdp->interleave;
    meta->timestamp = timestamp.tv_sec;

    for (disk = 0; disk < rdp->total_disks; disk++) {
	if (rdp->disks[disk].dev) {
	    struct ata_channel *ch = 
		device_get_softc(device_get_parent(rdp->disks[disk].dev));
	    struct ata_device *atadev = device_get_softc(rdp->disks[disk].dev);

	    meta->controller_pci_id =
		(pci_get_vendor(GRANDPARENT(rdp->disks[disk].dev)) << 16) |
		pci_get_device(GRANDPARENT(rdp->disks[disk].dev));
	    bcopy(atadev->param.model, meta->model, sizeof(meta->model));

	    /* XXX SOS if total_disks > 2 this may not float */
	    meta->disk_number = 1 + atadev->unit + (ch->unit << 1);

	    if (testing || bootverbose)
		ata_raid_sis_print_meta(meta);

	    if (ata_raid_rw(rdp->disks[disk].dev,
			    SIS_LBA(rdp->disks[disk].dev),
			    meta, sizeof(struct sis_raid_conf),
			    ATA_R_WRITE | ATA_R_DIRECT)) {
		device_printf(rdp->disks[disk].dev, "write metadata failed\n");
		error = EIO;
	    }
	}
    }
    kfree(meta, M_AR);
    return error;
}

/* VIA Tech V-RAID Metadata */
static int
ata_raid_via_read_meta(device_t dev, struct ar_softc **raidp)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    device_t parent = device_get_parent(dev);
    struct via_raid_conf *meta;
    struct ar_softc *raid = NULL;
    u_int8_t checksum, *ptr;
    int array, count, disk, retval = 0;

    meta = (struct via_raid_conf *)kmalloc(sizeof(struct via_raid_conf), M_AR,
	M_WAITOK | M_ZERO);

    if (ata_raid_rw(parent, VIA_LBA(parent),
		    meta, sizeof(struct via_raid_conf), ATA_R_READ)) {
	if (testing || bootverbose)
	    device_printf(parent, "VIA read metadata failed\n");
	goto via_out;
    }

    /* check if this is a VIA RAID struct */
    if (meta->magic != VIA_MAGIC) {
	if (testing || bootverbose)
	    device_printf(parent, "VIA check1 failed\n");
	goto via_out;
    }

    /* calculate checksum and compare for valid */
    for (checksum = 0, ptr = (u_int8_t *)meta, count = 0; count < 50; count++)
	checksum += *ptr++;
    if (checksum != meta->checksum) {  
	if (testing || bootverbose)
	    device_printf(parent, "VIA check2 failed\n");
	goto via_out;
    }

    if (testing || bootverbose)
	ata_raid_via_print_meta(meta);

    /* now convert VIA meta into our generic form */
    for (array = 0; array < MAX_ARRAYS; array++) {
	if (!raidp[array]) {
	    raidp[array] = 
		(struct ar_softc *)kmalloc(sizeof(struct ar_softc), M_AR,
					  M_WAITOK | M_ZERO);
	}
	raid = raidp[array];
	if (raid->format && (raid->format != AR_F_VIA_RAID))
	    continue;

	if (raid->format == AR_F_VIA_RAID && (raid->magic_0 != meta->disks[0]))
	    continue;

	switch (meta->type & VIA_T_MASK) {
	case VIA_T_RAID0:
	    raid->type = AR_T_RAID0;
	    raid->width = meta->stripe_layout & VIA_L_DISKS;
	    if (!raid->total_sectors ||
		(raid->total_sectors > (raid->width * meta->disk_sectors)))
		raid->total_sectors = raid->width * meta->disk_sectors;
	    break;

	case VIA_T_RAID1:
	    raid->type = AR_T_RAID1;
	    raid->width = 1;
	    raid->total_sectors = meta->disk_sectors;
	    break;

	case VIA_T_RAID01:
	    raid->type = AR_T_RAID01;
	    raid->width = meta->stripe_layout & VIA_L_DISKS;
	    if (!raid->total_sectors ||
		(raid->total_sectors > (raid->width * meta->disk_sectors)))
		raid->total_sectors = raid->width * meta->disk_sectors;
	    break;

	case VIA_T_RAID5:
	    raid->type = AR_T_RAID5;
	    raid->width = meta->stripe_layout & VIA_L_DISKS;
	    if (!raid->total_sectors ||
		(raid->total_sectors > ((raid->width - 1)*meta->disk_sectors)))
		raid->total_sectors = (raid->width - 1) * meta->disk_sectors;
	    break;

	case VIA_T_SPAN:
	    raid->type = AR_T_SPAN;
	    raid->width = 1;
	    raid->total_sectors += meta->disk_sectors;
	    break;

	default:
	    device_printf(parent,"VIA unknown RAID type 0x%02x\n", meta->type);
	    kfree(raidp[array], M_AR);
	    raidp[array] = NULL;
	    goto via_out;
	}
	raid->magic_0 = meta->disks[0];
	raid->format = AR_F_VIA_RAID;
	raid->generation = 0;
	raid->interleave = 
	    0x08 << ((meta->stripe_layout & VIA_L_MASK) >> VIA_L_SHIFT);
	for (count = 0, disk = 0; disk < 8; disk++)
	    if (meta->disks[disk])
		count++;
	raid->total_disks = count;
	raid->heads = 255;
	raid->sectors = 63;
	raid->cylinders = raid->total_sectors / (63 * 255);
	raid->offset_sectors = 0;
	raid->rebuild_lba = 0;
	raid->lun = array;

	for (disk = 0; disk < raid->total_disks; disk++) {
	    if (meta->disks[disk] == meta->disk_id) {
		raid->disks[disk].dev = parent;
		bcopy(&meta->disk_id, raid->disks[disk].serial,
		      sizeof(u_int32_t));
		raid->disks[disk].sectors = meta->disk_sectors;
		raid->disks[disk].flags =
		    (AR_DF_ONLINE | AR_DF_PRESENT | AR_DF_ASSIGNED);
		ars->raid[raid->volume] = raid;
		ars->disk_number[raid->volume] = disk;
		retval = 1;
		break;
	    }
	}
	break;
    }

via_out:
    kfree(meta, M_AR);
    return retval;
}

static int
ata_raid_via_write_meta(struct ar_softc *rdp)
{
    struct via_raid_conf *meta;
    int disk, error = 0;

    meta = (struct via_raid_conf *)kmalloc(sizeof(struct via_raid_conf), M_AR,
	M_WAITOK | M_ZERO);

    rdp->generation++;

    meta->magic = VIA_MAGIC;
    meta->dummy_0 = 0x02;
    switch (rdp->type) {
    case AR_T_SPAN:
	meta->type = VIA_T_SPAN;
	meta->stripe_layout = (rdp->total_disks & VIA_L_DISKS);
	break;

    case AR_T_RAID0:
	meta->type = VIA_T_RAID0;
	meta->stripe_layout = ((rdp->interleave >> 1) & VIA_L_MASK);
	meta->stripe_layout |= (rdp->total_disks & VIA_L_DISKS);
	break;

    case AR_T_RAID1:
	meta->type = VIA_T_RAID1;
	meta->stripe_layout = (rdp->total_disks & VIA_L_DISKS);
	break;

    case AR_T_RAID5:
	meta->type = VIA_T_RAID5;
	meta->stripe_layout = ((rdp->interleave >> 1) & VIA_L_MASK);
	meta->stripe_layout |= (rdp->total_disks & VIA_L_DISKS);
	break;

    case AR_T_RAID01:
	meta->type = VIA_T_RAID01;
	meta->stripe_layout = ((rdp->interleave >> 1) & VIA_L_MASK);
	meta->stripe_layout |= (rdp->width & VIA_L_DISKS);
	break;

    default:
	kfree(meta, M_AR);
	return ENODEV;
    }
    meta->type |= VIA_T_BOOTABLE;       /* XXX SOS */
    meta->disk_sectors = 
	rdp->total_sectors / (rdp->width - (rdp->type == AR_RAID5));
    for (disk = 0; disk < rdp->total_disks; disk++)
	meta->disks[disk] = (u_int32_t)(uintptr_t)rdp->disks[disk].dev;

    for (disk = 0; disk < rdp->total_disks; disk++) {
	if (rdp->disks[disk].dev) {
	    u_int8_t *ptr;
	    int count;

	    meta->disk_index = disk * sizeof(u_int32_t);
	    if (rdp->type == AR_T_RAID01)
		meta->disk_index = ((meta->disk_index & 0x08) << 2) |
				   (meta->disk_index & ~0x08);
	    meta->disk_id = meta->disks[disk];
	    meta->checksum = 0;
	    for (ptr = (u_int8_t *)meta, count = 0; count < 50; count++)
		meta->checksum += *ptr++;

	    if (testing || bootverbose)
		ata_raid_via_print_meta(meta);

	    if (ata_raid_rw(rdp->disks[disk].dev,
			    VIA_LBA(rdp->disks[disk].dev),
			    meta, sizeof(struct via_raid_conf),
			    ATA_R_WRITE | ATA_R_DIRECT)) {
		device_printf(rdp->disks[disk].dev, "write metadata failed\n");
		error = EIO;
	    }
	}
    }
    kfree(meta, M_AR);
    return error;
}

static struct ata_request *
ata_raid_init_request(struct ar_softc *rdp, struct bio *bio)
{
    struct ata_request *request;

    if (!(request = ata_alloc_request())) {
	kprintf("FAILURE - out of memory in ata_raid_init_request\n");
	return NULL;
    }
    request->timeout = ATA_DEFAULT_TIMEOUT;
    request->retries = 2;
    request->callback = ata_raid_done;
    request->driver = rdp;
    request->bio = bio;
    switch (request->bio->bio_buf->b_cmd) {
    case BUF_CMD_READ:
	request->flags = ATA_R_READ;
	break;
    case BUF_CMD_WRITE:
	request->flags = ATA_R_WRITE;
	break;
    case BUF_CMD_FLUSH:
	request->flags = ATA_R_CONTROL;
	break;
    default:
	kprintf("ar%d: FAILURE - unknown BUF operation\n", rdp->lun);
	ata_free_request(request);
	return(NULL);
    }
    return request;
}

static int
ata_raid_send_request(struct ata_request *request)
{
    struct ata_device *atadev = device_get_softc(request->dev);
  
    request->transfersize = min(request->bytecount, atadev->max_iosize);
    if (request->flags & ATA_R_READ) {
	if (atadev->mode >= ATA_DMA) {
	    request->flags |= ATA_R_DMA;
	    request->u.ata.command = ATA_READ_DMA;
	}
	else if (atadev->max_iosize > DEV_BSIZE)
	    request->u.ata.command = ATA_READ_MUL;
	else
	    request->u.ata.command = ATA_READ;
    }
    else if (request->flags & ATA_R_WRITE) {
	if (atadev->mode >= ATA_DMA) {
	    request->flags |= ATA_R_DMA;
	    request->u.ata.command = ATA_WRITE_DMA;
	}
	else if (atadev->max_iosize > DEV_BSIZE)
	    request->u.ata.command = ATA_WRITE_MUL;
	else
	    request->u.ata.command = ATA_WRITE;
    }
    else {
	device_printf(request->dev, "FAILURE - unknown IO operation\n");
	ata_free_request(request);
	return EIO;
    }
    request->flags |= (ATA_R_ORDERED | ATA_R_THREAD);
    ata_queue_request(request);
    return 0;
}

static int
ata_raid_rw(device_t dev, u_int64_t lba, void *data, u_int bcount, int flags)
{
    struct ata_device *atadev = device_get_softc(dev);
    struct ata_request *request;
    int error;

    if (bcount % DEV_BSIZE) {
	device_printf(dev, "FAILURE - transfers must be modulo sectorsize\n");
	return ENOMEM;
    }
	
    if (!(request = ata_alloc_request())) {
	device_printf(dev, "FAILURE - out of memory in ata_raid_rw\n");
	return ENOMEM;
    }

    /* setup request */
    request->dev = dev;
    request->timeout = ATA_DEFAULT_TIMEOUT;
    request->retries = 0;
    request->data = data;
    request->bytecount = bcount;
    request->transfersize = DEV_BSIZE;
    request->u.ata.lba = lba;
    request->u.ata.count = request->bytecount / DEV_BSIZE;
    request->flags = flags;

    if (flags & ATA_R_READ) {
	if (atadev->mode >= ATA_DMA) {
	    request->u.ata.command = ATA_READ_DMA;
	    request->flags |= ATA_R_DMA;
	}
	else
	    request->u.ata.command = ATA_READ;
	ata_queue_request(request);
    }
    else if (flags & ATA_R_WRITE) {
	if (atadev->mode >= ATA_DMA) {
	    request->u.ata.command = ATA_WRITE_DMA;
	    request->flags |= ATA_R_DMA;
	}
	else
	    request->u.ata.command = ATA_WRITE;
	ata_queue_request(request);
    }
    else {
	device_printf(dev, "FAILURE - unknown IO operation\n");
	request->result = EIO;
    }
    error = request->result;
    ata_free_request(request);
    return error;
}

/*
 * module handeling
 */
static int
ata_raid_subdisk_probe(device_t dev)
{
    device_quiet(dev);
    return 0;
}

static int
ata_raid_subdisk_attach(device_t dev)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    int volume;

    for (volume = 0; volume < MAX_VOLUMES; volume++) {
	ars->raid[volume] = NULL;
	ars->disk_number[volume] = -1;
    }
    ata_raid_read_metadata(dev);
    return 0;
}

static int
ata_raid_subdisk_detach(device_t dev)
{
    struct ata_raid_subdisk *ars = device_get_softc(dev);
    int volume;

    for (volume = 0; volume < MAX_VOLUMES; volume++) {
	if (ars->raid[volume]) {
	    ars->raid[volume]->disks[ars->disk_number[volume]].flags &= 
		~(AR_DF_PRESENT | AR_DF_ONLINE);
	    ars->raid[volume]->disks[ars->disk_number[volume]].dev = NULL;
#if 0
	    if (mtx_initialized(&ars->raid[volume]->lock))
#endif
		ata_raid_config_changed(ars->raid[volume], 1);
	    ars->raid[volume] = NULL;
	    ars->disk_number[volume] = -1;
	}
    }
    return 0;
}

static device_method_t ata_raid_sub_methods[] = {
    /* device interface */
    DEVMETHOD(device_probe,     ata_raid_subdisk_probe),
    DEVMETHOD(device_attach,    ata_raid_subdisk_attach),
    DEVMETHOD(device_detach,    ata_raid_subdisk_detach),
    DEVMETHOD_END
};

static driver_t ata_raid_sub_driver = {
    "subdisk",
    ata_raid_sub_methods,
    sizeof(struct ata_raid_subdisk)
};

DRIVER_MODULE(subdisk, ad, ata_raid_sub_driver, ata_raid_sub_devclass, NULL, NULL);

static int
ata_raid_module_event_handler(module_t mod, int what, void *arg)
{
    int i;

    switch (what) {
    case MOD_LOAD:
	if (testing || bootverbose)
	    kprintf("ATA PseudoRAID loaded\n");
#if 0
	/* setup table to hold metadata for all ATA PseudoRAID arrays */
	ata_raid_arrays = kmalloc(sizeof(struct ar_soft *) * MAX_ARRAYS,
				M_AR, M_WAITOK | M_ZERO);
#endif
	/* attach found PseudoRAID arrays */
	for (i = 0; i < MAX_ARRAYS; i++) {
	    struct ar_softc *rdp = ata_raid_arrays[i];
	    
	    if (!rdp || !rdp->format)
		continue;
	    if (testing || bootverbose)
		ata_raid_print_meta(rdp);
	    ata_raid_attach(rdp, 0);
	}   
	ata_raid_ioctl_func = ata_raid_ioctl;
	return 0;

    case MOD_UNLOAD:
	/* detach found PseudoRAID arrays */
	for (i = 0; i < MAX_ARRAYS; i++) {
	    struct ar_softc *rdp = ata_raid_arrays[i];

	    if (!rdp || !rdp->status)
		continue;
#if 0
	    if (mtx_initialized(&rdp->lock))
		lockuninit(&rdp->lock);
#endif
	    disk_destroy(&rdp->disk);
	}
	if (testing || bootverbose)
	    kprintf("ATA PseudoRAID unloaded\n");
#if 0
	kfree(ata_raid_arrays, M_AR);
#endif
	ata_raid_ioctl_func = NULL;
	return 0;
	
    default:
	return EOPNOTSUPP;
    }
}

static moduledata_t ata_raid_moduledata =
    { "ataraid", ata_raid_module_event_handler, NULL };
DECLARE_MODULE(ata, ata_raid_moduledata, SI_SUB_RAID, SI_ORDER_FIRST);
MODULE_VERSION(ataraid, 1);
MODULE_DEPEND(ataraid, ata, 1, 1, 1);
MODULE_DEPEND(ataraid, ad, 1, 1, 1);

static char *
ata_raid_format(struct ar_softc *rdp)
{
    switch (rdp->format) {
    case AR_F_FREEBSD_RAID:     return "FreeBSD PseudoRAID";
    case AR_F_ADAPTEC_RAID:     return "Adaptec HostRAID";
    case AR_F_HPTV2_RAID:       return "HighPoint v2 RocketRAID";
    case AR_F_HPTV3_RAID:       return "HighPoint v3 RocketRAID";
    case AR_F_INTEL_RAID:       return "Intel MatrixRAID";
    case AR_F_ITE_RAID:         return "Integrated Technology Express";
    case AR_F_JMICRON_RAID:     return "JMicron Technology Corp";
    case AR_F_LSIV2_RAID:       return "LSILogic v2 MegaRAID";
    case AR_F_LSIV3_RAID:       return "LSILogic v3 MegaRAID";
    case AR_F_NVIDIA_RAID:      return "nVidia MediaShield";
    case AR_F_PROMISE_RAID:     return "Promise Fasttrak";
    case AR_F_SII_RAID:         return "Silicon Image Medley";
    case AR_F_SIS_RAID:         return "Silicon Integrated Systems";
    case AR_F_VIA_RAID:         return "VIA Tech V-RAID";
    default:                    return "UNKNOWN";
    }
}

static char *
ata_raid_type(struct ar_softc *rdp)
{
    switch (rdp->type) {
    case AR_T_JBOD:     return "JBOD";
    case AR_T_SPAN:     return "SPAN";
    case AR_T_RAID0:    return "RAID0";
    case AR_T_RAID1:    return "RAID1";
    case AR_T_RAID3:    return "RAID3";
    case AR_T_RAID4:    return "RAID4";
    case AR_T_RAID5:    return "RAID5";
    case AR_T_RAID01:   return "RAID0+1";
    default:            return "UNKNOWN";
    }
}

static char *
ata_raid_flags(struct ar_softc *rdp)
{
    switch (rdp->status & (AR_S_READY | AR_S_DEGRADED | AR_S_REBUILDING)) {
    case AR_S_READY:                                    return "READY";
    case AR_S_READY | AR_S_DEGRADED:                    return "DEGRADED";
    case AR_S_READY | AR_S_REBUILDING:
    case AR_S_READY | AR_S_DEGRADED | AR_S_REBUILDING:  return "REBUILDING";
    default:                                            return "BROKEN";
    }
}

/* debugging gunk */
static void
ata_raid_print_meta(struct ar_softc *raid)
{
    int i;

    kprintf("********** ATA PseudoRAID ar%d Metadata **********\n", raid->lun);
    kprintf("=================================================\n");
    kprintf("format              %s\n", ata_raid_format(raid));
    kprintf("type                %s\n", ata_raid_type(raid));
    kprintf("flags               0x%02x %pb%i\n", raid->status,
	   "\20\3REBUILDING\2DEGRADED\1READY\n", raid->status);
    kprintf("magic_0             0x%016jx\n", raid->magic_0);
    kprintf("magic_1             0x%016jx\n",raid->magic_1);
    kprintf("generation          %u\n", raid->generation);
    kprintf("total_sectors       %ju\n", raid->total_sectors);
    kprintf("offset_sectors      %ju\n", raid->offset_sectors);
    kprintf("heads               %u\n", raid->heads);
    kprintf("sectors             %u\n", raid->sectors);
    kprintf("cylinders           %u\n", raid->cylinders);
    kprintf("width               %u\n", raid->width);
    kprintf("interleave          %u\n", raid->interleave);
    kprintf("total_disks         %u\n", raid->total_disks);
    for (i = 0; i < raid->total_disks; i++) {
	kprintf("    disk %d:      flags = 0x%02x %pb%i\n", i, raid->disks[i].flags,
	       "\20\4ONLINE\3SPARE\2ASSIGNED\1PRESENT\n", raid->disks[i].flags);
	if (raid->disks[i].dev) {
	    kprintf("        ");
	    device_printf(raid->disks[i].dev, " sectors %jd\n",
			  raid->disks[i].sectors);
	}
    }
    kprintf("=================================================\n");
}

static char *
ata_raid_adaptec_type(int type)
{
    static char buffer[16];

    switch (type) {
    case ADP_T_RAID0:   return "RAID0";
    case ADP_T_RAID1:   return "RAID1";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_adaptec_print_meta(struct adaptec_raid_conf *meta)
{
    int i;

    kprintf("********* ATA Adaptec HostRAID Metadata *********\n");
    kprintf("magic_0             <0x%08x>\n", be32toh(meta->magic_0));
    kprintf("generation          0x%08x\n", be32toh(meta->generation));
    kprintf("dummy_0             0x%04x\n", be16toh(meta->dummy_0));
    kprintf("total_configs       %u\n", be16toh(meta->total_configs));
    kprintf("dummy_1             0x%04x\n", be16toh(meta->dummy_1));
    kprintf("checksum            0x%04x\n", be16toh(meta->checksum));
    kprintf("dummy_2             0x%08x\n", be32toh(meta->dummy_2));
    kprintf("dummy_3             0x%08x\n", be32toh(meta->dummy_3));
    kprintf("flags               0x%08x\n", be32toh(meta->flags));
    kprintf("timestamp           0x%08x\n", be32toh(meta->timestamp));
    kprintf("dummy_4             0x%08x 0x%08x 0x%08x 0x%08x\n",
	   be32toh(meta->dummy_4[0]), be32toh(meta->dummy_4[1]),
	   be32toh(meta->dummy_4[2]), be32toh(meta->dummy_4[3]));
    kprintf("dummy_5             0x%08x 0x%08x 0x%08x 0x%08x\n",
	   be32toh(meta->dummy_5[0]), be32toh(meta->dummy_5[1]),
	   be32toh(meta->dummy_5[2]), be32toh(meta->dummy_5[3]));

    for (i = 0; i < be16toh(meta->total_configs); i++) {
	kprintf("    %d   total_disks  %u\n", i,
	       be16toh(meta->configs[i].disk_number));
	kprintf("    %d   generation   %u\n", i,
	       be16toh(meta->configs[i].generation));
	kprintf("    %d   magic_0      0x%08x\n", i,
	       be32toh(meta->configs[i].magic_0));
	kprintf("    %d   dummy_0      0x%02x\n", i, meta->configs[i].dummy_0);
	kprintf("    %d   type         %s\n", i,
	       ata_raid_adaptec_type(meta->configs[i].type));
	kprintf("    %d   dummy_1      0x%02x\n", i, meta->configs[i].dummy_1);
	kprintf("    %d   flags        %d\n", i,
	       be32toh(meta->configs[i].flags));
	kprintf("    %d   dummy_2      0x%02x\n", i, meta->configs[i].dummy_2);
	kprintf("    %d   dummy_3      0x%02x\n", i, meta->configs[i].dummy_3);
	kprintf("    %d   dummy_4      0x%02x\n", i, meta->configs[i].dummy_4);
	kprintf("    %d   dummy_5      0x%02x\n", i, meta->configs[i].dummy_5);
	kprintf("    %d   disk_number  %u\n", i,
	       be32toh(meta->configs[i].disk_number));
	kprintf("    %d   dummy_6      0x%08x\n", i,
	       be32toh(meta->configs[i].dummy_6));
	kprintf("    %d   sectors      %u\n", i,
	       be32toh(meta->configs[i].sectors));
	kprintf("    %d   stripe_shift %u\n", i,
	       be16toh(meta->configs[i].stripe_shift));
	kprintf("    %d   dummy_7      0x%08x\n", i,
	       be32toh(meta->configs[i].dummy_7));
	kprintf("    %d   dummy_8      0x%08x 0x%08x 0x%08x 0x%08x\n", i,
	       be32toh(meta->configs[i].dummy_8[0]),
	       be32toh(meta->configs[i].dummy_8[1]),
	       be32toh(meta->configs[i].dummy_8[2]),
	       be32toh(meta->configs[i].dummy_8[3]));
	kprintf("    %d   name         <%s>\n", i, meta->configs[i].name);
    }
    kprintf("magic_1             <0x%08x>\n", be32toh(meta->magic_1));
    kprintf("magic_2             <0x%08x>\n", be32toh(meta->magic_2));
    kprintf("magic_3             <0x%08x>\n", be32toh(meta->magic_3));
    kprintf("magic_4             <0x%08x>\n", be32toh(meta->magic_4));
    kprintf("=================================================\n");
}

static char *
ata_raid_hptv2_type(int type)
{
    static char buffer[16];

    switch (type) {
    case HPTV2_T_RAID0:         return "RAID0";
    case HPTV2_T_RAID1:         return "RAID1";
    case HPTV2_T_RAID01_RAID0:  return "RAID01_RAID0";
    case HPTV2_T_SPAN:          return "SPAN";
    case HPTV2_T_RAID_3:        return "RAID3";
    case HPTV2_T_RAID_5:        return "RAID5";
    case HPTV2_T_JBOD:          return "JBOD";
    case HPTV2_T_RAID01_RAID1:  return "RAID01_RAID1";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_hptv2_print_meta(struct hptv2_raid_conf *meta)
{
    int i;

    kprintf("****** ATA Highpoint V2 RocketRAID Metadata *****\n");
    kprintf("magic               0x%08x\n", meta->magic);
    kprintf("magic_0             0x%08x\n", meta->magic_0);
    kprintf("magic_1             0x%08x\n", meta->magic_1);
    kprintf("order               0x%08x\n", meta->order);
    kprintf("array_width         %u\n", meta->array_width);
    kprintf("stripe_shift        %u\n", meta->stripe_shift);
    kprintf("type                %s\n", ata_raid_hptv2_type(meta->type));
    kprintf("disk_number         %u\n", meta->disk_number);
    kprintf("total_sectors       %u\n", meta->total_sectors);
    kprintf("disk_mode           0x%08x\n", meta->disk_mode);
    kprintf("boot_mode           0x%08x\n", meta->boot_mode);
    kprintf("boot_disk           0x%02x\n", meta->boot_disk);
    kprintf("boot_protect        0x%02x\n", meta->boot_protect);
    kprintf("log_entries         0x%02x\n", meta->error_log_entries);
    kprintf("log_index           0x%02x\n", meta->error_log_index);
    if (meta->error_log_entries) {
	kprintf("    timestamp  reason disk  status  sectors lba\n");
	for (i = meta->error_log_index;
	     i < meta->error_log_index + meta->error_log_entries; i++)
	    kprintf("    0x%08x  0x%02x  0x%02x  0x%02x    0x%02x    0x%08x\n",
		   meta->errorlog[i%32].timestamp,
		   meta->errorlog[i%32].reason,
		   meta->errorlog[i%32].disk, meta->errorlog[i%32].status,
		   meta->errorlog[i%32].sectors, meta->errorlog[i%32].lba);
    }
    kprintf("rebuild_lba         0x%08x\n", meta->rebuild_lba);
    kprintf("dummy_1             0x%02x\n", meta->dummy_1);
    kprintf("name_1              <%.15s>\n", meta->name_1);
    kprintf("dummy_2             0x%02x\n", meta->dummy_2);
    kprintf("name_2              <%.15s>\n", meta->name_2);
    kprintf("=================================================\n");
}

static char *
ata_raid_hptv3_type(int type)
{
    static char buffer[16];

    switch (type) {
    case HPTV3_T_SPARE: return "SPARE";
    case HPTV3_T_JBOD:  return "JBOD";
    case HPTV3_T_SPAN:  return "SPAN";
    case HPTV3_T_RAID0: return "RAID0";
    case HPTV3_T_RAID1: return "RAID1";
    case HPTV3_T_RAID3: return "RAID3";
    case HPTV3_T_RAID5: return "RAID5";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_hptv3_print_meta(struct hptv3_raid_conf *meta)
{
    int i;

    kprintf("****** ATA Highpoint V3 RocketRAID Metadata *****\n");
    kprintf("magic               0x%08x\n", meta->magic);
    kprintf("magic_0             0x%08x\n", meta->magic_0);
    kprintf("checksum_0          0x%02x\n", meta->checksum_0);
    kprintf("mode                0x%02x\n", meta->mode);
    kprintf("user_mode           0x%02x\n", meta->user_mode);
    kprintf("config_entries      0x%02x\n", meta->config_entries);
    for (i = 0; i < meta->config_entries; i++) {
	kprintf("config %d:\n", i);
	kprintf("    total_sectors       %ju\n",
	       meta->configs[0].total_sectors +
	       ((u_int64_t)meta->configs_high[0].total_sectors << 32));
	kprintf("    type                %s\n",
	       ata_raid_hptv3_type(meta->configs[i].type)); 
	kprintf("    total_disks         %u\n", meta->configs[i].total_disks);
	kprintf("    disk_number         %u\n", meta->configs[i].disk_number);
	kprintf("    stripe_shift        %u\n", meta->configs[i].stripe_shift);
	kprintf("    status              %pb%i\n",
	       "\20\2RAID5\1NEED_REBUILD\n", meta->configs[i].status);
	kprintf("    critical_disks      %u\n", meta->configs[i].critical_disks);
	kprintf("    rebuild_lba         %ju\n",
	       meta->configs_high[0].rebuild_lba +
	       ((u_int64_t)meta->configs_high[0].rebuild_lba << 32));
    }
    kprintf("name                <%.16s>\n", meta->name);
    kprintf("timestamp           0x%08x\n", meta->timestamp);
    kprintf("description         <%.16s>\n", meta->description);
    kprintf("creator             <%.16s>\n", meta->creator);
    kprintf("checksum_1          0x%02x\n", meta->checksum_1);
    kprintf("dummy_0             0x%02x\n", meta->dummy_0);
    kprintf("dummy_1             0x%02x\n", meta->dummy_1);
    kprintf("flags               %pb%i\n",
	   "\20\4RCACHE\3WCACHE\2NCQ\1TCQ\n", meta->flags);
    kprintf("=================================================\n");
}

static char *
ata_raid_intel_type(int type)
{
    static char buffer[16];

    switch (type) {
    case INTEL_T_RAID0: return "RAID0";
    case INTEL_T_RAID1: return "RAID1";
    case INTEL_T_RAID5: return "RAID5";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_intel_print_meta(struct intel_raid_conf *meta)
{
    struct intel_raid_mapping *map;
    int i, j;

    kprintf("********* ATA Intel MatrixRAID Metadata *********\n");
    kprintf("intel_id            <%.24s>\n", meta->intel_id);
    kprintf("version             <%.6s>\n", meta->version);
    kprintf("checksum            0x%08x\n", meta->checksum);
    kprintf("config_size         0x%08x\n", meta->config_size);
    kprintf("config_id           0x%08x\n", meta->config_id);
    kprintf("generation          0x%08x\n", meta->generation);
    kprintf("total_disks         %u\n", meta->total_disks);
    kprintf("total_volumes       %u\n", meta->total_volumes);
    kprintf("DISK#   serial disk_sectors disk_id flags\n");
    for (i = 0; i < meta->total_disks; i++ ) {
	kprintf("    %d   <%.16s> %u 0x%08x 0x%08x\n", i,
	       meta->disk[i].serial, meta->disk[i].sectors,
	       meta->disk[i].id, meta->disk[i].flags);
    }
    map = (struct intel_raid_mapping *)&meta->disk[meta->total_disks];
    for (j = 0; j < meta->total_volumes; j++) {
	kprintf("name                %.16s\n", map->name);
	kprintf("total_sectors       %ju\n", map->total_sectors);
	kprintf("state               %u\n", map->state);
	kprintf("reserved            %u\n", map->reserved);
	kprintf("offset              %u\n", map->offset);
	kprintf("disk_sectors        %u\n", map->disk_sectors);
	kprintf("stripe_count        %u\n", map->stripe_count);
	kprintf("stripe_sectors      %u\n", map->stripe_sectors);
	kprintf("status              %u\n", map->status);
	kprintf("type                %s\n", ata_raid_intel_type(map->type));
	kprintf("total_disks         %u\n", map->total_disks);
	kprintf("magic[0]            0x%02x\n", map->magic[0]);
	kprintf("magic[1]            0x%02x\n", map->magic[1]);
	kprintf("magic[2]            0x%02x\n", map->magic[2]);
	for (i = 0; i < map->total_disks; i++ ) {
	    kprintf("    disk %d at disk_idx 0x%08x\n", i, map->disk_idx[i]);
	}
	map = (struct intel_raid_mapping *)&map->disk_idx[map->total_disks];
    }
    kprintf("=================================================\n");
}

static char *
ata_raid_ite_type(int type)
{
    static char buffer[16];

    switch (type) {
    case ITE_T_RAID0:   return "RAID0";
    case ITE_T_RAID1:   return "RAID1";
    case ITE_T_RAID01:  return "RAID0+1";
    case ITE_T_SPAN:    return "SPAN";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_ite_print_meta(struct ite_raid_conf *meta)
{
    kprintf("*** ATA Integrated Technology Express Metadata **\n");
    kprintf("ite_id              <%.40s>\n", meta->ite_id);
    kprintf("timestamp_0         %04x/%02x/%02x %02x:%02x:%02x.%02x\n",
	   *((u_int16_t *)meta->timestamp_0), meta->timestamp_0[2],
	   meta->timestamp_0[3], meta->timestamp_0[5], meta->timestamp_0[4],
	   meta->timestamp_0[7], meta->timestamp_0[6]);
    kprintf("total_sectors       %jd\n", meta->total_sectors);
    kprintf("type                %s\n", ata_raid_ite_type(meta->type));
    kprintf("stripe_1kblocks     %u\n", meta->stripe_1kblocks);
    kprintf("timestamp_1         %04x/%02x/%02x %02x:%02x:%02x.%02x\n",
	   *((u_int16_t *)meta->timestamp_1), meta->timestamp_1[2],
	   meta->timestamp_1[3], meta->timestamp_1[5], meta->timestamp_1[4],
	   meta->timestamp_1[7], meta->timestamp_1[6]);
    kprintf("stripe_sectors      %u\n", meta->stripe_sectors);
    kprintf("array_width         %u\n", meta->array_width);
    kprintf("disk_number         %u\n", meta->disk_number);
    kprintf("disk_sectors        %u\n", meta->disk_sectors);
    kprintf("=================================================\n");
}

static char *
ata_raid_jmicron_type(int type)
{
    static char buffer[16];

    switch (type) {
    case JM_T_RAID0:	return "RAID0";
    case JM_T_RAID1:	return "RAID1";
    case JM_T_RAID01:	return "RAID0+1";
    case JM_T_JBOD:	return "JBOD";
    case JM_T_RAID5:	return "RAID5";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_jmicron_print_meta(struct jmicron_raid_conf *meta)
{
    int i;

    kprintf("***** ATA JMicron Technology Corp Metadata ******\n");
    kprintf("signature           %.2s\n", meta->signature);
    kprintf("version             0x%04x\n", meta->version);
    kprintf("checksum            0x%04x\n", meta->checksum);
    kprintf("disk_id             0x%08x\n", meta->disk_id);
    kprintf("offset              0x%08x\n", meta->offset);
    kprintf("disk_sectors_low    0x%08x\n", meta->disk_sectors_low);
    kprintf("disk_sectors_high   0x%08x\n", meta->disk_sectors_high);
    kprintf("name                %.16s\n", meta->name);
    kprintf("type                %s\n", ata_raid_jmicron_type(meta->type));
    kprintf("stripe_shift        %d\n", meta->stripe_shift);
    kprintf("flags               0x%04x\n", meta->flags);
    kprintf("spare:\n");
    for (i=0; i < 2 && meta->spare[i]; i++)
	kprintf("    %d                  0x%08x\n", i, meta->spare[i]);
    kprintf("disks:\n");
    for (i=0; i < 8 && meta->disks[i]; i++)
	kprintf("    %d                  0x%08x\n", i, meta->disks[i]);
    kprintf("=================================================\n");
}

static char *
ata_raid_lsiv2_type(int type)
{
    static char buffer[16];

    switch (type) {
    case LSIV2_T_RAID0: return "RAID0";
    case LSIV2_T_RAID1: return "RAID1";
    case LSIV2_T_SPARE: return "SPARE";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_lsiv2_print_meta(struct lsiv2_raid_conf *meta)
{
    int i;

    kprintf("******* ATA LSILogic V2 MegaRAID Metadata *******\n");
    kprintf("lsi_id              <%s>\n", meta->lsi_id);
    kprintf("dummy_0             0x%02x\n", meta->dummy_0);
    kprintf("flags               0x%02x\n", meta->flags);
    kprintf("version             0x%04x\n", meta->version);
    kprintf("config_entries      0x%02x\n", meta->config_entries);
    kprintf("raid_count          0x%02x\n", meta->raid_count);
    kprintf("total_disks         0x%02x\n", meta->total_disks);
    kprintf("dummy_1             0x%02x\n", meta->dummy_1);
    kprintf("dummy_2             0x%04x\n", meta->dummy_2);
    for (i = 0; i < meta->config_entries; i++) {
	kprintf("    type             %s\n",
	       ata_raid_lsiv2_type(meta->configs[i].raid.type));
	kprintf("    dummy_0          %02x\n", meta->configs[i].raid.dummy_0);
	kprintf("    stripe_sectors   %u\n",
	       meta->configs[i].raid.stripe_sectors);
	kprintf("    array_width      %u\n",
	       meta->configs[i].raid.array_width);
	kprintf("    disk_count       %u\n", meta->configs[i].raid.disk_count);
	kprintf("    config_offset    %u\n",
	       meta->configs[i].raid.config_offset);
	kprintf("    dummy_1          %u\n", meta->configs[i].raid.dummy_1);
	kprintf("    flags            %02x\n", meta->configs[i].raid.flags);
	kprintf("    total_sectors    %u\n",
	       meta->configs[i].raid.total_sectors);
    }
    kprintf("disk_number         0x%02x\n", meta->disk_number);
    kprintf("raid_number         0x%02x\n", meta->raid_number);
    kprintf("timestamp           0x%08x\n", meta->timestamp);
    kprintf("=================================================\n");
}

static char *
ata_raid_lsiv3_type(int type)
{
    static char buffer[16];

    switch (type) {
    case LSIV3_T_RAID0: return "RAID0";
    case LSIV3_T_RAID1: return "RAID1";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_lsiv3_print_meta(struct lsiv3_raid_conf *meta)
{
    int i;

    kprintf("******* ATA LSILogic V3 MegaRAID Metadata *******\n");
    kprintf("lsi_id              <%.6s>\n", meta->lsi_id);
    kprintf("dummy_0             0x%04x\n", meta->dummy_0);
    kprintf("version             0x%04x\n", meta->version);
    kprintf("dummy_0             0x%04x\n", meta->dummy_1);
    kprintf("RAID configs:\n");
    for (i = 0; i < 8; i++) {
	if (meta->raid[i].total_disks) {
	    kprintf("%02d  stripe_pages       %u\n", i,
		   meta->raid[i].stripe_pages);
	    kprintf("%02d  type               %s\n", i,
		   ata_raid_lsiv3_type(meta->raid[i].type));
	    kprintf("%02d  total_disks        %u\n", i,
		   meta->raid[i].total_disks);
	    kprintf("%02d  array_width        %u\n", i,
		   meta->raid[i].array_width);
	    kprintf("%02d  sectors            %u\n", i, meta->raid[i].sectors);
	    kprintf("%02d  offset             %u\n", i, meta->raid[i].offset);
	    kprintf("%02d  device             0x%02x\n", i,
		   meta->raid[i].device);
	}
    }
    kprintf("DISK configs:\n");
    for (i = 0; i < 6; i++) {
	    if (meta->disk[i].disk_sectors) {
	    kprintf("%02d  disk_sectors       %u\n", i,
		   meta->disk[i].disk_sectors);
	    kprintf("%02d  flags              0x%02x\n", i, meta->disk[i].flags);
	}
    }
    kprintf("device              0x%02x\n", meta->device);
    kprintf("timestamp           0x%08x\n", meta->timestamp);
    kprintf("checksum_1          0x%02x\n", meta->checksum_1);
    kprintf("=================================================\n");
}

static char *
ata_raid_nvidia_type(int type)
{
    static char buffer[16];

    switch (type) {
    case NV_T_SPAN:     return "SPAN";
    case NV_T_RAID0:    return "RAID0";
    case NV_T_RAID1:    return "RAID1";
    case NV_T_RAID3:    return "RAID3";
    case NV_T_RAID5:    return "RAID5";
    case NV_T_RAID01:   return "RAID0+1";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_nvidia_print_meta(struct nvidia_raid_conf *meta)
{
    kprintf("******** ATA nVidia MediaShield Metadata ********\n");
    kprintf("nvidia_id           <%.8s>\n", meta->nvidia_id);
    kprintf("config_size         %u\n", meta->config_size);
    kprintf("checksum            0x%08x\n", meta->checksum);
    kprintf("version             0x%04x\n", meta->version);
    kprintf("disk_number         %u\n", meta->disk_number);
    kprintf("dummy_0             0x%02x\n", meta->dummy_0);
    kprintf("total_sectors       %u\n", meta->total_sectors);
    kprintf("sectors_size        %u\n", meta->sector_size);
    kprintf("serial              %.16s\n", meta->serial);
    kprintf("revision            %.4s\n", meta->revision);
    kprintf("dummy_1             0x%08x\n", meta->dummy_1);
    kprintf("magic_0             0x%08x\n", meta->magic_0);
    kprintf("magic_1             0x%016jx\n", meta->magic_1);
    kprintf("magic_2             0x%016jx\n", meta->magic_2);
    kprintf("flags               0x%02x\n", meta->flags);
    kprintf("array_width         %u\n", meta->array_width);
    kprintf("total_disks         %u\n", meta->total_disks);
    kprintf("dummy_2             0x%02x\n", meta->dummy_2);
    kprintf("type                %s\n", ata_raid_nvidia_type(meta->type));
    kprintf("dummy_3             0x%04x\n", meta->dummy_3);
    kprintf("stripe_sectors      %u\n", meta->stripe_sectors);
    kprintf("stripe_bytes        %u\n", meta->stripe_bytes);
    kprintf("stripe_shift        %u\n", meta->stripe_shift);
    kprintf("stripe_mask         0x%08x\n", meta->stripe_mask);
    kprintf("stripe_sizesectors  %u\n", meta->stripe_sizesectors);
    kprintf("stripe_sizebytes    %u\n", meta->stripe_sizebytes);
    kprintf("rebuild_lba         %u\n", meta->rebuild_lba);
    kprintf("dummy_4             0x%08x\n", meta->dummy_4);
    kprintf("dummy_5             0x%08x\n", meta->dummy_5);
    kprintf("status              0x%08x\n", meta->status);
    kprintf("=================================================\n");
}

static char *
ata_raid_promise_type(int type)
{
    static char buffer[16];

    switch (type) {
    case PR_T_RAID0:    return "RAID0";
    case PR_T_RAID1:    return "RAID1";
    case PR_T_RAID3:    return "RAID3";
    case PR_T_RAID5:    return "RAID5";
    case PR_T_SPAN:     return "SPAN";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_promise_print_meta(struct promise_raid_conf *meta)
{
    int i;

    kprintf("********* ATA Promise FastTrak Metadata *********\n");
    kprintf("promise_id          <%s>\n", meta->promise_id);
    kprintf("dummy_0             0x%08x\n", meta->dummy_0);
    kprintf("magic_0             0x%016jx\n", meta->magic_0);
    kprintf("magic_1             0x%04x\n", meta->magic_1);
    kprintf("magic_2             0x%08x\n", meta->magic_2);
    kprintf("integrity           0x%08x %pb%i\n", meta->raid.integrity,
	   "\20\10VALID\n", meta->raid.integrity);
    kprintf("flags               0x%02x %pb%i\n",
	   meta->raid.flags,
	   "\20\10READY\7DOWN\6REDIR\5DUPLICATE\4SPARE"
	   "\3ASSIGNED\2ONLINE\1VALID\n", meta->raid.flags);
    kprintf("disk_number         %d\n", meta->raid.disk_number);
    kprintf("channel             0x%02x\n", meta->raid.channel);
    kprintf("device              0x%02x\n", meta->raid.device);
    kprintf("magic_0             0x%016jx\n", meta->raid.magic_0);
    kprintf("disk_offset         %u\n", meta->raid.disk_offset);
    kprintf("disk_sectors        %u\n", meta->raid.disk_sectors);
    kprintf("rebuild_lba         0x%08x\n", meta->raid.rebuild_lba);
    kprintf("generation          0x%04x\n", meta->raid.generation);
    kprintf("status              0x%02x %pb%i\n",
	    meta->raid.status,
	   "\20\6MARKED\5DEGRADED\4READY\3INITED\2ONLINE\1VALID\n",
	    meta->raid.status);
    kprintf("type                %s\n", ata_raid_promise_type(meta->raid.type));
    kprintf("total_disks         %u\n", meta->raid.total_disks);
    kprintf("stripe_shift        %u\n", meta->raid.stripe_shift);
    kprintf("array_width         %u\n", meta->raid.array_width);
    kprintf("array_number        %u\n", meta->raid.array_number);
    kprintf("total_sectors       %u\n", meta->raid.total_sectors);
    kprintf("cylinders           %u\n", meta->raid.cylinders);
    kprintf("heads               %u\n", meta->raid.heads);
    kprintf("sectors             %u\n", meta->raid.sectors);
    kprintf("magic_1             0x%016jx\n", meta->raid.magic_1);
    kprintf("DISK#   flags dummy_0 channel device  magic_0\n");
    for (i = 0; i < 8; i++) {
	kprintf("  %d    %pb%i    0x%02x  0x%02x  0x%02x  ", i,
	       "\20\10READY\7DOWN\6REDIR\5DUPLICATE\4SPARE"
	       "\3ASSIGNED\2ONLINE\1VALID\n",
	       meta->raid.disk[i].flags, meta->raid.disk[i].dummy_0,
	       meta->raid.disk[i].channel, meta->raid.disk[i].device);
	kprintf("0x%016jx\n", meta->raid.disk[i].magic_0);
    }
    kprintf("checksum            0x%08x\n", meta->checksum);
    kprintf("=================================================\n");
}

static char *
ata_raid_sii_type(int type)
{
    static char buffer[16];

    switch (type) {
    case SII_T_RAID0:   return "RAID0";
    case SII_T_RAID1:   return "RAID1";
    case SII_T_RAID01:  return "RAID0+1";
    case SII_T_SPARE:   return "SPARE";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_sii_print_meta(struct sii_raid_conf *meta)
{
    kprintf("******* ATA Silicon Image Medley Metadata *******\n");
    kprintf("total_sectors       %ju\n", meta->total_sectors);
    kprintf("dummy_0             0x%04x\n", meta->dummy_0);
    kprintf("dummy_1             0x%04x\n", meta->dummy_1);
    kprintf("controller_pci_id   0x%08x\n", meta->controller_pci_id);
    kprintf("version_minor       0x%04x\n", meta->version_minor);
    kprintf("version_major       0x%04x\n", meta->version_major);
    kprintf("timestamp           20%02x/%02x/%02x %02x:%02x:%02x\n",
	   meta->timestamp[5], meta->timestamp[4], meta->timestamp[3],
	   meta->timestamp[2], meta->timestamp[1], meta->timestamp[0]);
    kprintf("stripe_sectors      %u\n", meta->stripe_sectors);
    kprintf("dummy_2             0x%04x\n", meta->dummy_2);
    kprintf("disk_number         %u\n", meta->disk_number);
    kprintf("type                %s\n", ata_raid_sii_type(meta->type));
    kprintf("raid0_disks         %u\n", meta->raid0_disks);
    kprintf("raid0_ident         %u\n", meta->raid0_ident);
    kprintf("raid1_disks         %u\n", meta->raid1_disks);
    kprintf("raid1_ident         %u\n", meta->raid1_ident);
    kprintf("rebuild_lba         %ju\n", meta->rebuild_lba);
    kprintf("generation          0x%08x\n", meta->generation);
    kprintf("status              0x%02x %pb%i\n",
	    meta->status, "\20\1READY\n", meta->status);
    kprintf("base_raid1_position %02x\n", meta->base_raid1_position);
    kprintf("base_raid0_position %02x\n", meta->base_raid0_position);
    kprintf("position            %02x\n", meta->position);
    kprintf("dummy_3             %04x\n", meta->dummy_3);
    kprintf("name                <%.16s>\n", meta->name);
    kprintf("checksum_0          0x%04x\n", meta->checksum_0);
    kprintf("checksum_1          0x%04x\n", meta->checksum_1);
    kprintf("=================================================\n");
}

static char *
ata_raid_sis_type(int type)
{
    static char buffer[16];

    switch (type) {
    case SIS_T_JBOD:    return "JBOD";
    case SIS_T_RAID0:   return "RAID0";
    case SIS_T_RAID1:   return "RAID1";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_sis_print_meta(struct sis_raid_conf *meta)
{
    kprintf("**** ATA Silicon Integrated Systems Metadata ****\n");
    kprintf("magic               0x%04x\n", meta->magic);
    kprintf("disks               0x%02x\n", meta->disks);
    kprintf("type                %s\n",
	   ata_raid_sis_type(meta->type_total_disks & SIS_T_MASK));
    kprintf("total_disks         %u\n", meta->type_total_disks & SIS_D_MASK);
    kprintf("dummy_0             0x%08x\n", meta->dummy_0);
    kprintf("controller_pci_id   0x%08x\n", meta->controller_pci_id);
    kprintf("stripe_sectors      %u\n", meta->stripe_sectors);
    kprintf("dummy_1             0x%04x\n", meta->dummy_1);
    kprintf("timestamp           0x%08x\n", meta->timestamp);
    kprintf("model               %.40s\n", meta->model);
    kprintf("disk_number         %u\n", meta->disk_number);
    kprintf("dummy_2             0x%02x 0x%02x 0x%02x\n",
	   meta->dummy_2[0], meta->dummy_2[1], meta->dummy_2[2]);
    kprintf("=================================================\n");
}

static char *
ata_raid_via_type(int type)
{
    static char buffer[16];

    switch (type) {
    case VIA_T_RAID0:   return "RAID0";
    case VIA_T_RAID1:   return "RAID1";
    case VIA_T_RAID5:   return "RAID5";
    case VIA_T_RAID01:  return "RAID0+1";
    case VIA_T_SPAN:    return "SPAN";
    default:            ksprintf(buffer, "UNKNOWN 0x%02x", type);
			return buffer;
    }
}

static void
ata_raid_via_print_meta(struct via_raid_conf *meta)
{
    int i;
  
    kprintf("*************** ATA VIA Metadata ****************\n");
    kprintf("magic               0x%02x\n", meta->magic);
    kprintf("dummy_0             0x%02x\n", meta->dummy_0);
    kprintf("type                %s\n",
	   ata_raid_via_type(meta->type & VIA_T_MASK));
    kprintf("bootable            %d\n", meta->type & VIA_T_BOOTABLE);
    kprintf("unknown             %d\n", meta->type & VIA_T_UNKNOWN);
    kprintf("disk_index          0x%02x\n", meta->disk_index);
    kprintf("stripe_layout       0x%02x\n", meta->stripe_layout);
    kprintf(" stripe_disks       %d\n", meta->stripe_layout & VIA_L_DISKS);
    kprintf(" stripe_sectors     %d\n",
	   0x08 << ((meta->stripe_layout & VIA_L_MASK) >> VIA_L_SHIFT));
    kprintf("disk_sectors        %ju\n", meta->disk_sectors);
    kprintf("disk_id             0x%08x\n", meta->disk_id);
    kprintf("DISK#   disk_id\n");
    for (i = 0; i < 8; i++) {
	if (meta->disks[i])
	    kprintf("  %d    0x%08x\n", i, meta->disks[i]);
    }    
    kprintf("checksum            0x%02x\n", meta->checksum);
    kprintf("=================================================\n");
}
