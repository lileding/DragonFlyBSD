/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM runtime backend.
 *
 * This is derived from the SVM hardware backend in DragonFly NVMM.  It owns
 * only SVM CPU state and execution resources; it has no NVMM owner, ioctl,
 * user mapping, or device-model dependency.
 */
#ifndef VMM_X64_SVM_H
#define VMM_X64_SVM_H

#include "../../vmm_backend.h"

extern const struct vmm_backend_ops vmm_x64_svm_backend;

#endif /* VMM_X64_SVM_H */
