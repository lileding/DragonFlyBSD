/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Nouveau syncobj/timeline glue for EXEC and VM_BIND futures.
 */

#ifndef _NVDRM_SYNC_H_
#define _NVDRM_SYNC_H_

#include <sys/stdint.h>

struct dma_fence;
struct drm_file;
struct nvgpu_fence;
struct nvdrm_sync_signal;

struct nvdrm_sync_wait_set {
	struct nvgpu_fence **fences;
	uint32_t count;
};

struct nvdrm_sync_signal_set {
	struct nvdrm_sync_signal *signals;
	uint32_t count;
};

/* Convert userspace wait syncobjs into nvgpu fence references owned by set. */
int nvdrm_sync_collect_wait_fences(struct drm_file *file, uint32_t count,
    uint64_t wait_ptr, struct nvdrm_sync_wait_set *set);

/* Resolve userspace signal handles and prepare timeline chain nodes. */
int nvdrm_sync_prepare_signals(struct drm_file *file, uint32_t count,
    uint64_t sig_ptr, struct nvgpu_fence *done_fence,
    struct nvdrm_sync_signal_set *set);

/* Publish prepared signal handles after the future is ready for submission. */
void nvdrm_sync_publish_signals(struct nvdrm_sync_signal_set *set);

/* Drop unpublished signal resources after an error. */
void nvdrm_sync_cleanup_signals(struct nvdrm_sync_signal_set *set);

/* Drop wait fence references collected for a syscall. */
void nvdrm_sync_cleanup_waits(struct nvdrm_sync_wait_set *set);

#endif /* _NVDRM_SYNC_H_ */
