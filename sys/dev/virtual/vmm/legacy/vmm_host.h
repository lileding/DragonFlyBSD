/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host-local helpers.  There is no vmm_host object: host state is not part of
 * the VM object model.  vmm_host_next_cpu() is a global vCPU placement cursor
 * used to distribute vCPU lwkts across physical CPUs.
 *
 * Types (uint32_t) come from the includer.
 */
#ifndef VMM_HOST_H
#define VMM_HOST_H

int	vmm_host_next_cpu(void);

#endif /* VMM_HOST_H */
