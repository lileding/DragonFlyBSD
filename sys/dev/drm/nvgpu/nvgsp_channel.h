/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP channel backend boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_CHANNEL_H_
#define _NVGSP_CHANNEL_H_

#include <sys/stdint.h>

#define NVGSP_CHANNEL_ENGINE_GRAPHICS	1u
#define NVGSP_CHANNEL_ENGINE_COPY0	11u
#define NVGSP_CHANNEL_ENGINE_COPY1	12u
#define NVGSP_CHANNEL_ENGINE_COPY2	13u

struct nvgpu_device;
struct nvgsp_channel;
struct nvgsp_channel_object;
struct nvgsp_vmm;

/* Create the bootstrap channel during boot. */
int nvgsp_channel_create_bootstrap(struct nvgpu_device *gpu);
/* Destroy the bootstrap channel after submissions have stopped. */
void nvgsp_channel_destroy_bootstrap(struct nvgpu_device *gpu);
/* Create the golden channel used as a template for user channels. */
int nvgsp_channel_create_golden(struct nvgpu_device *gpu);
/* Destroy golden channel state after user channel creation has stopped. */
void nvgsp_channel_destroy_golden(struct nvgpu_device *gpu);
/* Create a user submission channel on an existing per-process VMM. */
int nvgsp_channel_create_user(struct nvgsp_vmm *vmm, uint32_t engine_type,
    struct nvgsp_channel **out);
/* Destroy a user submission channel after scheduler work has drained. */
void nvgsp_channel_destroy_user(struct nvgsp_channel *chan);
/* Promote GR context buffers for a user channel before GR-class object allocation. */
int nvgsp_channel_promote_graphics_context(struct nvgsp_channel *chan);
/* Allocate an RM engine object under a user channel. */
int nvgsp_channel_alloc_object(struct nvgsp_channel *chan, uint32_t handle,
    uint32_t oclass, struct nvgsp_channel_object **out);
/* Free an RM engine object allocated by nvgsp_channel_alloc_object(). */
void nvgsp_channel_free_object(struct nvgsp_channel_object *obj);

#endif /* _NVGSP_CHANNEL_H_ */
