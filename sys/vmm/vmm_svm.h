/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM backend for vmm_vcpu.c.
 */
#ifndef VMM_SVM_H
#define VMM_SVM_H

struct vmm_launch;
struct vmm_machine;
struct vmm_vcpu_thread;

int	vmm_svm_available(void);
int	vmm_svm_vcpu_create(struct vmm_machine *m,
	    const struct vmm_launch *launch, void **backendp);
void	vmm_svm_vcpu_destroy(void *backend);
void	vmm_svm_vcpu_run(void *backend, struct vmm_vcpu_thread *vc);

#endif /* VMM_SVM_H */
