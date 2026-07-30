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
	 * allocator, mut_closing, and every device mut_ attachment field. */
	struct lwkt_token		token_registry;
	struct vmm_pcie_root	own_mut_host_root;
	struct vmm_pcie_device_tree	mut_devices;
	uint64_t			mut_next_device_id;
	uint64_t			mut_next_root_id;
	int			mut_closing;
};

void	vmm_pcie_init(struct vmm_pcie *pcie);
void	vmm_pcie_uninit(struct vmm_pcie *pcie);
int	vmm_pcie_begin_shutdown(struct vmm_pcie *pcie);
void	vmm_pcie_cancel_shutdown(struct vmm_pcie *pcie);
uint64_t vmm_pcie_root_id_alloc(struct vmm_pcie *pcie);
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
int	vmm_pcie_device_at_root(const struct vmm_device *device,
	    const struct vmm_pcie_root *root);
struct vmm_pcie_root *vmm_pcie_device_root(
	    const struct vmm_device *device);
uint32_t vmm_pcie_device_bdf(const struct vmm_device *device);
int	vmm_pcie_device_provider_attach(struct vmm_device *device,
	    struct vmm_pcie_user *provider);
int	vmm_pcie_device_provider_register(struct vmm_device *device,
	    struct vmm_pcie_user *provider,
	    const struct vmm_pcie_abi_register *request,
	    struct vmm_pcie_abi_registered *response,
	    struct file **bar_fps, unsigned int *bar_countp);
void	vmm_pcie_device_provider_detach(struct vmm_device *device,
	    struct vmm_pcie_user *provider);
int	vmm_pcie_device_consumer_attach(struct vmm_device *device,
	    struct vmm_pcie_user *consumer,
	    struct vmm_pcie_abi_consumer_ready *response);
void	vmm_pcie_device_consumer_detach(struct vmm_device *device,
	    struct vmm_pcie_user *consumer);

#endif /* VMM_PCIE_H */
