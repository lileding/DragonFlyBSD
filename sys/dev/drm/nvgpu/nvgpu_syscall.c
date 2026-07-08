/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Nouveau syscall boundary for one GPU process.
 */

#include "nvgpu_syscall.h"
#include "nvgpu_debug.h"

#include <sys/errno.h>

/* Log GETPARAM until the real syscall implementation is moved in. */
int
nvgpu_syscall_getparam(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall getparam proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}

/* Log VM_INIT until the real syscall implementation is moved in. */
int
nvgpu_syscall_vm_init(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall vm_init proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}

/* Log NVIF until the real syscall implementation is moved in. */
int
nvgpu_syscall_nvif(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall nvif proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}

/* Log CHANNEL_ALLOC until the real syscall implementation is moved in. */
int
nvgpu_syscall_channel_alloc(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "syscall channel_alloc proc=%p data=%p\n", proc, data);
	return (EOPNOTSUPP);
}

/* Log CHANNEL_FREE until the real syscall implementation is moved in. */
int
nvgpu_syscall_channel_free(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "syscall channel_free proc=%p data=%p\n", proc, data);
	return (EOPNOTSUPP);
}

/* Log GEM_NEW until the real syscall implementation is moved in. */
int
nvgpu_syscall_gem_new(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall gem_new proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}

/* Log GEM_INFO until the real syscall implementation is moved in. */
int
nvgpu_syscall_gem_info(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall gem_info proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}

/* Log GEM_CPU_PREP until the real syscall implementation is moved in. */
int
nvgpu_syscall_gem_cpu_prep(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "syscall gem_cpu_prep proc=%p data=%p\n", proc, data);
	return (EOPNOTSUPP);
}

/* Log GEM_CPU_FINI until the real syscall implementation is moved in. */
int
nvgpu_syscall_gem_cpu_fini(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "syscall gem_cpu_fini proc=%p data=%p\n", proc, data);
	return (EOPNOTSUPP);
}

/* Log VM_BIND until the real syscall implementation is moved in. */
int
nvgpu_syscall_vm_bind(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall vm_bind proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}

/* Log EXEC until the real syscall implementation is moved in. */
int
nvgpu_syscall_exec(struct nvgpu_proc *proc, void *data)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "syscall exec proc=%p data=%p\n",
	    proc, data);
	return (EOPNOTSUPP);
}
