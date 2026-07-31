/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pure configuration-space tests for one vPCIe provider function.
 */
#include <sys/endian.h>
#include <sys/errno.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmm_pcie_config.h"

static void expect_result(const char *name, int got, int want);
static void expect_value(const char *name, uint64_t got, uint64_t want);
static uint64_t config_read(struct vmm_pcie_config *config,
    unsigned int offset, int size);
static void config_write(struct vmm_pcie_config *config, unsigned int offset,
    int size, uint64_t value);
static void config_decode(struct vmm_pcie_config *config, unsigned int index,
    int want, uint64_t want_gpa, uint64_t want_size);

int
main(void)
{
	struct vmm_pcie_abi_register request;
	struct vmm_pcie_config *config;
	uint64_t value;

	memset(&request, 0, sizeof(request));
	request.le_vendor_id = htole16(0x1b36);
	request.le_device_id = htole16(0xdf01);
	request.le_subsystem_vendor_id = htole16(0x1b36);
	request.le_subsystem_device_id = htole16(0xdf01);
	request.le_class_code = htole32(0xff0000);
	request.revision = 3;
	request.le_msix_vectors = htole16(4);
	request.bar[0].le_size = htole64(0x1000);
	request.bar[0].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY);
	config = NULL;
	expect_result("create", vmm_pcie_config_create(&config, &request), 0);
	expect_value("vendor", config_read(config, 0x00, 2), 0x1b36);
	expect_value("device", config_read(config, 0x02, 2), 0xdf01);
	expect_value("status", config_read(config, 0x06, 2), 0x0010);
	expect_value("cap pointer", config_read(config, 0x34, 1), 0x50);
	expect_value("pcie cap", config_read(config, 0x50, 2), 0x7010);
	expect_value("msix cap", config_read(config, 0x70, 4), 0x00030011);
	expect_value("msix table", config_read(config, 0x74, 4), 0);
	expect_value("msix pba", config_read(config, 0x78, 4), 0x40);
	config_write(config, 0x72, 2, 0xffffU);
	expect_value("msix control mask", config_read(config, 0x72, 2),
	    0xc003U);
	if (!vmm_pcie_config_msix_enabled_locked(config) ||
	    !vmm_pcie_config_msix_function_masked_locked(config))
		err(1, "msix enable/function mask");
	config_write(config, 0x73, 1, 0);
	expect_value("msix control enable clear", config_read(config, 0x72, 2),
	    0x0003U);
	if (vmm_pcie_config_msix_enabled_locked(config) ||
	    vmm_pcie_config_msix_function_masked_locked(config))
		err(1, "msix control clear");
	if (!vmm_pcie_config_msix_vector_masked(1) ||
	    vmm_pcie_config_msix_vector_masked(0))
		err(1, "msix vector mask");
	expect_value("bar initial", config_read(config, 0x10, 4), 0);
	config_write(config, 0x10, 4, 0xffffffffU);
	expect_value("bar sizing", config_read(config, 0x10, 4), 0xfffff000U);
	config_write(config, 0x10, 4, 0xc0001000U);
	expect_value("bar programmed", config_read(config, 0x10, 4),
	    0xc0001000U);
	config_decode(config, 0, ENOENT, 0, 0);
	config_write(config, 0x04, 4, 0xffffffffU);
	expect_value("command mask", config_read(config, 0x04, 4),
	    0x00100007U);
	config_decode(config, 0, 0, 0xc0001000U, 0x1000U);
	vmm_pcie_config_reset_locked(config);
	expect_value("bar reset", config_read(config, 0x10, 4), 0);
	config_decode(config, 0, ENOENT, 0, 0);
	expect_value("extended read", config_read(config, 0x100, 4), 0);
	value = 0;
	expect_result("unaligned access", vmm_pcie_config_access_locked(config,
	    0x01, 0, 2, &value), EINVAL);
	vmm_pcie_config_destroy(config);
	request.le_msix_vectors = htole16(VMM_PCIE_ABI_MAX_MSIX_VECTORS);
	request.bar[0].le_size = htole64(0x8000);
	expect_result("max msix bar too small", vmm_pcie_config_create(&config,
	    &request), EINVAL);
	request.bar[0].le_size = htole64(0x10000);
	expect_result("max msix bar", vmm_pcie_config_create(&config, &request),
	    0);
	vmm_pcie_config_destroy(config);
	return 0;
}

static void
expect_result(const char *name, int got, int want)
{

	if (got != want)
		errx(1, "%s: got %d want %d", name, got, want);
}

static void
expect_value(const char *name, uint64_t got, uint64_t want)
{

	if (got != want)
		errx(1, "%s: got 0x%jx want 0x%jx", name,
		    (uintmax_t)got, (uintmax_t)want);
}

static uint64_t
config_read(struct vmm_pcie_config *config, unsigned int offset, int size)
{
	uint64_t value;

	value = 0;
	expect_result("config read", vmm_pcie_config_access_locked(config,
	    offset, 0, size, &value), 0);
	return value;
}

static void
config_write(struct vmm_pcie_config *config, unsigned int offset, int size,
    uint64_t value)
{

	expect_result("config write", vmm_pcie_config_access_locked(config,
	    offset, 1, size, &value), 0);
}

static void
config_decode(struct vmm_pcie_config *config, unsigned int index, int want,
    uint64_t want_gpa, uint64_t want_size)
{
	uint64_t gpa;
	uint64_t size;

	gpa = 0;
	size = 0;
	expect_result("bar decode", vmm_pcie_config_bar_decode_locked(config,
	    index, &gpa, &size), want);
	if (want == 0) {
		expect_value("bar decode gpa", gpa, want_gpa);
		expect_value("bar decode size", size, want_size);
	}
}
