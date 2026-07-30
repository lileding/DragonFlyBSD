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

#define VMM_DEVICE_NAME_MAX	63

struct vmm_pcie;
struct vmm_pcie_root;

enum vmm_device_state {
	VMM_DEVICE_NEW,
	VMM_DEVICE_REGISTERED,
	VMM_DEVICE_FAILED,
};

struct vmm_device {
	/* The fabric token_registry protects every mut_ field below. */
	char			imm_name[VMM_DEVICE_NAME_MAX + 1];
	size_t			imm_name_len;
	uint64_t		imm_id;
	struct vmm_pcie		*borrow_imm_pcie;
	struct vmm_pcie_root	*borrow_mut_consumer;
	uint32_t		mut_bdf;
	enum vmm_device_state	mut_state;
	RB_ENTRY(vmm_device)	own_mut_registry_entry;
};

void	vmm_device_init(struct vmm_device *device, const char *name, int nlen,
	    uint64_t id, struct vmm_pcie *pcie, struct vmm_pcie_root *consumer,
	    uint32_t bdf);
void	vmm_device_uninit(struct vmm_device *device);
int	vmm_device_name_eq(const struct vmm_device *device, const char *name,
	    int nlen);

#endif /* VMM_DEVICE_H */
