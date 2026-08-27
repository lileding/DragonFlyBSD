/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMMFS x64 platform data prepared before its loader runs.  This is the sole
 * VMMFS x64 platform ABI: Cloud Hypervisor's BAR/MMIO [0xc0000000,
 * 0xe8000000), ECAM [0xe8000000, 0xf8000000), and high-RAM-at-4-GiB layout.
 * Direct Linux/DragonFly loaders and a CloudHV PVH firmware consume one
 * machine.  Loaders must not overwrite this region and must publish its RSDP
 * address through their guest boot protocol.
 */
#ifndef VMMFS_PLATFORM_X64_H
#define VMMFS_PLATFORM_X64_H

#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct vmmfs_memory;
struct vmmfs_pciroot;
struct vmmfs_serialroot;

#define VMMFS_PLATFORM_X64_ACPI_GPA 0x70000ULL
#define VMMFS_PLATFORM_X64_ACPI_SIZE (64ULL * 1024ULL)
#define VMMFS_PLATFORM_X64_RSDP_GPA VMMFS_PLATFORM_X64_ACPI_GPA

/* Runtime x86 platform PIO state owned by one vmmfs machine. */
struct vmmfs_platform_x64 {
	struct vmmfs_machine *machine;
	struct lwkt_token token;
	vmm_machine_t runtime_machine;
	uint64_t tsc_base;
	vmm_io_t delay_read;
	vmm_io_t delay_write;
	vmm_io_t power_read;
	vmm_io_t power_write;
	vmm_io_t timer_read;
	vmm_io_t fch_pm_read;
	vmm_io_t fch_pm_write;
	vmm_io_t fallback_read;
	vmm_io_t fallback_write;
};

int vmmfs_platform_x64_init(struct vmmfs_machine *,
	struct vmmfs_platform_x64 *);
int vmmfs_platform_x64_fini(struct vmmfs_platform_x64 *);
int vmmfs_platform_x64_prepare(struct vmmfs_platform_x64 *,
	struct vmmfs_memory *, uint32_t,
	struct vmmfs_pciroot *,
	struct vmmfs_serialroot *);
int vmmfs_platform_x64_start(struct vmmfs_platform_x64 *, vmm_machine_t);
int vmmfs_platform_x64_stop(struct vmmfs_platform_x64 *);

#endif /* VMMFS_PLATFORM_X64_H */
