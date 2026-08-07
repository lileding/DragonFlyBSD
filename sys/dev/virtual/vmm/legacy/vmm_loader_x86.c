/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86 launch-manifest loader support.
 *
 * fd4 contains one page.  Multi-byte fields are little-endian native integer
 * fields because the producer and consumer are both x86 DragonFly processes.
 *
 * Manifest page:
 *
 *   +0x00  struct vmm_manifest_header
 *          +0x00 char     magic[8]       "VMMLD0\0\0"
 *          +0x08 uint16_t abi_version    1
 *          +0x0a uint16_t arch           1 = x64
 *          +0x0c uint32_t header_size    sizeof(header)
 *          +0x10 uint32_t total_size     header + records, <= PAGE_SIZE
 *          +0x14 uint32_t record_count
 *          +0x18 uint64_t mem_size       fd3 size
 *          +0x20 uint32_t flags          0
 *          +0x24 uint32_t reserved       0
 *
 *   +header_size
 *          record[0]
 *          record[1]
 *          ...
 *
 * Record layout, repeated until total_size:
 *
 *   +0x00  struct vmm_manifest_record
 *          +0x00 uint16_t type
 *          +0x02 uint16_t flags          bit0 = mandatory
 *          +0x04 uint32_t size           payload bytes
 *   +0x08  uint8_t payload[size]
 *          uint8_t zero_padding[]        record total is 8-byte aligned
 *
 * Mandatory type 1 payload, struct vmm_x64_vcpu_state:
 *
 *   +0x000 uint32_t vcpu_id              must be 0
 *   +0x004 uint32_t flags                currently 0
 *   +0x008 uint64_t runnable             must be 1
 *   +0x010 uint64_t gpr[18]              RAX..RFLAGS
 *   +0x0a0 uint64_t cr[6]                CR0,CR2,CR3,CR4,CR8,XCR0
 *   +0x0d0 uint64_t msr[11]              EFER..TSC
 *   +0x128 struct vmm_x64_seg_state[10]  ES,CS,SS,DS,FS,GS,GDT,IDT,LDT,TR
 *   +0x1c8 uint64_t intr_flags           currently 0
 *
 * Mandatory type 2 payload, struct vmm_gpa_range[]:
 *
 *   +0x00 uint64_t start
 *   +0x08 uint64_t size
 *   +0x10 uint32_t type
 *   +0x14 uint32_t flags
 *
 * Mandatory type 3 payload, struct vmm_x64_time_state:
 *
 *   +0x00 uint64_t tsc_hz               0 = host-native; otherwise fixed
 *                                        guest TSC frequency in Hz
 *
 * x86 memory topology visible to loaders:
 *
 *   [0xfee00000, 0xfee01000) is the architectural local-APIC MMIO page.
 *   fd3 is still a mem_size-sized mmap object, but this GPA page is not guest
 *   RAM.  Manifest ranges and launch state pointers must not overlap it; an
 *   OS memory map emitted by a loader must mark it reserved.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <vm/vm.h>

#include "vmm_loader_x86.h"

#define VMM_MANIFEST_MAGIC	"VMMLD0\0\0"
#define VMM_MANIFEST_ABI	1
#define VMM_MANIFEST_ARCH_X64	1

#define VMM_REC_X64_VCPU_STATE	1
#define VMM_REC_GPA_RANGE	2
#define VMM_REC_X64_TIME_STATE	3
#define VMM_REC_X64_CPU_TOPOLOGY 4
#define VMM_REC_F_MANDATORY	1

#define VMM_X64_RFLAGS_FIXED	(1ULL << 1)
#define VMM_X64_RFLAGS_RESERVED ((1ULL << 3) | (1ULL << 5) | \
				 (1ULL << 15) | (~0ULL << 22))

#define VMM_X64_PAT_UC		0x00U
#define VMM_X64_PAT_WC		0x01U
#define VMM_X64_PAT_WT		0x04U
#define VMM_X64_PAT_WP		0x05U
#define VMM_X64_PAT_WB		0x06U
#define VMM_X64_PAT_UCMINUS	0x07U

#define VMM_X64_SEG_ATTR_UNUSABLE 0x1000U
#define VMM_X64_SEG_ATTR_RESERVED 0xe000U

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
} __packed;

struct vmm_manifest_record {
	uint16_t	type;
	uint16_t	flags;
	uint32_t	size;
} __packed;

static size_t
vmm_align8(size_t v)
{
	return (v + 7) & ~(size_t)7;
}

static int
vmm_gpa_inside(uint64_t mem_size, uint64_t start, uint64_t size)
{
	return size != 0 && start < mem_size && size <= mem_size - start;
}

static int
vmm_gpa_ram_inside(uint64_t mem_size, uint64_t start, uint64_t size)
{
	uint64_t end;
	uint64_t lapic_end;

	if (!vmm_gpa_inside(mem_size, start, size))
		return 0;
	end = start + size;
	lapic_end = VMM_X86_LAPIC_MMIO_GPA + VMM_X86_LAPIC_MMIO_SIZE;
	return start >= lapic_end || VMM_X86_LAPIC_MMIO_GPA >= end;
}

static int
vmm_gpa_addr(uint64_t mem_size, uint64_t addr)
{
	return vmm_gpa_ram_inside(mem_size, addr, 1);
}

static int
vmm_gpa_page(uint64_t mem_size, uint64_t addr)
{
	return (addr & PAGE_MASK) == 0 &&
	    vmm_gpa_ram_inside(mem_size, addr, PAGE_SIZE);
}

static int
vmm_gpa_limit(uint64_t mem_size, uint64_t base, uint32_t limit)
{
	return vmm_gpa_ram_inside(mem_size, base, (uint64_t)limit + 1);
}

static int
vmm_gpa_range_type_valid(uint32_t type)
{
	return type >= VMM_GPA_RANGE_LOAD &&
	    type <= VMM_GPA_RANGE_GUEST_STACK;
}

static int
vmm_padding_zero(const uint8_t *buf, size_t off, size_t end)
{
	while (off < end) {
		if (buf[off] != 0)
			return 0;
		off++;
	}
	return 1;
}

static int
vmm_loader_x86_rflags_valid(uint64_t rflags)
{
	return (rflags & VMM_X64_RFLAGS_FIXED) != 0 &&
	    (rflags & VMM_X64_RFLAGS_RESERVED) == 0;
}

int
vmm_loader_x86_xcr0_valid(uint64_t xcr0)
{
	uint64_t avx512;
	uint64_t mpx;
	uint64_t xtile;

	if ((xcr0 & VMM_X64_XCR0_X87) == 0)
		return 0;
	if ((xcr0 & VMM_X64_XCR0_AVX) != 0 &&
	    (xcr0 & VMM_X64_XCR0_SSE) == 0)
		return 0;
	mpx = xcr0 & VMM_X64_XCR0_MPX;
	if (mpx != 0 && mpx != VMM_X64_XCR0_MPX)
		return 0;
	avx512 = xcr0 & VMM_X64_XCR0_AVX512;
	if (avx512 != 0 &&
	    (avx512 != VMM_X64_XCR0_AVX512 ||
	     (xcr0 & VMM_X64_XCR0_AVX) == 0)) {
		return 0;
	}
	xtile = xcr0 & VMM_X64_XCR0_XTILE;
	if (xtile != 0 && xtile != VMM_X64_XCR0_XTILE)
		return 0;
	return 1;
}

static int
vmm_loader_x86_pat_entry_valid(uint8_t entry)
{
	switch (entry) {
	case VMM_X64_PAT_UC:
	case VMM_X64_PAT_WC:
	case VMM_X64_PAT_WT:
	case VMM_X64_PAT_WP:
	case VMM_X64_PAT_WB:
	case VMM_X64_PAT_UCMINUS:
		return 1;
	default:
		return 0;
	}
}

int
vmm_loader_x86_pat_valid(uint64_t pat)
{
	unsigned int i;

	for (i = 0; i < 8; i++) {
		if (!vmm_loader_x86_pat_entry_valid((pat >> (i * 8)) & 0xff))
			return 0;
	}
	return 1;
}

static int
vmm_loader_x86_segments_valid(const struct vmm_x64_vcpu_state *vcpu)
{
	unsigned int i;

	for (i = 0; i < VMM_X64_NSEG; i++) {
		if ((vcpu->seg[i].attrib & VMM_X64_SEG_ATTR_RESERVED) != 0)
			return 0;
	}
	return 1;
}

static int
vmm_loader_x86_validate_vcpu(uint64_t mem_size,
    const struct vmm_x64_vcpu_state *vcpu)
{
	if (vcpu->vcpu_id != 0 || vcpu->flags != 0 || vcpu->runnable != 1)
		return EINVAL;
	if (!vmm_gpa_addr(mem_size, vcpu->gpr[VMM_X64_GPR_RIP]))
		return EINVAL;
	if (!vmm_gpa_inside(mem_size, vcpu->gpr[VMM_X64_GPR_RSP],
	    sizeof(uint64_t)))
		return EINVAL;
	if (!vmm_loader_x86_rflags_valid(vcpu->gpr[VMM_X64_GPR_RFLAGS]))
		return EINVAL;
	if (!vmm_gpa_page(mem_size, vcpu->cr[VMM_X64_CR_CR3]))
		return EINVAL;
	if (!vmm_loader_x86_xcr0_valid(vcpu->cr[VMM_X64_CR_XCR0]))
		return EINVAL;
	if (!vmm_loader_x86_pat_valid(vcpu->msr[VMM_X64_MSR_PAT]))
		return EINVAL;
	if (!vmm_loader_x86_segments_valid(vcpu))
		return EINVAL;
	if (!vmm_gpa_limit(mem_size, vcpu->seg[VMM_X64_SEG_GDT].base,
	    vcpu->seg[VMM_X64_SEG_GDT].limit))
		return EINVAL;
	if (!vmm_gpa_limit(mem_size, vcpu->seg[VMM_X64_SEG_IDT].base,
	    vcpu->seg[VMM_X64_SEG_IDT].limit))
		return EINVAL;
	if ((vcpu->seg[VMM_X64_SEG_TR].attrib &
	    VMM_X64_SEG_ATTR_UNUSABLE) == 0 &&
	    !vmm_gpa_limit(mem_size, vcpu->seg[VMM_X64_SEG_TR].base,
	    vcpu->seg[VMM_X64_SEG_TR].limit))
		return EINVAL;
	if (vcpu->intr_flags != 0)
		return EINVAL;
	return 0;
}

static int
vmm_loader_x86_validate_ranges(uint64_t mem_size, const uint8_t *payload,
    uint32_t size, struct vmm_launch *launch)
{
	const struct vmm_gpa_range *range;
	uint32_t i, count;

	if (size == 0 || (size % sizeof(*range)) != 0)
		return EINVAL;
	range = (const struct vmm_gpa_range *)payload;
	count = size / sizeof(*range);
	if (count > VMM_GPA_RANGE_MAX)
		return EINVAL;
	for (i = 0; i < count; i++) {
		if (!vmm_gpa_ram_inside(mem_size, range[i].start, range[i].size))
			return EINVAL;
		if (!vmm_gpa_range_type_valid(range[i].type))
			return EINVAL;
		if (range[i].flags != 0)
			return EINVAL;
	}
	if (launch != NULL) {
		bcopy(range, launch->imm_ranges, sizeof(*range) * count);
		launch->imm_range_count = count;
	}
	return 0;
}

int
vmm_loader_x86_manifest_load(uint64_t mem_size, const uint8_t *buf,
    size_t cap, struct vmm_launch *launch)
{
	struct vmm_manifest_header hdr;
	struct vmm_manifest_record rec;
	struct vmm_launch tmp;
	struct vmm_launch *out = NULL;
	size_t off;
	uint32_t records = 0;
	int have_vcpu = 0;
	int have_range = 0;
	int have_time = 0;
	int error;

	if (launch != NULL) {
		bzero(launch, sizeof(*launch));
		bzero(&tmp, sizeof(tmp));
		tmp.imm_cpu_topology.imm_vcpu_count = 1;
		tmp.imm_cpu_topology.imm_apic_ids[0] = 0;
		out = &tmp;
	}
	if (buf == NULL || mem_size == 0)
		return EINVAL;
	if (cap < sizeof(hdr))
		return EINVAL;
	bcopy(buf, &hdr, sizeof(hdr));
	if (bcmp(hdr.magic, VMM_MANIFEST_MAGIC, sizeof(hdr.magic)) != 0 ||
	    hdr.abi_version != VMM_MANIFEST_ABI ||
	    hdr.arch != VMM_MANIFEST_ARCH_X64 ||
	    hdr.header_size != sizeof(hdr) ||
	    hdr.total_size < hdr.header_size ||
	    hdr.total_size > cap ||
	    hdr.mem_size != mem_size ||
	    hdr.flags != 0 ||
	    hdr.reserved != 0)
		return EINVAL;

	off = hdr.header_size;
	while (off < hdr.total_size) {
		const uint8_t *payload;
		size_t payload_end;
		size_t next;

		if (hdr.total_size - off < sizeof(rec)) {
			error = EINVAL;
			return error;
		}
		bcopy(buf + off, &rec, sizeof(rec));
		if ((rec.flags & ~VMM_REC_F_MANDATORY) != 0) {
			error = EINVAL;
			return error;
		}
		next = off + vmm_align8(sizeof(rec) + rec.size);
		payload_end = off + sizeof(rec) + rec.size;
		if (next < off || next > hdr.total_size ||
		    payload_end > hdr.total_size) {
			error = EINVAL;
			return error;
		}
		if (!vmm_padding_zero(buf, payload_end, next)) {
			error = EINVAL;
			return error;
		}
		payload = buf + off + sizeof(rec);
		switch (rec.type) {
		case VMM_REC_X64_VCPU_STATE:
			if ((rec.flags & VMM_REC_F_MANDATORY) == 0 ||
			    rec.size != sizeof(struct vmm_x64_vcpu_state) ||
			    have_vcpu) {
				error = EINVAL;
				return error;
			}
			error = vmm_loader_x86_validate_vcpu(mem_size,
			    (const struct vmm_x64_vcpu_state *)payload);
			if (error)
				return error;
			if (out != NULL) {
				bcopy(payload, &out->imm_vcpu0,
				    sizeof(out->imm_vcpu0));
			}
			have_vcpu = 1;
			break;
		case VMM_REC_GPA_RANGE:
			if ((rec.flags & VMM_REC_F_MANDATORY) == 0 ||
			    have_range) {
				error = EINVAL;
				return error;
			}
			error = vmm_loader_x86_validate_ranges(mem_size,
			    payload, rec.size, out);
			if (error)
				return error;
			have_range = 1;
			break;
		case VMM_REC_X64_TIME_STATE:
			if ((rec.flags & VMM_REC_F_MANDATORY) == 0 ||
			    rec.size != sizeof(struct vmm_x64_time_state) ||
			    have_time) {
				error = EINVAL;
				return error;
			}
			if (out != NULL) {
				bcopy(payload, &out->imm_guest_tsc_hz,
				    sizeof(out->imm_guest_tsc_hz));
			}
			have_time = 1;
			break;
		case VMM_REC_X64_CPU_TOPOLOGY:
			/* CPU count and APIC IDs belong to vmmfs config, never fd4. */
			return EINVAL;
		default:
			if (rec.flags & VMM_REC_F_MANDATORY) {
				error = EINVAL;
				return error;
			}
			break;
		}
		records++;
		off = next;
	}
	if (records != hdr.record_count || !have_vcpu || !have_range ||
	    !have_time)
		return EINVAL;
	if (out != NULL) {
		out->imm_mem_size = mem_size;
		bcopy(out, launch, sizeof(*launch));
	}
	return 0;
}
