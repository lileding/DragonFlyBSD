/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Userland loader for the first NuttX qemu-intel64 bring-up test.
 *
 * Contract:
 *   fd 3 = guest memory mmap object
 *   fd 4 = vmm launch manifest mmap object
 *   argv[1] or /var/tmp/nuttx.elf = Apache NuttX qemu-intel64 ELF image
 *
 * The qemu-intel64 image carries a PVH note that points at its 32-bit entry,
 * but the entry still consumes Multiboot2 %eax/%ebx boot parameters.  This
 * loader therefore uses the PVH note only to find start32, then provides a
 * minimal Multiboot2 info block and ACPI RSDP/RSDT/MADT tables.
 */
#include <sys/mman.h>
#include <sys/stat.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUTTX_ELF_PATH		"/var/tmp/nuttx.elf"
#define VMM_MANIFEST_MAGIC	"VMMLD0\0\0"
#define VMM_MANIFEST_ABI	0
#define VMM_MANIFEST_ARCH_X64	1
#define VMM_REC_X64_VCPU_STATE	1
#define VMM_REC_GPA_RANGE	2
#define VMM_REC_F_MANDATORY	1
#define VMM_GPA_RANGE_MAX	32

#define EI_NIDENT	16
#define ELFMAG0		0x7f
#define ELFMAG1		'E'
#define ELFMAG2		'L'
#define ELFMAG3		'F'
#define ELFCLASS64	2
#define ELFDATA2LSB	1
#define EV_CURRENT	1
#define EM_X86_64	62
#define PT_LOAD		1
#define PT_NOTE		4
#define XEN_ELFNOTE_PHYS32_ENTRY 18

#define MULTIBOOT2_BOOTLOADER_MAGIC	0x36d76289U
#define MULTIBOOT_TAG_TYPE_END		0U
#define MULTIBOOT_TAG_TYPE_ACPI_NEW	15U

#define PAGE_SIZE_GUEST	4096ULL
#define BOOT_STACK_SIZE	(16ULL * 1024ULL)
#define ACPI_RSDP_GPA	0x000f0000ULL
#define ACPI_RSDT_GPA	0x000f1000ULL
#define ACPI_MADT_GPA	0x000f2000ULL
#define ACPI_REGION_GPA	ACPI_RSDP_GPA
#define ACPI_REGION_SIZE	(3ULL * PAGE_SIZE_GUEST)
#define ACPI_LAPIC_BASE	0xfee00000U
#define ACPI_IOAPIC_BASE	0xfec00000U

#define VMM_X64_NGPR	18
#define VMM_X64_NCR	6
#define VMM_X64_NMSR	11
#define VMM_X64_NSEG	10

#define VMM_X64_GPR_RAX	0
#define VMM_X64_GPR_RBX	3
#define VMM_X64_GPR_RSP	4
#define VMM_X64_GPR_RIP	16
#define VMM_X64_GPR_RFLAGS 17

#define VMM_X64_CR_CR0	0
#define VMM_X64_CR_CR2	1
#define VMM_X64_CR_CR3	2
#define VMM_X64_CR_CR4	3
#define VMM_X64_CR_CR8	4
#define VMM_X64_CR_XCR0	5

#define VMM_X64_MSR_EFER 0
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
#define XCR0_X87	0x00000001ULL

#define SEG_S		0x0010U
#define SEG_P		0x0080U
#define SEG_DB		0x0400U
#define SEG_G		0x0800U
#define SEG_UNUSABLE	0x1000U

#define GPA_RANGE_LOAD	1
#define GPA_RANGE_BOOT	8
#define GPA_RANGE_STACK	9

typedef struct {
	unsigned char	e_ident[EI_NIDENT];
	uint16_t	e_type;
	uint16_t	e_machine;
	uint32_t	e_version;
	uint64_t	e_entry;
	uint64_t	e_phoff;
	uint64_t	e_shoff;
	uint32_t	e_flags;
	uint16_t	e_ehsize;
	uint16_t	e_phentsize;
	uint16_t	e_phnum;
	uint16_t	e_shentsize;
	uint16_t	e_shnum;
	uint16_t	e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
	uint32_t	p_type;
	uint32_t	p_flags;
	uint64_t	p_offset;
	uint64_t	p_vaddr;
	uint64_t	p_paddr;
	uint64_t	p_filesz;
	uint64_t	p_memsz;
	uint64_t	p_align;
} __attribute__((packed)) Elf64_Phdr;

typedef struct {
	uint32_t	n_namesz;
	uint32_t	n_descsz;
	uint32_t	n_type;
} __attribute__((packed)) Elf64_Nhdr;

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

struct multiboot_info_header {
	uint32_t	total_size;
	uint32_t	reserved;
} __attribute__((packed));

struct multiboot_tag_header {
	uint32_t	type;
	uint32_t	size;
} __attribute__((packed));

struct acpi_rsdp {
	char		signature[8];
	uint8_t		checksum;
	char		oem_id[6];
	uint8_t		revision;
	uint32_t	rsdt_addr;
	uint32_t	length;
	uint64_t	xsdt_addr;
	uint8_t		ext_checksum;
	uint8_t		reserved[3];
} __attribute__((packed));

struct acpi_sdt {
	char		signature[4];
	uint32_t	length;
	uint8_t		revision;
	uint8_t		checksum;
	char		oem_id[6];
	char		oem_table_id[8];
	uint32_t	oem_revision;
	uint32_t	creator_id;
	uint32_t	creator_revision;
} __attribute__((packed));

struct acpi_rsdt {
	struct acpi_sdt	sdt;
	uint32_t	table_ptrs[1];
} __attribute__((packed));

struct acpi_madt {
	struct acpi_sdt	sdt;
	uint32_t	lapic_addr;
	uint32_t	flags;
	uint8_t		entries[];
} __attribute__((packed));

struct acpi_lapic_entry {
	uint8_t		type;
	uint8_t		length;
	uint8_t		acpi_id;
	uint8_t		apic_id;
	uint32_t	flags;
} __attribute__((packed));

struct acpi_ioapic_entry {
	uint8_t		type;
	uint8_t		length;
	uint8_t		ioapic_id;
	uint8_t		reserved;
	uint32_t	ioapic_addr;
	uint32_t	gsi_base;
} __attribute__((packed));

struct gpa_span {
	uint64_t	start;
	uint64_t	end;
	uint32_t	type;
};

struct guest_alloc {
	uint64_t	mem_size;
	struct gpa_span span[64];
	size_t	span_count;
};

static uint64_t
align_up(uint64_t v, uint64_t align)
{
	return (v + align - 1) & ~(align - 1);
}

static uint64_t
align_down(uint64_t v, uint64_t align)
{
	return v & ~(align - 1);
}

static uint64_t
read_le_desc(const uint8_t *p, uint32_t size)
{
	uint64_t v = 0;
	uint32_t i;

	if (size > sizeof(v))
		errx(1, "unsupported PVH entry descriptor size %u", size);
	for (i = 0; i < size; i++)
		v |= (uint64_t)p[i] << (i * 8);
	return v;
}

static int
u64_add_overflow(uint64_t a, uint64_t b, uint64_t *out)
{
	*out = a + b;
	return *out < a;
}

static int
span_overlaps(uint64_t start, uint64_t end, uint64_t other_start,
    uint64_t other_end)
{
	return start < other_end && other_start < end;
}

static void
add_span(struct guest_alloc *ga, uint64_t start, uint64_t size, uint32_t type)
{
	uint64_t end;

	if (size == 0)
		return;
	if (u64_add_overflow(start, size, &end) || end > ga->mem_size)
		errx(1, "guest range 0x%jx+0x%jx exceeds memory",
		    (uintmax_t)start, (uintmax_t)size);
	if (ga->span_count == sizeof(ga->span) / sizeof(ga->span[0]))
		errx(1, "too many guest ranges");
	ga->span[ga->span_count].start = start;
	ga->span[ga->span_count].end = end;
	ga->span[ga->span_count].type = type;
	ga->span_count++;
}

static uint64_t
guest_alloc_down(struct guest_alloc *ga, uint64_t size, uint64_t align,
    uint32_t type)
{
	uint64_t top = ga->mem_size;

	size = align_up(size, align);
	for (;;) {
		uint64_t start;
		uint64_t end;
		size_t i;
		int conflict = 0;

		if (top < size)
			errx(1, "no guest memory left for boot data");
		start = align_down(top - size, align);
		end = start + size;
		for (i = 0; i < ga->span_count; i++) {
			if (!span_overlaps(start, end, ga->span[i].start,
			    ga->span[i].end)) {
				continue;
			}
			top = ga->span[i].start;
			conflict = 1;
			break;
		}
		if (!conflict) {
			add_span(ga, start, size, type);
			return start;
		}
	}
}

static void
check_file_range(uint64_t file_size, uint64_t off, uint64_t size)
{
	uint64_t end;

	if (u64_add_overflow(off, size, &end) || end > file_size)
		errx(1, "ELF range 0x%jx+0x%jx exceeds file size 0x%jx",
		    (uintmax_t)off, (uintmax_t)size, (uintmax_t)file_size);
}

static void
check_guest_range(uint64_t mem_size, uint64_t pa, uint64_t size)
{
	uint64_t end;

	if (size == 0 || u64_add_overflow(pa, size, &end) || end > mem_size)
		errx(1, "guest physical range 0x%jx+0x%jx exceeds memory 0x%jx",
		    (uintmax_t)pa, (uintmax_t)size, (uintmax_t)mem_size);
}

static void
validate_elf_header(const Elf64_Ehdr *eh, uint64_t file_size)
{
	uint64_t ph_size;

	if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
	    eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3)
		errx(1, "not an ELF file");
	if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
	    eh->e_ident[6] != EV_CURRENT)
		errx(1, "ELF is not little-endian x86_64 class");
	if (eh->e_machine != EM_X86_64 || eh->e_version != EV_CURRENT)
		errx(1, "ELF machine/version is not x86_64/current");
	if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0)
		errx(1, "ELF has unsupported program header table");
	if (u64_add_overflow(0, (uint64_t)eh->e_phentsize * eh->e_phnum,
	    &ph_size))
		errx(1, "ELF program header size overflow");
	check_file_range(file_size, eh->e_phoff, ph_size);
}

static const Elf64_Phdr *
phdr_at(const uint8_t *elf, const Elf64_Ehdr *eh, uint16_t i)
{
	return (const Elf64_Phdr *)(const void *)(elf + eh->e_phoff +
	    (uint64_t)i * eh->e_phentsize);
}

static uint64_t
find_pvh_entry(const uint8_t *elf, uint64_t file_size, const Elf64_Ehdr *eh)
{
	uint16_t i;

	for (i = 0; i < eh->e_phnum; i++) {
		const Elf64_Phdr *ph = phdr_at(elf, eh, i);
		uint64_t off;
		uint64_t end;

		if (ph->p_type != PT_NOTE)
			continue;
		check_file_range(file_size, ph->p_offset, ph->p_filesz);
		off = ph->p_offset;
		end = ph->p_offset + ph->p_filesz;
		while (off + sizeof(Elf64_Nhdr) <= end) {
			const Elf64_Nhdr *nh;
			const uint8_t *name;
			const uint8_t *desc;
			uint64_t name_off;
			uint64_t desc_off;
			uint64_t next;

			nh = (const Elf64_Nhdr *)(const void *)(elf + off);
			name_off = off + sizeof(*nh);
			desc_off = align_up(name_off + nh->n_namesz, 4);
			next = align_up(desc_off + nh->n_descsz, 4);
			if (next > end || next <= off)
				errx(1, "malformed ELF note");
			name = elf + name_off;
			desc = elf + desc_off;
			if (nh->n_type == XEN_ELFNOTE_PHYS32_ENTRY &&
			    ((nh->n_namesz == 4 && memcmp(name, "Xen", 4) == 0) ||
			     (nh->n_namesz == 6 && memcmp(name, "nuttx", 6) == 0))) {
				uint64_t entry = read_le_desc(desc, nh->n_descsz);

				if (entry > UINT32_MAX)
					errx(1, "PVH entry is above 4GB");
				return entry;
			}
			off = next;
		}
	}
	errx(1, "ELF has no Xen PVH 32-bit entry note");
}

static uint64_t
segment_guest_pa(const Elf64_Phdr *ph)
{
	return ph->p_paddr != 0 ? ph->p_paddr : ph->p_vaddr;
}

static void
load_elf_segments(uint8_t *mem, uint64_t mem_size, const uint8_t *elf,
    uint64_t file_size, const Elf64_Ehdr *eh, struct guest_alloc *ga)
{
	uint16_t i;
	int loaded = 0;

	for (i = 0; i < eh->e_phnum; i++) {
		const Elf64_Phdr *ph = phdr_at(elf, eh, i);
		uint64_t pa;

		if (ph->p_type != PT_LOAD)
			continue;
		if (ph->p_memsz < ph->p_filesz)
			errx(1, "ELF PT_LOAD memsz smaller than filesz");
		check_file_range(file_size, ph->p_offset, ph->p_filesz);
		pa = segment_guest_pa(ph);
		check_guest_range(mem_size, pa, ph->p_memsz);
		memset(mem + pa, 0, (size_t)ph->p_memsz);
		memcpy(mem + pa, elf + ph->p_offset, (size_t)ph->p_filesz);
		add_span(ga, pa, ph->p_memsz, GPA_RANGE_LOAD);
		loaded = 1;
	}
	if (!loaded)
		errx(1, "ELF has no loadable segments");
}

static uint64_t
gdt_entry32(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
	uint64_t d = 0;

	d |= limit & 0xffffU;
	d |= (uint64_t)(base & 0xffffffU) << 16;
	d |= (uint64_t)access << 40;
	d |= (uint64_t)((limit >> 16) & 0xfU) << 48;
	d |= (uint64_t)(flags & 0xfU) << 52;
	d |= (uint64_t)((base >> 24) & 0xffU) << 56;
	return d;
}

static void
write64(uint8_t *mem, uint64_t gpa, uint64_t value)
{
	memcpy(mem + gpa, &value, sizeof(value));
}

static void
set_seg(struct vmm_x64_seg_state *seg, uint16_t selector, uint16_t attrib,
    uint32_t limit, uint64_t base)
{
	seg->selector = selector;
	seg->attrib = attrib;
	seg->limit = limit;
	seg->base = base;
}

static uint8_t
checksum_bytes(const void *ptr, size_t len)
{
	const uint8_t *p = ptr;
	uint8_t sum = 0;
	size_t i;

	for (i = 0; i < len; i++)
		sum += p[i];
	return (uint8_t)(0U - sum);
}

static void
fill_sdt_header(struct acpi_sdt *sdt, const char *sig, uint32_t len,
    uint8_t revision)
{
	memset(sdt, 0, sizeof(*sdt));
	memcpy(sdt->signature, sig, 4);
	sdt->length = len;
	sdt->revision = revision;
	memcpy(sdt->oem_id, "DFVMM ", 6);
	memcpy(sdt->oem_table_id, "DFVMM   ", 8);
	sdt->oem_revision = 1;
	sdt->creator_id = 0x4d4d5644U;		/* DVMM */
	sdt->creator_revision = 1;
}

static void
finish_sdt(void *table)
{
	struct acpi_sdt *sdt = table;

	sdt->checksum = 0;
	sdt->checksum = checksum_bytes(table, sdt->length);
}

static void
build_acpi_tables(uint8_t *mem, struct guest_alloc *ga)
{
	struct acpi_rsdp *rsdp;
	struct acpi_rsdt *rsdt;
	struct acpi_madt *madt;
	struct acpi_lapic_entry *lapic;
	struct acpi_ioapic_entry *ioapic;
	uint8_t *entry;
	uint32_t madt_len;

	check_guest_range(ga->mem_size, ACPI_REGION_GPA, ACPI_REGION_SIZE);
	memset(mem + ACPI_REGION_GPA, 0, ACPI_REGION_SIZE);
	add_span(ga, ACPI_REGION_GPA, ACPI_REGION_SIZE, GPA_RANGE_BOOT);

	rsdp = (struct acpi_rsdp *)(void *)(mem + ACPI_RSDP_GPA);
	memset(rsdp, 0, sizeof(*rsdp));
	memcpy(rsdp->signature, "RSD PTR ", 8);
	memcpy(rsdp->oem_id, "DFVMM ", 6);
	rsdp->revision = 2;
	rsdp->rsdt_addr = ACPI_RSDT_GPA;
	rsdp->length = sizeof(*rsdp);
	rsdp->checksum = checksum_bytes(rsdp, 20);
	rsdp->ext_checksum = checksum_bytes(rsdp, sizeof(*rsdp));

	rsdt = (struct acpi_rsdt *)(void *)(mem + ACPI_RSDT_GPA);
	fill_sdt_header(&rsdt->sdt, "RSDT", sizeof(*rsdt), 1);
	rsdt->table_ptrs[0] = ACPI_MADT_GPA;
	finish_sdt(rsdt);

	madt = (struct acpi_madt *)(void *)(mem + ACPI_MADT_GPA);
	madt_len = sizeof(*madt) + sizeof(*lapic) + sizeof(*ioapic);
	fill_sdt_header(&madt->sdt, "APIC", madt_len, 1);
	madt->lapic_addr = ACPI_LAPIC_BASE;
	madt->flags = 1;
	entry = madt->entries;
	lapic = (struct acpi_lapic_entry *)(void *)entry;
	lapic->type = 0;
	lapic->length = sizeof(*lapic);
	lapic->acpi_id = 0;
	lapic->apic_id = 0;
	lapic->flags = 1;
	entry += sizeof(*lapic);
	ioapic = (struct acpi_ioapic_entry *)(void *)entry;
	ioapic->type = 1;
	ioapic->length = sizeof(*ioapic);
	ioapic->ioapic_id = 1;
	ioapic->ioapic_addr = ACPI_IOAPIC_BASE;
	ioapic->gsi_base = 0;
	finish_sdt(madt);
}

static uint64_t
build_multiboot2_info(uint8_t *mem, struct guest_alloc *ga)
{
	struct multiboot_info_header *mb;
	struct multiboot_tag_header *tag;
	uint64_t gpa;
	size_t off;

	gpa = guest_alloc_down(ga, PAGE_SIZE_GUEST, PAGE_SIZE_GUEST,
	    GPA_RANGE_BOOT);
	memset(mem + gpa, 0, PAGE_SIZE_GUEST);

	mb = (struct multiboot_info_header *)(void *)(mem + gpa);
	off = sizeof(*mb);

	tag = (struct multiboot_tag_header *)(void *)(mem + gpa + off);
	tag->type = MULTIBOOT_TAG_TYPE_ACPI_NEW;
	tag->size = sizeof(*tag) + sizeof(struct acpi_rsdp);
	memcpy((uint8_t *)tag + sizeof(*tag), mem + ACPI_RSDP_GPA,
	    sizeof(struct acpi_rsdp));
	off = align_up(off + tag->size, 8);

	tag = (struct multiboot_tag_header *)(void *)(mem + gpa + off);
	tag->type = MULTIBOOT_TAG_TYPE_END;
	tag->size = sizeof(*tag);
	off = align_up(off + tag->size, 8);

	mb->total_size = off;
	mb->reserved = 0;
	return gpa;
}

static void
build_boot_data(uint8_t *mem, struct guest_alloc *ga, uint64_t pvh_entry,
    struct vmm_x64_vcpu_state *vcpu)
{
	uint64_t stack_base;
	uint64_t stack_top;
	uint64_t gdt;
	uint64_t tss;
	uint64_t multiboot_gpa;

	build_acpi_tables(mem, ga);
	stack_base = guest_alloc_down(ga, BOOT_STACK_SIZE, PAGE_SIZE_GUEST,
	    GPA_RANGE_STACK);
	gdt = guest_alloc_down(ga, PAGE_SIZE_GUEST, PAGE_SIZE_GUEST,
	    GPA_RANGE_BOOT);
	tss = guest_alloc_down(ga, PAGE_SIZE_GUEST, PAGE_SIZE_GUEST,
	    GPA_RANGE_BOOT);
	multiboot_gpa = build_multiboot2_info(mem, ga);
	stack_top = stack_base + BOOT_STACK_SIZE;

	memset(mem + gdt, 0, PAGE_SIZE_GUEST);
	write64(mem, gdt + 0x08, gdt_entry32(0, 0xfffff, 0x9a, 0x0c));
	write64(mem, gdt + 0x10, gdt_entry32(0, 0xfffff, 0x92, 0x0c));
	write64(mem, gdt + 0x18, gdt_entry32((uint32_t)tss, 0x67, 0x89, 0));

	memset(mem + tss, 0, PAGE_SIZE_GUEST);

	memset(vcpu, 0, sizeof(*vcpu));
	vcpu->runnable = 1;
	vcpu->gpr[VMM_X64_GPR_RBX] = multiboot_gpa;
	vcpu->gpr[VMM_X64_GPR_RAX] = MULTIBOOT2_BOOTLOADER_MAGIC;
	vcpu->gpr[VMM_X64_GPR_RSP] = stack_top - sizeof(uint64_t);
	vcpu->gpr[VMM_X64_GPR_RIP] = pvh_entry;
	vcpu->gpr[VMM_X64_GPR_RFLAGS] = 2;
	vcpu->cr[VMM_X64_CR_CR0] = CR0_PE | CR0_NE;
	vcpu->cr[VMM_X64_CR_CR2] = 0;
	vcpu->cr[VMM_X64_CR_CR3] = 0;
	vcpu->cr[VMM_X64_CR_CR4] = 0;
	vcpu->cr[VMM_X64_CR_CR8] = 0;
	vcpu->cr[VMM_X64_CR_XCR0] = XCR0_X87;
	vcpu->msr[VMM_X64_MSR_EFER] = 0;
	vcpu->msr[VMM_X64_MSR_PAT] = 0x0007040600070406ULL;
	set_seg(&vcpu->seg[VMM_X64_SEG_ES], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_seg(&vcpu->seg[VMM_X64_SEG_CS], 0x08,
	    0xb | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_seg(&vcpu->seg[VMM_X64_SEG_SS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_seg(&vcpu->seg[VMM_X64_SEG_DS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_seg(&vcpu->seg[VMM_X64_SEG_FS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_seg(&vcpu->seg[VMM_X64_SEG_GS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	set_seg(&vcpu->seg[VMM_X64_SEG_GDT], 0, 0, 0x1f, gdt);
	set_seg(&vcpu->seg[VMM_X64_SEG_IDT], 0, 0, 0, 0);
	set_seg(&vcpu->seg[VMM_X64_SEG_LDT], 0, SEG_UNUSABLE, 0, 0);
	set_seg(&vcpu->seg[VMM_X64_SEG_TR], 0x18, 0x9 | SEG_P, 0x67, tss);
}

static size_t
align8_size(size_t v)
{
	return (v + 7U) & ~(size_t)7U;
}

static uint8_t *
add_record(uint8_t *p, uint16_t type, uint16_t flags, const void *payload,
    uint32_t size)
{
	struct vmm_manifest_record rec;
	size_t total;

	rec.type = type;
	rec.flags = flags;
	rec.size = size;
	total = align8_size(sizeof(rec) + size);
	memcpy(p, &rec, sizeof(rec));
	memcpy(p + sizeof(rec), payload, size);
	memset(p + sizeof(rec) + size, 0, total - sizeof(rec) - size);
	return p + total;
}

static void
fill_manifest(uint8_t *manifest, uint64_t manifest_size, uint64_t mem_size,
    const struct vmm_x64_vcpu_state *vcpu, const struct guest_alloc *ga)
{
	struct vmm_manifest_header hdr;
	struct vmm_gpa_range ranges[VMM_GPA_RANGE_MAX];
	uint8_t *p;
	size_t range_count = 0;
	size_t i;

	if (manifest_size < PAGE_SIZE_GUEST)
		errx(1, "manifest fd is smaller than one page");
	memset(manifest, 0, (size_t)manifest_size);
	for (i = 0; i < ga->span_count && range_count < VMM_GPA_RANGE_MAX; i++) {
		ranges[range_count].start = ga->span[i].start;
		ranges[range_count].size = ga->span[i].end - ga->span[i].start;
		ranges[range_count].type = ga->span[i].type;
		ranges[range_count].flags = 0;
		range_count++;
	}
	if (i != ga->span_count)
		errx(1, "too many guest ranges for manifest");

	p = manifest + sizeof(hdr);
	p = add_record(p, VMM_REC_X64_VCPU_STATE, VMM_REC_F_MANDATORY, vcpu,
	    sizeof(*vcpu));
	p = add_record(p, VMM_REC_GPA_RANGE, VMM_REC_F_MANDATORY, ranges,
	    (uint32_t)(range_count * sizeof(ranges[0])));
	if ((uint64_t)(p - manifest) > manifest_size)
		errx(1, "manifest does not fit fd4");

	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, VMM_MANIFEST_MAGIC, sizeof(hdr.magic));
	hdr.abi_version = VMM_MANIFEST_ABI;
	hdr.arch = VMM_MANIFEST_ARCH_X64;
	hdr.header_size = sizeof(hdr);
	hdr.total_size = (uint32_t)(p - manifest);
	hdr.record_count = 2;
	hdr.mem_size = mem_size;
	memcpy(manifest, &hdr, sizeof(hdr));
}

int
main(int argc, char **argv)
{
	struct stat mem_st;
	struct stat manifest_st;
	struct stat elf_st;
	int elf_fd;
	uint8_t *mem;
	uint8_t *manifest;
	uint8_t *elf;
	const Elf64_Ehdr *eh;
	struct guest_alloc ga;
	struct vmm_x64_vcpu_state vcpu;
	uint64_t pvh_entry;
	const char *elf_path = argc >= 2 ? argv[1] : NUTTX_ELF_PATH;

	if (fstat(3, &mem_st) != 0)
		err(1, "fstat fd3");
	if (fstat(4, &manifest_st) != 0)
		err(1, "fstat fd4");
	if (mem_st.st_size <= 0 || manifest_st.st_size <= 0)
		errx(1, "invalid loader fd size");
	if ((uint64_t)mem_st.st_size > SIZE_MAX ||
	    (uint64_t)manifest_st.st_size > SIZE_MAX)
		errx(1, "loader fd too large for process address space");

	mem = mmap(NULL, (size_t)mem_st.st_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, 3, 0);
	if (mem == MAP_FAILED)
		err(1, "mmap fd3");
	manifest = mmap(NULL, (size_t)manifest_st.st_size,
	    PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
	if (manifest == MAP_FAILED)
		err(1, "mmap fd4");

	elf_fd = open(elf_path, O_RDONLY);
	if (elf_fd < 0)
		err(1, "open %s", elf_path);
	if (fstat(elf_fd, &elf_st) != 0)
		err(1, "fstat %s", elf_path);
	if (elf_st.st_size <= (off_t)sizeof(Elf64_Ehdr))
		errx(1, "%s is too small", elf_path);
	elf = mmap(NULL, (size_t)elf_st.st_size, PROT_READ, MAP_PRIVATE,
	    elf_fd, 0);
	if (elf == MAP_FAILED)
		err(1, "mmap %s", elf_path);

	eh = (const Elf64_Ehdr *)(const void *)elf;
	validate_elf_header(eh, (uint64_t)elf_st.st_size);
	pvh_entry = find_pvh_entry(elf, (uint64_t)elf_st.st_size, eh);
	check_guest_range((uint64_t)mem_st.st_size, pvh_entry, 1);

	memset(&ga, 0, sizeof(ga));
	ga.mem_size = (uint64_t)mem_st.st_size;
	memset(mem, 0, (size_t)mem_st.st_size);
	load_elf_segments(mem, ga.mem_size, elf, (uint64_t)elf_st.st_size, eh,
	    &ga);
	build_boot_data(mem, &ga, pvh_entry, &vcpu);
	fill_manifest(manifest, (uint64_t)manifest_st.st_size, ga.mem_size,
	    &vcpu, &ga);

	return 0;
}
