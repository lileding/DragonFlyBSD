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

struct vmmfs_memory;
struct vmmfs_serialroot;

#define VMMFS_PLATFORM_X64_ACPI_GPA 0x70000ULL
#define VMMFS_PLATFORM_X64_ACPI_SIZE (64ULL * 1024ULL)
#define VMMFS_PLATFORM_X64_RSDP_GPA VMMFS_PLATFORM_X64_ACPI_GPA

int vmmfs_platform_x64_prepare(struct vmmfs_memory *, uint32_t,
	struct vmmfs_serialroot *);

#endif /* VMMFS_PLATFORM_X64_H */
