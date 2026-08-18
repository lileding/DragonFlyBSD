/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86-64 platform data prepared by vmmfs before its loader runs.  The region
 * is part of the vmmfs platform ABI; loaders must not overwrite it and must
 * publish its RSDP address through their guest boot protocol.
 */
#ifndef VMMFS_PLATFORM_X64_H
#define VMMFS_PLATFORM_X64_H

#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct vmmfs_machine;
struct vmmfs_memory;
struct vmmfs_serialroot;

#define VMMFS_PLATFORM_X64_ACPI_GPA 0x70000ULL
#define VMMFS_PLATFORM_X64_ACPI_SIZE (64ULL * 1024ULL)
#define VMMFS_PLATFORM_X64_RSDP_GPA VMMFS_PLATFORM_X64_ACPI_GPA

/* Runtime x86 platform PIO state owned by one vmmfs machine. */
struct vmmfs_platform_x64 {
	struct vmmfs_machine *machine;
	vmm_machine_t runtime_machine;
	uint64_t tsc_base;
	vmm_io_t delay_read;
	vmm_io_t delay_write;
	vmm_io_t acpi_read;
	vmm_io_t acpi_write;
	vmm_io_t fch_pm_read;
	vmm_io_t fch_pm_write;
	vmm_io_t fallback_read;
	vmm_io_t fallback_write;
};

int vmmfs_platform_x64_create(struct vmmfs_machine *,
	struct vmmfs_platform_x64 *);
int vmmfs_platform_x64_destroy(struct vmmfs_platform_x64 *);
int vmmfs_platform_x64_prepare(struct vmmfs_platform_x64 *,
	struct vmmfs_memory *, uint32_t,
	struct vmmfs_serialroot *);
int vmmfs_platform_x64_start(struct vmmfs_platform_x64 *, vmm_machine_t);
int vmmfs_platform_x64_stop(struct vmmfs_platform_x64 *);

#endif /* VMMFS_PLATFORM_X64_H */
