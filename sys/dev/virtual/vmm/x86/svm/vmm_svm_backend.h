/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM runtime backend.
 *
 * This is derived from the SVM hardware backend in DragonFly NVMM.  It owns
 * only SVM CPU state and execution resources; it has no NVMM owner, ioctl,
 * user mapping, or device-model dependency.
 */
#ifndef VMM_SVM_BACKEND_H
#define VMM_SVM_BACKEND_H

#include "../../vmm_backend.h"

extern const struct vmm_backend_ops vmm_svm_backend;

#endif /* VMM_SVM_BACKEND_H */
