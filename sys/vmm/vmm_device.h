/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One vPCIe function.  PCIe fabric ownership and attachment live in
 * vmm_pcie.c; vmmfs only presents this core object.
 */
#ifndef VMM_DEVICE_H
#define VMM_DEVICE_H

#include <sys/tree.h>
#include <sys/types.h>

#include "vmm_pcie_bar.h"
#include "vmm_pcie_abi.h"

#define VMM_DEVICE_NAME_MAX	63

struct vmm_pcie;
struct vmm_pcie_config;
struct vmm_pcie_root;
struct vmm_pcie_user;
struct vmm_dma_cap;
struct vmm_pcie_event;

struct vmm_device {
	/* The fabric token_registry protects every mut_ field below. */
	char			imm_name[VMM_DEVICE_NAME_MAX + 1];
	size_t			imm_name_len;
	uint64_t		imm_id;
	struct vmm_pcie		*borrow_imm_pcie;
	struct vmm_pcie_root	*borrow_mut_root;
	struct vmm_pcie_user	*borrow_mut_provider;
	struct vmm_pcie_user	*borrow_mut_offload;
	struct vmm_dma_cap	*own_mut_dma_cap;
	struct vmm_pcie_event	*own_mut_event;
	uint32_t		mut_bdf;
	uint64_t		mut_attachment_generation;
	uint64_t		mut_run_generation;
	int			mut_stop_requested;
	uint16_t		mut_vendor_id;
	uint16_t		mut_device_id;
	uint16_t		mut_subsystem_vendor_id;
	uint16_t		mut_subsystem_device_id;
	uint32_t		mut_class_code;
	uint16_t		mut_msix_vectors;
	uint8_t			mut_revision;
	int			mut_registered;
	struct vmm_pcie_config	*own_mut_config;
	struct vmm_pcie_bar	own_mut_bars[VMM_PCIE_ABI_MAX_BARS];
	struct vmm_pcie_abi_bar_range
			own_mut_bar_range[VMM_PCIE_ABI_MAX_BAR_RANGES];
	unsigned int		mut_bar_range_count;
	uint64_t		mut_bar_gpa[VMM_PCIE_ABI_MAX_BARS];
	RB_ENTRY(vmm_device)	own_mut_registry_entry;
};

void	vmm_device_init(struct vmm_device *device, const char *name, int nlen,
	    uint64_t id, struct vmm_pcie *pcie, struct vmm_pcie_root *consumer,
	    uint32_t bdf);
void	vmm_device_uninit(struct vmm_device *device);
int	vmm_device_name_eq(const struct vmm_device *device, const char *name,
	    int nlen);

#endif /* VMM_DEVICE_H */
