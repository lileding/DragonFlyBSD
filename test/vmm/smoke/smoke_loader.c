/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Minimal fd3/fd4 loader for pc64 SVM smoke tests.
 */
#include <sys/mman.h>
#include <sys/stat.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PAGE_SIZE_GUEST	4096ULL
#define PML4_GPA	0x2000ULL
#define PDPT_GPA	0x3000ULL
#define PD_GPA		0x4000ULL
#define GDT_GPA		0x5000ULL
#define TSS_GPA		0x6000ULL
#define IDT_GPA		0x6800ULL
#define IOAPIC_PD_GPA	0x7000ULL
#define ENTRY_GPA	0x100000ULL
#define TIMER_HANDLER_GPA (ENTRY_GPA + 0x80ULL)
#define SERIAL_HANDLER_GPA (ENTRY_GPA + 0x80ULL)
#define UD_HANDLER_GPA	(ENTRY_GPA + 0x300ULL)
#define AVIC_HANDLER_GPA (ENTRY_GPA + 0x300ULL)
#define PM64_LONG_GPA	(ENTRY_GPA + 0x80ULL)
#define STACK_GPA	0x180000ULL
#define IOAPIC_GPA	0xfec00000ULL
#define APIC_GPA	0xfee00000ULL
#define IOAPIC_PDPT_INDEX	((IOAPIC_GPA >> 30) & 0x1ffULL)
#define IOAPIC_PD_INDEX	((IOAPIC_GPA >> 21) & 0x1ffULL)
#define APIC_PD_INDEX	((APIC_GPA >> 21) & 0x1ffULL)
#define APIC_REG_EOI	0x0b0U
#define APIC_REG_LVTT	0x320U
#define APIC_REG_LDR	0x0d0U
#define APIC_REG_TMICT	0x380U
#define APIC_REG_TDCR	0x3e0U
#define APIC_REG_ICR_LOW 0x300U
#define APIC_REG_ICR_HIGH 0x310U
#define IOAPIC_VERSION	0x00170011U
#define IOAPIC_MASKED_VECTOR32	0x00010020U

#define VMM_MANIFEST_MAGIC	"VMMLD0\0\0"
#define VMM_MANIFEST_ARCH_X64	1
#define VMM_REC_X64_VCPU_STATE	1
#define VMM_REC_GPA_RANGE	2
#define VMM_REC_F_MANDATORY	1

#define VMM_X64_NGPR	18
#define VMM_X64_NCR	6
#define VMM_X64_NMSR	11
#define VMM_X64_NSEG	10
#define VMM_X64_GPR_RSP	4
#define VMM_X64_GPR_RIP	16
#define VMM_X64_GPR_RFLAGS	17
#define VMM_X64_CR_CR0	0
#define VMM_X64_CR_CR3	2
#define VMM_X64_CR_CR4	3
#define VMM_X64_CR_XCR0	5
#define VMM_X64_MSR_EFER	0
#define VMM_X64_MSR_PAT	9
#define VMM_X64_SEG_ES	0
#define VMM_X64_SEG_CS	1
#define VMM_X64_SEG_SS	2
#define VMM_X64_SEG_DS	3
#define VMM_X64_SEG_FS	4
#define VMM_X64_SEG_GS	5
#define VMM_X64_SEG_GDT	6
#define VMM_X64_SEG_IDT	7
#define VMM_X64_SEG_LDT	8
#define VMM_X64_SEG_TR	9

#define CR0_PE		0x00000001ULL
#define CR0_NE		0x00000020ULL
#define CR0_PG		0x80000000ULL
#define CR4_PAE		0x00000020ULL
#define EFER_LME	0x00000100ULL
#define EFER_LMA	0x00000400ULL
#define XCR0_X87	0x00000001ULL
#define SEG_S		0x0010U
#define SEG_P		0x0080U
#define SEG_L		0x0200U
#define SEG_DB		0x0400U
#define SEG_G		0x0800U
#define SEG_UNUSABLE	0x1000U
#define SERIAL_VECTOR	0x24U
#define TIMER_VECTOR	0x2eU
#define AVIC_VECTOR	0x40U
#define UD_VECTOR	6U
#define AVIC_MAGIC	0x43495641U
#define AVIC_OP_DELIVER	1U
#define AVIC_OP_MARKER	2U
#define AVIC_MARKER	0xa51c0040U
#define MSR_AMD_PATCH_LEVEL	0x0000008bU
#define MSR_MTRR_CAP	0x000000feU
#define MSR_SYSCFG	0xc0010010U
#define MSR_K7_HWCR	0xc0010015U
#define HWCR_SMOKE_VALUE ((1U << 24) | (1U << 18) | 0x148U)
#define PCI_CFG_ADDR_PORT	0x0cf8U
#define PCI_CFG_DATA_PORT	0x0cfcU
#define PIT_CH2_PORT		0x0042U
#define PIT_CMD_PORT		0x0043U
#define PIT_PORTB		0x0061U
#define CMOS_INDEX_PORT		0x0070U
#define CMOS_DATA_PORT		0x0071U

struct vmm_manifest_header {
	char		magic[8];
	uint16_t	abi_version;
	uint16_t	arch;
	uint32_t	header_size;
	uint32_t	total_size;
	uint32_t	record_count;
	uint64_t	mem_size;
	uint32_t	flags;
	uint32_t	reserved;
} __attribute__((packed));

struct vmm_manifest_record {
	uint16_t	type;
	uint16_t	flags;
	uint32_t	size;
} __attribute__((packed));

struct vmm_x64_seg_state {
	uint16_t	selector;
	uint16_t	attrib;
	uint32_t	limit;
	uint64_t	base;
} __attribute__((packed));

struct vmm_x64_vcpu_state {
	uint32_t	vcpu_id;
	uint32_t	flags;
	uint64_t	runnable;
	uint64_t	gpr[VMM_X64_NGPR];
	uint64_t	cr[VMM_X64_NCR];
	uint64_t	msr[VMM_X64_NMSR];
	struct vmm_x64_seg_state seg[VMM_X64_NSEG];
	uint64_t	intr_flags;
} __attribute__((packed));

struct vmm_gpa_range {
	uint64_t	start;
	uint64_t	size;
	uint32_t	type;
	uint32_t	flags;
} __attribute__((packed));

struct x64_idt_gate {
	uint16_t	offset_low;
	uint16_t	selector;
	uint8_t		ist;
	uint8_t		type_attr;
	uint16_t	offset_mid;
	uint32_t	offset_high;
	uint32_t	zero;
} __attribute__((packed));

static size_t
align8(size_t value)
{
	return (value + 7U) & ~(size_t)7U;
}

static void
write64(uint8_t *mem, uint64_t offset, uint64_t value)
{
	memcpy(mem + offset, &value, sizeof(value));
}

static void
write_idt_gate(uint8_t *mem, unsigned int vector, uint64_t target)
{
	struct x64_idt_gate gate;

	memset(&gate, 0, sizeof(gate));
	gate.offset_low = target & 0xffffU;
	gate.selector = 0x08;
	gate.type_attr = 0x8e;
	gate.offset_mid = (target >> 16) & 0xffffU;
	gate.offset_high = (target >> 32) & 0xffffffffU;
	memcpy(mem + IDT_GPA + vector * sizeof(gate), &gate, sizeof(gate));
}

static void
set_segment(struct vmm_x64_seg_state *seg, uint16_t selector,
    uint16_t attrib, uint32_t limit, uint64_t base)
{
	seg->selector = selector;
	seg->attrib = attrib;
	seg->limit = limit;
	seg->base = base;
}

static uint8_t *
add_record(uint8_t *ptr, uint16_t type, const void *payload, uint32_t size)
{
	struct vmm_manifest_record rec;
	size_t total;

	rec.type = type;
	rec.flags = VMM_REC_F_MANDATORY;
	rec.size = size;
	total = align8(sizeof(rec) + size);
	memcpy(ptr, &rec, sizeof(rec));
	memcpy(ptr + sizeof(rec), payload, size);
	memset(ptr + sizeof(rec) + size, 0, total - sizeof(rec) - size);
	return ptr + total;
}

static void
emit(uint8_t *code, size_t *len, size_t cap, const uint8_t *bytes, size_t n)
{
	if (*len + n > cap)
		errx(1, "guest code buffer too small");
	memcpy(code + *len, bytes, n);
	*len += n;
}

static void
emit_mov_dx(uint8_t *code, size_t *len, size_t cap, uint16_t port)
{
	uint8_t bytes[] = { 0xba, port & 0xffU, port >> 8 };

	emit(code, len, cap, bytes, sizeof(bytes));
}

static void
emit_mov_al(uint8_t *code, size_t *len, size_t cap, uint8_t val)
{
	uint8_t bytes[] = { 0xb0, val };

	emit(code, len, cap, bytes, sizeof(bytes));
}

static void
emit_out_dx_al(uint8_t *code, size_t *len, size_t cap)
{
	static const uint8_t bytes[] = { 0xee };

	emit(code, len, cap, bytes, sizeof(bytes));
}

static void
emit_outb(uint8_t *code, size_t *len, size_t cap, uint16_t port, uint8_t val)
{
	emit_mov_dx(code, len, cap, port);
	emit_mov_al(code, len, cap, val);
	emit_out_dx_al(code, len, cap);
}

static size_t
emit_jne32(uint8_t *code, size_t *len, size_t cap)
{
	static const uint8_t bytes[] = { 0x0f, 0x85, 0x00, 0x00, 0x00, 0x00 };
	size_t disp;

	emit(code, len, cap, bytes, sizeof(bytes));
	disp = *len - 4;
	return disp;
}

static size_t
emit_je8(uint8_t *code, size_t *len, size_t cap)
{
	static const uint8_t bytes[] = { 0x74, 0x00 };
	size_t disp;

	emit(code, len, cap, bytes, sizeof(bytes));
	disp = *len - 1;
	return disp;
}

static void
patch_rel8(uint8_t *code, size_t disp, size_t target)
{
	int64_t rel = (int64_t)target - (int64_t)(disp + 1);

	if (rel < -128 || rel > 127)
		errx(1, "guest branch target is out of rel8 range");
	code[disp] = (uint8_t)rel;
}

static void
patch_rel32(uint8_t *code, size_t disp, size_t target)
{
	int64_t rel = (int64_t)target - (int64_t)(disp + 4);
	uint32_t urel;

	if (rel < -2147483648LL || rel > 2147483647LL)
		errx(1, "guest branch target is out of rel32 range");
	urel = (uint32_t)rel;
	code[disp] = urel & 0xffU;
	code[disp + 1] = (urel >> 8) & 0xffU;
	code[disp + 2] = (urel >> 16) & 0xffU;
	code[disp + 3] = (urel >> 24) & 0xffU;
}

static void
emit_u32(uint8_t *code, size_t *len, size_t cap, uint32_t val)
{
	uint8_t bytes[] = {
	    val & 0xffU, (val >> 8) & 0xffU, (val >> 16) & 0xffU,
	    (val >> 24) & 0xffU
	};

	emit(code, len, cap, bytes, sizeof(bytes));
}

static void
emit_u16(uint8_t *code, size_t *len, size_t cap, uint16_t val)
{
	uint8_t bytes[] = { val & 0xffU, (val >> 8) & 0xffU };

	emit(code, len, cap, bytes, sizeof(bytes));
}

static void
emit_mov_eax(uint8_t *code, size_t *len, size_t cap, uint32_t val)
{
	static const uint8_t op[] = { 0xb8 };

	emit(code, len, cap, op, sizeof(op));
	emit_u32(code, len, cap, val);
}

static void
emit_cmp_eax(uint8_t *code, size_t *len, size_t cap, uint32_t val)
{
	static const uint8_t op[] = { 0x3d };

	emit(code, len, cap, op, sizeof(op));
	emit_u32(code, len, cap, val);
}

static void
emit_serial_char(uint8_t *code, size_t *len, size_t cap, uint8_t ch)
{
	static const uint8_t wait_lsr[] = {
		0xba, 0xfd, 0x03,	/* mov dx,0x3fd */
		0xec,			/* in al,dx */
		0xa8, 0x20,		/* test al,0x20 */
		0x74, 0xfb		/* jz in */
	};

	emit(code, len, cap, wait_lsr, sizeof(wait_lsr));
	emit_outb(code, len, cap, 0x3f8, ch);
}

static size_t
guest_serial_code(uint8_t *code, size_t cap)
{
	static const char msg[] = "dfvmm-serial-ok\n";
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	size_t len = 0;
	size_t i;

	emit_outb(code, &len, cap, 0x3f9, 0x00);	/* IER: interrupts off */
	emit_outb(code, &len, cap, 0x3fb, 0x80);	/* LCR: DLAB on */
	emit_outb(code, &len, cap, 0x3f8, 0x03);	/* DLL: 38400 */
	emit_outb(code, &len, cap, 0x3f9, 0x00);	/* DLM */
	emit_outb(code, &len, cap, 0x3fb, 0x03);	/* LCR: 8N1, DLAB off */
	emit_outb(code, &len, cap, 0x3fc, 0x03);	/* MCR: DTR + RTS */
	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	return len;
}

static size_t
guest_serialin_code(uint8_t *code, size_t cap)
{
	static const uint8_t setup[] = {
	    0xba, 0xfd, 0x03,	/* mov dx,0x3fd */
	    0xec,		/* in al,dx */
	    0xa8, 0x01,		/* test al,0x01 */
	    0x74, 0xfb,		/* jz in */
	    0xba, 0xf8, 0x03,	/* mov dx,0x3f8 */
	    0xec,		/* in al,dx */
	    0x3c, 0x5a,		/* cmp al,'Z' */
	    0x74, 0x03,		/* je ok */
	    0xf4,		/* hlt */
	    0xeb, 0xfe		/* jmp . */
	};
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const char msg[] = "dfvmm-serialin-ok\n";
	size_t len = 0;
	size_t i;

	emit(code, &len, cap, setup, sizeof(setup));
	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	return len;
}

static size_t
guest_serialirq_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_ioapic[] = {
	    0xbf, 0x00, 0x00, 0xc0, 0xfe /* mov edi,0xfec00000 */
	};
	static const uint8_t mov_eax_to_rdi[] = {
	    0x89, 0x07		/* mov [rdi],eax */
	};
	static const uint8_t mov_eax_to_rdi_10[] = {
	    0x89, 0x47, 0x10	/* mov [rdi+0x10],eax */
	};
	static const uint8_t wait_irq[] = {
	    0xfb,		/* sti */
	    0xf4,		/* hlt */
	    0xeb, 0xfe		/* jmp . */
	};
	static const uint8_t handler[] = {
	    0xba, 0xf8, 0x03,	/* mov dx,0x3f8 */
	    0xec,		/* in al,dx */
	    0x3c, 0x51,		/* cmp al,'Q' */
	    0x74, 0x03,		/* je ok */
	    0xf4,		/* hlt */
	    0xeb, 0xfe		/* jmp . */
	};
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const char msg[] = "dfvmm-serialirq-ok\n";
	size_t len = 0;
	size_t i;

	emit(code, &len, cap, mov_edi_ioapic, sizeof(mov_edi_ioapic));
	emit_mov_eax(code, &len, cap, 0x19);
	emit(code, &len, cap, mov_eax_to_rdi, sizeof(mov_eax_to_rdi));
	emit_mov_eax(code, &len, cap, 0);
	emit(code, &len, cap, mov_eax_to_rdi_10,
	    sizeof(mov_eax_to_rdi_10));
	emit_mov_eax(code, &len, cap, 0x18);
	emit(code, &len, cap, mov_eax_to_rdi, sizeof(mov_eax_to_rdi));
	emit_mov_eax(code, &len, cap, SERIAL_VECTOR);
	emit(code, &len, cap, mov_eax_to_rdi_10,
	    sizeof(mov_eax_to_rdi_10));
	emit_outb(code, &len, cap, 0x3fc, 0x0b);
	emit_outb(code, &len, cap, 0x3f9, 0x01);
	emit(code, &len, cap, wait_irq, sizeof(wait_irq));
	while (len < SERIAL_HANDLER_GPA - ENTRY_GPA) {
		static const uint8_t nop[] = { 0x90 };

		emit(code, &len, cap, nop, sizeof(nop));
	}
	emit(code, &len, cap, handler, sizeof(handler));
	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	return len;
}

static size_t
guest_timer_code(uint8_t *code, size_t cap)
{
	static const uint8_t setup[] = {
	    0xfb,		/* sti */
	    0xb9, 0x1b, 0x00, 0x00, 0x00, /* mov ecx,MSR_APICBASE */
	    0x0f, 0x32,		/* rdmsr */
	    0x0d, 0x00, 0x0d, 0x00, 0x00, /* or eax,BSP|X2APIC|EN */
	    0x0f, 0x30,		/* wrmsr */
	    0xb9, 0x32, 0x08, 0x00, 0x00, /* mov ecx,x2APIC LVT timer */
	    0xb8, 0x2e, 0x00, 0x04, 0x00, /* mov eax,0x4002e */
	    0x31, 0xd2,		/* xor edx,edx */
	    0x0f, 0x30,		/* wrmsr */
	    0xb9, 0xe0, 0x06, 0x00, 0x00, /* mov ecx,MSR_TSC_DEADLINE */
	    0xb8, 0x01, 0x00, 0x00, 0x00, /* mov eax,1 */
	    0x31, 0xd2,		/* xor edx,edx */
	    0x0f, 0x30,		/* wrmsr */
	    0xf4,		/* hlt */
	    0xeb, 0xfe		/* jmp . */
	};
	static const uint8_t handler[] = { 0x0f, 0x01, 0xd9 };
	size_t len = 0;

	emit(code, &len, cap, setup, sizeof(setup));
	while (len < TIMER_HANDLER_GPA - ENTRY_GPA) {
		static const uint8_t nop[] = { 0x90 };

		emit(code, &len, cap, nop, sizeof(nop));
	}
	emit(code, &len, cap, handler, sizeof(handler));
	return len;
}

static size_t
guest_lapictimer_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_tdcr[] =
	    { 0x89, 0x87, 0xe0, 0x03, 0x00, 0x00 };
	static const uint8_t mov_eax_to_lvtt[] =
	    { 0x89, 0x87, 0x20, 0x03, 0x00, 0x00 };
	static const uint8_t mov_eax_to_tmict[] =
	    { 0x89, 0x87, 0x80, 0x03, 0x00, 0x00 };
	static const uint8_t mov_eax_to_eoi[] =
	    { 0x89, 0x87, 0xb0, 0x00, 0x00, 0x00 };
	static const uint8_t sti_hlt_loop[] = { 0xfb, 0xf4, 0xeb, 0xfe };
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const char msg[] = "dfvmm-lapic-timer-ok\n";
	size_t len = 0;
	size_t i;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 0x0000000bU);
	emit(code, &len, cap, mov_eax_to_tdcr, sizeof(mov_eax_to_tdcr));
	emit_mov_eax(code, &len, cap, TIMER_VECTOR);
	emit(code, &len, cap, mov_eax_to_lvtt, sizeof(mov_eax_to_lvtt));
	emit_mov_eax(code, &len, cap, 0x00000100U);
	emit(code, &len, cap, mov_eax_to_tmict, sizeof(mov_eax_to_tmict));
	emit(code, &len, cap, sti_hlt_loop, sizeof(sti_hlt_loop));
	while (len < TIMER_HANDLER_GPA - ENTRY_GPA) {
		static const uint8_t nop[] = { 0x90 };

		emit(code, &len, cap, nop, sizeof(nop));
	}
	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 0);
	emit(code, &len, cap, mov_eax_to_eoi, sizeof(mov_eax_to_eoi));
	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	return len;
}

static size_t
guest_ud_code(uint8_t *code, size_t cap)
{
	static const uint8_t setup[] = {
	    0x0f, 0x0b		/* ud2 */
	};
	static const uint8_t fixup[] = {
	    0x48, 0x83, 0x04, 0x24, 0x02 /* addq $2,(%rsp) */
	};
	static const char handler_msg[] = "dfvmm-ud-ok\n";
	static const char return_msg[] = "dfvmm-iret-ok\n";
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const uint8_t iretq[] = { 0x48, 0xcf };
	size_t len = 0;
	size_t i;

	emit(code, &len, cap, setup, sizeof(setup));
	for (i = 0; i < sizeof(return_msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)return_msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	while (len < UD_HANDLER_GPA - ENTRY_GPA) {
		static const uint8_t nop[] = { 0x90 };

		emit(code, &len, cap, nop, sizeof(nop));
	}
	emit(code, &len, cap, fixup, sizeof(fixup));
	for (i = 0; i < sizeof(handler_msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)handler_msg[i]);
	emit(code, &len, cap, iretq, sizeof(iretq));
	return len;
}


static size_t
guest_avicirq_code(uint8_t *code, size_t cap)
{
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const uint8_t sti_hlt_loop[] = {
	    0xfb, 0xf4, 0xeb, 0xfe
	};
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, (const uint8_t[]){ 0xfa }, 1);
	emit_mov_eax(code, &len, cap, AVIC_MAGIC);
	emit(code, &len, cap, (const uint8_t[]){ 0xbb }, 1);
	emit_u32(code, &len, cap, AVIC_OP_DELIVER);
	emit(code, &len, cap, (const uint8_t[]){ 0xb9 }, 1);
	emit_u32(code, &len, cap, AVIC_VECTOR);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	emit(code, &len, cap, sti_hlt_loop, sizeof(sti_hlt_loop));
	while (len < AVIC_HANDLER_GPA - ENTRY_GPA) {
		static const uint8_t nop[] = { 0x90 };

		emit(code, &len, cap, nop, sizeof(nop));
	}
	emit_mov_eax(code, &len, cap, AVIC_MAGIC);
	emit(code, &len, cap, (const uint8_t[]){ 0xbb }, 1);
	emit_u32(code, &len, cap, AVIC_OP_MARKER);
	emit(code, &len, cap, (const uint8_t[]){ 0xb9 }, 1);
	emit_u32(code, &len, cap, AVIC_MARKER);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_avicipi_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_icr_high[] =
	    { 0x89, 0x87, 0x10, 0x03, 0x00, 0x00 };
	static const uint8_t mov_eax_to_icr_low[] =
	    { 0x89, 0x87, 0x00, 0x03, 0x00, 0x00 };
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 1U << 24);
	emit(code, &len, cap, mov_eax_to_icr_high,
	    sizeof(mov_eax_to_icr_high));
	emit_mov_eax(code, &len, cap, 0x00004041U);
	emit(code, &len, cap, mov_eax_to_icr_low, sizeof(mov_eax_to_icr_low));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_aviclvt_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_lvt_error[] =
	    { 0x89, 0x87, 0x70, 0x03, 0x00, 0x00 };
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 0x000100feU);
	emit(code, &len, cap, mov_eax_to_lvt_error,
	    sizeof(mov_eax_to_lvt_error));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_avictimercfg_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_lvtt[] =
	    { 0x89, 0x87, 0x20, 0x03, 0x00, 0x00 };
	static const uint8_t mov_eax_to_tmict[] =
	    { 0x89, 0x87, 0x80, 0x03, 0x00, 0x00 };
	static const uint8_t mov_eax_to_tdcr[] =
	    { 0x89, 0x87, 0xe0, 0x03, 0x00, 0x00 };
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 0x0001002eU);
	emit(code, &len, cap, mov_eax_to_lvtt, sizeof(mov_eax_to_lvtt));
	emit_mov_eax(code, &len, cap, 0x0000000bU);
	emit(code, &len, cap, mov_eax_to_tdcr, sizeof(mov_eax_to_tdcr));
	emit_mov_eax(code, &len, cap, 0x00000100U);
	emit(code, &len, cap, mov_eax_to_tmict, sizeof(mov_eax_to_tmict));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_aviclint_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_lvt0[] =
	    { 0x89, 0x87, 0x50, 0x03, 0x00, 0x00 };
	static const uint8_t mov_eax_to_lvt1[] =
	    { 0x89, 0x87, 0x60, 0x03, 0x00, 0x00 };
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 0x00018720U);
	emit(code, &len, cap, mov_eax_to_lvt0, sizeof(mov_eax_to_lvt0));
	emit_mov_eax(code, &len, cap, 0x00010400U);
	emit(code, &len, cap, mov_eax_to_lvt1, sizeof(mov_eax_to_lvt1));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_aviclvtpc_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_lvt_thermal[] =
	    { 0x89, 0x87, 0x30, 0x03, 0x00, 0x00 };
	static const uint8_t mov_eax_to_lvt_pc[] =
	    { 0x89, 0x87, 0x40, 0x03, 0x00, 0x00 };
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 0x00010430U);
	emit(code, &len, cap, mov_eax_to_lvt_thermal,
	    sizeof(mov_eax_to_lvt_thermal));
	emit_mov_eax(code, &len, cap, 0x00010431U);
	emit(code, &len, cap, mov_eax_to_lvt_pc,
	    sizeof(mov_eax_to_lvt_pc));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_avicesr_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_esr[] =
	    { 0x89, 0x87, 0x80, 0x02, 0x00, 0x00 };
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 0);
	emit(code, &len, cap, mov_eax_to_esr, sizeof(mov_eax_to_esr));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_avicsvr_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_svr[] =
	    { 0x89, 0x87, 0xf0, 0x00, 0x00, 0x00 };
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 0x3ffU);
	emit(code, &len, cap, mov_eax_to_svr, sizeof(mov_eax_to_svr));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_avicnoaccel_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_apic[] = { 0xbf, 0x00, 0x00, 0xe0, 0xfe };
	static const uint8_t mov_eax_to_ldr[] =
	    { 0x89, 0x87, 0xd0, 0x00, 0x00, 0x00 };
	static const uint8_t hlt_loop[] = { 0xf4, 0xeb, 0xfe };
	size_t len = 0;

	emit(code, &len, cap, mov_edi_apic, sizeof(mov_edi_apic));
	emit_mov_eax(code, &len, cap, 1U << 24);
	emit(code, &len, cap, mov_eax_to_ldr, sizeof(mov_eax_to_ldr));
	emit(code, &len, cap, hlt_loop, sizeof(hlt_loop));
	return len;
}

static size_t
guest_pic_code(uint8_t *code, size_t cap)
{
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const char msg[] = "dfvmm-pic-ok\n";
	size_t len = 0;
	size_t i;

	emit_outb(code, &len, cap, 0x20, 0x11);
	emit_outb(code, &len, cap, 0xa0, 0x11);
	emit_outb(code, &len, cap, 0x21, 0x20);
	emit_outb(code, &len, cap, 0xa1, 0x28);
	emit_outb(code, &len, cap, 0x21, 0x04);
	emit_outb(code, &len, cap, 0xa1, 0x02);
	emit_outb(code, &len, cap, 0x21, 0x01);
	emit_outb(code, &len, cap, 0xa1, 0x01);
	emit_outb(code, &len, cap, 0x21, 0xff);
	emit_outb(code, &len, cap, 0xa1, 0xff);
	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	return len;
}

static size_t
guest_ioapic_code(uint8_t *code, size_t cap)
{
	static const uint8_t mov_edi_ioapic[] =
	    { 0xbf, 0x00, 0x00, 0xc0, 0xfe };
	static const uint8_t mov_eax_to_rdi[] = { 0x89, 0x07 };
	static const uint8_t mov_eax_to_rdi_10[] = { 0x89, 0x47, 0x10 };
	static const uint8_t mov_rdi_10_to_eax[] = { 0x8b, 0x47, 0x10 };
	static const uint8_t fail[] = { 0xf4, 0xeb, 0xfe };
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const char msg[] = "dfvmm-ioapic-ok\n";
	size_t len = 0;
	size_t fail_label;
	size_t jver;
	size_t jredir;
	size_t i;

	emit(code, &len, cap, mov_edi_ioapic, sizeof(mov_edi_ioapic));
	emit_mov_eax(code, &len, cap, 1);
	emit(code, &len, cap, mov_eax_to_rdi, sizeof(mov_eax_to_rdi));
	emit(code, &len, cap, mov_rdi_10_to_eax, sizeof(mov_rdi_10_to_eax));
	emit_cmp_eax(code, &len, cap, IOAPIC_VERSION);
	jver = emit_jne32(code, &len, cap);

	emit_mov_eax(code, &len, cap, 0x10);
	emit(code, &len, cap, mov_eax_to_rdi, sizeof(mov_eax_to_rdi));
	emit_mov_eax(code, &len, cap, IOAPIC_MASKED_VECTOR32);
	emit(code, &len, cap, mov_eax_to_rdi_10, sizeof(mov_eax_to_rdi_10));
	emit_mov_eax(code, &len, cap, 0x10);
	emit(code, &len, cap, mov_eax_to_rdi, sizeof(mov_eax_to_rdi));
	emit(code, &len, cap, mov_rdi_10_to_eax, sizeof(mov_rdi_10_to_eax));
	emit_cmp_eax(code, &len, cap, IOAPIC_MASKED_VECTOR32);
	jredir = emit_jne32(code, &len, cap);

	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));

	fail_label = len;
	emit(code, &len, cap, fail, sizeof(fail));
	patch_rel32(code, jver, fail_label);
	patch_rel32(code, jredir, fail_label);
	return len;
}

static size_t
guest_x2apic_code(uint8_t *code, size_t cap)
{
	static const uint8_t setup[] = {
	    0xb9, 0x1b, 0x00, 0x00, 0x00, /* mov ecx,MSR_APICBASE */
	    0x0f, 0x32,		/* rdmsr */
	    0x0d, 0x00, 0x0d, 0x00, 0x00, /* or eax,BSP|X2APIC|EN */
	    0x0f, 0x30,		/* wrmsr */
	    0xb9, 0x30, 0x08, 0x00, 0x00, /* mov ecx,x2APIC ICR */
	    0xb8, 0x00, 0x85, 0x08, 0x00, /* mov eax,0x88500 */
	    0x31, 0xd2,		/* xor edx,edx */
	    0x0f, 0x30,		/* wrmsr */
	    0x0f, 0x32,		/* rdmsr */
	    0xa9, 0x00, 0x10, 0x00, 0x00	/* test eax,0x1000 */
	};
	static const uint8_t fail[] = { 0xf4, 0xeb, 0xfe };
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const char msg[] = "dfvmm-x2apic-ok\n";
	size_t len = 0;
	size_t jok;
	size_t ok_label;
	size_t i;

	emit(code, &len, cap, setup, sizeof(setup));
	jok = emit_je8(code, &len, cap);
	emit(code, &len, cap, fail, sizeof(fail));
	ok_label = len;
	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	patch_rel8(code, jok, ok_label);
	return len;
}

static size_t
guest_cachetlb_code(uint8_t *code, size_t cap)
{
	static const uint8_t setup[] = {
	    0x0f, 0x08,		/* invd */
	    0x0f, 0x09,		/* wbinvd */
	    0x31, 0xc0,		/* xor eax,eax */
	    0x0f, 0x01, 0x38	/* invlpg (%rax) */
	};
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const char msg[] = "dfvmm-cachetlb-ok\n";
	size_t len = 0;
	size_t i;

	emit(code, &len, cap, setup, sizeof(setup));
	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	return len;
}

static size_t
guest_pm64_code(uint8_t *code, size_t cap)
{
	static const uint8_t bootstrap[] = {
	    0xfa,				/* cli */
	    0x0f, 0x20, 0xe0,			/* mov eax,cr4 */
	    0x83, 0xc8, 0x20,			/* or eax,CR4.PAE */
	    0x0f, 0x22, 0xe0,			/* mov cr4,eax */
	    0xb9, 0x80, 0x00, 0x00, 0xc0,	/* mov ecx,MSR_EFER */
	    0x0f, 0x32,				/* rdmsr */
	    0x0d, 0x00, 0x01, 0x00, 0x00,	/* or eax,EFER.LME */
	    0x0f, 0x30,				/* wrmsr */
	    0xb8				/* mov eax,PML4_GPA */
	};
	static const uint8_t set_cr3_cr0_ljmp[] = {
	    0x0f, 0x22, 0xd8,			/* mov cr3,eax */
	    0x0f, 0x20, 0xc0,			/* mov eax,cr0 */
	    0x0d, 0x00, 0x00, 0x00, 0x80,	/* or eax,CR0.PG */
	    0x0f, 0x22, 0xc0,			/* mov cr0,eax */
	    0xea				/* ljmp $0x20,$PM64_LONG_GPA */
	};
	static const char msg[] = "dfvmm-pm64-ok\n";
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	size_t len = 0;
	size_t i;

	emit(code, &len, cap, bootstrap, sizeof(bootstrap));
	emit_u32(code, &len, cap, PML4_GPA);
	emit(code, &len, cap, set_cr3_cr0_ljmp, sizeof(set_cr3_cr0_ljmp));
	emit_u32(code, &len, cap, PM64_LONG_GPA);
	emit_u16(code, &len, cap, 0x20);
	while (len < PM64_LONG_GPA - ENTRY_GPA) {
		static const uint8_t nop[] = { 0x90 };

		emit(code, &len, cap, nop, sizeof(nop));
	}
	for (i = 0; i < sizeof(msg) - 1; i++)
		emit_serial_char(code, &len, cap, (uint8_t)msg[i]);
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	return len;
}

static size_t
guest_msrpatch_code(uint8_t *code, size_t cap)
{
	static const uint8_t rdmsr[] = {
	    0xb9, MSR_AMD_PATCH_LEVEL & 0xffU,
	    (MSR_AMD_PATCH_LEVEL >> 8) & 0xffU,
	    (MSR_AMD_PATCH_LEVEL >> 16) & 0xffU,
	    (MSR_AMD_PATCH_LEVEL >> 24) & 0xffU,
	    0x0f, 0x32,		/* rdmsr */
	    0x0f, 0x01, 0xd9	/* vmmcall */
	};
	size_t len = 0;

	emit(code, &len, cap, rdmsr, sizeof(rdmsr));
	return len;
}

static size_t
guest_msrsyscfg_code(uint8_t *code, size_t cap)
{
	static const uint8_t rdmsr[] = {
	    0xb9, MSR_SYSCFG & 0xffU,
	    (MSR_SYSCFG >> 8) & 0xffU,
	    (MSR_SYSCFG >> 16) & 0xffU,
	    (MSR_SYSCFG >> 24) & 0xffU,
	    0x0f, 0x32,		/* rdmsr */
	    0x0f, 0x01, 0xd9	/* vmmcall */
	};
	size_t len = 0;

	emit(code, &len, cap, rdmsr, sizeof(rdmsr));
	return len;
}

static size_t
guest_mtrrcap_code(uint8_t *code, size_t cap)
{
	static const uint8_t rdmsr[] = {
	    0xb9, MSR_MTRR_CAP & 0xffU,
	    (MSR_MTRR_CAP >> 8) & 0xffU,
	    (MSR_MTRR_CAP >> 16) & 0xffU,
	    (MSR_MTRR_CAP >> 24) & 0xffU,
	    0x0f, 0x32,		/* rdmsr */
	    0x0f, 0x01, 0xd9	/* vmmcall */
	};
	size_t len = 0;

	emit(code, &len, cap, rdmsr, sizeof(rdmsr));
	return len;
}

static size_t
guest_msrhwcr_code(uint8_t *code, size_t cap)
{
	static const uint8_t rdmsr[] = {
	    0xb9, MSR_K7_HWCR & 0xffU,
	    (MSR_K7_HWCR >> 8) & 0xffU,
	    (MSR_K7_HWCR >> 16) & 0xffU,
	    (MSR_K7_HWCR >> 24) & 0xffU,
	    0x0f, 0x32		/* rdmsr */
	};
	static const uint8_t wrmsr[] = {
	    0xb9, MSR_K7_HWCR & 0xffU,
	    (MSR_K7_HWCR >> 8) & 0xffU,
	    (MSR_K7_HWCR >> 16) & 0xffU,
	    (MSR_K7_HWCR >> 24) & 0xffU,
	    0xba, 0x00, 0x00, 0x00, 0x00, /* mov edx,0 */
	    0xb8, HWCR_SMOKE_VALUE & 0xffU,
	    (HWCR_SMOKE_VALUE >> 8) & 0xffU,
	    (HWCR_SMOKE_VALUE >> 16) & 0xffU,
	    (HWCR_SMOKE_VALUE >> 24) & 0xffU,
	    0x0f, 0x30		/* wrmsr */
	};
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	size_t len = 0;

	emit(code, &len, cap, rdmsr, sizeof(rdmsr));
	emit(code, &len, cap, wrmsr, sizeof(wrmsr));
	emit(code, &len, cap, rdmsr, sizeof(rdmsr));
	emit(code, &len, cap, vmmcall, sizeof(vmmcall));
	return len;
}

static size_t
guest_pcicfg_code(uint8_t *code, size_t cap)
{
	static const uint8_t seq[] = {
	    0xba, PCI_CFG_ADDR_PORT & 0xffU,
	    (PCI_CFG_ADDR_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb8, 0x00, 0x00, 0x00, 0x80,	/* mov eax,0x80000000 */
	    0xef,				/* out dx,eax */
	    0xba, PCI_CFG_DATA_PORT & 0xffU,
	    (PCI_CFG_DATA_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xed,				/* in eax,dx */
	    0x31, 0xc0,				/* xor eax,eax */
	    0xef,				/* out dx,eax */
	    0x0f, 0x01, 0xd9			/* vmmcall */
	};
	size_t len = 0;

	emit(code, &len, cap, seq, sizeof(seq));
	return len;
}

static size_t
guest_pitfallback_code(uint8_t *code, size_t cap)
{
	static const uint8_t seq[] = {
	    0xba, PIT_PORTB & 0xffU, (PIT_PORTB >> 8) & 0xffU, 0x00, 0x00,
	    0xec,				/* in al,dx */
	    0x24, 0xfd,				/* and al,~0x02 */
	    0x0c, 0x01,				/* or al,0x01 */
	    0xee,				/* out dx,al */
	    0xba, PIT_CMD_PORT & 0xffU,
	    (PIT_CMD_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb0, 0xb0,				/* mov al,0xb0 */
	    0xee,				/* out dx,al */
	    0xba, PIT_CH2_PORT & 0xffU,
	    (PIT_CH2_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb0, 0xff,				/* mov al,0xff */
	    0xee,				/* out dx,al */
	    0xee,				/* out dx,al */
	    0xec,				/* in al,dx */
	    0x0f, 0x01, 0xd9			/* vmmcall */
	};
	size_t len = 0;

	emit(code, &len, cap, seq, sizeof(seq));
	return len;
}

static size_t
guest_rtccmos_code(uint8_t *code, size_t cap)
{
	static const uint8_t seq[] = {
	    0xba, CMOS_INDEX_PORT & 0xffU,
	    (CMOS_INDEX_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb0, 0x0a,			/* mov al,RTC_REG_A */
	    0xee,			/* out dx,al */
	    0xba, CMOS_DATA_PORT & 0xffU,
	    (CMOS_DATA_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xec,			/* in al,dx */
	    0xba, CMOS_INDEX_PORT & 0xffU,
	    (CMOS_INDEX_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb0, 0x0b,			/* mov al,RTC_REG_B */
	    0xee,			/* out dx,al */
	    0xba, CMOS_DATA_PORT & 0xffU,
	    (CMOS_DATA_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xec,			/* in al,dx */
	    0xba, CMOS_INDEX_PORT & 0xffU,
	    (CMOS_INDEX_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb0, 0x00,			/* mov al,RTC_SECONDS */
	    0xee,			/* out dx,al */
	    0xba, CMOS_DATA_PORT & 0xffU,
	    (CMOS_DATA_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xec,			/* in al,dx */
	    0xba, CMOS_INDEX_PORT & 0xffU,
	    (CMOS_INDEX_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb0, 0x0f,			/* mov al,shutdown status */
	    0xee,			/* out dx,al */
	    0xba, CMOS_DATA_PORT & 0xffU,
	    (CMOS_DATA_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb0, 0x5a,			/* mov al,0x5a */
	    0xee,			/* out dx,al */
	    0xba, CMOS_INDEX_PORT & 0xffU,
	    (CMOS_INDEX_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xb0, 0x0f,			/* mov al,shutdown status */
	    0xee,			/* out dx,al */
	    0xba, CMOS_DATA_PORT & 0xffU,
	    (CMOS_DATA_PORT >> 8) & 0xffU, 0x00, 0x00,
	    0xec,			/* in al,dx */
	    0x0f, 0x01, 0xd9		/* vmmcall */
	};
	size_t len = 0;

	emit(code, &len, cap, seq, sizeof(seq));
	return len;
}

static size_t
guest_code(const char *mode, uint8_t *code, size_t cap)
{
	static const uint8_t vmmcall[] = { 0x0f, 0x01, 0xd9 };
	static const uint8_t cpuid_vmmcall[] =
	    { 0x31, 0xc0, 0x0f, 0xa2, 0x0f, 0x01, 0xd9 };
	static const uint8_t time_vmmcall[] = {
	    0x0f, 0x31, 0xf3, 0x90, 0x0f, 0x01, 0xf9, 0x0f, 0x01, 0xd9
	};
	static const uint8_t xsetbv_vmmcall[] = {
	    0x31, 0xc9,		/* xor ecx,ecx */
	    0x31, 0xd2,		/* xor edx,edx */
	    0xb8, 0x01, 0x00, 0x00, 0x00, /* mov eax,1 */
	    0x0f, 0x01, 0xd1,	/* xsetbv */
	    0x0f, 0x01, 0xd9	/* vmmcall */
	};
	static const uint8_t apicmsr_vmmcall[] = {
	    0xb9, 0x1b, 0x00, 0x00, 0x00, /* mov ecx,MSR_APICBASE */
	    0x0f, 0x32,		/* rdmsr */
	    0x0d, 0x00, 0x0d, 0x00, 0x00, /* or eax,BSP|X2APIC|EN */
	    0x0f, 0x30,		/* wrmsr */
	    0xb9, 0xe0, 0x06, 0x00, 0x00, /* mov ecx,MSR_TSC_DEADLINE */
	    0xb8, 0xff, 0xff, 0xff, 0xff, /* mov eax,0xffffffff */
	    0xba, 0xff, 0xff, 0xff, 0xff, /* mov edx,0xffffffff */
	    0x0f, 0x30,		/* wrmsr */
	    0xb9, 0x32, 0x08, 0x00, 0x00, /* mov ecx,x2APIC LVT timer */
	    0xb8, 0x2e, 0x00, 0x04, 0x00, /* mov eax,0x4002e */
	    0x31, 0xd2,		/* xor edx,edx */
	    0x0f, 0x30,		/* wrmsr */
	    0xb9, 0x02, 0x08, 0x00, 0x00, /* mov ecx,x2APIC ID */
	    0x0f, 0x32,		/* rdmsr */
	    0xb9, 0x03, 0x08, 0x00, 0x00, /* mov ecx,x2APIC VERSION */
	    0x0f, 0x32,		/* rdmsr */
	    0x0f, 0x01, 0xd9	/* vmmcall */
	};
	static const uint8_t hlt[] = { 0xf4 };
	static const uint8_t loop[] = { 0xeb, 0xfe };
	static const uint8_t cliloop[] = { 0xfa, 0xeb, 0xfe };
	const uint8_t *src;
	size_t len;

	if (strcmp(mode, "vmmcall") == 0) {
		src = vmmcall;
		len = sizeof(vmmcall);
	} else if (strcmp(mode, "cpuid") == 0) {
		src = cpuid_vmmcall;
		len = sizeof(cpuid_vmmcall);
	} else if (strcmp(mode, "serial") == 0) {
		return guest_serial_code(code, cap);
	} else if (strcmp(mode, "serialin") == 0) {
		return guest_serialin_code(code, cap);
	} else if (strcmp(mode, "serialirq") == 0) {
		return guest_serialirq_code(code, cap);
	} else if (strcmp(mode, "timerint") == 0) {
		return guest_timer_code(code, cap);
	} else if (strcmp(mode, "lapictimer") == 0) {
		return guest_lapictimer_code(code, cap);
	} else if (strcmp(mode, "ud") == 0) {
		return guest_ud_code(code, cap);
	} else if (strcmp(mode, "pic") == 0) {
		return guest_pic_code(code, cap);
	} else if (strcmp(mode, "ioapic") == 0) {
		return guest_ioapic_code(code, cap);
	} else if (strcmp(mode, "x2apic") == 0) {
		return guest_x2apic_code(code, cap);
	} else if (strcmp(mode, "cachetlb") == 0) {
		return guest_cachetlb_code(code, cap);
	} else if (strcmp(mode, "avicirq") == 0) {
		return guest_avicirq_code(code, cap);
	} else if (strcmp(mode, "avicipi") == 0) {
		return guest_avicipi_code(code, cap);
	} else if (strcmp(mode, "aviclvt") == 0) {
		return guest_aviclvt_code(code, cap);
	} else if (strcmp(mode, "avictimercfg") == 0) {
		return guest_avictimercfg_code(code, cap);
	} else if (strcmp(mode, "aviclint") == 0) {
		return guest_aviclint_code(code, cap);
	} else if (strcmp(mode, "aviclvtpc") == 0) {
		return guest_aviclvtpc_code(code, cap);
	} else if (strcmp(mode, "avicesr") == 0) {
		return guest_avicesr_code(code, cap);
	} else if (strcmp(mode, "avicsvr") == 0) {
		return guest_avicsvr_code(code, cap);
	} else if (strcmp(mode, "avicnoaccel") == 0) {
		return guest_avicnoaccel_code(code, cap);
	} else if (strcmp(mode, "pm64") == 0) {
		return guest_pm64_code(code, cap);
	} else if (strcmp(mode, "msrpatch") == 0) {
		return guest_msrpatch_code(code, cap);
	} else if (strcmp(mode, "msrsyscfg") == 0) {
		return guest_msrsyscfg_code(code, cap);
	} else if (strcmp(mode, "mtrrcap") == 0) {
		return guest_mtrrcap_code(code, cap);
	} else if (strcmp(mode, "msrhwcr") == 0) {
		return guest_msrhwcr_code(code, cap);
	} else if (strcmp(mode, "pcicfg") == 0) {
		return guest_pcicfg_code(code, cap);
	} else if (strcmp(mode, "pitfallback") == 0) {
		return guest_pitfallback_code(code, cap);
	} else if (strcmp(mode, "rtccmos") == 0) {
		return guest_rtccmos_code(code, cap);
	} else if (strcmp(mode, "time") == 0) {
		src = time_vmmcall;
		len = sizeof(time_vmmcall);
	} else if (strcmp(mode, "xsetbv") == 0) {
		src = xsetbv_vmmcall;
		len = sizeof(xsetbv_vmmcall);
	} else if (strcmp(mode, "apicmsr") == 0) {
		src = apicmsr_vmmcall;
		len = sizeof(apicmsr_vmmcall);
	} else if (strcmp(mode, "hlt") == 0) {
		src = hlt;
		len = sizeof(hlt);
	} else if (strcmp(mode, "loop") == 0) {
		src = loop;
		len = sizeof(loop);
	} else if (strcmp(mode, "cliloop") == 0) {
		src = cliloop;
		len = sizeof(cliloop);
	} else {
		errx(1, "unknown smoke mode: %s", mode);
	}
	if (len > cap)
		errx(1, "guest code buffer too small");
	memcpy(code, src, len);
	return len;
}

static void
build_guest(uint8_t *mem, size_t mem_size, const char *mode, size_t *code_len)
{
	uint8_t code[1024];

	if (mem_size < 2 * 1024 * 1024)
		errx(1, "fd3 is smaller than 2M");
	memset(mem + PML4_GPA, 0, PAGE_SIZE_GUEST * 3);
	memset(mem + IOAPIC_PD_GPA, 0, PAGE_SIZE_GUEST);
	write64(mem, PML4_GPA, PDPT_GPA | 3);
	write64(mem, PDPT_GPA, PD_GPA | 3);
	write64(mem, PDPT_GPA + IOAPIC_PDPT_INDEX * 8, IOAPIC_PD_GPA | 3);
	write64(mem, PD_GPA, 0x83);
	write64(mem, IOAPIC_PD_GPA + IOAPIC_PD_INDEX * 8, IOAPIC_GPA | 0x83);
	write64(mem, IOAPIC_PD_GPA + APIC_PD_INDEX * 8, APIC_GPA | 0x83);

	memset(mem + GDT_GPA, 0, PAGE_SIZE_GUEST);
	if (strcmp(mode, "pm64") == 0) {
		write64(mem, GDT_GPA + 8, 0x00cf9a000000ffffULL);
		write64(mem, GDT_GPA + 16, 0x00cf92000000ffffULL);
		write64(mem, GDT_GPA + 32, 0x00209a0000000000ULL);
	} else {
		write64(mem, GDT_GPA + 8, 0x00209a0000000000ULL);
		write64(mem, GDT_GPA + 16, 0x0000920000000000ULL);
	}
	write64(mem, GDT_GPA + 24, 0x0000890060000067ULL);
	memset(mem + TSS_GPA, 0, 0x68);
	if (strcmp(mode, "timerint") == 0 ||
	    strcmp(mode, "lapictimer") == 0 ||
	    strcmp(mode, "serialirq") == 0 || strcmp(mode, "ud") == 0 ||
	    strcmp(mode, "avicirq") == 0) {
		memset(mem + IDT_GPA, 0, PAGE_SIZE_GUEST);
		if (strcmp(mode, "timerint") == 0 ||
		    strcmp(mode, "lapictimer") == 0)
			write_idt_gate(mem, TIMER_VECTOR, TIMER_HANDLER_GPA);
		else if (strcmp(mode, "serialirq") == 0)
			write_idt_gate(mem, SERIAL_VECTOR, SERIAL_HANDLER_GPA);
		else if (strcmp(mode, "avicirq") == 0)
			write_idt_gate(mem, AVIC_VECTOR, AVIC_HANDLER_GPA);
		else
			write_idt_gate(mem, UD_VECTOR, UD_HANDLER_GPA);
	}

	*code_len = guest_code(mode, code, sizeof(code));
	memcpy(mem + ENTRY_GPA, code, *code_len);
}

static void
build_vcpu(struct vmm_x64_vcpu_state *vcpu, const char *mode)
{
	memset(vcpu, 0, sizeof(*vcpu));
	vcpu->runnable = 1;
	vcpu->gpr[VMM_X64_GPR_RSP] = STACK_GPA;
	vcpu->gpr[VMM_X64_GPR_RIP] = ENTRY_GPA;
	vcpu->gpr[VMM_X64_GPR_RFLAGS] = 2;
	if (strcmp(mode, "pm64") == 0) {
		vcpu->cr[VMM_X64_CR_CR0] = CR0_PE | CR0_NE;
		vcpu->cr[VMM_X64_CR_CR3] = 0;
		vcpu->cr[VMM_X64_CR_CR4] = 0;
	} else {
		vcpu->cr[VMM_X64_CR_CR0] = CR0_PE | CR0_NE | CR0_PG;
		vcpu->cr[VMM_X64_CR_CR3] = PML4_GPA;
		vcpu->cr[VMM_X64_CR_CR4] = CR4_PAE;
	}
	vcpu->cr[VMM_X64_CR_XCR0] = XCR0_X87;
	if (strcmp(mode, "pm64") != 0)
		vcpu->msr[VMM_X64_MSR_EFER] = EFER_LME | EFER_LMA;
	vcpu->msr[VMM_X64_MSR_PAT] = 0x0007040600070406ULL;
	if (strcmp(mode, "pm64") == 0) {
		set_segment(&vcpu->seg[VMM_X64_SEG_ES], 0x10,
		    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
		set_segment(&vcpu->seg[VMM_X64_SEG_CS], 0x08,
		    0xb | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	} else {
		set_segment(&vcpu->seg[VMM_X64_SEG_ES], 0, SEG_UNUSABLE, 0, 0);
		set_segment(&vcpu->seg[VMM_X64_SEG_CS], 0x08,
		    0xb | SEG_S | SEG_P | SEG_L | SEG_G, 0xffffffffU, 0);
	}
	set_segment(&vcpu->seg[VMM_X64_SEG_SS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_DS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	if (strcmp(mode, "pm64") == 0) {
		set_segment(&vcpu->seg[VMM_X64_SEG_FS], 0x10,
		    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
		set_segment(&vcpu->seg[VMM_X64_SEG_GS], 0x10,
		    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
		set_segment(&vcpu->seg[VMM_X64_SEG_GDT], 0, 0, 47, GDT_GPA);
	} else {
		set_segment(&vcpu->seg[VMM_X64_SEG_FS], 0, SEG_UNUSABLE, 0, 0);
		set_segment(&vcpu->seg[VMM_X64_SEG_GS], 0, SEG_UNUSABLE, 0, 0);
		set_segment(&vcpu->seg[VMM_X64_SEG_GDT], 0, 0, 39, GDT_GPA);
	}
	if (strcmp(mode, "timerint") == 0 ||
	    strcmp(mode, "lapictimer") == 0) {
		set_segment(&vcpu->seg[VMM_X64_SEG_IDT], 0, 0,
		    TIMER_VECTOR * 16 + 15, IDT_GPA);
	} else if (strcmp(mode, "serialirq") == 0) {
		set_segment(&vcpu->seg[VMM_X64_SEG_IDT], 0, 0,
		    SERIAL_VECTOR * 16 + 15, IDT_GPA);
	} else if (strcmp(mode, "ud") == 0) {
		set_segment(&vcpu->seg[VMM_X64_SEG_IDT], 0, 0,
		    UD_VECTOR * 16 + 15, IDT_GPA);
	} else if (strcmp(mode, "avicirq") == 0) {
		set_segment(&vcpu->seg[VMM_X64_SEG_IDT], 0, 0,
		    AVIC_VECTOR * 16 + 15, IDT_GPA);
	} else {
		set_segment(&vcpu->seg[VMM_X64_SEG_IDT], 0, 0, 0, 0);
	}
	set_segment(&vcpu->seg[VMM_X64_SEG_LDT], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_TR], 0x18, 0x9 | SEG_P, 0x67,
	    TSS_GPA);
}

static void
build_manifest(uint8_t *manifest, size_t manifest_size, size_t mem_size,
    const struct vmm_x64_vcpu_state *vcpu, size_t code_len)
{
	struct vmm_manifest_header hdr;
	struct vmm_gpa_range ranges[5];
	uint8_t *ptr;

	if (manifest_size < PAGE_SIZE_GUEST)
		errx(1, "fd4 is smaller than one page");
	ranges[0] = (struct vmm_gpa_range){ ENTRY_GPA, code_len, 1, 0 };
	ranges[1] = (struct vmm_gpa_range){ PML4_GPA, PAGE_SIZE_GUEST * 3, 5, 0 };
	ranges[2] = (struct vmm_gpa_range){ GDT_GPA, PAGE_SIZE_GUEST * 2, 6, 0 };
	ranges[3] = (struct vmm_gpa_range){ STACK_GPA - PAGE_SIZE_GUEST,
	    PAGE_SIZE_GUEST, 7, 0 };
	ranges[4] = (struct vmm_gpa_range){ IOAPIC_PD_GPA, PAGE_SIZE_GUEST,
	    8, 0 };

	memset(manifest, 0, manifest_size);
	ptr = manifest + sizeof(hdr);
	ptr = add_record(ptr, VMM_REC_X64_VCPU_STATE, vcpu, sizeof(*vcpu));
	ptr = add_record(ptr, VMM_REC_GPA_RANGE, ranges, sizeof(ranges));
	if ((size_t)(ptr - manifest) > manifest_size)
		errx(1, "manifest does not fit fd4");

	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, VMM_MANIFEST_MAGIC, sizeof(hdr.magic));
	hdr.arch = VMM_MANIFEST_ARCH_X64;
	hdr.header_size = sizeof(hdr);
	hdr.total_size = (uint32_t)(ptr - manifest);
	hdr.record_count = 2;
	hdr.mem_size = mem_size;
	memcpy(manifest, &hdr, sizeof(hdr));
}

int
main(int argc, char **argv)
{
	struct stat mem_stat;
	struct stat manifest_stat;
	struct vmm_x64_vcpu_state vcpu;
	uint8_t *mem;
	uint8_t *manifest;
	size_t code_len;

	if (argc != 2)
		errx(1, "usage: %s vmmcall|cpuid|serial|serialin|serialirq|time|xsetbv|apicmsr|timerint|lapictimer|ud|pic|ioapic|x2apic|cachetlb|pm64|msrpatch|msrsyscfg|mtrrcap|msrhwcr|pcicfg|pitfallback|rtccmos|hlt|loop|cliloop|avicirq|avicipi|aviclvt|avictimercfg|aviclint|aviclvtpc|avicesr|avicsvr|avicnoaccel", argv[0]);
	if (fstat(3, &mem_stat) != 0 || fstat(4, &manifest_stat) != 0)
		err(1, "fstat fd3/fd4");
	if (mem_stat.st_size <= 0 || manifest_stat.st_size <= 0)
		errx(1, "invalid loader fd size");
	mem = mmap(NULL, (size_t)mem_stat.st_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, 3, 0);
	if (mem == MAP_FAILED)
		err(1, "mmap fd3");
	manifest = mmap(NULL, (size_t)manifest_stat.st_size,
	    PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
	if (manifest == MAP_FAILED)
		err(1, "mmap fd4");

	build_guest(mem, (size_t)mem_stat.st_size, argv[1], &code_len);
	build_vcpu(&vcpu, argv[1]);
	build_manifest(manifest, (size_t)manifest_stat.st_size,
	    (size_t)mem_stat.st_size, &vcpu, code_len);
	return 0;
}
