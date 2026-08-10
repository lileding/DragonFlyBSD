/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM backend registration and vmm core adapter.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include "../../vmm_backend.h"
#include "vmm_svm.h"
#include "vmm_svm_avic.h"

const struct vmm_svm_interrupt_ops *vmm_svm_interrupt_ops;

int
vmm_svm_probe(void)
{
	if (!vmm_svm_ident())
		return ENXIO;
	if (vmm_svm_avic_available())
		vmm_svm_interrupt_ops = &vmm_svm_avic_interrupt_ops;
	else
		vmm_svm_interrupt_ops = &vmm_svm_soft_interrupt_ops;
	kprintf("vmm: SVM interrupt mode: %s\n", vmm_svm_interrupt_ops->name);
	return 0;
}

const struct vmm_backend_ops vmm_svm_backend = {
	.name = "x64/svm",
	.probe = vmm_svm_probe,
	.init = vmm_svm_init,
	.fini = vmm_svm_fini,
	.capability = vmm_svm_capability,
	.get_supported_cpuid = vmm_svm_get_supported_cpuid,
	.machine_create = vmm_svm_machine_create,
	.machine_create_irqchip = vmm_svm_machine_create_irqchip,
	.machine_destroy = vmm_svm_machine_destroy,
	.vcpu_create = vmm_svm_vcpu_create,
	.vcpu_set_cpuid = vmm_svm_vcpu_set_cpuid,
	.vcpu_destroy = vmm_svm_vcpu_destroy,
	.vcpu_run = vmm_svm_vcpu_run,
	.vcpu_getstate = vmm_svm_vcpu_getstate,
	.vcpu_kick = vmm_svm_vcpu_kick,
};

VMM_BACKEND_SET(vmm_svm_backend);
