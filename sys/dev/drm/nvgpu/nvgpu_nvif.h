/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Nouveau NVIF syscall implementation.
 */

#ifndef _NVGPU_NVIF_H_
#define _NVGPU_NVIF_H_

struct nvgpu_proc;

/* Handle one variable-size NVIF ioctl payload copied by the DRM shim. */
int nvgpu_nvif_ioctl(struct nvgpu_proc *proc, void *data);

#endif /* _NVGPU_NVIF_H_ */
