/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unavailable SVM backend for no-hardware control-plane builds.
 */
#include <sys/types.h>
#include <sys/errno.h>

#include "vmm_svm.h"

int
vmm_svm_available(void)
{
	return 0;
}

int
vmm_svm_vcpu_create(struct vmm_machine *m, const struct vmm_launch *launch,
    void **backendp)
{
	(void)m;
	(void)launch;
	(void)backendp;
	return EOPNOTSUPP;
}

void
vmm_svm_vcpu_destroy(void *backend)
{
	(void)backend;
}

void
vmm_svm_vcpu_run(void *backend, struct vmm_vcpu_thread *vc)
{
	(void)backend;
	(void)vc;
}
