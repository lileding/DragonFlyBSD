/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Intel VMX backend registration and interrupt-controller selection.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "../../vmm_backend.h"
#include "../../vmm_internal.h"
#include "vmm_vmx.h"
#include "vmm_vmx_apicv.h"

SYSCTL_DECL(_hw_vmm);

const struct vmm_vmx_interrupt_ops *vmm_vmx_interrupt_ops;
static int vmm_vmx_apicv_supported;
static int vmm_vmx_apicv_enable;

static int vmm_vmx_sysctl_apicv_enable(SYSCTL_HANDLER_ARGS);

SYSCTL_PROC(_hw_vmm, OID_AUTO, apicv_enable, CTLTYPE_INT | CTLFLAG_RW,
	NULL, 0, vmm_vmx_sysctl_apicv_enable, "I",
	"Use Intel APICv for newly created VMM machines");
SYSCTL_INT(_hw_vmm, OID_AUTO, apicv_available, CTLFLAG_RD,
	&vmm_vmx_apicv_supported, 0, "Intel APICv hardware is available");

static int
vmm_vmx_sysctl_apicv_enable(SYSCTL_HANDLER_ARGS)
{
	int enable;
	int error;

	enable = vmm_vmx_apicv_enable;
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
	if (enable != 0 && !vmm_vmx_apicv_supported) {
		lwkt_reltoken(&vmm_token);
		return ENOTSUP;
	}
	vmm_vmx_apicv_enable = enable;
	vmm_vmx_interrupt_ops = enable ? &vmm_vmx_apicv_interrupt_ops :
	    &vmm_vmx_softirq_interrupt_ops;
	lwkt_reltoken(&vmm_token);
	return 0;
}

int
vmm_vmx_probe(void)
{
	if (!vmm_vmx_ident())
		return ENXIO;
	vmm_vmx_apicv_supported = vmm_vmx_apicv_available();
	vmm_vmx_apicv_enable = vmm_vmx_apicv_supported;
	vmm_vmx_interrupt_ops = vmm_vmx_apicv_enable ?
	    &vmm_vmx_apicv_interrupt_ops : &vmm_vmx_softirq_interrupt_ops;
	kprintf("vmm: VMX interrupt mode: %s\n", vmm_vmx_interrupt_ops->name);
	return 0;
}

const struct vmm_backend_ops vmm_vmx_backend = {
	.name = "x64/vmx",
	.probe = vmm_vmx_probe,
	.init = vmm_vmx_init,
	.fini = vmm_vmx_fini,
	.capability = vmm_vmx_capability,
	.get_supported_cpuid = vmm_vmx_get_supported_cpuid,
	.machine_create = vmm_vmx_machine_create,
	.machine_set_tsc = vmm_vmx_machine_set_tsc,
	.irqchip_available = vmm_vmx_irqchip_available,
	.machine_create_irqchip = vmm_vmx_machine_create_irqchip,
	.irq_raise_msi = vmm_vmx_irq_raise_msi,
	.machine_set_irq = vmm_vmx_machine_set_irq,
	.machine_raise_legacy = vmm_vmx_machine_raise_legacy,
	.machine_get_ioapic = vmm_vmx_machine_get_ioapic,
	.machine_set_ioapic = vmm_vmx_machine_set_ioapic,
	.machine_destroy = vmm_vmx_machine_destroy,
	.vcpu_create = vmm_vmx_vcpu_create,
	.vcpu_set_cpuid = vmm_vmx_vcpu_set_cpuid,
	.vcpu_get_tsc = vmm_vmx_vcpu_get_tsc,
	.vcpu_get_lapic = vmm_vmx_vcpu_get_lapic,
	.vcpu_set_lapic = vmm_vmx_vcpu_set_lapic,
	.vcpu_io = vmm_vmx_vcpu_io,
	.vcpu_mmio = vmm_vmx_vcpu_mmio,
	.vcpu_memory_mapping_changed = vmm_vmx_vcpu_memory_mapping_changed,
	.vcpu_destroy = vmm_vmx_vcpu_destroy,
	.vcpu_setstate = vmm_vmx_vcpu_setstate,
	.vcpu_run = vmm_vmx_vcpu_run,
	.vcpu_getstate = vmm_vmx_vcpu_getstate,
	.vcpu_kick = vmm_vmx_vcpu_kick,
};

VMM_BACKEND_SET(vmm_vmx_backend);
