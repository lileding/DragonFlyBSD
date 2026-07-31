/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ECAM dispatch for the vPCIe fabric -- see vmm_pcie_ecam.h.
 */
#include <sys/errno.h>
#include <sys/types.h>

#include "vmm_device.h"
#include "vmm_pcie.h"
#include "vmm_pcie_config.h"
#include "vmm_pcie_ecam.h"
#include "vmm_pcie_root.h"

int
vmm_pcie_ecam_access(struct vmm_pcie_root *root, uint64_t gpa,
    int write, int size, uint64_t *valuep)
{
	struct vmm_device *device;
	struct vmm_pcie_bar_mapping mappings[VMM_PCIE_ABI_MAX_BARS];
	struct vmm_pcie *pcie;
	uint64_t offset;
	uint32_t bdf;
	uint64_t absent;
	int error;

	if (root == NULL || valuep == NULL || root->borrow_imm_pcie == NULL ||
	    (size != 1 && size != 2 && size != 4) ||
	    gpa < VMM_PCIE_ECAM_BASE ||
	    gpa - VMM_PCIE_ECAM_BASE > VMM_PCIE_ECAM_SIZE - (uint64_t)size)
		return EINVAL;
	offset = gpa - VMM_PCIE_ECAM_BASE;
	__builtin_memset(mappings, 0, sizeof(mappings));
	bdf = VMM_PCIE_ABI_BDF((offset >> 20) & 0xffU,
	    (offset >> 15) & 0x1fU, (offset >> 12) & 0x7U);
	pcie = root->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
		if (device->borrow_mut_root == root && device->mut_bdf == bdf)
			break;
	}
	if (device == NULL || !device->mut_registered ||
	    device->own_mut_config == NULL) {
		if (!write) {
			absent = size == 1 ? 0xffU : size == 2 ? 0xffffU :
			    0xffffffffU;
			*valuep = absent;
		}
		error = 0;
	} else {
		error = vmm_pcie_config_access_locked(device->own_mut_config,
		    (unsigned int)(offset &
		    (VMM_PCIE_CONFIG_SPACE_SIZE - 1U)), write, size, valuep);
		if (error == 0 && write)
			vmm_pcie_device_bar_mappings_take_locked(device, mappings);
	}
	lwkt_reltoken(&pcie->token_registry);
	if (write)
		vmm_pcie_root_bar_mappings_unmap(root, mappings);
	return error;
}
