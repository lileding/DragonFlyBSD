/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86-64 ACPI platform tables owned by vmmfs.
 */
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/systm.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include "vmmfs.h"
#include "vmmfs_memory.h"
#include "vmmfs_platform_x64.h"
#include "vmmfs_serialport.h"
#include "vmmfs_serialroot.h"

#define VMMFS_PLATFORM_X64_XSDT_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0x100ULL)
#define VMMFS_PLATFORM_X64_FADT_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0x200ULL)
#define VMMFS_PLATFORM_X64_MADT_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0x400ULL)
#define VMMFS_PLATFORM_X64_DSDT_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0xe00ULL)
#define VMMFS_PLATFORM_X64_LAPIC_GPA 0xfee00000ULL
#define VMMFS_PLATFORM_X64_IOAPIC_GPA 0xfec00000ULL

#define VMMFS_ACPI_HEADER_SIZE 36U
#define VMMFS_ACPI_RSDP_SIZE 36U
#define VMMFS_ACPI_FADT_SIZE 276U
#define VMMFS_ACPI_MADT_LAPIC_SIZE 8U
#define VMMFS_ACPI_MADT_IOAPIC_SIZE 12U
#define VMMFS_ACPI_DSDT_HEADER_SIZE VMMFS_ACPI_HEADER_SIZE
#define VMMFS_ACPI_SERIAL_AML_SIZE 55U

static int vmmfs_platform_x64_write_memory(struct vmmfs_memory *, uint64_t,
	const void *, size_t);
static void vmmfs_platform_x64_header(uint8_t *, const char[4], uint32_t,
	uint8_t);
static void vmmfs_platform_x64_checksum(uint8_t *, uint32_t, uint32_t);
static uint8_t vmmfs_platform_x64_sum(const uint8_t *, uint32_t);
static uint8_t *vmmfs_platform_x64_pkg_length(uint8_t *, uint32_t);
static uint8_t *vmmfs_platform_x64_append_serial(uint8_t *,
	const struct vmmfs_serialport *);
static void vmmfs_platform_x64_write16(uint8_t *, uint32_t, uint16_t);
static void vmmfs_platform_x64_write32(uint8_t *, uint32_t, uint32_t);
static void vmmfs_platform_x64_write64(uint8_t *, uint32_t, uint64_t);

static const uint8_t vmmfs_platform_x64_s5_aml[] = {
	0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x02,
	0x0a, 0x05, 0x0a, 0x05,
};

static const uint8_t vmmfs_platform_x64_serial_aml[] = {
	0x5b, 0x82, 0x35, 0x43, 0x4f, 0x4d, 0x31,
	0x08, 0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x05,
	0x01, 0x08, 0x5f, 0x55, 0x49, 0x44, 0x01,
	0x14, 0x09, 0x5f, 0x53, 0x54, 0x41, 0x00, 0xa4, 0x0a,
	0x0f, 0x08, 0x5f, 0x43, 0x52, 0x53, 0x11, 0x10, 0x0a,
	0x0d, 0x47, 0x01, 0xf8, 0x03, 0xf8, 0x03, 0x01, 0x08,
	0x22, 0x10, 0x00, 0x79, 0x00,
};

int
vmmfs_platform_x64_prepare(struct vmmfs_memory *memory, uint32_t vcpu_count,
	struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_serialport *port;
	uint8_t *cursor;
	uint8_t *table;
	uint8_t *tables;
	uint32_t dsdt_length;
	uint32_t madt_length;
	uint32_t scope_length;
	uint32_t serial_count;
	uint32_t index;
	int error;

	if (memory == NULL || memory->object == NULL ||
	    memory->machine == NULL || serialroot == NULL ||
	    serialroot->machine != memory->machine || vcpu_count == 0 ||
	    vcpu_count > UINT8_MAX + 1U ||
	    memory->machine->spec.memory.size <
	    VMMFS_PLATFORM_X64_ACPI_GPA + VMMFS_PLATFORM_X64_ACPI_SIZE)
		return EINVAL;
	tables = kmalloc(VMMFS_PLATFORM_X64_ACPI_SIZE, M_VMMFS,
	    M_WAITOK | M_ZERO);
	if (tables == NULL)
		return ENOMEM;

	table = tables;
	bcopy("RSD PTR ", table, 8);
	bcopy("DFLY  ", table + 9, 6);
	table[15] = 2;
	vmmfs_platform_x64_write32(table, 20, VMMFS_ACPI_RSDP_SIZE);
	vmmfs_platform_x64_write64(table, 24, VMMFS_PLATFORM_X64_XSDT_GPA);
	vmmfs_platform_x64_checksum(table, 20, 8);
	vmmfs_platform_x64_checksum(table, VMMFS_ACPI_RSDP_SIZE, 32);

	table = tables + 0x100;
	vmmfs_platform_x64_header(table, "XSDT", VMMFS_ACPI_HEADER_SIZE + 16,
	    1);
	vmmfs_platform_x64_write64(table, 36, VMMFS_PLATFORM_X64_FADT_GPA);
	vmmfs_platform_x64_write64(table, 44, VMMFS_PLATFORM_X64_MADT_GPA);
	vmmfs_platform_x64_checksum(table, VMMFS_ACPI_HEADER_SIZE + 16, 9);

	table = tables + 0x200;
	vmmfs_platform_x64_header(table, "FACP", VMMFS_ACPI_FADT_SIZE, 6);
	vmmfs_platform_x64_write32(table, 40, VMMFS_PLATFORM_X64_DSDT_GPA);
	vmmfs_platform_x64_write64(table, 140, VMMFS_PLATFORM_X64_DSDT_GPA);
	vmmfs_platform_x64_checksum(table, VMMFS_ACPI_FADT_SIZE, 9);

	table = tables + 0x400;
	madt_length = VMMFS_ACPI_HEADER_SIZE + 8 +
	    vcpu_count * VMMFS_ACPI_MADT_LAPIC_SIZE +
	    VMMFS_ACPI_MADT_IOAPIC_SIZE;
	vmmfs_platform_x64_header(table, "APIC", madt_length, 3);
	vmmfs_platform_x64_write32(table, 36, VMMFS_PLATFORM_X64_LAPIC_GPA);
	vmmfs_platform_x64_write32(table, 40, 1);
	for (index = 0; index < vcpu_count; ++index) {
		uint8_t *lapic;

		lapic = table + 44 + index * VMMFS_ACPI_MADT_LAPIC_SIZE;
		lapic[0] = 0;
		lapic[1] = VMMFS_ACPI_MADT_LAPIC_SIZE;
		lapic[2] = (uint8_t)index;
		lapic[3] = (uint8_t)index;
		vmmfs_platform_x64_write32(lapic, 4, 1);
	}
	table += 44 + vcpu_count * VMMFS_ACPI_MADT_LAPIC_SIZE;
	table[0] = 1;
	table[1] = VMMFS_ACPI_MADT_IOAPIC_SIZE;
	table[2] = 1;
	vmmfs_platform_x64_write32(table, 4, VMMFS_PLATFORM_X64_IOAPIC_GPA);
	vmmfs_platform_x64_checksum(tables + 0x400, madt_length, 9);

	table = tables + 0xe00;
	cursor = table + VMMFS_ACPI_DSDT_HEADER_SIZE;
	bcopy(vmmfs_platform_x64_s5_aml, cursor,
	    sizeof(vmmfs_platform_x64_s5_aml));
	cursor += sizeof(vmmfs_platform_x64_s5_aml);
	serial_count = 0;
	lwkt_gettoken(&serialroot->machine->token);
	RB_FOREACH(port, vmmfs_serialport_tree, &serialroot->ports)
		++serial_count;
	if (serial_count != 0) {
		scope_length = 5 + serial_count * VMMFS_ACPI_SERIAL_AML_SIZE;
		*cursor++ = 0x10;
		cursor = vmmfs_platform_x64_pkg_length(cursor, scope_length);
		*cursor++ = 0x5c;
		*cursor++ = 0x5f;
		*cursor++ = 0x53;
		*cursor++ = 0x42;
		*cursor++ = 0x5f;
		RB_FOREACH(port, vmmfs_serialport_tree, &serialroot->ports)
			cursor = vmmfs_platform_x64_append_serial(cursor, port);
	}
	lwkt_reltoken(&serialroot->machine->token);
	dsdt_length = (uint32_t)(cursor - table);
	if (dsdt_length > VMMFS_PLATFORM_X64_ACPI_SIZE - 0xe00) {
		kfree(tables, M_VMMFS);
		return EOVERFLOW;
	}
	vmmfs_platform_x64_header(table, "DSDT", dsdt_length, 2);
	vmmfs_platform_x64_checksum(table, dsdt_length, 9);

	error = vmmfs_platform_x64_write_memory(memory,
	    VMMFS_PLATFORM_X64_ACPI_GPA, tables,
	    VMMFS_PLATFORM_X64_ACPI_SIZE);
	kfree(tables, M_VMMFS);
	return error;
}

static int
vmmfs_platform_x64_write_memory(struct vmmfs_memory *memory, uint64_t gpa,
	const void *buffer, size_t length)
{
	const uint8_t *source;
	vm_page_t page;
	uint64_t page_gpa;
	size_t chunk;
	size_t offset;

	if (memory == NULL || memory->object == NULL || buffer == NULL ||
	    gpa > memory->machine->spec.memory.size ||
	    length > memory->machine->spec.memory.size - gpa)
		return EINVAL;
	source = buffer;
	while (length != 0) {
		page_gpa = trunc_page(gpa);
		offset = (size_t)(gpa - page_gpa);
		chunk = PAGE_SIZE - offset;
		if (chunk > length)
			chunk = length;
		page = vm_page_grab(memory->object, OFF_TO_IDX(page_gpa),
		    VM_ALLOC_NORMAL | VM_ALLOC_RETRY | VM_ALLOC_ZERO);
		bcopy(source, (void *)(PHYS_TO_DMAP(VM_PAGE_TO_PHYS(page)) +
		    offset), chunk);
		vm_page_dirty(page);
		vm_page_wakeup(page);
		gpa += chunk;
		source += chunk;
		length -= chunk;
	}
	return 0;
}

static void
vmmfs_platform_x64_header(uint8_t *table, const char signature[4],
	uint32_t length, uint8_t revision)
{
	bzero(table, length);
	bcopy(signature, table, 4);
	vmmfs_platform_x64_write32(table, 4, length);
	table[8] = revision;
	bcopy("DFLY  ", table + 10, 6);
	bcopy("VMMFS   ", table + 16, 8);
	vmmfs_platform_x64_write32(table, 24, 1);
	bcopy("VMM ", table + 28, 4);
	vmmfs_platform_x64_write32(table, 32, 1);
}

static void
vmmfs_platform_x64_checksum(uint8_t *table, uint32_t length, uint32_t offset)
{
	table[offset] = 0;
	table[offset] = (uint8_t)(0U - vmmfs_platform_x64_sum(table, length));
}

static uint8_t
vmmfs_platform_x64_sum(const uint8_t *table, uint32_t length)
{
	uint8_t sum;
	uint32_t index;

	sum = 0;
	for (index = 0; index < length; ++index)
		sum += table[index];
	return sum;
}

static uint8_t *
vmmfs_platform_x64_pkg_length(uint8_t *buffer, uint32_t length)
{
	if (length <= 0x3f) {
		*buffer++ = (uint8_t)length;
		return buffer;
	}
	KKASSERT(length <= 0xfff);
	*buffer++ = (uint8_t)(0x40 | (length & 0x0f));
	*buffer++ = (uint8_t)(length >> 4);
	return buffer;
}

static uint8_t *
vmmfs_platform_x64_append_serial(uint8_t *buffer,
	const struct vmmfs_serialport *port)
{
	uint16_t irq_mask;

	bcopy(vmmfs_platform_x64_serial_aml, buffer,
	    sizeof(vmmfs_platform_x64_serial_aml));
	buffer[6] = (uint8_t)('0' + port->number);
	buffer[22] = port->number;
	vmmfs_platform_x64_write16(buffer, 44, port->base);
	vmmfs_platform_x64_write16(buffer, 46, port->base);
	irq_mask = (uint16_t)(1U << port->gsi);
	vmmfs_platform_x64_write16(buffer, 51, irq_mask);
	return buffer + sizeof(vmmfs_platform_x64_serial_aml);
}

static void
vmmfs_platform_x64_write16(uint8_t *buffer, uint32_t offset, uint16_t value)
{
	buffer[offset] = (uint8_t)value;
	buffer[offset + 1] = (uint8_t)(value >> 8);
}

static void
vmmfs_platform_x64_write32(uint8_t *buffer, uint32_t offset, uint32_t value)
{
	buffer[offset] = (uint8_t)value;
	buffer[offset + 1] = (uint8_t)(value >> 8);
	buffer[offset + 2] = (uint8_t)(value >> 16);
	buffer[offset + 3] = (uint8_t)(value >> 24);
}

static void
vmmfs_platform_x64_write64(uint8_t *buffer, uint32_t offset, uint64_t value)
{
	bcopy(&value, buffer + offset, sizeof(value));
}
