/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM backend registration and vmm core adapter.
 */
#include <sys/errno.h>
#include <sys/kernel.h>

#include "../../vmm_backend.h"
#include "vmm_svm.h"

int
vmm_svm_probe(void)
{
	return vmm_svm_ident() ? 0 : ENXIO;
}

const struct vmm_backend_ops vmm_svm_backend = {
	.name = "x64/svm",
	.probe = vmm_svm_probe,
	.init = vmm_svm_init,
	.fini = vmm_svm_fini,
	.capability = vmm_svm_capability,
	.machine_create = vmm_svm_machine_create,
	.machine_destroy = vmm_svm_machine_destroy,
	.vcpu_create = vmm_svm_vcpu_create,
	.vcpu_destroy = vmm_svm_vcpu_destroy,
	.vcpu_run = vmm_svm_vcpu_run,
	.vcpu_getstate = vmm_svm_vcpu_getstate,
	.vcpu_inject = vmm_svm_vcpu_inject,
	.vcpu_kick = vmm_svm_vcpu_kick,
};

VMM_BACKEND_SET(vmm_svm_backend);
