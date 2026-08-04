/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86 platform data owned by vmm core, not by an fd4 loader.
 */
#ifndef VMM_PLATFORM_X86_H
#define VMM_PLATFORM_X86_H

#define VMM_PLATFORM_X86_ACPI_GPA	0x70000ULL
#define VMM_PLATFORM_X86_ACPI_SIZE	(2ULL * 4096ULL)

struct vmm_launch;
struct vmm_mem;

/* Write the immutable ACPI platform and derive launch topology from config. */
int	vmm_platform_x86_prepare(struct vmm_mem *mem, uint32_t vcpu_count,
	    struct vmm_launch *launch);

#endif /* VMM_PLATFORM_X86_H */
