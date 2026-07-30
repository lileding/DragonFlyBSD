/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One pathless shared-memory BAR capability.
 */
#ifndef VMM_PCIE_BAR_H
#define VMM_PCIE_BAR_H

#include <sys/types.h>

struct file;

struct vmm_pcie_bar {
	/* The device owns this file reference until provider detach. */
	struct file	*own_mut_fp;
	uint64_t	imm_size;
	uint32_t	imm_flags;
};

int	vmm_pcie_bar_create(struct vmm_pcie_bar *bar, uint64_t size,
	    uint32_t flags);
void	vmm_pcie_bar_destroy(struct vmm_pcie_bar *bar);
int	vmm_pcie_bar_mmap_active(void);

#endif /* VMM_PCIE_BAR_H */
