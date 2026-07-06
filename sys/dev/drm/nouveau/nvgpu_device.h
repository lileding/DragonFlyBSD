/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical GPU lifetime boundary for the native NVIDIA driver.
 */

#ifndef _NVGPU_DEVICE_H_
#define _NVGPU_DEVICE_H_

/*
 * struct nvgpu_device
 *
 * Ownership:
 *   Owned by the PCI/newbus attachment and borrowed by DRM, GSP, display,
 *   memory-management, and per-open process objects.
 *
 * Lifetime:
 *   Created during PCI attach and destroyed during detach after DRM users,
 *   scheduler work, interrupts, display state, GSP state, and BAR resources
 *   have been drained or torn down.
 *
 * Threading:
 *   This is an opaque device root.  Callers must use subsystem functions rather
 *   than reading fields directly; each subsystem documents its own lock or
 *   token rules at the function boundary.
 */
struct nvgpu_device;

#endif /* _NVGPU_DEVICE_H_ */
