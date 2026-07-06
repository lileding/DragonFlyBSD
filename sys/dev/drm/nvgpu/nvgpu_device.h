/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical GPU lifetime boundary for the native NVIDIA driver.
 */

#ifndef _NVGPU_DEVICE_H_
#define _NVGPU_DEVICE_H_

#include <sys/bus.h>

/* Opaque physical GPU root.  Owned by PCI attach; subsystems borrow it while detach is excluded. */
struct nvgpu_device;

/* Return gpu's borrowed device_t.  NULL gpu means the current default GPU, if any. */
device_t nvgpu_device_dev(struct nvgpu_device *gpu);

#endif /* _NVGPU_DEVICE_H_ */
