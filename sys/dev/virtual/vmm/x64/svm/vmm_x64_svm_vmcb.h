/*-
 * Copyright (c) 2018-2021 Maxime Villard, m00nbsd.net
 * All rights reserved.
 *
 * Portions derive from DragonFly NVMM's AMD SVM backend.  See
 * vmm_x64_svm.c for the complete BSD license text.
 *
 * AMD SVM VMCB hardware layout.
 */
#ifndef VMM_X64_SVM_VMCB_H
#define VMM_X64_SVM_VMCB_H

#define VMM_X64_SVM_CTRL_V_INTR_MASKING	(1ULL << 24)
#define VMM_X64_SVM_CTRL_V_AVIC_ENABLE	(1ULL << 31)

struct vmm_x64_svm_ctrl {
	uint32_t intercept_cr;
	uint32_t intercept_dr;
	uint32_t intercept_vec;
	uint32_t intercept_misc1;
	uint32_t intercept_misc2;
	uint32_t intercept_misc3;
	uint8_t reserved1[36];
	uint16_t pause_filter_threshold;
	uint16_t pause_filter_count;
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
	uint32_t clean;
	uint32_t reserved2;
	uint64_t nrip;
	uint8_t inst_len;
	uint8_t inst_bytes[15];
	uint64_t avic_backing_page_pa;
	uint64_t reserved3;
	uint64_t avic_logical_table_pa;
	uint64_t avic_physical_table;
	uint64_t reserved4;
	uint64_t vmsa_pa;
	uint8_t pad[752];
} __packed;

CTASSERT(sizeof(struct vmm_x64_svm_ctrl) == 0x400);
CTASSERT(__offsetof(struct vmm_x64_svm_ctrl, avic) == 0x98);
CTASSERT(__offsetof(struct vmm_x64_svm_ctrl, n_cr3) == 0xb0);
CTASSERT(__offsetof(struct vmm_x64_svm_ctrl, avic_backing_page_pa) == 0x0e0);

struct vmm_x64_svm_segment {
	uint16_t selector;
	uint16_t attrib;
	uint32_t limit;
	uint64_t base;
} __packed;

CTASSERT(sizeof(struct vmm_x64_svm_segment) == 16);

struct vmm_x64_svm_state {
	struct vmm_x64_svm_segment es;
	struct vmm_x64_svm_segment cs;
	struct vmm_x64_svm_segment ss;
	struct vmm_x64_svm_segment ds;
	struct vmm_x64_svm_segment fs;
	struct vmm_x64_svm_segment gs;
	struct vmm_x64_svm_segment gdt;
	struct vmm_x64_svm_segment ldt;
	struct vmm_x64_svm_segment idt;
	struct vmm_x64_svm_segment tr;
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
	uint64_t pat;
	uint64_t dbgctl;
	uint64_t br_from;
	uint64_t br_to;
	uint64_t int_from;
	uint64_t int_to;
	uint8_t pad[2408];
} __packed;

CTASSERT(sizeof(struct vmm_x64_svm_state) == 0xc00);

struct vmm_x64_svm_vmcb {
	struct vmm_x64_svm_ctrl ctrl;
	struct vmm_x64_svm_state state;
} __packed;

CTASSERT(sizeof(struct vmm_x64_svm_vmcb) == PAGE_SIZE);
CTASSERT(__offsetof(struct vmm_x64_svm_vmcb, state) == 0x400);

#endif /* VMM_X64_SVM_VMCB_H */
