/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open vPCIe provider or consumer socket session.
 */
#ifndef VMM_PCIE_USER_H
#define VMM_PCIE_USER_H

struct socket;
struct ucred;
struct vmm_device;

enum vmm_pcie_user_role {
	VMM_PCIE_USER_PROVIDER,
	VMM_PCIE_USER_CONSUMER,
};

struct vmm_pcie_user;

int	vmm_pcie_user_open(struct vmm_device *device,
	    enum vmm_pcie_user_role role, struct ucred *cred,
	    struct socket **user_socketp);
/* Caller holds the parent fabric token_registry. */
int	vmm_pcie_user_force_close(struct vmm_pcie_user *user);

#endif /* VMM_PCIE_USER_H */
