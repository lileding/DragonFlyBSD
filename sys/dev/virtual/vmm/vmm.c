/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm kernel module entry points.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/globaldata.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/thread.h>

#include "vmm.h"
#include "vmm_internal.h"

SET_DECLARE(vmm_backend_set, const struct vmm_backend_ops);

const struct vmm_backend_ops *vmm_backend;
struct lwkt_token vmm_token;
int vmm_machine_count;
bool vmm_draining;
static uint64_t vmm_vmexit_count[MAXCPU];
static uint64_t vmm_vcpu_run_return_count[MAXCPU];

static int vmm_sysctl_stats(SYSCTL_HANDLER_ARGS);

SYSCTL_NODE(_hw, OID_AUTO, vmm, CTLFLAG_RW, 0, "VMM configuration");
SYSCTL_NODE(_hw_vmm, OID_AUTO, stats, CTLFLAG_RD, 0, "VMM statistics");
SYSCTL_PROC(_hw_vmm_stats, OID_AUTO, vmexit, CTLTYPE_U64 | CTLFLAG_RD,
    (void *)vmm_vmexit_count, 0, vmm_sysctl_stats, "QU",
    "Number of hardware VM exits");
SYSCTL_PROC(_hw_vmm_stats, OID_AUTO, vcpu_run_return,
    CTLTYPE_U64 | CTLFLAG_RD, (void *)vmm_vcpu_run_return_count, 0,
    vmm_sysctl_stats, "QU", "Number of vmm_vcpu_run returns after VM entry");

static int
vmm_sysctl_stats(SYSCTL_HANDLER_ARGS)
{
	uint64_t *counts = arg1;
	uint64_t total;
	int cpu;

	total = 0;
	for (cpu = 0; cpu < ncpus; ++cpu)
		total += atomic_load_acq_64(&counts[cpu]);
	return sysctl_handle_64(oidp, &total, 0, req);
}

void
vmm_stat_vmexit(void)
{
	atomic_add_64(&vmm_vmexit_count[mycpu->gd_cpuid], 1);
}

void
vmm_stat_vcpu_run_return(void)
{
	atomic_add_64(&vmm_vcpu_run_return_count[mycpu->gd_cpuid], 1);
}

int
vmm_x64_get_capability(struct vmm_x64_capability *capability)
{
	const struct vmm_backend_ops *backend;

	if (capability == NULL)
		return EINVAL;
	backend = vmm_backend;
	if (backend == NULL)
		return ENXIO;
	return backend->capability(capability);
}

int
vmm_x64_get_supported_cpuid(struct vmm_cpuid_entry *entries,
	size_t *entry_count)
{
	const struct vmm_backend_ops *backend;

	if (entry_count == NULL)
		return EINVAL;
	backend = vmm_backend;
	if (backend == NULL || backend->get_supported_cpuid == NULL)
		return ENXIO;
	return backend->get_supported_cpuid(entries, entry_count);
}

static int
vmm_modevent(module_t module, int event, void *arg)
{
	const struct vmm_backend_ops **ops;
	const struct vmm_backend_ops *backend;
	int error;

	(void)module;
	(void)arg;

	switch (event) {
	case MOD_LOAD:
		lwkt_token_init(&vmm_token, "vmm");
		backend = NULL;
		vmm_machine_count = 0;
		vmm_draining = false;
		bzero(vmm_vmexit_count, sizeof(vmm_vmexit_count));
		bzero(vmm_vcpu_run_return_count, sizeof(vmm_vcpu_run_return_count));
		error = ENXIO;
		SET_FOREACH(ops, vmm_backend_set) {
			error = (*ops)->probe();
			if (error != 0)
				continue;
			error = (*ops)->init();
			if (error != 0) {
				kprintf("vmm: %s backend init failed (%d)\n",
				    (*ops)->name, error);
				return error;
			}
			backend = *ops;
			break;
		}
		if (backend == NULL) {
			kprintf("vmm: no usable backend (%d)\n", error);
			return error;
		}
		vmm_backend = backend;
		kprintf("vmm: selected %s backend\n", backend->name);
		return 0;
	case MOD_UNLOAD:
		lwkt_gettoken(&vmm_token);
		if (vmm_machine_count != 0) {
			lwkt_reltoken(&vmm_token);
			return EBUSY;
		}
		vmm_draining = true;
		backend = vmm_backend;
		lwkt_reltoken(&vmm_token);
		if (backend != NULL)
			backend->fini();
		return 0;
	case MOD_SHUTDOWN:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static moduledata_t vmm_mod = {
	.name = "vmm",
	.evhand = vmm_modevent,
	.priv = NULL,
};

DECLARE_MODULE(vmm, vmm_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(vmm, 1);
