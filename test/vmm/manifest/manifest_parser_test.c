/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Userland smoke tests for the kernel x86 manifest parser.
 */
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmm_loader_x86.h"

#define VMM_MANIFEST_MAGIC	"VMMLD0\0\0"
#define VMM_MANIFEST_ABI	1
#define VMM_MANIFEST_ARCH_X64	1
#define VMM_REC_X64_VCPU_STATE	1
#define VMM_REC_GPA_RANGE	2
#define VMM_REC_X64_TIME_STATE	3
#define VMM_REC_X64_CPU_TOPOLOGY 4
#define VMM_REC_F_MANDATORY	1

#define MEM_SIZE		(2ULL * 1024ULL * 1024ULL)
#define LARGE_MEM_SIZE		(VMM_X86_LAPIC_MMIO_GPA + \
				 (2ULL * 1024ULL * 1024ULL))
#define MANIFEST_SIZE		4096U
#define PAGE_SIZE_GUEST		4096ULL
#define PML4_GPA		0x1000ULL
#define GDT_GPA			0x5000ULL
#define IDT_GPA			0x6000ULL
#define TSS_GPA			0x7000ULL
#define ENTRY_GPA		0x100000ULL
#define STACK_GPA		0x180000ULL

#define CR0_PE			0x00000001ULL
#define CR0_NE			0x00000020ULL
#define CR0_PG			0x80000000ULL
#define CR4_PAE			0x00000020ULL
#define RFLAGS_FIXED		0x00000002ULL
#define PAT_DEFAULT		0x0007040600070406ULL

#define SEG_S			0x0010U
#define SEG_P			0x0080U
#define SEG_L			0x0200U
#define SEG_DB			0x0400U
#define SEG_G			0x0800U
#define SEG_UNUSABLE		0x1000U
#define SEG_RESERVED		0x2000U

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

static size_t
align8(size_t v)
{
	return (v + 7U) & ~(size_t)7U;
}

static uint8_t *
add_record_flags(uint8_t *p, uint16_t type, uint16_t flags,
    const void *payload, uint32_t size)
{
	struct vmm_manifest_record rec;
	size_t total;

	rec.type = type;
	rec.flags = flags;
	rec.size = size;
	total = align8(sizeof(rec) + size);
	memcpy(p, &rec, sizeof(rec));
	if (size != 0)
		memcpy(p + sizeof(rec), payload, size);
	memset(p + sizeof(rec) + size, 0, total - sizeof(rec) - size);
	return p + total;
}

static uint8_t *
add_record(uint8_t *p, uint16_t type, const void *payload, uint32_t size)
{
	return add_record_flags(p, type, VMM_REC_F_MANDATORY, payload, size);
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

static void
build_manifest(uint8_t *manifest, struct vmm_x64_vcpu_state *vcpu,
    const struct vmm_x64_time_state *time, struct vmm_gpa_range *range,
    size_t range_count)
{
	struct vmm_manifest_header hdr;
	uint8_t *p;

	memset(manifest, 0, MANIFEST_SIZE);
	memset(&hdr, 0, sizeof(hdr));
	p = manifest + sizeof(hdr);
	p = add_record(p, VMM_REC_X64_VCPU_STATE, vcpu, sizeof(*vcpu));
	p = add_record(p, VMM_REC_X64_TIME_STATE, time, sizeof(*time));
	p = add_record(p, VMM_REC_GPA_RANGE, range,
	    (uint32_t)(sizeof(*range) * range_count));

	memcpy(hdr.magic, VMM_MANIFEST_MAGIC, sizeof(hdr.magic));
	hdr.abi_version = VMM_MANIFEST_ABI;
	hdr.arch = VMM_MANIFEST_ARCH_X64;
	hdr.header_size = sizeof(hdr);
	hdr.total_size = (uint32_t)(p - manifest);
	hdr.record_count = 3;
	hdr.mem_size = MEM_SIZE;
	memcpy(manifest, &hdr, sizeof(hdr));
}

static void
build_valid_state(uint8_t *manifest)
{
	struct vmm_x64_vcpu_state vcpu;
	struct vmm_x64_time_state time;
	struct vmm_gpa_range range[4];

	memset(&vcpu, 0, sizeof(vcpu));
	vcpu.runnable = 1;
	vcpu.gpr[VMM_X64_GPR_RIP] = ENTRY_GPA;
	vcpu.gpr[VMM_X64_GPR_RSP] = STACK_GPA;
	vcpu.gpr[VMM_X64_GPR_RFLAGS] = RFLAGS_FIXED;
	vcpu.cr[VMM_X64_CR_CR0] = CR0_PE | CR0_NE | CR0_PG;
	vcpu.cr[VMM_X64_CR_CR3] = PML4_GPA;
	vcpu.cr[VMM_X64_CR_CR4] = CR4_PAE;
	vcpu.cr[VMM_X64_CR_XCR0] = VMM_X64_XCR0_X87;
	vcpu.msr[VMM_X64_MSR_PAT] = PAT_DEFAULT;
	set_segment(&vcpu.seg[VMM_X64_SEG_GDT], 0, 0, 0x27, GDT_GPA);
	set_segment(&vcpu.seg[VMM_X64_SEG_IDT], 0, 0, 0x0f, IDT_GPA);
	set_segment(&vcpu.seg[VMM_X64_SEG_LDT], 0, SEG_UNUSABLE, 0, 0);
	set_segment(&vcpu.seg[VMM_X64_SEG_TR], 0x18, 0x9 | SEG_P, 0x67,
	    TSS_GPA);

	range[0] = (struct vmm_gpa_range){ ENTRY_GPA, 4,
	    VMM_GPA_RANGE_LOAD, 0 };
	range[1] = (struct vmm_gpa_range){ PML4_GPA, PAGE_SIZE_GUEST,
	    VMM_GPA_RANGE_PAGE_TABLE, 0 };
	range[2] = (struct vmm_gpa_range){ GDT_GPA, PAGE_SIZE_GUEST,
	    VMM_GPA_RANGE_DESC_TABLE, 0 };
	range[3] = (struct vmm_gpa_range){ STACK_GPA - PAGE_SIZE_GUEST,
	    PAGE_SIZE_GUEST, VMM_GPA_RANGE_STACK, 0 };
	time.tsc_hz = 1000000000ULL;
	build_manifest(manifest, &vcpu, &time, range, 4);
}

static struct vmm_x64_vcpu_state *
manifest_vcpu(uint8_t *manifest)
{
	return (struct vmm_x64_vcpu_state *)(void *)(manifest +
	    sizeof(struct vmm_manifest_header) +
	    sizeof(struct vmm_manifest_record));
}

static struct vmm_manifest_header *
manifest_header(uint8_t *manifest)
{
	return (struct vmm_manifest_header *)(void *)manifest;
}

static struct vmm_manifest_record *
manifest_first_record(uint8_t *manifest)
{
	return (struct vmm_manifest_record *)(void *)(manifest +
	    sizeof(struct vmm_manifest_header));
}

static struct vmm_manifest_record *
manifest_time_record(uint8_t *manifest)
{
	return (struct vmm_manifest_record *)(void *)(manifest +
	    sizeof(struct vmm_manifest_header) +
	    align8(sizeof(struct vmm_manifest_record) +
	    sizeof(struct vmm_x64_vcpu_state)));
}

static struct vmm_x64_time_state *
manifest_time(uint8_t *manifest)
{
	return (struct vmm_x64_time_state *)(void *)(manifest +
	    sizeof(struct vmm_manifest_header) +
	    align8(sizeof(struct vmm_manifest_record) +
	    sizeof(struct vmm_x64_vcpu_state)) +
	    sizeof(struct vmm_manifest_record));
}

static struct vmm_gpa_range *
manifest_ranges(uint8_t *manifest)
{
	return (struct vmm_gpa_range *)(void *)(manifest +
	    sizeof(struct vmm_manifest_header) +
	    align8(sizeof(struct vmm_manifest_record) +
	    sizeof(struct vmm_x64_vcpu_state)) +
	    align8(sizeof(struct vmm_manifest_record) +
	    sizeof(struct vmm_x64_time_state)) +
	    sizeof(struct vmm_manifest_record));
}

static void
append_optional_unknown(uint8_t *manifest)
{
	struct vmm_manifest_header *hdr = manifest_header(manifest);
	const uint8_t payload[3] = { 1, 2, 3 };
	uint8_t *p;

	p = manifest + hdr->total_size;
	p = add_record_flags(p, 0x7fff, 0, payload, sizeof(payload));
	hdr->total_size = (uint32_t)(p - manifest);
	hdr->record_count++;
}

static void
append_duplicate_ranges(uint8_t *manifest)
{
	struct vmm_manifest_header *hdr = manifest_header(manifest);
	struct vmm_gpa_range *ranges = manifest_ranges(manifest);
	uint8_t *p;

	p = manifest + hdr->total_size;
	p = add_record(p, VMM_REC_GPA_RANGE, ranges,
	    4 * sizeof(struct vmm_gpa_range));
	hdr->total_size = (uint32_t)(p - manifest);
	hdr->record_count++;
}

static void
append_duplicate_time(uint8_t *manifest)
{
	struct vmm_manifest_header *hdr = manifest_header(manifest);
	struct vmm_x64_time_state *time = manifest_time(manifest);
	uint8_t *p;

	p = manifest + hdr->total_size;
	p = add_record(p, VMM_REC_X64_TIME_STATE, time, sizeof(*time));
	hdr->total_size = (uint32_t)(p - manifest);
	hdr->record_count++;
}

static void
append_cpu_topology(uint8_t *manifest, uint32_t vcpu_count,
    uint32_t apic_id1)
{
	struct vmm_manifest_header *hdr = manifest_header(manifest);
	struct vmm_x64_cpu_topology topology;
	uint8_t *p;

	memset(&topology, 0, sizeof(topology));
	topology.imm_vcpu_count = vcpu_count;
	topology.imm_apic_ids[0] = 0;
	if (vcpu_count > 1)
		topology.imm_apic_ids[1] = apic_id1;
	p = manifest + hdr->total_size;
	p = add_record(p, VMM_REC_X64_CPU_TOPOLOGY, &topology,
	    sizeof(topology));
	hdr->total_size = (uint32_t)(p - manifest);
	hdr->record_count++;
}

static void
poison_last_record_padding(uint8_t *manifest)
{
	struct vmm_manifest_header *hdr = manifest_header(manifest);

	manifest[hdr->total_size - 1] = 0xa5;
}

static int
launch_is_zero(const struct vmm_launch *launch)
{
	static const struct vmm_launch zero;

	return memcmp(launch, &zero, sizeof(*launch)) == 0;
}

static void
expect_load_result(const char *name, uint64_t mem_size, const uint8_t *manifest,
    size_t cap, int want)
{
	struct vmm_launch launch;
	int error;

	memset(&launch, 0xa5, sizeof(launch));
	error = vmm_loader_x86_manifest_load(mem_size, manifest, cap, &launch);
	if (error != want) {
		errx(1, "%s: got %d want %d", name, error, want);
	}
	if (want == 0) {
		if (launch.imm_mem_size != mem_size ||
		    launch.imm_range_count == 0 ||
		    launch.imm_vcpu0.gpr[VMM_X64_GPR_RIP] != ENTRY_GPA) {
			errx(1, "%s: successful load produced bad launch state",
			    name);
		}
	} else if (!launch_is_zero(&launch)) {
		errx(1, "%s: failed load left launch output", name);
	}
}

static void
expect_result(const char *name, uint8_t *manifest, int want)
{
	expect_load_result(name, MEM_SIZE, manifest, MANIFEST_SIZE, want);
}

int
main(void)
{
	uint8_t manifest[MANIFEST_SIZE];
	struct vmm_x64_vcpu_state *vcpu;
	struct vmm_gpa_range *ranges;
	struct vmm_x64_time_state *time;
	struct vmm_launch launch;

	build_valid_state(manifest);
	expect_result("valid", manifest, 0);
	if (vmm_loader_x86_manifest_load(MEM_SIZE, manifest, MANIFEST_SIZE,
	    &launch) != 0 || launch.imm_guest_tsc_hz != 1000000000ULL) {
		errx(1, "valid: missing explicit guest TSC frequency");
	}

	expect_load_result("null manifest", MEM_SIZE, NULL, MANIFEST_SIZE,
	    EINVAL);

	build_valid_state(manifest);
	expect_load_result("zero mem size", 0, manifest, MANIFEST_SIZE,
	    EINVAL);

	build_valid_state(manifest);
	expect_load_result("short cap", MEM_SIZE, manifest,
	    sizeof(struct vmm_manifest_header) - 1, EINVAL);

	build_valid_state(manifest);
	append_optional_unknown(manifest);
	expect_result("optional unknown record", manifest, 0);

	build_valid_state(manifest);
	append_cpu_topology(manifest, 2, 1);
	if (vmm_loader_x86_manifest_load(MEM_SIZE, manifest, MANIFEST_SIZE,
	    &launch) != 0 || launch.imm_cpu_topology.imm_vcpu_count != 2 ||
	    launch.imm_cpu_topology.imm_apic_ids[1] != 1) {
		errx(1, "two cpu topology: unexpected launch topology");
	}

	build_valid_state(manifest);
	append_cpu_topology(manifest, 2, 0);
	expect_result("duplicate cpu apic id", manifest, EINVAL);

	build_valid_state(manifest);
	append_cpu_topology(manifest, 0, 0);
	expect_result("zero cpu topology", manifest, EINVAL);

	build_valid_state(manifest);
	append_duplicate_ranges(manifest);
	expect_result("duplicate gpa range record", manifest, EINVAL);

	build_valid_state(manifest);
	append_duplicate_time(manifest);
	expect_result("duplicate x64 time record", manifest, EINVAL);

	build_valid_state(manifest);
	manifest_time_record(manifest)->type = 0x7fff;
	manifest_time_record(manifest)->flags = 0;
	expect_result("missing x64 time record", manifest, EINVAL);

	build_valid_state(manifest);
	manifest_time_record(manifest)->size =
	    sizeof(struct vmm_x64_time_state) - 1;
	expect_result("bad x64 time size", manifest, EINVAL);

	build_valid_state(manifest);
	time = manifest_time(manifest);
	time->tsc_hz = 0;
	expect_result("host native x64 time", manifest, 0);
	if (vmm_loader_x86_manifest_load(MEM_SIZE, manifest, MANIFEST_SIZE,
	    &launch) != 0 || launch.imm_guest_tsc_hz != 0) {
		errx(1, "host native x64 time: unexpected launch frequency");
	}

	build_valid_state(manifest);
	manifest_header(manifest)->abi_version = 0;
	expect_result("old manifest abi", manifest, EINVAL);

	build_valid_state(manifest);
	append_optional_unknown(manifest);
	poison_last_record_padding(manifest);
	expect_result("nonzero record padding", manifest, EINVAL);

	build_valid_state(manifest);
	manifest_first_record(manifest)->flags = VMM_REC_F_MANDATORY | 0x2;
	expect_result("reserved record flags", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->flags = 1;
	expect_result("bad vcpu flags", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->cr[VMM_X64_CR_CR3] = PML4_GPA + 1;
	expect_result("bad cr3 alignment", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->gpr[VMM_X64_GPR_RSP] = MEM_SIZE - 4;
	expect_result("bad rsp range", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->gpr[VMM_X64_GPR_RFLAGS] = 0;
	expect_result("bad rflags fixed bit", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->gpr[VMM_X64_GPR_RFLAGS] = RFLAGS_FIXED | (1ULL << 63);
	expect_result("bad rflags reserved bit", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->msr[VMM_X64_MSR_PAT] = 0x02;
	expect_result("bad pat memory type", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->msr[VMM_X64_MSR_PAT] = PAT_DEFAULT | (0x08ULL << 56);
	expect_result("bad pat reserved bits", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->seg[VMM_X64_SEG_CS].attrib = SEG_RESERVED;
	expect_result("bad segment reserved bits", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->seg[VMM_X64_SEG_GDT].base = MEM_SIZE - 8;
	vcpu->seg[VMM_X64_SEG_GDT].limit = 0x27;
	expect_result("bad gdt range", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->seg[VMM_X64_SEG_IDT].base = MEM_SIZE - 8;
	vcpu->seg[VMM_X64_SEG_IDT].limit = 0x0f;
	expect_result("bad idt range", manifest, EINVAL);

	build_valid_state(manifest);
	vcpu = manifest_vcpu(manifest);
	vcpu->seg[VMM_X64_SEG_TR].base = MEM_SIZE - 8;
	vcpu->seg[VMM_X64_SEG_TR].limit = 0x67;
	expect_result("bad tr range", manifest, EINVAL);

	build_valid_state(manifest);
	ranges = manifest_ranges(manifest);
	ranges[0].flags = 1;
	expect_result("bad range flags", manifest, EINVAL);

	build_valid_state(manifest);
	ranges = manifest_ranges(manifest);
	ranges[0].type = 0;
	expect_result("bad range type zero", manifest, EINVAL);

	build_valid_state(manifest);
	ranges = manifest_ranges(manifest);
	ranges[0].type = VMM_GPA_RANGE_GUEST_STACK + 1;
	expect_result("bad range type high", manifest, EINVAL);

	build_valid_state(manifest);
	manifest_header(manifest)->mem_size = LARGE_MEM_SIZE;
	vcpu = manifest_vcpu(manifest);
	vcpu->gpr[VMM_X64_GPR_RIP] = VMM_X86_LAPIC_MMIO_GPA;
	expect_load_result("bad rip lapic hole", LARGE_MEM_SIZE, manifest,
	    MANIFEST_SIZE, EINVAL);

	build_valid_state(manifest);
	manifest_header(manifest)->mem_size = LARGE_MEM_SIZE;
	vcpu = manifest_vcpu(manifest);
	vcpu->cr[VMM_X64_CR_CR3] = VMM_X86_LAPIC_MMIO_GPA;
	expect_load_result("bad cr3 lapic hole", LARGE_MEM_SIZE, manifest,
	    MANIFEST_SIZE, EINVAL);

	build_valid_state(manifest);
	manifest_header(manifest)->mem_size = LARGE_MEM_SIZE;
	ranges = manifest_ranges(manifest);
	ranges[0].start = VMM_X86_LAPIC_MMIO_GPA - PAGE_SIZE_GUEST;
	ranges[0].size = PAGE_SIZE_GUEST * 2;
	expect_load_result("bad range overlaps lapic hole", LARGE_MEM_SIZE,
	    manifest, MANIFEST_SIZE, EINVAL);

	printf("PASS: x86 manifest parser\n");
	return 0;
}
