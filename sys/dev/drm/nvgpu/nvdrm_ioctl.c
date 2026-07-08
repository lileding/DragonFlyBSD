/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Nouveau-compatible DRM ioctl dispatch table.
 */

#include "nvdrm_ioctl.h"
#include "nvdrm_file.h"
#include "nvgpu_debug.h"
#include "nvgpu_syscall.h"

#include <drm/drmP.h>
#include <drm/drm_ioctl.h>

#define DRM_NOUVEAU_GETPARAM		0x00
#define DRM_NOUVEAU_CHANNEL_ALLOC	0x02
#define DRM_NOUVEAU_CHANNEL_FREE	0x03
#define DRM_NOUVEAU_NVIF		0x07
#define DRM_NOUVEAU_VM_INIT		0x10
#define DRM_NOUVEAU_VM_BIND		0x11
#define DRM_NOUVEAU_EXEC		0x12
#define DRM_NOUVEAU_GEM_NEW		0x40
#define DRM_NOUVEAU_GEM_CPU_PREP	0x42
#define DRM_NOUVEAU_GEM_CPU_FINI	0x43
#define DRM_NOUVEAU_GEM_INFO		0x44

struct drm_nouveau_getparam {
	uint64_t param;
	uint64_t value;
};

struct drm_nouveau_vm_init {
	uint64_t kernel_managed_addr;
	uint64_t kernel_managed_size;
};

struct drm_nouveau_channel_alloc {
	uint32_t fb_ctxdma_handle;
	uint32_t tt_ctxdma_handle;
	int32_t channel;
	uint32_t pushbuf_domains;
	uint32_t notifier_handle;
	struct {
		uint32_t handle;
		uint32_t grclass;
	} subchan[8];
	uint32_t nr_subchan;
};

struct drm_nouveau_channel_free {
	int32_t channel;
};

struct drm_nouveau_gem_info {
	uint32_t handle;
	uint32_t domain;
	uint64_t size;
	uint64_t offset;
	uint64_t map_handle;
	uint32_t tile_mode;
	uint32_t tile_flags;
};

struct drm_nouveau_gem_new {
	struct drm_nouveau_gem_info info;
	uint32_t channel_hint;
	uint32_t align;
};

struct drm_nouveau_gem_cpu_prep {
	uint32_t handle;
	uint32_t flags;
};

struct drm_nouveau_gem_cpu_fini {
	uint32_t handle;
};

struct drm_nouveau_vm_bind {
	uint32_t op_count;
	uint32_t flags;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t op_ptr;
};

struct drm_nouveau_exec {
	uint32_t channel;
	uint32_t push_count;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t push_ptr;
};

#define DRM_IOCTL_NOUVEAU_GETPARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GETPARAM, struct drm_nouveau_getparam)
#define DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_ALLOC, struct drm_nouveau_channel_alloc)
#define DRM_IOCTL_NOUVEAU_CHANNEL_FREE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_FREE, struct drm_nouveau_channel_free)
#define DRM_IOCTL_NOUVEAU_NVIF \
	_IOC(IOC_INOUT, DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_NOUVEAU_NVIF, 0)
#define DRM_IOCTL_NOUVEAU_VM_INIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_INIT, struct drm_nouveau_vm_init)
#define DRM_IOCTL_NOUVEAU_VM_BIND \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_BIND, struct drm_nouveau_vm_bind)
#define DRM_IOCTL_NOUVEAU_EXEC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_EXEC, struct drm_nouveau_exec)
#define DRM_IOCTL_NOUVEAU_GEM_NEW \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_NEW, struct drm_nouveau_gem_new)
#define DRM_IOCTL_NOUVEAU_GEM_CPU_PREP \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_CPU_PREP, struct drm_nouveau_gem_cpu_prep)
#define DRM_IOCTL_NOUVEAU_GEM_CPU_FINI \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_CPU_FINI, struct drm_nouveau_gem_cpu_fini)
#define DRM_IOCTL_NOUVEAU_GEM_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_INFO, struct drm_nouveau_gem_info)

static int
nvdrm_ioctl_getparam(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_getparam(proc, data));
}

static int
nvdrm_ioctl_vm_init(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_vm_init(proc, data));
}

static int
nvdrm_ioctl_nvif(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_nvif(proc, data));
}

static int
nvdrm_ioctl_channel_alloc(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_channel_alloc(proc, data));
}

static int
nvdrm_ioctl_channel_free(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_channel_free(proc, data));
}

static int
nvdrm_ioctl_gem_new(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_gem_new(proc, data));
}

static int
nvdrm_ioctl_gem_info(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_gem_info(proc, data));
}

static int
nvdrm_ioctl_gem_cpu_prep(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_gem_cpu_prep(proc, data));
}

static int
nvdrm_ioctl_gem_cpu_fini(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_gem_cpu_fini(proc, data));
}

static int
nvdrm_ioctl_vm_bind(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_vm_bind(proc, data));
}

static int
nvdrm_ioctl_exec(struct drm_device *ddev __unused, void *data,
    struct drm_file *file_priv)
{
	struct nvdrm_file *file;
	struct nvgpu_proc *proc;

	file = nvdrm_file_from_drm(file_priv);
	proc = nvdrm_file_proc(file);
	if (proc == NULL)
		return (ENXIO);
	return (nvgpu_syscall_exec(proc, data));
}

const struct drm_ioctl_desc nvdrm_ioctl_descs[NVDRM_IOCTL_COUNT] = {
	DRM_IOCTL_DEF_DRV(NOUVEAU_GETPARAM, nvdrm_ioctl_getparam,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_VM_INIT, nvdrm_ioctl_vm_init,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_NVIF, nvdrm_ioctl_nvif,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_CHANNEL_ALLOC, nvdrm_ioctl_channel_alloc,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_CHANNEL_FREE, nvdrm_ioctl_channel_free,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_NEW, nvdrm_ioctl_gem_new,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_INFO, nvdrm_ioctl_gem_info,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_CPU_PREP, nvdrm_ioctl_gem_cpu_prep,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_CPU_FINI, nvdrm_ioctl_gem_cpu_fini,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_VM_BIND, nvdrm_ioctl_vm_bind,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_EXEC, nvdrm_ioctl_exec,
	    DRM_AUTH | DRM_RENDER_ALLOW),
};
