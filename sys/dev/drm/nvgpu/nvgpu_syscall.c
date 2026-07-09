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
#include "nvgpu_exec.h"
#include "nvgpu_info.h"
#include "nvgpu_nvif.h"
#include "nvgpu_vm.h"

#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <machine/cpufunc.h>

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

int
nvgpu_syscall_gem_cpu_prep(struct nvgpu_proc *proc __unused,
    struct drm_file *file, void *data)
{
	struct drm_nouveau_gem_cpu_prep *req = data;
	struct nvgpu_bo *bo;
	int error;

	if (req == NULL)
		return (EINVAL);
	error = nvgpu_bo_lookup(file, req->handle, &bo);
	if (error != 0)
		return (error);
	error = nvgpu_bo_resv_wait(bo, true,
	    (req->flags & NOUVEAU_GEM_CPU_PREP_WRITE) != 0,
	    (req->flags & NOUVEAU_GEM_CPU_PREP_NOWAIT) != 0);
	nvgpu_bo_put(bo);
	return (error);
}

int
nvgpu_syscall_gem_cpu_fini(struct nvgpu_proc *proc __unused,
    struct drm_file *file, void *data)
{
	struct drm_nouveau_gem_cpu_fini *req = data;
	struct nvgpu_bo *bo;
	int error;

	if (req == NULL)
		return (EINVAL);
	error = nvgpu_bo_lookup(file, req->handle, &bo);
	if (error != 0)
		return (error);
	cpu_sfence();
	nvgpu_bo_put(bo);
	return (0);
}

/* Log VM_BIND operations until the real mapping implementation is moved in. */
int
nvgpu_syscall_vm_bind(struct nvgpu_proc *proc, struct drm_file *file __unused,
    void *data)
{
	struct drm_nouveau_vm_bind *req = data;
	struct drm_nouveau_vm_bind_op *ops;
	struct nvgpu_vm_bind_op *vm_ops;
	size_t size;
	int error;

	if (req == NULL)
		return (EINVAL);
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "syscall vm_bind proc=%p ops=%u flags=0x%x waits=%u sigs=%u op_ptr=0x%llx\n",
	    proc, req->op_count, req->flags, req->wait_count, req->sig_count,
	    (unsigned long long)req->op_ptr);
	if (req->wait_count != 0 || req->sig_count != 0 ||
	    (req->flags & DRM_NOUVEAU_VM_BIND_RUN_ASYNC) != 0)
		return (EOPNOTSUPP);
	if (req->op_count == 0)
		return (0);
	size = sizeof(*ops) * req->op_count;
	ops = kmalloc(size, M_TEMP, M_WAITOK | M_ZERO);
	vm_ops = kmalloc(sizeof(*vm_ops) * req->op_count, M_TEMP,
	    M_WAITOK | M_ZERO);
	error = copyin((const void *)(uintptr_t)req->op_ptr, ops, size);
	if (error != 0) {
		kfree(vm_ops);
		kfree(ops);
		return (error);
	}
	for (uint32_t i = 0; i < req->op_count; i++) {
		nvgpu_log(NVGPU_LOG_DEBUG,
		    "syscall vm_bind op[%u] op=%u flags=0x%x handle=%u addr=0x%llx bo_offset=0x%llx range=0x%llx\n",
		    i, ops[i].op, ops[i].flags, ops[i].handle,
		    (unsigned long long)ops[i].addr,
		    (unsigned long long)ops[i].bo_offset,
		    (unsigned long long)ops[i].range);
		vm_ops[i].op = ops[i].op;
		vm_ops[i].flags = ops[i].flags;
		vm_ops[i].handle = ops[i].handle;
		vm_ops[i].addr = ops[i].addr;
		vm_ops[i].bo_offset = ops[i].bo_offset;
		vm_ops[i].range = ops[i].range;
	}
	error = nvgpu_vm_bind(proc, file, vm_ops, req->op_count);
	kfree(vm_ops);
	kfree(ops);
	return (error);
}

/* Submit a synchronization-only EXEC future until channel submit is moved in. */
int
nvgpu_syscall_exec(struct nvgpu_proc *proc, struct drm_file *file,
    void *data)
{
	return (nvgpu_exec_submit_fake(proc, file, data));
}
