/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The physical host machine.  Stub: today the host is just the source of the
 * PCIe passthrough pool (devices with no guest owner).  The pool entries + their
 * fs nodes still live in the mount; as passthrough grows the device inventory
 * will move here.  This struct makes the host a first-class vmm_ object
 * instead of scattered fs state.
 *
 * Types (uint32_t) come from the includer.
 */
#ifndef VMM_HOST_H
#define VMM_HOST_H

struct vmm_host {
	/*
	 * Lock map:
	 * mut_device_count is changed during filesystem-side device pool setup
	 * under the vmmfs mount registry lock.  atomic_mut_next_cpu is updated
	 * locklessly by vCPU start workers for round-robin placement.
	 */
	uint32_t	mut_device_count;	/* devices in the host pool */
	uint32_t	atomic_mut_next_cpu;	/* vCPU placement cursor */
};

void		vmm_host_init(struct vmm_host *h);
void		vmm_host_add_device(struct vmm_host *h);
uint32_t	vmm_host_device_count(const struct vmm_host *h);
int		vmm_host_next_cpu(struct vmm_host *h);

#endif /* VMM_HOST_H */
