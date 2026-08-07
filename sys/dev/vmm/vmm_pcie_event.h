/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One revocable, bidirectional provider event capability.
 */
#ifndef VMM_PCIE_EVENT_H
#define VMM_PCIE_EVENT_H

#include <sys/types.h>

struct file;
struct vmm_pcie;
struct vmm_pcie_event;
struct vmm_pcie_user;

int	vmm_pcie_event_create(struct vmm_pcie_event **,
	    struct vmm_pcie *, struct vmm_pcie_user *, uint64_t);
struct file *vmm_pcie_event_file_hold(struct vmm_pcie_event *);
void	vmm_pcie_event_hold(struct vmm_pcie_event *);
void	vmm_pcie_event_release(struct vmm_pcie_event *);
void	vmm_pcie_event_signal(struct vmm_pcie_event *);
void	vmm_pcie_event_revoke(struct vmm_pcie_event *);
void	vmm_pcie_event_destroy(struct vmm_pcie_event *);
int	vmm_pcie_event_active(void);
struct vmm_pcie *vmm_pcie_event_pcie(const struct vmm_pcie_event *);
struct vmm_pcie_user *vmm_pcie_event_provider(const struct vmm_pcie_event *);
uint64_t vmm_pcie_event_device_id(const struct vmm_pcie_event *);

#endif /* VMM_PCIE_EVENT_H */
