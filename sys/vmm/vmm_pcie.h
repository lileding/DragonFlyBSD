/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Core vPCIe fabric: one registry and host root per VMM owner.
 */
#ifndef VMM_PCIE_H
#define VMM_PCIE_H

#include <sys/thread.h>
#include <sys/tree.h>
#include <sys/types.h>

#include "vmm_device.h"
#include "vmm_pcie_abi.h"
#include "vmm_pcie_root.h"

RB_HEAD(vmm_pcie_device_tree, vmm_device);
RB_PROTOTYPE(vmm_pcie_device_tree, vmm_device, own_mut_registry_entry,
    vmm_pcie_device_cmp);

struct vmm_pcie {
	/* token_registry protects mut_devices, mut_next_device_id, every root BDF
	 * allocator, and every device mut_ attachment field. */
	struct lwkt_token		token_registry;
	struct vmm_pcie_root	own_mut_host_root;
	struct vmm_pcie_device_tree	mut_devices;
	uint64_t			mut_next_device_id;
};

void	vmm_pcie_init(struct vmm_pcie *pcie);
void	vmm_pcie_uninit(struct vmm_pcie *pcie);
struct vmm_pcie_root *vmm_pcie_host_root(struct vmm_pcie *pcie);
int	vmm_pcie_device_create(struct vmm_pcie *pcie,
	    struct vmm_pcie_root *consumer, const char *name, int nlen,
	    struct vmm_device *device);
int	vmm_pcie_device_destroy(struct vmm_pcie *pcie,
	    struct vmm_device *device);
int	vmm_pcie_device_move(struct vmm_pcie *pcie,
	    struct vmm_device *device, struct vmm_pcie_root *consumer);
struct vmm_device *vmm_pcie_device_find(struct vmm_pcie *pcie,
	    const struct vmm_pcie_root *consumer, const char *name, int nlen);
struct vmm_device *vmm_pcie_device_find_name(struct vmm_pcie *pcie,
	    const char *name, int nlen);
int	vmm_pcie_device_attached_to(const struct vmm_device *device,
	    const struct vmm_pcie_root *consumer);
struct vmm_pcie_root *vmm_pcie_device_consumer(
	    const struct vmm_device *device);
uint32_t vmm_pcie_device_bdf(const struct vmm_device *device);

#endif /* VMM_PCIE_H */
