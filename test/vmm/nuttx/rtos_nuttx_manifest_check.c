/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Offline verifier for rtos_nuttx_loader.c output.
 *
 * It inspects ordinary files used as fd3/fd4 stand-ins.  It does not load the
 * vmm module, mount vmmfs, or execute a guest.
 */
#include <sys/mman.h>
#include <sys/stat.h>

#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VMM_MANIFEST_MAGIC	"VMMLD0\0\0"
#define VMM_MANIFEST_ABI	1
#define VMM_MANIFEST_ARCH_X64	1
#define VMM_REC_X64_VCPU_STATE	1
#define VMM_REC_GPA_RANGE	2
#define VMM_REC_X64_TIME_STATE	3
#define VMM_REC_F_MANDATORY	1
#define VMM_GPA_RANGE_MAX	32

#define MULTIBOOT2_BOOTLOADER_MAGIC	0x36d76289U
#define MULTIBOOT_TAG_TYPE_END		0U
#define MULTIBOOT_TAG_TYPE_ACPI_NEW	15U

#define PAGE_SIZE_GUEST		4096ULL
#define ACPI_RSDP_GPA		0x000f0000ULL
#define ACPI_RSDT_GPA		0x000f1000ULL
#define ACPI_MADT_GPA		0x000f2000ULL
#define ACPI_REGION_SIZE	(3ULL * PAGE_SIZE_GUEST)
#define ACPI_LAPIC_BASE		0xfee00000U
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

struct vmm_x64_time_state {
	uint64_t	tsc_hz;
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

static size_t
align8(size_t v)
{
	return (v + 7U) & ~(size_t)7U;
}

static void
check_range(uint64_t cap, uint64_t off, uint64_t size, const char *what)
{
	if (size == 0 || off > cap || size > cap - off) {
		errx(1, "%s range 0x%jx+0x%jx exceeds 0x%jx", what,
		    (uintmax_t)off, (uintmax_t)size, (uintmax_t)cap);
	}
}

static uint8_t
sum8(const void *ptr, size_t len)
{
	const uint8_t *p = ptr;
	uint8_t sum = 0;
	size_t i;

	for (i = 0; i < len; i++)
		sum += p[i];
	return sum;
}

static void *
map_file(const char *path, size_t *sizep)
{
	struct stat st;
	void *p;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		err(1, "open %s", path);
	if (fstat(fd, &st) != 0)
		err(1, "fstat %s", path);
	if (st.st_size <= 0)
		errx(1, "%s is empty", path);
	p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		err(1, "mmap %s", path);
	close(fd);
	*sizep = (size_t)st.st_size;
	return p;
}

static void
check_seg(const struct vmm_x64_seg_state *seg, uint16_t selector,
    uint16_t attrib, uint32_t limit, uint64_t base, const char *name)
{
	if (seg->selector != selector || seg->attrib != attrib ||
	    seg->limit != limit || seg->base != base) {
		errx(1, "%s segment mismatch", name);
	}
}

static void
check_acpi(const uint8_t *mem, size_t mem_size)
{
	const struct acpi_rsdp *rsdp;
	const struct acpi_rsdt *rsdt;
	const struct acpi_madt *madt;
	const struct acpi_lapic_entry *lapic;
	const struct acpi_ioapic_entry *ioapic;
	uint32_t madt_len;

	check_range(mem_size, ACPI_RSDP_GPA, ACPI_REGION_SIZE, "ACPI");
	rsdp = (const struct acpi_rsdp *)(const void *)(mem + ACPI_RSDP_GPA);
	rsdt = (const struct acpi_rsdt *)(const void *)(mem + ACPI_RSDT_GPA);
	madt = (const struct acpi_madt *)(const void *)(mem + ACPI_MADT_GPA);

	if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0)
		errx(1, "RSDP signature mismatch");
	if (rsdp->revision != 2 || rsdp->rsdt_addr != ACPI_RSDT_GPA ||
	    rsdp->length != sizeof(*rsdp) || rsdp->xsdt_addr != 0) {
		errx(1, "RSDP fields mismatch");
	}
	if (memcmp(rsdp->oem_id, "VMM   ", 6) != 0)
		errx(1, "RSDP OEM ID mismatch");
	if (sum8(rsdp, 20) != 0 || sum8(rsdp, sizeof(*rsdp)) != 0)
		errx(1, "RSDP checksum mismatch");

	if (memcmp(rsdt->sdt.signature, "RSDT", 4) != 0 ||
	    rsdt->sdt.length != sizeof(*rsdt) ||
	    rsdt->table_ptrs[0] != ACPI_MADT_GPA) {
		errx(1, "RSDT fields mismatch");
	}
	if (memcmp(rsdt->sdt.oem_id, "VMM   ", 6) != 0 ||
	    memcmp(rsdt->sdt.oem_table_id, "VMM     ", 8) != 0 ||
	    rsdt->sdt.creator_id != 0x204d4d56U) {
		errx(1, "RSDT OEM fields mismatch");
	}
	if (sum8(rsdt, rsdt->sdt.length) != 0)
		errx(1, "RSDT checksum mismatch");

	madt_len = sizeof(*madt) + sizeof(*lapic) + sizeof(*ioapic);
	if (memcmp(madt->sdt.signature, "APIC", 4) != 0 ||
	    madt->sdt.length != madt_len ||
	    madt->lapic_addr != ACPI_LAPIC_BASE ||
	    madt->flags != 1) {
		errx(1, "MADT fields mismatch");
	}
	if (memcmp(madt->sdt.oem_id, "VMM   ", 6) != 0 ||
	    memcmp(madt->sdt.oem_table_id, "VMM     ", 8) != 0 ||
	    madt->sdt.creator_id != 0x204d4d56U) {
		errx(1, "MADT OEM fields mismatch");
	}
	if (sum8(madt, madt->sdt.length) != 0)
		errx(1, "MADT checksum mismatch");

	lapic = (const struct acpi_lapic_entry *)(const void *)madt->entries;
	ioapic = (const struct acpi_ioapic_entry *)(const void *)
	    (madt->entries + sizeof(*lapic));
	if (lapic->type != 0 || lapic->length != sizeof(*lapic) ||
	    lapic->acpi_id != 0 || lapic->apic_id != 0 ||
	    lapic->flags != 1) {
		errx(1, "MADT LAPIC entry mismatch");
	}
	if (ioapic->type != 1 || ioapic->length != sizeof(*ioapic) ||
	    ioapic->ioapic_id != 1 || ioapic->ioapic_addr != ACPI_IOAPIC_BASE ||
	    ioapic->gsi_base != 0) {
		errx(1, "MADT IOAPIC entry mismatch");
	}
}

static void
check_multiboot(const uint8_t *mem, size_t mem_size, uint64_t gpa)
{
	const struct multiboot_info_header *mb;
	const struct multiboot_tag_header *tag;
	int found_acpi = 0;
	uint64_t off;

	check_range(mem_size, gpa, sizeof(*mb), "multiboot header");
	mb = (const struct multiboot_info_header *)(const void *)(mem + gpa);
	if (mb->reserved != 0 || mb->total_size < sizeof(*mb) + sizeof(*tag) ||
	    mb->total_size > PAGE_SIZE_GUEST) {
		errx(1, "Multiboot2 header mismatch");
	}
	check_range(mem_size, gpa, mb->total_size, "multiboot info");
	off = sizeof(*mb);
	for (;;) {
		uint64_t next;

		if (off + sizeof(*tag) > mb->total_size)
			errx(1, "Multiboot2 tag overrun");
		tag = (const struct multiboot_tag_header *)(const void *)
		    (mem + gpa + off);
		if (tag->size < sizeof(*tag) || off + tag->size > mb->total_size)
			errx(1, "Multiboot2 malformed tag");
		if (tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW) {
			const struct acpi_rsdp *rsdp;

			if (tag->size != sizeof(*tag) + sizeof(*rsdp))
				errx(1, "Multiboot2 ACPI tag size mismatch");
			rsdp = (const struct acpi_rsdp *)(const void *)
			    ((const uint8_t *)tag + sizeof(*tag));
			if (memcmp(rsdp, mem + ACPI_RSDP_GPA, sizeof(*rsdp)) != 0)
				errx(1, "Multiboot2 ACPI tag does not copy RSDP");
			found_acpi = 1;
		} else if (tag->type == MULTIBOOT_TAG_TYPE_END) {
			if (tag->size != sizeof(*tag))
				errx(1, "Multiboot2 end tag size mismatch");
			break;
		}
		next = align8(off + tag->size);
		if (next <= off)
			errx(1, "Multiboot2 tag wrap");
		off = next;
	}
	if (!found_acpi)
		errx(1, "Multiboot2 ACPI tag missing");
}

static void
check_ranges(const struct vmm_gpa_range *ranges, uint32_t count,
    uint64_t mem_size)
{
	uint32_t i;
	int have_acpi = 0;
	int have_boot = 0;
	int have_load = 0;
	int have_stack = 0;

	if (count == 0 || count > VMM_GPA_RANGE_MAX)
		errx(1, "range count invalid");
	for (i = 0; i < count; i++) {
		check_range(mem_size, ranges[i].start, ranges[i].size,
		    "GPA range");
		if (ranges[i].flags != 0)
			errx(1, "GPA range flags nonzero");
		switch (ranges[i].type) {
		case GPA_RANGE_LOAD:
			have_load = 1;
			break;
		case GPA_RANGE_BOOT:
			have_boot = 1;
			if (ranges[i].start == ACPI_RSDP_GPA &&
			    ranges[i].size == ACPI_REGION_SIZE) {
				have_acpi = 1;
			}
			break;
		case GPA_RANGE_STACK:
			have_stack = 1;
			break;
		default:
			errx(1, "unknown GPA range type %u", ranges[i].type);
		}
	}
	if (!have_load || !have_boot || !have_stack || !have_acpi)
		errx(1, "required GPA ranges missing");
}

static void
check_vcpu(const uint8_t *mem, size_t mem_size,
    const struct vmm_x64_vcpu_state *vcpu)
{
	if (vcpu->vcpu_id != 0 || vcpu->flags != 0 || vcpu->runnable != 1)
		errx(1, "vCPU header mismatch");
	if (vcpu->gpr[VMM_X64_GPR_RAX] != MULTIBOOT2_BOOTLOADER_MAGIC)
		errx(1, "vCPU RAX is not Multiboot2 magic");
	check_multiboot(mem, mem_size, vcpu->gpr[VMM_X64_GPR_RBX]);
	check_range(mem_size, vcpu->gpr[VMM_X64_GPR_RIP], 1, "RIP");
	check_range(mem_size, vcpu->gpr[VMM_X64_GPR_RSP], 8, "RSP");
	if (vcpu->gpr[VMM_X64_GPR_RFLAGS] != 2)
		errx(1, "RFLAGS mismatch");
	if (vcpu->cr[VMM_X64_CR_CR0] != (CR0_PE | CR0_NE) ||
	    vcpu->cr[VMM_X64_CR_CR2] != 0 ||
	    vcpu->cr[VMM_X64_CR_CR3] != 0 ||
	    vcpu->cr[VMM_X64_CR_CR4] != 0 ||
	    vcpu->cr[VMM_X64_CR_CR8] != 0 ||
	    vcpu->cr[VMM_X64_CR_XCR0] != XCR0_X87) {
		errx(1, "control register launch state mismatch");
	}
	if (vcpu->msr[VMM_X64_MSR_EFER] != 0 ||
	    vcpu->msr[VMM_X64_MSR_PAT] != 0x0007040600070406ULL) {
		errx(1, "MSR launch state mismatch");
	}
	check_seg(&vcpu->seg[VMM_X64_SEG_ES], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0, "ES");
	check_seg(&vcpu->seg[VMM_X64_SEG_CS], 0x08,
	    0xb | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0, "CS");
	check_seg(&vcpu->seg[VMM_X64_SEG_SS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0, "SS");
	check_seg(&vcpu->seg[VMM_X64_SEG_DS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0, "DS");
	check_seg(&vcpu->seg[VMM_X64_SEG_FS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0, "FS");
	check_seg(&vcpu->seg[VMM_X64_SEG_GS], 0x10,
	    0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0, "GS");
	check_range(mem_size, vcpu->seg[VMM_X64_SEG_GDT].base,
	    PAGE_SIZE_GUEST, "GDT");
	if (vcpu->seg[VMM_X64_SEG_GDT].limit != 0x1f ||
	    vcpu->seg[VMM_X64_SEG_IDT].limit != 0 ||
	    vcpu->seg[VMM_X64_SEG_IDT].base != 0 ||
	    vcpu->seg[VMM_X64_SEG_LDT].attrib != SEG_UNUSABLE) {
		errx(1, "descriptor-table launch state mismatch");
	}
	check_range(mem_size, vcpu->seg[VMM_X64_SEG_TR].base,
	    PAGE_SIZE_GUEST, "TSS");
	if (vcpu->seg[VMM_X64_SEG_TR].selector != 0x18 ||
	    vcpu->seg[VMM_X64_SEG_TR].attrib != (0x9 | SEG_P) ||
	    vcpu->seg[VMM_X64_SEG_TR].limit != 0x67 ||
	    vcpu->intr_flags != 0) {
		errx(1, "TR/intr launch state mismatch");
	}
}

static void
check_manifest(const uint8_t *mem, size_t mem_size, const uint8_t *manifest,
    size_t manifest_size)
{
	const struct vmm_manifest_header *hdr;
	const struct vmm_x64_vcpu_state *vcpu = NULL;
	const struct vmm_x64_time_state *time = NULL;
	const struct vmm_gpa_range *ranges = NULL;
	uint32_t range_count = 0;
	uint32_t records = 0;
	size_t off;

	if (manifest_size < sizeof(*hdr))
		errx(1, "manifest is too small");
	hdr = (const struct vmm_manifest_header *)(const void *)manifest;
	if (memcmp(hdr->magic, VMM_MANIFEST_MAGIC, sizeof(hdr->magic)) != 0 ||
	    hdr->abi_version != VMM_MANIFEST_ABI ||
	    hdr->arch != VMM_MANIFEST_ARCH_X64 ||
	    hdr->header_size != sizeof(*hdr) ||
	    hdr->total_size < hdr->header_size ||
	    hdr->total_size > manifest_size ||
	    hdr->record_count != 3 ||
	    hdr->mem_size != mem_size ||
	    hdr->flags != 0 ||
	    hdr->reserved != 0) {
		errx(1, "manifest header mismatch");
	}
	off = hdr->header_size;
	while (off < hdr->total_size) {
		const struct vmm_manifest_record *rec;
		const uint8_t *payload;
		size_t next;

		if (hdr->total_size - off < sizeof(*rec))
			errx(1, "manifest record overrun");
		rec = (const struct vmm_manifest_record *)(const void *)
		    (manifest + off);
		next = off + align8(sizeof(*rec) + rec->size);
		if (next < off || next > hdr->total_size ||
		    off + sizeof(*rec) + rec->size > hdr->total_size) {
			errx(1, "manifest record size mismatch");
		}
		if ((rec->flags & VMM_REC_F_MANDATORY) == 0)
			errx(1, "manifest record is not mandatory");
		payload = manifest + off + sizeof(*rec);
		switch (rec->type) {
		case VMM_REC_X64_VCPU_STATE:
			if (vcpu != NULL || rec->size != sizeof(*vcpu))
				errx(1, "vCPU record mismatch");
			vcpu = (const struct vmm_x64_vcpu_state *)(const void *)
			    payload;
			break;
		case VMM_REC_X64_TIME_STATE:
			if (time != NULL || rec->size != sizeof(*time))
				errx(1, "time record mismatch");
			time = (const struct vmm_x64_time_state *)(const void *)
			    payload;
			if (time->tsc_hz != 0)
				errx(1, "unexpected scaled TSC");
			break;
		case VMM_REC_GPA_RANGE:
			if (ranges != NULL || rec->size == 0 ||
			    (rec->size % sizeof(*ranges)) != 0) {
				errx(1, "GPA range record mismatch");
			}
			ranges = (const struct vmm_gpa_range *)(const void *)
			    payload;
			range_count = rec->size / sizeof(*ranges);
			break;
		default:
			errx(1, "unexpected manifest record type %u", rec->type);
		}
		records++;
		off = next;
	}
	if (records != hdr->record_count || vcpu == NULL || time == NULL ||
	    ranges == NULL)
		errx(1, "manifest records missing");
	check_vcpu(mem, mem_size, vcpu);
	check_ranges(ranges, range_count, mem_size);
}

int
main(int argc, char **argv)
{
	uint8_t *mem;
	uint8_t *manifest;
	size_t mem_size;
	size_t manifest_size;

	if (argc != 3)
		errx(1, "usage: %s mem-file manifest-file", argv[0]);
	mem = map_file(argv[1], &mem_size);
	manifest = map_file(argv[2], &manifest_size);
	check_acpi(mem, mem_size);
	check_manifest(mem, mem_size, manifest, manifest_size);
	printf("PASS: NuttX loader manifest and boot data\n");
	return 0;
}
