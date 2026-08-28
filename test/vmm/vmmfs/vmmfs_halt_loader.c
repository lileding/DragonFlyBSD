/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Minimal headless vmmfs loader used to verify VMRUN and forced stop.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sys/vmm.h>

#define PAGE_SIZE_GUEST	4096ULL
#define PML4_GPA		0x2000ULL
#define PDPT_GPA		0x3000ULL
#define PD_GPA			0x4000ULL
#define GDT_GPA		0x5000ULL
#define TSS_GPA		0x6000ULL
#define ECAM_PD_GPA		0x9000ULL
#define STACK_TOP_GPA		0x80000ULL
#define ENTRY_GPA		0x100000ULL
#define XSDT_GPA		0x70100ULL
#define FADT_GPA		0x70200ULL
#define MCFG_GPA		0x70600ULL
#define MCFG_SIZE		60U
#define PCI_ECAM_GPA		0xe8000000ULL

#define CR0_PE			0x00000001ULL
#define CR0_NE			0x00000020ULL
#define CR0_PG			0x80000000ULL
#define CR4_PAE			0x00000020ULL
#define EFER_LME		0x00000100ULL
#define EFER_LMA		0x00000400ULL
#define XCR0_X87		0x00000001ULL
#define SEG_S			0x0010U
#define SEG_P			0x0080U
#define SEG_L			0x0200U
#define SEG_DB			0x0400U
#define SEG_G			0x0800U
#define SEG_UNUSABLE		0x1000U

static void build_cpu_state(struct vmm_cpustate *);
static void build_guest(uint8_t *, uint64_t, int, int, int, int);
static void check_pci_topology(const uint8_t *, uint64_t);
static void check_gas(const uint8_t *, uint8_t, uint8_t, uint64_t);
static void set_segment(struct vmm_segment *, uint16_t, uint16_t, uint32_t,
    uint64_t);
static void write64(uint8_t *, uint64_t, uint64_t);

int
main(int argc, char **argv)
{
	struct vmm_cpustate state;
	struct stat st;
	uint8_t *memory;
	ssize_t written;
	int check_pci;
	int check_pci_config;
	int check_pci_doorbell;
	int loop_pci;

	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--check-pci") != 0 &&
	    strcmp(argv[1], "--check-pci-loop") != 0 &&
	    strcmp(argv[1], "--check-pci-doorbell-loop") != 0 &&
	    strcmp(argv[1], "--check-pci-config-loop") != 0))
		errx(1, "usage: %s [--check-pci|--check-pci-loop|--check-pci-doorbell-loop|--check-pci-config-loop]",
		    argv[0]);
	check_pci = argc == 2;
	check_pci_config = argc == 2 &&
	    strcmp(argv[1], "--check-pci-config-loop") == 0;
	check_pci_doorbell = argc == 2 &&
	    (strcmp(argv[1], "--check-pci-doorbell-loop") == 0 ||
	    check_pci_config);
	loop_pci = argc == 2 && strcmp(argv[1], "--check-pci-loop") == 0;
	if (fstat(3, &st) != 0)
		err(1, "fstat fd3");
	if (st.st_size < 2 * 1024 * 1024)
		errx(1, "guest memory must be at least 2M");
	memory = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, 3, 0);
	if (memory == MAP_FAILED)
		err(1, "mmap fd3");
	if (check_pci)
		check_pci_topology(memory, (uint64_t)st.st_size);
	build_guest(memory, (uint64_t)st.st_size, check_pci, loop_pci,
	    check_pci_doorbell, check_pci_config);
	build_cpu_state(&state);
	written = write(3, &state, sizeof(state));
	if (written < 0)
		err(1, "write fd3 cpustate");
	if ((size_t)written != sizeof(state))
		errx(1, "short write to fd3 cpustate");
	return (0);
}

static void
check_pci_topology(const uint8_t *memory, uint64_t memory_size)
{
	uint32_t length;
	uint32_t fadt_flags;
	uint64_t address;

	if (memory_size < MCFG_GPA + MCFG_SIZE)
		errx(1, "guest memory lacks MCFG");
	if (memcmp(memory + FADT_GPA, "FACP", 4) != 0)
		errx(1, "FADT signature");
	memcpy(&fadt_flags, memory + FADT_GPA + 112, sizeof(fadt_flags));
	if (fadt_flags != ((1U << 20) | (1U << 10) | (1U << 8)) ||
	    memory[FADT_GPA + 128] != 1 || memory[FADT_GPA + 131] != 3)
		errx(1, "FADT hardware-reduced reset");
	check_gas(memory + FADT_GPA + 116, 8, 1, 0x600);
	check_gas(memory + FADT_GPA + 208, 32, 3, 0x608);
	check_gas(memory + FADT_GPA + 244, 8, 1, 0x600);
	check_gas(memory + FADT_GPA + 256, 8, 1, 0x600);
	if (memcmp(memory + MCFG_GPA, "MCFG", 4) != 0)
		errx(1, "MCFG signature");
	memcpy(&length, memory + MCFG_GPA + 4, sizeof(length));
	if (length != MCFG_SIZE)
		errx(1, "MCFG length");
	memcpy(&address, memory + MCFG_GPA + 44, sizeof(address));
	if (address != PCI_ECAM_GPA)
		errx(1, "MCFG ECAM address");
	if (memory[MCFG_GPA + 54] != 0 ||
	    memory[MCFG_GPA + 55] != UINT8_MAX)
		errx(1, "MCFG bus range");
	memcpy(&address, memory + XSDT_GPA + 52, sizeof(address));
	if (address != MCFG_GPA)
		errx(1, "XSDT MCFG entry");
}

static void
check_gas(const uint8_t *gas, uint8_t width, uint8_t access,
	uint64_t expected_address)
{
	uint64_t address;

	if (gas[0] != 1 || gas[1] != width || gas[2] != 0 ||
	    gas[3] != access)
		errx(1, "FADT GAS format");
	memcpy(&address, gas + 4, sizeof(address));
	if (address != expected_address)
		errx(1, "FADT GAS address");
}

static void
build_guest(uint8_t *memory, uint64_t memory_size, int check_pci, int loop_pci,
	int check_pci_doorbell, int check_pci_config)
{
	unsigned int index;

	if (ENTRY_GPA + 64 > memory_size || STACK_TOP_GPA > memory_size)
		errx(1, "guest memory is too small");
	memset(memory + PML4_GPA, 0, PAGE_SIZE_GUEST * 3);
	write64(memory, PML4_GPA, PDPT_GPA | 3);
	write64(memory, PDPT_GPA, PD_GPA | 3);
	if (check_pci)
		write64(memory, PDPT_GPA + 3 * sizeof(uint64_t), ECAM_PD_GPA | 3);
	for (index = 0; index < 512; ++index) {
		write64(memory, PD_GPA + index * sizeof(uint64_t),
		    (uint64_t)index * 0x200000ULL | 0x83ULL);
	}
	if (check_pci) {
		memset(memory + ECAM_PD_GPA, 0, PAGE_SIZE_GUEST);
		for (index = 0; index < 512; ++index) {
			write64(memory, ECAM_PD_GPA + index * sizeof(uint64_t),
			    (0xc0000000ULL + (uint64_t)index * 0x200000ULL) | 0x83ULL);
		}
	}
	memset(memory + GDT_GPA, 0, PAGE_SIZE_GUEST);
	write64(memory, GDT_GPA + 16, 0x00209a0000000000ULL);
	write64(memory, GDT_GPA + 24, 0x0000920000000000ULL);
	write64(memory, GDT_GPA + 32, 0x0000890060000067ULL);
	memset(memory + TSS_GPA, 0, 0x68);
	if (check_pci_doorbell) {
		static const uint8_t guest[] = {
			0xb8, 0x00, 0x80, 0x00, 0xe8,	/* mov eax, 0xe8008000 */
			0x8b, 0x00,				/* mov eax, [rax] */
			0x3d, 0xf4, 0x1a, 0x42, 0x10,	/* cmp eax, 0x10421af4 */
			0x74, 0x02,				/* je 2 */
			0x0f, 0x0b,				/* ud2 */
			0xb8, 0x04, 0x80, 0x00, 0xe8,	/* mov eax, 0xe8008004 */
			0xc7, 0x00, 0x02, 0x00, 0x00, 0x00,	/* mov dword [rax], 2 */
			0xb8, 0x10, 0x80, 0x00, 0xe8,	/* mov eax, 0xe8008010 */
			0x8b, 0x00,				/* mov eax, [rax] */
			0x83, 0xe0, 0xf0,			/* and eax, 0xfffffff0 */
			0x48, 0x89, 0xc1,			/* mov rcx, rax */
			0xc7, 0x01, 0xef, 0xbe, 0xad, 0xde,	/* mov dword [rcx], 0xdeadbeef */
			0x8b, 0x81, 0x00, 0x01, 0x00, 0x00,	/* mov eax, [rcx+0x100] */
			0x3d, 0xfe, 0xca, 0xde, 0xc0,	/* cmp eax, 0xc0decafe */
			0x74, 0x02,				/* je 2 */
			0x0f, 0x0b,				/* ud2 */
			0xeb, 0xfe,				/* jmp . */
		};

		if (check_pci_config)
			memcpy(memory + ENTRY_GPA, guest, sizeof(guest));
		else
			memcpy(memory + ENTRY_GPA, guest, sizeof(guest) - 15);
	} else if (check_pci) {
		memory[ENTRY_GPA] = 0xb8;
		memory[ENTRY_GPA + 1] = 0x00;
		memory[ENTRY_GPA + 2] = 0x00;
		memory[ENTRY_GPA + 3] = 0x00;
		memory[ENTRY_GPA + 4] = 0xe8;
		memory[ENTRY_GPA + 5] = 0x8b;
		memory[ENTRY_GPA + 6] = 0x00;
		memory[ENTRY_GPA + 7] = 0x83;
		memory[ENTRY_GPA + 8] = 0xf8;
		memory[ENTRY_GPA + 9] = 0xff;
		memory[ENTRY_GPA + 10] = 0x74;
		memory[ENTRY_GPA + 11] = loop_pci ? 0x02 : 0x01;
		memory[ENTRY_GPA + 12] = loop_pci ? 0xeb : 0xf4;
		memory[ENTRY_GPA + 13] = loop_pci ? 0xfe : 0x0f;
		memory[ENTRY_GPA + 14] = 0x0b;
		if (loop_pci) {
			memory[ENTRY_GPA + 14] = 0x0f;
			memory[ENTRY_GPA + 15] = 0x0b;
		}
	} else {
		memory[ENTRY_GPA] = 0xf4;
		memory[ENTRY_GPA + 1] = 0xeb;
		memory[ENTRY_GPA + 2] = 0xfd;
	}
}

static void
build_cpu_state(struct vmm_cpustate *state)
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
	segment->attrib.rsvd = 0;
	segment->limit = limit;
	segment->base = base;
}

static void
write64(uint8_t *memory, uint64_t offset, uint64_t value)
{
	memcpy(memory + offset, &value, sizeof(value));
}
