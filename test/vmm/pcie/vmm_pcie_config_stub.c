/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Link substitute for relation tests that do not exercise provider registration.
 */
#include <sys/errno.h>

#include "vmm_pcie_config.h"

int
vmm_pcie_config_create(struct vmm_pcie_config **configp,
    const struct vmm_pcie_abi_register *request)
{

	(void)configp;
	(void)request;
	return EOPNOTSUPP;
}

void
vmm_pcie_config_destroy(struct vmm_pcie_config *config)
{

	(void)config;
}

int
vmm_pcie_config_access_locked(struct vmm_pcie_config *config,
    unsigned int offset, int write, int size, uint64_t *valuep)
{

	(void)config;
	(void)offset;
	(void)write;
	(void)size;
	(void)valuep;
	return EOPNOTSUPP;
}
