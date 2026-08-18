/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs x86 CMOS/RTC platform device.
 */
#ifndef VMMFS_RTC_H
#define VMMFS_RTC_H

#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;

/*
 * This is an internal x86 platform device.  It owns the CMOS PIO traps while
 * its parent machine runs; no vnode exposes it directly.
 */
struct vmmfs_rtc {
	struct vmmfs_machine *machine;
	vmm_machine_t runtime_machine;
	uint8_t index;
	uint8_t nmi_disabled;
	uint8_t register_a;
	uint8_t register_b;
	uint8_t register_c;
	uint8_t ram[128];
	vmm_io_t read_io;
	vmm_io_t write_io;
};

int vmmfs_rtc_create(struct vmmfs_machine *, struct vmmfs_rtc *);
int vmmfs_rtc_destroy(struct vmmfs_rtc *);
int vmmfs_rtc_start(struct vmmfs_rtc *, vmm_machine_t);
int vmmfs_rtc_stop(struct vmmfs_rtc *);

#endif /* VMMFS_RTC_H */
