/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM AVIC hardware delivery extension.
 *
 * AVIC is kept separate from the base SVM backend: it augments hardware
 * interrupt delivery and does not define guest platform devices.
 */
#ifndef VMM_X64_SVM_AVIC_H
#define VMM_X64_SVM_AVIC_H

struct pmap;
struct vmm_x64_svm_vmcb;
struct vmm_x64_svm_avic_machine;
struct vmm_x64_svm_avic_vcpu;

int vmm_x64_svm_avic_probe(void);
int vmm_x64_svm_avic_machine_create(struct vmm_x64_svm_avic_machine **);
void vmm_x64_svm_avic_machine_destroy(struct vmm_x64_svm_avic_machine *);
int vmm_x64_svm_avic_machine_pmap_init(struct vmm_x64_svm_avic_machine *,
	struct pmap *);
int vmm_x64_svm_avic_vcpu_create(struct vmm_x64_svm_avic_machine *,
	struct vmm_x64_svm_vmcb *, uint32_t,
	struct vmm_x64_svm_avic_vcpu **);
void vmm_x64_svm_avic_vcpu_destroy(struct vmm_x64_svm_avic_vcpu *);
void vmm_x64_svm_avic_vcpu_bind(struct vmm_x64_svm_avic_vcpu *);
void vmm_x64_svm_avic_vcpu_unbind(struct vmm_x64_svm_avic_vcpu *);
int vmm_x64_svm_avic_deliver(struct vmm_x64_svm_avic_vcpu *, uint8_t);

#endif /* VMM_X64_SVM_AVIC_H */
