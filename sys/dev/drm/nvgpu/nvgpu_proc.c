/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#include "nvgpu_proc.h"
#include "nvgpu_channel.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_unload.h"
#include "nvgpu_vm.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <stdbool.h>

static MALLOC_DEFINE(M_NVGPU_PROC, "nvgpu_proc", "nvgpu process state");

struct nvgpu_proc {
	struct nvgpu_device *gpu;
	struct lwkt_token token;
	uint32_t refs;
	struct nvgpu_channel_list channels;
	struct nvgpu_vm *vm;
	bool shutdown;
};

static void nvgpu_proc_destroy(struct nvgpu_proc *proc);

struct nvgpu_device *
nvgpu_proc_get_device(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return (NULL);
	return (proc->gpu);
}

struct nvgpu_channel_list *
nvgpu_proc_get_channels(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return (NULL);
	return (&proc->channels);
}

struct nvgpu_vm *
nvgpu_proc_get_vm(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return (NULL);
	return (proc->vm);
}

void
nvgpu_proc_set_vm(struct nvgpu_proc *proc, struct nvgpu_vm *vm)
{
	if (proc == NULL)
		return;
	proc->vm = vm;
}

int
nvgpu_proc_create(struct nvgpu_device *gpu, struct nvgpu_proc **procp)
{
	struct nvgpu_proc *proc;

	if (gpu == NULL || procp == NULL)
		return (EINVAL);

	proc = kmalloc(sizeof(*proc), M_NVGPU_PROC, M_WAITOK | M_ZERO);

	proc->gpu = gpu;
	lwkt_token_init(&proc->token, "nvgprc");
	proc->refs = 1;
	TAILQ_INIT(&proc->channels);
	proc->vm = NULL;
	proc->shutdown = false;

	*procp = proc;
	nvgpu_log(NVGPU_LOG_DEBUG, "proc created proc=%p\n", proc);
	return (0);
}

void
nvgpu_proc_stop(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return;

	lwkt_gettoken(&proc->token);
	proc->shutdown = true;
	lwkt_reltoken(&proc->token);
	nvgpu_log(NVGPU_LOG_DEBUG, "proc stop requested proc=%p\n", proc);
	nvgpu_proc_release(proc);
}

void
nvgpu_proc_hold(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return;
	lwkt_gettoken(&proc->token);
	proc->refs++;
	lwkt_reltoken(&proc->token);
}

void
nvgpu_proc_release(struct nvgpu_proc *proc)
{
	bool destroy = false;

	if (proc == NULL)
		return;
	lwkt_gettoken(&proc->token);
	KASSERT(proc->refs > 0, ("nvgpu proc refs underflow"));
	proc->refs--;
	if (proc->shutdown && proc->refs == 0)
		destroy = true;
	lwkt_reltoken(&proc->token);
	if (destroy)
		nvgpu_proc_destroy(proc);
}

static void
nvgpu_proc_destroy(struct nvgpu_proc *proc)
{
	nvgpu_channel_destroy_all(proc);
	nvgpu_vm_destroy(proc);
	nvgpu_unload_release_by_drm(proc->gpu);
	nvgpu_log(NVGPU_LOG_DEBUG, "proc destroy proc=%p\n", proc);
	lwkt_token_uninit(&proc->token);
	_kfree(proc, M_NVGPU_PROC);
}
