/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Standard PCIe configuration header for one registered function.
 */
#ifndef VMM_PCIE_CONFIG_H
#define VMM_PCIE_CONFIG_H

#include <sys/endian.h>
#include <sys/types.h>

#include "vmm_pcie_abi.h"

#define VMM_PCIE_CONFIG_SPACE_SIZE	4096U
#define VMM_PCIE_CONFIG_HEADER_SIZE	256U

struct vmm_pcie_config {
	/* The parent fabric token_registry protects every mut_ field below. */
	uint8_t			own_imm_reset_bytes[VMM_PCIE_CONFIG_HEADER_SIZE];
	uint8_t			own_mut_bytes[VMM_PCIE_CONFIG_HEADER_SIZE];
	uint64_t		imm_bar_size[VMM_PCIE_ABI_MAX_BARS];
	uint32_t		imm_bar_flags[VMM_PCIE_ABI_MAX_BARS];
	uint8_t			mut_bar_probe[VMM_PCIE_ABI_MAX_BARS];
};

int	vmm_pcie_config_create(struct vmm_pcie_config **configp,
	    const struct vmm_pcie_abi_register *request);
void	vmm_pcie_config_destroy(struct vmm_pcie_config *config);
void	vmm_pcie_config_reset_locked(struct vmm_pcie_config *config);
int	vmm_pcie_config_access_locked(struct vmm_pcie_config *config,
	    unsigned int offset, int write, int size, uint64_t *valuep);
int	vmm_pcie_config_bar_decode_locked(const struct vmm_pcie_config *config,
	    unsigned int index, uint64_t *gpap, uint64_t *sizep);
int	vmm_pcie_config_msix_enabled_locked(const struct vmm_pcie_config *config);
int	vmm_pcie_config_msix_function_masked_locked(
	    const struct vmm_pcie_config *config);
int	vmm_pcie_config_msix_vector_masked(uint32_t vector_control);

#endif /* VMM_PCIE_CONFIG_H */
