/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vkernel64 no-hardware vCPU backend.
 */
#include <sys/types.h>
#include <sys/errno.h>

#include "vmm_vcpu.h"

static const char *
vmm_vkernel_probe(void)
{
	return NULL;
}

static int
vmm_vkernel_vcpu_create(struct vmm_machine *m,
    const struct vmm_launch *launch, void **backendp)
{
	(void)m;
	(void)launch;
	(void)backendp;
	return EOPNOTSUPP;
}

static void
vmm_vkernel_vcpu_destroy(void *backend)
{
	(void)backend;
}

static enum vmm_vcpu_exit_reason
vmm_vkernel_vcpu_run(void *backend, struct vmm_vcpu_thread *vc)
{
	(void)backend;
	(void)vc;
	return VMM_VCPU_EXIT_NONE;
}

const struct vmm_vcpu_backend_ops vmm_vkernel_backend_ops = {
	.imm_name = "vkernel",
	.probe = vmm_vkernel_probe,
	.create = vmm_vkernel_vcpu_create,
	.destroy = vmm_vkernel_vcpu_destroy,
	.run = vmm_vkernel_vcpu_run,
};

VMM_VCPU_BACKEND_SET(vmm_vkernel_backend_ops);
