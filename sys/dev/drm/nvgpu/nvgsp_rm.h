/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP RM object boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_RM_H_
#define _NVGSP_RM_H_

struct nvgpu_device;

/* Create the RM client object during boot. */
int nvgsp_rm_create_client(struct nvgpu_device *gpu);
/* Create the RM device object under the client. */
int nvgsp_rm_create_device(struct nvgpu_device *gpu);
/* Create the RM subdevice object for GPU-wide controls. */
int nvgsp_rm_create_subdevice(struct nvgpu_device *gpu);
/* Create the RM usermode object before channel use. */
int nvgsp_rm_create_usermode_object(struct nvgpu_device *gpu);
/* Free an RM graphics object after GPU use has stopped. */
int nvgsp_rm_free_graphics_object(struct nvgpu_device *gpu);

#endif /* _NVGSP_RM_H_ */
