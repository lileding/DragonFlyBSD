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
struct vmm_cpustate;
struct vmm_cpuexit;
struct vmm_svm_context;

/* The VMCB control area is a fixed AMD hardware ABI. */
struct vmm_svm_ctrl {
	uint32_t intercept_cr;
	uint32_t intercept_dr;
	uint32_t intercept_vec;
	uint32_t intercept_misc1;
	uint32_t intercept_misc2;
	uint32_t intercept_misc3;
	uint8_t reserved1[36];
	uint16_t pause_filt_thresh;
	uint16_t pause_filt_cnt;
	uint64_t iopm_base_pa;
	uint64_t msrpm_base_pa;
	uint64_t tsc_offset;
	uint32_t guest_asid;
	uint32_t tlb_ctrl;
	uint64_t v;
	uint64_t intr;
	uint64_t exitcode;
	uint64_t exitinfo1;
	uint64_t exitinfo2;
	uint64_t exitintinfo;
	uint64_t enable1;
	uint64_t avic;
	uint64_t ghcb;
	uint64_t eventinj;
	uint64_t n_cr3;
	uint64_t enable2;
	uint32_t vmcb_clean;
	uint32_t reserved2;
	uint64_t nrip;
	uint8_t inst_len;
	uint8_t inst_bytes[15];
	uint64_t avic_abpp;
	uint64_t reserved3;
	uint64_t avic_ltp;
	uint64_t avic_phys;
	uint64_t reserved4;
	uint64_t vmsa_ptr;
	uint8_t pad[752];
} __packed;

/* The VMCB save area is a fixed AMD hardware ABI. */
struct vmm_svm_segment {
	uint16_t selector;
	uint16_t attrib;
	uint32_t limit;
	uint64_t base;
} __packed;

struct vmm_svm_state {
	struct vmm_svm_segment es;
	struct vmm_svm_segment cs;
	struct vmm_svm_segment ss;
	struct vmm_svm_segment ds;
	struct vmm_svm_segment fs;
	struct vmm_svm_segment gs;
	struct vmm_svm_segment gdt;
	struct vmm_svm_segment ldt;
	struct vmm_svm_segment idt;
	struct vmm_svm_segment tr;
	uint8_t reserved1[43];
	uint8_t cpl;
	uint8_t reserved2[4];
	uint64_t efer;
	uint8_t reserved3[112];
	uint64_t cr4;
	uint64_t cr3;
	uint64_t cr0;
	uint64_t dr7;
	uint64_t dr6;
	uint64_t rflags;
	uint64_t rip;
	uint8_t reserved4[88];
	uint64_t rsp;
	uint64_t s_cet;
	uint64_t ssp;
	uint64_t isst_addr;
	uint64_t rax;
	uint64_t star;
	uint64_t lstar;
	uint64_t cstar;
	uint64_t sfmask;
	uint64_t kernelgsbase;
	uint64_t sysenter_cs;
	uint64_t sysenter_esp;
	uint64_t sysenter_eip;
	uint64_t cr2;
	uint8_t reserved5[32];
	uint64_t g_pat;
	uint64_t dbgctl;
	uint64_t br_from;
	uint64_t br_to;
	uint64_t int_from;
	uint64_t int_to;
	uint8_t pad[2408];
} __packed;

struct vmm_svm_vmcb {
	struct vmm_svm_ctrl ctrl;
	struct vmm_svm_state state;
} __packed;

int vmm_svm_probe(void);
int vmm_svm_init(void);
void vmm_svm_fini(void);
int vmm_svm_machine_create(struct vmm_machine *);
void vmm_svm_machine_destroy(struct vmm_machine *);
int vmm_svm_vcpu_create(struct vmm_vcpu *);
void vmm_svm_vcpu_destroy(struct vmm_vcpu *);
int vmm_svm_vcpu_run(struct vmm_vcpu *, struct vmm_cpuexit **);
void vmm_svm_vcpu_kick(struct vmm_vcpu *);
int vmm_svm_state_create(struct vmm_svm_vmcb *,
	const struct vmm_cpustate *, uint64_t *);
int vmm_svm_state_load(struct vmm_svm_vmcb *,
	const struct vmm_cpustate *, uint64_t *);
void vmm_svm_state_store(const struct vmm_svm_vmcb *,
	struct vmm_cpustate *, uint64_t);
void vmm_svm_vmrun(uint64_t, uint64_t *);
void vmm_svm_restore_tr(uint16_t);

#endif /* VMM_SVM_H */
