/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pure core tests for PCIe root BDF allocation.
 */
#include <sys/errno.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmm_pcie_abi.h"
#include "vmm_pcie_root.h"

static void
expect_result(const char *name, int got, int want)
{
	if (got != want)
		errx(1, "%s: got %d want %d", name, got, want);
}

uint64_t
vmm_pcie_root_id_alloc(struct vmm_pcie *pcie)
{

	(void)pcie;
	return 1;
}

int
main(void)
{
	struct vmm_pcie_root root;
	uint32_t bdf;
	unsigned int i;

	memset(&root, 0, sizeof(root));
	root.mut_bdf_mask = VMM_PCIE_ROOT_BDF_RESERVED;

	for (i = 1; i < VMM_PCIE_ROOT_BDF_COUNT; i++) {
		bdf = vmm_pcie_root_bdf_alloc_locked(&root);
		if (bdf != VMM_PCIE_ABI_BDF(0, i, 0))
			errx(1, "BDF %u: got %#x", i, bdf);
	}
	expect_result("root full", vmm_pcie_root_bdf_alloc_locked(&root), 0);

	vmm_pcie_root_bdf_release_locked(&root, VMM_PCIE_ABI_BDF(0, 7, 0));
	expect_result("released BDF", vmm_pcie_root_bdf_alloc_locked(&root),
	    VMM_PCIE_ABI_BDF(0, 7, 0));
	vmm_pcie_root_bdf_release_locked(&root, VMM_PCIE_ABI_BDF(0, 0, 0));
	vmm_pcie_root_bdf_release_locked(&root,
	    VMM_PCIE_ABI_BDF(0, VMM_PCIE_ROOT_BDF_COUNT, 0));
	if ((root.mut_bdf_mask & VMM_PCIE_ROOT_BDF_RESERVED) == 0)
		errx(1, "reserved BDF was released");
	return 0;
}
