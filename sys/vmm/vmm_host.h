/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The physical host machine.  Stub: today the host is just the source of the
 * PCIe passthrough pool (devices with no guest owner).  The pool entries + their
 * fs nodes still live in the mount; as passthrough grows the device inventory
 * will move here.  This struct makes the host a first-class vmm_ object (three-
 * file shape) instead of scattered fs state.  Pure; no kernel/VFS deps.
 *
 * Types (uint32_t) come from the includer.
 */
#ifndef VMM_HOST_H
#define VMM_HOST_H

struct vmm_host {
	uint32_t	device_count;	/* devices in the host pool */
#ifdef _KERNEL
	uint32_t	next_cpu;	/* round-robin vCPU placement cursor */
#endif
};

void		vmm_host_init(struct vmm_host *h);
void		vmm_host_add_device(struct vmm_host *h);
uint32_t	vmm_host_device_count(const struct vmm_host *h);
#ifdef _KERNEL
int		vmm_host_next_cpu(struct vmm_host *h);
#endif

#endif /* VMM_HOST_H */
