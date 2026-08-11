/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM backend registration and vmm core adapter.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "../../vmm_backend.h"
#include "../../vmm_internal.h"
#include "vmm_svm.h"
#include "vmm_svm_avic.h"

SYSCTL_DECL(_hw_vmm);

const struct vmm_svm_interrupt_ops *vmm_svm_interrupt_ops;
static int vmm_svm_avic_supported;
static int vmm_svm_avic_enable;

static int vmm_svm_sysctl_avic_enable(SYSCTL_HANDLER_ARGS);

SYSCTL_PROC(_hw_vmm, OID_AUTO, avic_enable, CTLTYPE_INT | CTLFLAG_RW,
	NULL, 0, vmm_svm_sysctl_avic_enable, "I",
	"Use AMD AVIC for newly created VMM machines");
SYSCTL_INT(_hw_vmm, OID_AUTO, avic_available, CTLFLAG_RD,
	&vmm_svm_avic_supported, 0, "AMD AVIC hardware is available");

static int
vmm_svm_sysctl_avic_enable(SYSCTL_HANDLER_ARGS)
{
	int enable;
	int error;

	enable = vmm_svm_avic_enable;
	error = sysctl_handle_int(oidp, &enable, 0, req);
	if (error != 0 || req->newptr == NULL)
		return error;
	if (enable != 0 && enable != 1)
		return EINVAL;
	lwkt_gettoken(&vmm_token);
	if (vmm_machine_count != 0 || vmm_draining) {
		lwkt_reltoken(&vmm_token);
		return EBUSY;
	}
	if (enable != 0 && !vmm_svm_avic_supported) {
		lwkt_reltoken(&vmm_token);
		return ENOTSUP;
	}
	vmm_svm_avic_enable = enable;
	vmm_svm_interrupt_ops = enable ? &vmm_svm_avic_interrupt_ops :
	    &vmm_svm_soft_interrupt_ops;
	lwkt_reltoken(&vmm_token);
	return 0;
}

int
vmm_svm_probe(void)
{
	if (!vmm_svm_ident())
		return ENXIO;
	vmm_svm_avic_supported = vmm_svm_avic_available();
	vmm_svm_avic_enable = vmm_svm_avic_supported;
	if (vmm_svm_avic_enable)
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
	.irqchip_available = vmm_svm_irqchip_available,
	.machine_create_irqchip = vmm_svm_machine_create_irqchip,
	.irq_raise_msi = vmm_svm_irq_raise_msi,
	.machine_raise_irq = vmm_svm_machine_raise_irq,
	.machine_set_irq = vmm_svm_machine_set_irq,
	.machine_destroy = vmm_svm_machine_destroy,
	.vcpu_create = vmm_svm_vcpu_create,
	.vcpu_set_cpuid = vmm_svm_vcpu_set_cpuid,
	.vcpu_destroy = vmm_svm_vcpu_destroy,
	.vcpu_run = vmm_svm_vcpu_run,
	.vcpu_getstate = vmm_svm_vcpu_getstate,
	.vcpu_kick = vmm_svm_vcpu_kick,
};

VMM_BACKEND_SET(vmm_svm_backend);
