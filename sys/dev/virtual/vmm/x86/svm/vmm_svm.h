/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM core backend interface.
 */
#ifndef VMM_SVM_H
#define VMM_SVM_H

#include <sys/types.h>

struct vmm_machine;
struct vmm_vcpu;
struct vmm_cpuexit;

int vmm_svm_probe(void);
int vmm_svm_init(void);
void vmm_svm_fini(void);
int vmm_svm_machine_create(struct vmm_machine *);
void vmm_svm_machine_destroy(struct vmm_machine *);
int vmm_svm_vcpu_create(struct vmm_vcpu *);
void vmm_svm_vcpu_destroy(struct vmm_vcpu *);
int vmm_svm_vcpu_run(struct vmm_vcpu *, struct vmm_cpuexit **);
void vmm_svm_vcpu_kick(struct vmm_vcpu *);
void vmm_svm_vmrun(uint64_t, uint64_t *);
void vmm_svm_restore_tr(uint16_t);

#endif /* VMM_SVM_H */
