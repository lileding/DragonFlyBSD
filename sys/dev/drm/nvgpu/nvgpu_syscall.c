/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Nouveau syscall boundary for one GPU process.
 */

#include "nvgpu_syscall.h"
#include "nvdrm_nouveau_abi.h"
#include "nvgpu_bo.h"
#include "nvgpu_channel.h"
#include "nvgpu_debug.h"
#include "nvgpu_info.h"
#include "nvgpu_nvif.h"
#include "nvgpu_vm.h"

#include <sys/errno.h>

int
nvgpu_syscall_getparam(struct nvgpu_proc *proc, struct drm_file *file __unused,
    void *data)
{
	struct drm_nouveau_getparam *req = data;

	return (nvgpu_info_get_param(proc, req->param, &req->value));
}

int
nvgpu_syscall_vm_init(struct nvgpu_proc *proc, struct drm_file *file __unused,
    void *data)
{
	struct drm_nouveau_vm_init *req = data;

	return (nvgpu_vm_set_kernel_managed(proc, req->kernel_managed_addr,
	    req->kernel_managed_size));
}

int
nvgpu_syscall_nvif(struct nvgpu_proc *proc, struct drm_file *file __unused,
    void *data)
{
	return (nvgpu_nvif_ioctl(proc, data));
}

int
nvgpu_syscall_channel_alloc(struct nvgpu_proc *proc,
    struct drm_file *file __unused, void *data)
{
	struct drm_nouveau_channel_alloc *req = data;
	struct nvgpu_channel_alloc_args args;
	struct nvgpu_channel_alloc_reply reply;
	int error;

	args.fb_ctxdma_handle = req->fb_ctxdma_handle;
	args.tt_ctxdma_handle = req->tt_ctxdma_handle;
	error = nvgpu_channel_alloc(proc, &args, &reply);
	if (error != 0)
		return (error);
	req->channel = reply.channel;
	req->pushbuf_domains = reply.pushbuf_domains;
	req->notifier_handle = reply.notifier_handle;
	req->nr_subchan = reply.nr_subchan;
	return (0);
}

int
nvgpu_syscall_channel_free(struct nvgpu_proc *proc,
    struct drm_file *file __unused, void *data)
{
	struct drm_nouveau_channel_free *req = data;

	return (nvgpu_channel_free(proc, req->channel));
}

int
nvgpu_syscall_gem_new(struct nvgpu_proc *proc, struct drm_file *file,
    void *data)
{
	struct drm_nouveau_gem_new *req = data;
	struct nvgpu_bo_create_args args;
	struct nvgpu_bo_info info;
	int error;

	args.size = req->info.size;
	args.domain = req->info.domain;
	args.align = req->align;
	args.tile_mode = req->info.tile_mode;
	args.tile_flags = req->info.tile_flags;
	error = nvgpu_bo_create_handle(proc, file, &args, &info);
	if (error != 0)
		return (error);
	req->info.handle = info.handle;
	req->info.domain = info.domain;
	req->info.size = info.size;
	req->info.offset = info.offset;
	req->info.map_handle = info.map_handle;
	req->info.tile_mode = info.tile_mode;
	req->info.tile_flags = info.tile_flags;
	return (0);
}

int
nvgpu_syscall_gem_info(struct nvgpu_proc *proc __unused,
    struct drm_file *file, void *data)
{
	struct drm_nouveau_gem_info *req = data;
	struct nvgpu_bo_info info;
	int error;

	error = nvgpu_bo_get_info(file, req->handle, &info);
	if (error != 0)
		return (error);
	req->domain = info.domain;
	req->size = info.size;
	req->offset = info.offset;
	req->map_handle = info.map_handle;
	req->tile_mode = info.tile_mode;
	req->tile_flags = info.tile_flags;
	return (0);
}

/* Log GEM_CPU_PREP until the real syscall implementation is moved in. */
int
nvgpu_syscall_gem_cpu_prep(struct nvgpu_proc *proc,
    struct drm_file *file __unused, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "syscall gem_cpu_prep proc=%p data=%p\n", proc, data);
	return (EOPNOTSUPP);
}

/* Log GEM_CPU_FINI until the real syscall implementation is moved in. */
int
nvgpu_syscall_gem_cpu_fini(struct nvgpu_proc *proc,
    struct drm_file *file __unused, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "syscall gem_cpu_fini proc=%p data=%p\n", proc, data);
	return (EOPNOTSUPP);
}

/* Log VM_BIND until the real syscall implementation is moved in. */
int
nvgpu_syscall_vm_bind(struct nvgpu_proc *proc, struct drm_file *file __unused,
    void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall vm_bind proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}

/* Log EXEC until the real syscall implementation is moved in. */
int
nvgpu_syscall_exec(struct nvgpu_proc *proc, struct drm_file *file __unused,
    void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall exec proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}
