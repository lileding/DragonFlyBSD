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
#define ENTRY_GPA	0x100000ULL
#define TIMER_HANDLER_GPA (ENTRY_GPA + 0x80ULL)
#define STACK_GPA	0x180000ULL

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
#define TIMER_VECTOR	0x2eU

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
	} else if (strcmp(mode, "timerint") == 0) {
		return guest_timer_code(code, cap);
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
	uint8_t code[256];

	if (mem_size < 2 * 1024 * 1024)
		errx(1, "fd3 is smaller than 2M");
	memset(mem + PML4_GPA, 0, PAGE_SIZE_GUEST * 3);
	write64(mem, PML4_GPA, PDPT_GPA | 3);
	write64(mem, PDPT_GPA, PD_GPA | 3);
	write64(mem, PD_GPA, 0x83);

	memset(mem + GDT_GPA, 0, PAGE_SIZE_GUEST);
	write64(mem, GDT_GPA + 8, 0x00209a0000000000ULL);
	write64(mem, GDT_GPA + 16, 0x0000920000000000ULL);
	write64(mem, GDT_GPA + 24, 0x0000890060000067ULL);
	memset(mem + TSS_GPA, 0, 0x68);
	if (strcmp(mode, "timerint") == 0) {
		memset(mem + IDT_GPA, 0, 0x400);
		write_idt_gate(mem, TIMER_VECTOR, TIMER_HANDLER_GPA);
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
	vcpu->cr[VMM_X64_CR_CR0] = CR0_PE | CR0_NE | CR0_PG;
	vcpu->cr[VMM_X64_CR_CR3] = PML4_GPA;
	vcpu->cr[VMM_X64_CR_CR4] = CR4_PAE;
	vcpu->cr[VMM_X64_CR_XCR0] = XCR0_X87;
	vcpu->msr[VMM_X64_MSR_EFER] = EFER_LME | EFER_LMA;
	vcpu->msr[VMM_X64_MSR_PAT] = 0x0007040600070406ULL;
	set_segment(&vcpu->seg[VMM_X64_SEG_ES], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_CS], 0x08,
	    0xb | SEG_S | SEG_P | SEG_L | SEG_G, 0xffffffffU, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_SS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_DS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_FS], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_GS], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&vcpu->seg[VMM_X64_SEG_GDT], 0, 0, 39, GDT_GPA);
	if (strcmp(mode, "timerint") == 0) {
		set_segment(&vcpu->seg[VMM_X64_SEG_IDT], 0, 0,
		    TIMER_VECTOR * 16 + 15, IDT_GPA);
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
	struct vmm_gpa_range ranges[4];
	uint8_t *ptr;

	if (manifest_size < PAGE_SIZE_GUEST)
		errx(1, "fd4 is smaller than one page");
	ranges[0] = (struct vmm_gpa_range){ ENTRY_GPA, code_len, 1, 0 };
	ranges[1] = (struct vmm_gpa_range){ PML4_GPA, PAGE_SIZE_GUEST * 3, 5, 0 };
	ranges[2] = (struct vmm_gpa_range){ GDT_GPA, PAGE_SIZE_GUEST * 2, 6, 0 };
	ranges[3] = (struct vmm_gpa_range){ STACK_GPA - PAGE_SIZE_GUEST,
	    PAGE_SIZE_GUEST, 7, 0 };

	memset(manifest, 0, manifest_size);
	ptr = manifest + sizeof(hdr);
	ptr = add_record(ptr, VMM_REC_X64_VCPU_STATE, vcpu, sizeof(*vcpu));
	ptr = add_record(ptr, VMM_REC_GPA_RANGE, ranges, sizeof(ranges));

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
		errx(1, "usage: %s vmmcall|cpuid|serial|time|xsetbv|apicmsr|timerint|hlt|loop", argv[0]);
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
