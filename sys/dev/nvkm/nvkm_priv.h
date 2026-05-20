/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Internal definitions shared across nvkm translation units.
 */

#ifndef _NVKM_PRIV_H_
#define _NVKM_PRIV_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/lock.h>

#define NVKM_PCI_VENDOR_NVIDIA	0x10de

/* Initial supported device. Phase 0 targets only TU102. */
#define NVKM_PCI_DEVICE_TU102	0x1e07

/* NVIDIA Master Control: chip identification register. */
#define NV_PMC_BOOT_0		0x00000000

#define NVKM_NUM_BARS		6

struct nvkm_softc {
	device_t		dev;

	int			bar_rid[NVKM_NUM_BARS];
	struct resource		*bar_res[NVKM_NUM_BARS];
};

#endif /* _NVKM_PRIV_H_ */
