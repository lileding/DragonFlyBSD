/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical GPU lifetime boundary for the native NVIDIA driver.
 */

#ifndef _NVGPU_DEVICE_H_
#define _NVGPU_DEVICE_H_

#include <sys/bus.h>
#include <sys/stdint.h>


struct nvgsp_state;
struct nvgpu_chip_config;
struct drm_device;
struct pci_dev;
struct resource;

/* Opaque physical GPU root.  Owned by PCI attach; subsystems borrow it while detach is excluded. */
struct nvgpu_device;

/* Return gpu's borrowed device_t.  NULL gpu means the current default GPU, if any. */
device_t nvgpu_device_dev(struct nvgpu_device *gpu);
/* Return borrowed immutable chip metadata for gpu. */
const struct nvgpu_chip_config *nvgpu_device_chip(struct nvgpu_device *gpu);
/* Return a borrowed BAR resource, or NULL when the BAR is not mapped. */
struct resource *nvgpu_device_bar(struct nvgpu_device *gpu, unsigned int bar);
uint32_t nvgpu_device_rd32(struct nvgpu_device *gpu, uint32_t offset);
void nvgpu_device_wr32(struct nvgpu_device *gpu, uint32_t offset, uint32_t val);
struct nvgsp_state *nvgpu_device_gsp(struct nvgpu_device *gpu);
void nvgpu_device_set_gsp(struct nvgpu_device *gpu, struct nvgsp_state *gsp);
void *nvgpu_device_intr(struct nvgpu_device *gpu);
void nvgpu_device_set_intr(struct nvgpu_device *gpu, void *intr);
struct drm_device *nvgpu_device_drm_dev(struct nvgpu_device *gpu);
struct pci_dev *nvgpu_device_drm_pdev(struct nvgpu_device *gpu);
void nvgpu_device_set_drm(struct nvgpu_device *gpu, struct drm_device *ddev,
    struct pci_dev *pdev);
void *nvgpu_device_unload_state(struct nvgpu_device *gpu);
void nvgpu_device_set_unload_state(struct nvgpu_device *gpu, void *state);

#endif /* _NVGPU_DEVICE_H_ */
