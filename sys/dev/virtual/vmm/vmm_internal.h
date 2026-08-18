/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM module-private backend lifetime state.
 */
#ifndef VMM_INTERNAL_H
#define VMM_INTERNAL_H

#include <sys/thread.h>

#include "vmm_backend.h"

extern const struct vmm_backend_ops *vmm_backend;
extern struct lwkt_token vmm_token;
extern int vmm_machine_count;
extern bool vmm_draining;

void vmm_stat_vmexit(void);
void vmm_stat_vcpu_run_return(void);
void vmm_stat_vcpu_run_restart_preentry(uint32_t);
void vmm_stat_vcpu_run_restart_postexit(uint32_t);

#endif /* VMM_INTERNAL_H */
