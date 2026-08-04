/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86 ACPI platform tables for the dfvmm machine ABI.
 */
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include "vmm_loader_x86.h"
#include "vmm_mem.h"
#include "vmm_pcie_layout.h"
#include "vmm_platform_x86.h"

#define VMM_PLATFORM_X86_RSDP_GPA	(VMM_PLATFORM_X86_ACPI_GPA + 0x000ULL)
#define VMM_PLATFORM_X86_XSDT_GPA	(VMM_PLATFORM_X86_ACPI_GPA + 0x100ULL)
#define VMM_PLATFORM_X86_FADT_GPA	(VMM_PLATFORM_X86_ACPI_GPA + 0x200ULL)
#define VMM_PLATFORM_X86_MADT_GPA	(VMM_PLATFORM_X86_ACPI_GPA + 0x400ULL)
#define VMM_PLATFORM_X86_HPET_GPA	(VMM_PLATFORM_X86_ACPI_GPA + 0xd00ULL)
#define VMM_PLATFORM_X86_DSDT_GPA	(VMM_PLATFORM_X86_ACPI_GPA + 0xe00ULL)
#define VMM_PLATFORM_X86_MCFG_GPA	(VMM_PLATFORM_X86_ACPI_GPA + 0x1000ULL)
#define VMM_PLATFORM_X86_HPET_MMIO_GPA	0xfed00000ULL
#define VMM_PLATFORM_X86_IOAPIC_GPA	0xfec00000ULL
#define VMM_PLATFORM_X86_LAPIC_GPA	0xfee00000ULL

#define VMM_ACPI_HEADER_SIZE	36U
#define VMM_ACPI_RSDP_SIZE	36U
#define VMM_ACPI_FADT_SIZE	276U
#define VMM_ACPI_MCFG_SIZE	60U
#define VMM_ACPI_MADT_LAPIC_SIZE	8U
#define VMM_ACPI_MADT_IOAPIC_SIZE	12U
#define VMM_ACPI_MADT_ISO_SIZE	10U

static uint8_t vmm_platform_x86_sum(const uint8_t *buf, uint32_t len);
static void vmm_platform_x86_checksum(uint8_t *table, uint32_t len,
	    uint32_t off);
static void vmm_platform_x86_header(uint8_t *table, const char signature[4],
	    uint32_t len, uint8_t revision);
static void vmm_platform_x86_write8(uint8_t *buf, uint32_t off,
	    uint8_t value);
static void vmm_platform_x86_write16(uint8_t *buf, uint32_t off,
	    uint16_t value);
static void vmm_platform_x86_write32(uint8_t *buf, uint32_t off,
	    uint32_t value);
static void vmm_platform_x86_write64(uint8_t *buf, uint32_t off,
	    uint64_t value);

static const uint8_t vmm_platform_x86_dsdt[] = {
	0x44, 0x53, 0x44, 0x54, 0xa5, 0x00, 0x00, 0x00,
	0x02, 0xb5, 0x44, 0x46, 0x56, 0x4d, 0x4d, 0x00,
	0x44, 0x46, 0x56, 0x4d, 0x4d, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x49, 0x4e, 0x54, 0x4c,
	0x12, 0x12, 0x25, 0x20,
	0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x02,
	0x0a, 0x05, 0x0a, 0x05,
	0x10, 0x44, 0x07, 0x5f,
	0x53, 0x42, 0x5f, 0x5b, 0x82, 0x35, 0x43, 0x4f,
	0x4d,
	0x31, 0x08, 0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41,
	0xd0, 0x05, 0x01, 0x08, 0x5f, 0x55, 0x49, 0x44,
	0x01, 0x14, 0x09, 0x5f, 0x53, 0x54, 0x41, 0x00,
	0xa4, 0x0a, 0x0f, 0x08, 0x5f, 0x43, 0x52, 0x53,
	0x11, 0x10, 0x0a, 0x0d, 0x47, 0x01, 0xf8, 0x03,
	0xf8, 0x03, 0x01, 0x08, 0x22, 0x10, 0x00, 0x79,
	0x00,
	0x5b, 0x82, 0x35, 0x52, 0x54, 0x43, 0x30, 0x08,
	0x5f, 0x48, 0x49, 0x44, 0x0c, 0x00, 0x0b, 0xd0,
	0x41, 0x08, 0x5f, 0x55, 0x49, 0x44, 0x00, 0x14,
	0x09, 0x5f, 0x53, 0x54, 0x41, 0x00, 0xa4, 0x0a,
	0x0f, 0x08, 0x5f, 0x43, 0x52, 0x53, 0x11, 0x10,
	0x0a, 0x0d, 0x47, 0x01, 0x70, 0x00, 0x70, 0x00,
	0x01, 0x02, 0x22, 0x00, 0x01, 0x79, 0x00,
};

static const uint8_t vmm_platform_x86_pci_root_aml[] = {
	0x10, 0x43, 0x06, 0x5f, 0x53, 0x42, 0x5f, 0x5b,
	0x82, 0x4b, 0x05, 0x50, 0x43, 0x49, 0x30, 0x08,
	0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x0a,
	0x08, 0x08, 0x5f, 0x43, 0x49, 0x44, 0x0c, 0x41,
	0xd0, 0x0a, 0x03, 0x08, 0x5f, 0x53, 0x45, 0x47,
	0x00, 0x08, 0x5f, 0x42, 0x42, 0x4e, 0x00, 0x08,
	0x5f, 0x43, 0x52, 0x53, 0x11, 0x2f, 0x0a, 0x2c,
	0x88, 0x0d, 0x00, 0x02, 0x0c, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x87, 0x17, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff,
	0xff, 0xdf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x20, 0x79, 0x00,
};

int
vmm_platform_x86_prepare(struct vmm_mem *mem, uint32_t vcpu_count,
    struct vmm_launch *launch)
{
	uint8_t *tables;
	uint8_t *table;
	uint32_t i;
	uint32_t madt_len;
	uint32_t dsdt_len;
	int error;

	if (mem == NULL || launch == NULL || vcpu_count == 0 ||
	    vcpu_count > VMM_X64_MAX_VCPU)
		return EINVAL;
	tables = kmalloc(VMM_PLATFORM_X86_ACPI_SIZE, M_TEMP, M_WAITOK | M_ZERO);

	table = tables;
	bcopy("RSD PTR ", table, 8);
	bcopy("DFVMM ", table + 9, 6);
	vmm_platform_x86_write8(table, 15, 2);
	vmm_platform_x86_write32(table, 20, VMM_ACPI_RSDP_SIZE);
	vmm_platform_x86_write64(table, 24, VMM_PLATFORM_X86_XSDT_GPA);
	vmm_platform_x86_checksum(table, 20, 8);
	vmm_platform_x86_checksum(table, VMM_ACPI_RSDP_SIZE, 32);

	table = tables + 0x100;
	vmm_platform_x86_header(table, "XSDT", VMM_ACPI_HEADER_SIZE + 32, 1);
	vmm_platform_x86_write64(table, 36, VMM_PLATFORM_X86_FADT_GPA);
	vmm_platform_x86_write64(table, 44, VMM_PLATFORM_X86_MADT_GPA);
	vmm_platform_x86_write64(table, 52, VMM_PLATFORM_X86_HPET_GPA);
	vmm_platform_x86_write64(table, 60, VMM_PLATFORM_X86_MCFG_GPA);
	vmm_platform_x86_checksum(table, VMM_ACPI_HEADER_SIZE + 32, 9);

	table = tables + 0xe00;
	bcopy(vmm_platform_x86_dsdt, table, sizeof(vmm_platform_x86_dsdt));
	dsdt_len = sizeof(vmm_platform_x86_dsdt) +
	    sizeof(vmm_platform_x86_pci_root_aml);
	bcopy(vmm_platform_x86_pci_root_aml,
	    table + sizeof(vmm_platform_x86_dsdt),
	    sizeof(vmm_platform_x86_pci_root_aml));
	vmm_platform_x86_write32(table, 4, dsdt_len);
	vmm_platform_x86_checksum(table, dsdt_len, 9);

	table = tables + 0x200;
	vmm_platform_x86_header(table, "FACP", VMM_ACPI_FADT_SIZE, 6);
	vmm_platform_x86_write32(table, 40, VMM_PLATFORM_X86_DSDT_GPA);
	vmm_platform_x86_write8(table, 45, 7);
	vmm_platform_x86_write32(table, 76, 0x408);
	vmm_platform_x86_write8(table, 91, 4);
	vmm_platform_x86_write16(table, 109, 0x0004);
	vmm_platform_x86_write32(table, 112, 0x00100401);
	vmm_platform_x86_write8(table, 116, 1);
	vmm_platform_x86_write8(table, 117, 8);
	vmm_platform_x86_write8(table, 119, 1);
	vmm_platform_x86_write64(table, 120, 0x40c);
	vmm_platform_x86_write8(table, 128, 1);
	vmm_platform_x86_write8(table, 131, 5);
	vmm_platform_x86_write64(table, 140, VMM_PLATFORM_X86_DSDT_GPA);
	vmm_platform_x86_write8(table, 208, 1);
	vmm_platform_x86_write8(table, 209, 32);
	vmm_platform_x86_write8(table, 211, 3);
	vmm_platform_x86_write64(table, 212, 0x408);
	vmm_platform_x86_write8(table, 244, 1);
	vmm_platform_x86_write8(table, 245, 8);
	vmm_platform_x86_write8(table, 247, 1);
	vmm_platform_x86_write64(table, 248, 0x404);
	vmm_platform_x86_write8(table, 256, 1);
	vmm_platform_x86_write8(table, 257, 8);
	vmm_platform_x86_write8(table, 259, 1);
	vmm_platform_x86_write64(table, 260, 0x405);
	vmm_platform_x86_checksum(table, VMM_ACPI_FADT_SIZE, 9);

	table = tables + 0x400;
	madt_len = VMM_ACPI_HEADER_SIZE + 8 +
	    vcpu_count * VMM_ACPI_MADT_LAPIC_SIZE +
	    VMM_ACPI_MADT_IOAPIC_SIZE + VMM_ACPI_MADT_ISO_SIZE;
	vmm_platform_x86_header(table, "APIC", madt_len, 3);
	vmm_platform_x86_write32(table, 36, VMM_PLATFORM_X86_LAPIC_GPA);
	for (i = 0; i < vcpu_count; i++) {
		uint8_t *lapic = table + 44 + i * VMM_ACPI_MADT_LAPIC_SIZE;

		vmm_platform_x86_write8(lapic, 1, VMM_ACPI_MADT_LAPIC_SIZE);
		vmm_platform_x86_write8(lapic, 2, (uint8_t)i);
		vmm_platform_x86_write8(lapic, 3, (uint8_t)i);
		vmm_platform_x86_write32(lapic, 4, 1);
	}
	table += 44 + vcpu_count * VMM_ACPI_MADT_LAPIC_SIZE;
	vmm_platform_x86_write8(table, 0, 1);
	vmm_platform_x86_write8(table, 1, VMM_ACPI_MADT_IOAPIC_SIZE);
	vmm_platform_x86_write8(table, 2, 1);
	vmm_platform_x86_write32(table, 4, VMM_PLATFORM_X86_IOAPIC_GPA);
	table += VMM_ACPI_MADT_IOAPIC_SIZE;
	vmm_platform_x86_write8(table, 0, 2);
	vmm_platform_x86_write8(table, 1, VMM_ACPI_MADT_ISO_SIZE);
	vmm_platform_x86_write8(table, 3, 4);
	vmm_platform_x86_write32(table, 4, 4);
	vmm_platform_x86_write16(table, 8, 0x0005);
	vmm_platform_x86_checksum(tables + 0x400, madt_len, 9);

	table = tables + 0xd00;
	vmm_platform_x86_header(table, "HPET", 56, 1);
	vmm_platform_x86_write32(table, 36, 0x80862201U);
	vmm_platform_x86_write8(table, 40, 0);
	vmm_platform_x86_write8(table, 41, 64);
	vmm_platform_x86_write8(table, 43, 4);
	vmm_platform_x86_write64(table, 44, VMM_PLATFORM_X86_HPET_MMIO_GPA);
	vmm_platform_x86_write16(table, 53, 0x80);
	vmm_platform_x86_checksum(table, 56, 9);

	table = tables + 0x1000;
	vmm_platform_x86_header(table, "MCFG", VMM_ACPI_MCFG_SIZE, 1);
	vmm_platform_x86_write64(table, 44, VMM_PCIE_ECAM_BASE);
	vmm_platform_x86_write8(table, 55, 0xff);
	vmm_platform_x86_checksum(table, VMM_ACPI_MCFG_SIZE, 9);

	for (i = 0; i < vcpu_count; i++)
		launch->imm_cpu_topology.imm_apic_ids[i] = i;
	for (; i < VMM_X64_MAX_VCPU; i++)
		launch->imm_cpu_topology.imm_apic_ids[i] = 0;
	launch->imm_cpu_topology.imm_vcpu_count = vcpu_count;
	error = vmm_mem_write_boot_gpa(mem, VMM_PLATFORM_X86_ACPI_GPA, tables,
	    VMM_PLATFORM_X86_ACPI_SIZE);
	kfree(tables, M_TEMP);
	return error;
}

static uint8_t
vmm_platform_x86_sum(const uint8_t *buf, uint32_t len)
{
	uint8_t sum = 0;
	uint32_t i;

	for (i = 0; i < len; i++)
		sum += buf[i];
	return sum;
}

static void
vmm_platform_x86_checksum(uint8_t *table, uint32_t len, uint32_t off)
{
	table[off] = 0;
	table[off] = (uint8_t)(0U - vmm_platform_x86_sum(table, len));
}

static void
vmm_platform_x86_header(uint8_t *table, const char signature[4], uint32_t len,
    uint8_t revision)
{
	bzero(table, len);
	bcopy(signature, table, 4);
	vmm_platform_x86_write32(table, 4, len);
	vmm_platform_x86_write8(table, 8, revision);
	bcopy("DFVMM ", table + 10, 6);
	bcopy("DFVMM   ", table + 16, 8);
	vmm_platform_x86_write32(table, 24, 1);
	bcopy("VMM ", table + 28, 4);
	vmm_platform_x86_write32(table, 32, 1);
}

static void
vmm_platform_x86_write8(uint8_t *buf, uint32_t off, uint8_t value)
{
	buf[off] = value;
}

static void
vmm_platform_x86_write16(uint8_t *buf, uint32_t off, uint16_t value)
{
	buf[off] = value & 0xffU;
	buf[off + 1] = value >> 8;
}

static void
vmm_platform_x86_write32(uint8_t *buf, uint32_t off, uint32_t value)
{
	buf[off] = value & 0xffU;
	buf[off + 1] = (value >> 8) & 0xffU;
	buf[off + 2] = (value >> 16) & 0xffU;
	buf[off + 3] = value >> 24;
}

static void
vmm_platform_x86_write64(uint8_t *buf, uint32_t off, uint64_t value)
{
	bcopy(&value, buf + off, sizeof(value));
}
