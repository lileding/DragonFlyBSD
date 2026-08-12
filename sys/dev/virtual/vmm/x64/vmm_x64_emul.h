/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private x86 memory-instruction emulation for VMM MMIO exits.
 */
#ifndef VMM_X64_EMUL_H
#define VMM_X64_EMUL_H

#include <sys/types.h>

struct vmm_vcpu;
struct vmm_cpuexit;

int vmm_x64_emul_init(struct vmm_vcpu *);
void vmm_x64_emul_uninit(struct vmm_vcpu *);
int vmm_x64_emul_memory(struct vmm_vcpu *, const struct vmm_cpuexit *);
int vmm_x64_emul_resume(struct vmm_vcpu *);
int vmm_x64_emul_complete_write(struct vmm_vcpu *);
int vmm_x64_emul_complete_read(struct vmm_vcpu *, const void *, size_t);
int vmm_x64_translate(struct vmm_vcpu *, uint64_t, uint64_t *);

#endif /* VMM_X64_EMUL_H */
