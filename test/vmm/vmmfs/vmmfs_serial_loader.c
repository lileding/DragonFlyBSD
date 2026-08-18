/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Minimal vmmfs loader that writes one line to COM1 and halts.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <dev/virtual/vmm/vmm.h>

#define PAGE_SIZE_GUEST 4096ULL
#define PML4_GPA 0x2000ULL
#define PDPT_GPA 0x3000ULL
#define PD_GPA 0x4000ULL
#define GDT_GPA 0x5000ULL
#define TSS_GPA 0x6000ULL
#define STACK_TOP_GPA 0x80000ULL
#define ENTRY_GPA 0x100000ULL

#define CR0_PE 0x00000001ULL
#define CR0_NE 0x00000020ULL
#define CR0_PG 0x80000000ULL
#define CR4_PAE 0x00000020ULL
#define EFER_LME 0x00000100ULL
#define EFER_LMA 0x00000400ULL
#define XCR0_X87 0x00000001ULL
#define SEG_S 0x0010U
#define SEG_P 0x0080U
#define SEG_L 0x0200U
#define SEG_DB 0x0400U
#define SEG_G 0x0800U
#define SEG_UNUSABLE 0x1000U

static void build_guest(uint8_t *, uint64_t);
static void build_cpustate(struct vmm_cpustate *);
static void set_segment(struct vmm_segment *, uint16_t, uint16_t, uint32_t,
    uint64_t);
static void write64(uint8_t *, uint64_t, uint64_t);

int
main(void)
{
	struct vmm_cpustate state;
	struct stat st;
	uint8_t *memory;
	ssize_t written;

	if (fstat(3, &st) != 0)
		err(1, "fstat fd3");
	if (st.st_size < 2 * 1024 * 1024)
		errx(1, "guest memory must be at least 2M");
	memory = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, 3, 0);
	if (memory == MAP_FAILED)
		err(1, "mmap fd3");
	build_guest(memory, (uint64_t)st.st_size);
	build_cpustate(&state);
	written = write(2, &state, sizeof(state));
	if (written < 0)
		err(1, "write fd2 cpustate");
	if ((size_t)written != sizeof(state))
		errx(1, "short write to fd2 cpustate");
	return 0;
}

static void
build_guest(uint8_t *memory, uint64_t memory_size)
{
	static const char message[] = "vmmfs-serial-ok\n";
	size_t code_size;
	size_t index;
	size_t offset;
	unsigned int page;

	code_size = sizeof(message) * 8 + 2;
	if (ENTRY_GPA + code_size > memory_size || STACK_TOP_GPA > memory_size)
		errx(1, "guest memory is too small");
	memset(memory + PML4_GPA, 0, PAGE_SIZE_GUEST * 3);
	write64(memory, PML4_GPA, PDPT_GPA | 3);
	write64(memory, PDPT_GPA, PD_GPA | 3);
	for (page = 0; page < 512; ++page) {
		write64(memory, PD_GPA + page * sizeof(uint64_t),
		    (uint64_t)page * 0x200000ULL | 0x83ULL);
	}
	memset(memory + GDT_GPA, 0, PAGE_SIZE_GUEST);
	write64(memory, GDT_GPA + 16, 0x00209a0000000000ULL);
	write64(memory, GDT_GPA + 24, 0x0000920000000000ULL);
	write64(memory, GDT_GPA + 32, 0x0000890060000067ULL);
	memset(memory + TSS_GPA, 0, 0x68);
	offset = ENTRY_GPA;
	for (index = 0; index < sizeof(message) - 1; ++index) {
		memory[offset++] = 0xba;
		memory[offset++] = 0xf8;
		memory[offset++] = 0x03;
		memory[offset++] = 0x00;
		memory[offset++] = 0x00;
		memory[offset++] = 0xb0;
		memory[offset++] = (uint8_t)message[index];
		memory[offset++] = 0xee;
	}
	memory[offset++] = 0xf4;
	memory[offset] = 0xeb;
	memory[offset + 1] = 0xfd;
}

static void
build_cpustate(struct vmm_cpustate *state)
{
	memset(state, 0, sizeof(*state));
	state->gprs[VMM_X64_GPR_RSP] = STACK_TOP_GPA;
	state->gprs[VMM_X64_GPR_RIP] = ENTRY_GPA;
	state->gprs[VMM_X64_GPR_RFLAGS] = 2;
	state->crs[VMM_X64_CR_CR0] = CR0_PE | CR0_NE | CR0_PG;
	state->crs[VMM_X64_CR_CR3] = PML4_GPA;
	state->crs[VMM_X64_CR_CR4] = CR4_PAE;
	state->crs[VMM_X64_CR_XCR0] = XCR0_X87;
	state->msrs[VMM_X64_MSR_EFER] = EFER_LME | EFER_LMA;
	state->msrs[VMM_X64_MSR_PAT] = 0x0007040600070406ULL;
	set_segment(&state->segs[VMM_X64_SEG_ES], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&state->segs[VMM_X64_SEG_CS], 0x10,
	    0xb | SEG_S | SEG_P | SEG_L | SEG_G, 0xffffffffU, 0);
	set_segment(&state->segs[VMM_X64_SEG_SS], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&state->segs[VMM_X64_SEG_DS], 0x18,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_segment(&state->segs[VMM_X64_SEG_FS], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&state->segs[VMM_X64_SEG_GS], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&state->segs[VMM_X64_SEG_GDT], 0, 0, 47, GDT_GPA);
	set_segment(&state->segs[VMM_X64_SEG_IDT], 0, 0, 0, 0);
	set_segment(&state->segs[VMM_X64_SEG_LDT], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&state->segs[VMM_X64_SEG_TR], 0x20, 0x9 | SEG_P, 0x67,
	    TSS_GPA);
}

static void
set_segment(struct vmm_segment *segment, uint16_t selector, uint16_t attrib,
    uint32_t limit, uint64_t base)
{
	segment->selector = selector;
	segment->attrib.type = attrib & 0xf;
	segment->attrib.s = (attrib & SEG_S) != 0;
	segment->attrib.dpl = (attrib >> 5) & 0x3;
	segment->attrib.p = (attrib & SEG_P) != 0;
	segment->attrib.avl = (attrib >> 8) & 0x1;
	segment->attrib.l = (attrib & SEG_L) != 0;
	segment->attrib.def = (attrib & SEG_DB) != 0;
	segment->attrib.g = (attrib & SEG_G) != 0;
	segment->limit = limit;
	segment->base = base;
}

static void
write64(uint8_t *memory, uint64_t offset, uint64_t value)
{
	memcpy(memory + offset, &value, sizeof(value));
}
