/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * PCIe consumer root -- see vmm_pcie_root.h.
 */
#include <sys/types.h>

#include "vmm_pcie_abi.h"
#include "vmm_pcie_root.h"

void
vmm_pcie_root_init(struct vmm_pcie_root *root, struct vmm_pcie *pcie,
    struct vmm_machine *machine)
{
	root->borrow_imm_pcie = pcie;
	root->borrow_imm_machine = machine;
	root->mut_bdf_mask = VMM_PCIE_ROOT_BDF_RESERVED;
}

void
vmm_pcie_root_uninit(struct vmm_pcie_root *root)
{
	root->borrow_imm_pcie = 0;
	root->borrow_imm_machine = 0;
	root->mut_bdf_mask = 0;
}

uint32_t
vmm_pcie_root_bdf_alloc_locked(struct vmm_pcie_root *root)
{
	unsigned int device;

	for (device = 1; device < VMM_PCIE_ROOT_BDF_COUNT; device++) {
		uint32_t bit = 1U << device;

		if ((root->mut_bdf_mask & bit) != 0)
			continue;
		root->mut_bdf_mask |= bit;
		return VMM_PCIE_ABI_BDF(0, device, 0);
	}
	return 0;
}

void
vmm_pcie_root_bdf_release_locked(struct vmm_pcie_root *root, uint32_t bdf)
{
	unsigned int device;

	device = (bdf >> VMM_PCIE_ABI_BDF_DEVICE_SHIFT) &
	    VMM_PCIE_ABI_BDF_DEVICE_MASK;
	if (device == 0 || device >= VMM_PCIE_ROOT_BDF_COUNT)
		return;
	root->mut_bdf_mask &= ~(1U << device);
}
