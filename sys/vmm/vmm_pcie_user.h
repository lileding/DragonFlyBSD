/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open vPCIe provider or consumer socket session.
 */
#ifndef VMM_PCIE_USER_H
#define VMM_PCIE_USER_H

#include <sys/types.h>

struct socket;
struct ucred;
struct vmm_device;

enum vmm_pcie_user_role {
	VMM_PCIE_USER_PROVIDER,
	VMM_PCIE_USER_CONSUMER,
};

struct vmm_pcie_user;
struct vmm_pcie_abi_start;
struct vmm_pcie_abi_stop;
struct file;

/* Active socket sessions veto module unload after their vnode has gone away. */
extern volatile u_int vmm_pcie_user_session_count;

int	vmm_pcie_user_open(struct vmm_device *device,
	    enum vmm_pcie_user_role role, struct ucred *cred,
	    struct socket **user_socketp);
/* Caller holds the parent fabric token_registry. */
int	vmm_pcie_user_force_close(struct vmm_pcie_user *user);
/* Caller holds a fabric attachment or another vmm_pcie_user reference. */
void	vmm_pcie_user_hold(struct vmm_pcie_user *user);
void	vmm_pcie_user_release(struct vmm_pcie_user *user);
int	vmm_pcie_user_send_start(struct vmm_pcie_user *user,
	    const struct vmm_pcie_abi_start *message);
int	vmm_pcie_user_send_stop(struct vmm_pcie_user *user,
	    const struct vmm_pcie_abi_stop *message);
int	vmm_pcie_user_mmio_access(struct vmm_pcie_user *user,
	    uint64_t device_id, uint64_t attachment_generation,
	    unsigned int bar_index, uint64_t offset, int write, int size,
	    uint64_t *valuep);

#endif /* VMM_PCIE_USER_H */
