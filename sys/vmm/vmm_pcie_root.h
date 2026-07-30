/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One consumer-local PCIe root and its bus-0 BDF allocator.
 */
#ifndef VMM_PCIE_ROOT_H
#define VMM_PCIE_ROOT_H

#include <sys/types.h>

#define VMM_PCIE_ROOT_BDF_COUNT		32U
#define VMM_PCIE_ROOT_BDF_RESERVED	0x00000001U

struct vmm_machine;
struct vmm_pcie;

struct vmm_pcie_root {
	/* The parent fabric token_registry protects mut_bdf_mask. */
	struct vmm_pcie		*borrow_imm_pcie;
	struct vmm_machine	*borrow_imm_machine;
	uint64_t		imm_id;
	uint32_t		mut_bdf_mask;
};

void	vmm_pcie_root_init(struct vmm_pcie_root *root,
	    struct vmm_pcie *pcie, struct vmm_machine *machine);
void	vmm_pcie_root_uninit(struct vmm_pcie_root *root);
uint32_t vmm_pcie_root_bdf_alloc_locked(struct vmm_pcie_root *root);
void	vmm_pcie_root_bdf_release_locked(struct vmm_pcie_root *root,
	    uint32_t bdf);

#endif /* VMM_PCIE_ROOT_H */
