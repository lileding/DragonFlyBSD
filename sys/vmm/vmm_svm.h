/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM backend for vmm_vcpu.c.
 */
#ifndef VMM_SVM_H
#define VMM_SVM_H

struct vmm_vcpu_backend_ops;

extern const struct vmm_vcpu_backend_ops vmm_svm_backend_ops;

#endif /* VMM_SVM_H */
