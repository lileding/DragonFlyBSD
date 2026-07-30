/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pure core tests for PCIe root attachment and BDF allocation.
 */
#include <sys/errno.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmm_pcie.h"

static void
expect_result(const char *name, int got, int want)
{
	if (got != want)
		errx(1, "%s: got %d want %d", name, got, want);
}

static void
expect_bdf(const char *name, const struct vmm_device *device, uint32_t want)
{
	uint32_t got;

	got = vmm_pcie_device_bdf(device);
	if (got != want)
		errx(1, "%s: got %#x want %#x", name, got, want);
}

int
main(void)
{
	struct vmm_pcie pcie;
	struct vmm_pcie_root root_a;
	struct vmm_pcie_root root_b;
	struct vmm_device host0;
	struct vmm_device host1;
	struct vmm_device a0;
	struct vmm_device duplicate;
	struct vmm_device capacity[VMM_PCIE_ROOT_BDF_COUNT - 3];
	struct vmm_device overflow;
	struct vmm_pcie_root *host_root;
	char name[VMM_DEVICE_NAME_MAX + 1];
	unsigned int i;

	memset(&pcie, 0, sizeof(pcie));
	memset(&root_a, 0, sizeof(root_a));
	memset(&root_b, 0, sizeof(root_b));
	memset(&host0, 0, sizeof(host0));
	memset(&host1, 0, sizeof(host1));
	memset(&a0, 0, sizeof(a0));
	memset(&duplicate, 0, sizeof(duplicate));
	memset(capacity, 0, sizeof(capacity));
	memset(&overflow, 0, sizeof(overflow));

	vmm_pcie_init(&pcie);
	host_root = vmm_pcie_host_root(&pcie);
	vmm_pcie_root_init(&root_a, &pcie,
	    (struct vmm_machine *)(uintptr_t)1);
	vmm_pcie_root_init(&root_b, &pcie,
	    (struct vmm_machine *)(uintptr_t)2);

	expect_result("create host0", vmm_pcie_device_create(&pcie, host_root,
	    "host0", 5, &host0), 0);
	expect_bdf("host0 bdf", &host0, VMM_PCIE_ABI_BDF(0, 1, 0));

	expect_result("create a0", vmm_pcie_device_create(&pcie, &root_a,
	    "a0", 2, &a0), 0);
	expect_bdf("a0 bdf", &a0, VMM_PCIE_ABI_BDF(0, 1, 0));

	expect_result("duplicate global name", vmm_pcie_device_create(&pcie,
	    &root_b, "a0", 2, &duplicate), EEXIST);

	expect_result("move host0 to root a", vmm_pcie_device_move(&pcie,
	    &host0, &root_a), 0);
	expect_bdf("moved host0 bdf", &host0, VMM_PCIE_ABI_BDF(0, 2, 0));
	if (vmm_pcie_device_find(&pcie, &root_a, "host0", 5) != &host0)
		errx(1, "moved host0 is absent from root a");
	if (vmm_pcie_device_find(&pcie, host_root, "host0", 5) != NULL)
		errx(1, "moved host0 remains in host root");

	expect_result("create host1", vmm_pcie_device_create(&pcie, host_root,
	    "host1", 5, &host1), 0);
	expect_bdf("released host bdf", &host1, VMM_PCIE_ABI_BDF(0, 1, 0));

	for (i = 0; i < sizeof(capacity) / sizeof(capacity[0]); i++) {
		(void)snprintf(name, sizeof(name), "capacity%u", i);
		expect_result("fill root a", vmm_pcie_device_create(&pcie, &root_a,
		    name, (int)strlen(name), &capacity[i]), 0);
	}
	expect_result("root a full", vmm_pcie_device_create(&pcie, &root_a,
	    "overflow", 8, &overflow), ENOSPC);

	for (i = 0; i < sizeof(capacity) / sizeof(capacity[0]); i++)
		expect_result("destroy capacity", vmm_pcie_device_destroy(&pcie,
		    &capacity[i]), 0);
	expect_result("destroy host1", vmm_pcie_device_destroy(&pcie, &host1), 0);
	expect_result("destroy host0", vmm_pcie_device_destroy(&pcie, &host0), 0);
	expect_result("destroy a0", vmm_pcie_device_destroy(&pcie, &a0), 0);
	vmm_pcie_root_uninit(&root_b);
	vmm_pcie_root_uninit(&root_a);
	vmm_pcie_uninit(&pcie);
	return 0;
}
