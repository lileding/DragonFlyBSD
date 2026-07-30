/* SPDX-License-Identifier: BSD-2-Clause */
#include <sys/errno.h>

#include "vmm_pcie_bar.h"

int
vmm_pcie_bar_create(struct vmm_pcie_bar *bar, uint64_t size, uint32_t flags)
{

	(void)bar;
	(void)size;
	(void)flags;
	return EOPNOTSUPP;
}

void
vmm_pcie_bar_destroy(struct vmm_pcie_bar *bar)
{

	(void)bar;
}

int
vmm_pcie_bar_mmap_active(void)
{

	return 0;
}
