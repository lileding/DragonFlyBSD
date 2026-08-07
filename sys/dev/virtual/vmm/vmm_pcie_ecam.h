/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One-segment, bus-0 PCIe enhanced configuration access mechanism.
 */
#ifndef VMM_PCIE_ECAM_H
#define VMM_PCIE_ECAM_H

#include <sys/types.h>

#include "vmm_pcie_layout.h"

struct vmm_pcie_root;

int	vmm_pcie_ecam_access(struct vmm_pcie_root *root, uint64_t gpa,
	    int write, int size, uint64_t *valuep);

#endif /* VMM_PCIE_ECAM_H */
