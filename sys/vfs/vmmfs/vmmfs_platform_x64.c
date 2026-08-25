/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86-64 ACPI platform tables owned by vmmfs.
 */
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/systm.h>

#include <machine/clock.h>
#include <machine/cpufunc.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_memory.h"
#include "vmmfs_platform_x64.h"
#include "vmmfs_pciroot.h"
#include "vmmfs_serialport.h"
#include "vmmfs_serialroot.h"

#define VMMFS_PLATFORM_X64_XSDT_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0x100ULL)
#define VMMFS_PLATFORM_X64_FADT_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0x200ULL)
#define VMMFS_PLATFORM_X64_FACS_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0x800ULL)
#define VMMFS_PLATFORM_X64_MADT_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0x400ULL)
#define VMMFS_PLATFORM_X64_DSDT_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0xe00ULL)
#define VMMFS_PLATFORM_X64_MCFG_GPA \
	(VMMFS_PLATFORM_X64_ACPI_GPA + 0x600ULL)
#define VMMFS_PLATFORM_X64_LAPIC_GPA 0xfee00000ULL
#define VMMFS_PLATFORM_X64_IOAPIC_GPA 0xfec00000ULL

#define VMMFS_PLATFORM_X64_DELAY_PORT 0x80U
#define VMMFS_PLATFORM_X64_DELAY_SIZE 0x10U
#define VMMFS_PLATFORM_X64_PM1_EVENT_PORT 0x400U
#define VMMFS_PLATFORM_X64_PM1_EVENT_SIZE 4U
#define VMMFS_PLATFORM_X64_PM1_CONTROL_PORT 0x404U
#define VMMFS_PLATFORM_X64_PM1_CONTROL_SIZE 2U
#define VMMFS_PLATFORM_X64_ACPI_PORT VMMFS_PLATFORM_X64_PM1_EVENT_PORT
#define VMMFS_PLATFORM_X64_ACPI_SIZE_PIO 13U
#define VMMFS_PLATFORM_X64_PM_TIMER_PORT 0x408U
#define VMMFS_PLATFORM_X64_PM_TIMER_LAST 0x40bU
#define VMMFS_PLATFORM_X64_RESET_PORT 0x40cU
#define VMMFS_PLATFORM_X64_RESET_VALUE 0x01U
#define VMMFS_PLATFORM_X64_PM1_SLP_TYPE_MASK 0x1c00U
#define VMMFS_PLATFORM_X64_PM1_SLP_TYPE_S5 (5U << 10)
#define VMMFS_PLATFORM_X64_PM1_SLP_ENABLE 0x2000U
#define VMMFS_PLATFORM_X64_PM_TIMER_FREQUENCY 3579545ULL
#define VMMFS_PLATFORM_X64_PM_TIMER_MASK 0x00ffffffU
#define VMMFS_PLATFORM_X64_FCH_PM_BASE 0xfed80300ULL
#define VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_STATUS 0x0c0ULL
#define VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_GPA \
	(VMMFS_PLATFORM_X64_FCH_PM_BASE + \
	VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_STATUS)

#define VMMFS_ACPI_HEADER_SIZE 36U
#define VMMFS_ACPI_RSDP_SIZE 36U
#define VMMFS_ACPI_FADT_SIZE 276U
#define VMMFS_ACPI_FACS_SIZE 64U
#define VMMFS_ACPI_MADT_LAPIC_SIZE 8U
#define VMMFS_ACPI_MADT_IOAPIC_SIZE 12U
#define VMMFS_ACPI_MCFG_SIZE 60U
#define VMMFS_ACPI_DSDT_HEADER_SIZE VMMFS_ACPI_HEADER_SIZE
#define VMMFS_ACPI_SERIAL_AML_SIZE 55U

static int vmmfs_platform_x64_read(vmm_vcpu_t, void *,
	struct vmm_io_read *);
static int vmmfs_platform_x64_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static uint32_t vmmfs_platform_x64_pm_timer(
	const struct vmmfs_platform_x64 *);
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

static const uint8_t vmmfs_platform_x64_pciroot_aml[] = {
	/*
	 * PCI0 publishes the bus and BAR apertures owned by VMMFS.  ECAM is
	 * configuration space, not a downstream resource; MCFG describes it.
	 */
	0x5b, 0x82, 0x4b, 0x06, 0x50, 0x43, 0x49, 0x30,
	0x08, 0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x0a,
	0x08, 0x08, 0x5f, 0x43, 0x49, 0x44, 0x0c, 0x41, 0xd0,
	0x0a, 0x03, 0x08, 0x5f, 0x53, 0x45, 0x47, 0x00, 0x08,
	0x5f, 0x42, 0x42, 0x4e, 0x00, 0x08, 0x5f, 0x43, 0x52,
	0x53, 0x11, 0x3f, 0x0a, 0x3c,
	/* WordBusNumber: 00-ff. */
	0x88, 0x0d, 0x00, 0x02, 0x0c, 0x00, 0x00, 0x00, 0x00,
	0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01,
	/* WordIO: 1000-bfff. */
	0x88, 0x0d, 0x00, 0x01, 0x0c, 0x03, 0x00, 0x00, 0x00,
	0x10, 0xff, 0xbf, 0x00, 0x00, 0x00, 0xb0,
	/* DWordMemory: c0000000-dfffffff. */
	0x87, 0x17, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xc0, 0xff, 0xff, 0xff, 0xdf, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x79, 0x00,
};

int
vmmfs_platform_x64_init(struct vmmfs_machine *machine,
	struct vmmfs_platform_x64 *platform)
{
	if (machine == NULL || platform == NULL)
		return (EINVAL);
	bzero(platform, sizeof(*platform));
	platform->machine = machine;
	lwkt_token_init(&platform->token, "vmmfsplatform");
	return (0);
}

int
vmmfs_platform_x64_fini(struct vmmfs_platform_x64 *platform)
{
	if (platform == NULL)
		return (EINVAL);
	if (platform->runtime_machine != NULL)
		return (EBUSY);
	platform->machine = NULL;
	lwkt_token_uninit(&platform->token);
	return (0);
}

int
vmmfs_platform_x64_prepare(struct vmmfs_platform_x64 *platform,
	struct vmmfs_memory *memory, uint32_t vcpu_count,
	struct vmmfs_pciroot *pciroot,
	struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_serialport *port;
	uint8_t *cursor;
	uint8_t *table;
	uint8_t *tables;
	uint32_t dsdt_length;
	uint32_t madt_length;
	uint32_t scope_body_length;
	uint32_t serial_count;
	uint32_t index;
	int error;

	if (platform == NULL || platform->machine == NULL || memory == NULL ||
	    memory->object == NULL ||
	    memory->machine == NULL || pciroot == NULL || serialroot == NULL ||
	    platform->machine != memory->machine ||
	    pciroot->machine != memory->machine ||
	    serialroot->machine != memory->machine || vcpu_count == 0 ||
	    vcpu_count > UINT8_MAX + 1U ||
	    memory->size <
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
	vmmfs_platform_x64_header(table, "XSDT", VMMFS_ACPI_HEADER_SIZE + 24,
	    1);
	vmmfs_platform_x64_write64(table, 36, VMMFS_PLATFORM_X64_FADT_GPA);
	vmmfs_platform_x64_write64(table, 44, VMMFS_PLATFORM_X64_MADT_GPA);
	vmmfs_platform_x64_write64(table, 52, VMMFS_PLATFORM_X64_MCFG_GPA);
	vmmfs_platform_x64_checksum(table, VMMFS_ACPI_HEADER_SIZE + 24, 9);

	table = tables + 0x200;
	vmmfs_platform_x64_header(table, "FACP", VMMFS_ACPI_FADT_SIZE, 6);
	vmmfs_platform_x64_write32(table, 36, VMMFS_PLATFORM_X64_FACS_GPA);
	vmmfs_platform_x64_write32(table, 40, VMMFS_PLATFORM_X64_DSDT_GPA);
	vmmfs_platform_x64_write16(table, 46, 9);
	vmmfs_platform_x64_write32(table, 56,
	    VMMFS_PLATFORM_X64_PM1_EVENT_PORT);
	vmmfs_platform_x64_write32(table, 64,
	    VMMFS_PLATFORM_X64_PM1_CONTROL_PORT);
	vmmfs_platform_x64_write32(table, 76,
	    VMMFS_PLATFORM_X64_PM_TIMER_PORT);
	table[88] = VMMFS_PLATFORM_X64_PM1_EVENT_SIZE;
	table[89] = VMMFS_PLATFORM_X64_PM1_CONTROL_SIZE;
	table[91] = 4;
	table[108] = 0x32;
	vmmfs_platform_x64_write32(table, 112, 1U << 10);
	table[116] = 1;
	table[117] = 8;
	table[119] = 1;
	vmmfs_platform_x64_write64(table, 120,
	    VMMFS_PLATFORM_X64_RESET_PORT);
	table[128] = VMMFS_PLATFORM_X64_RESET_VALUE;
	vmmfs_platform_x64_write64(table, 132, VMMFS_PLATFORM_X64_FACS_GPA);
	vmmfs_platform_x64_write64(table, 140, VMMFS_PLATFORM_X64_DSDT_GPA);
	vmmfs_platform_x64_checksum(table, VMMFS_ACPI_FADT_SIZE, 9);

	table = tables + 0x800;
	bcopy("FACS", table, 4);
	vmmfs_platform_x64_write32(table, 4, VMMFS_ACPI_FACS_SIZE);

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

	table = tables + 0x600;
	vmmfs_platform_x64_header(table, "MCFG", VMMFS_ACPI_MCFG_SIZE, 1);
	vmmfs_platform_x64_write64(table, 44, VMMFS_PCI_ECAM_GPA);
	table[55] = UINT8_MAX;
	vmmfs_platform_x64_checksum(table, VMMFS_ACPI_MCFG_SIZE, 9);

	table = tables + 0xe00;
	/*
	 * Initialize the header before emitting AML.  The generic header helper
	 * clears its complete length argument, so clearing the final DSDT length
	 * here would erase the AML body we build below.
	 */
	vmmfs_platform_x64_header(table, "DSDT",
	    VMMFS_ACPI_DSDT_HEADER_SIZE, 2);
	cursor = table + VMMFS_ACPI_DSDT_HEADER_SIZE;
	/* Place _S5_ in the root namespace for ACPI sleep discovery. */
	*cursor++ = 0x10;
	cursor = vmmfs_platform_x64_pkg_length(cursor,
	    sizeof(vmmfs_platform_x64_s5_aml) + 2);
	*cursor++ = 0x5c;
	*cursor++ = 0x00;
	bcopy(vmmfs_platform_x64_s5_aml, cursor,
	    sizeof(vmmfs_platform_x64_s5_aml));
	cursor += sizeof(vmmfs_platform_x64_s5_aml);
	serial_count = 0;
	lwkt_gettoken(&serialroot->machine->token);
	RB_FOREACH(port, vmmfs_serialport_tree, &serialroot->ports)
		++serial_count;
	scope_body_length = 5 + sizeof(vmmfs_platform_x64_pciroot_aml) +
	    serial_count * VMMFS_ACPI_SERIAL_AML_SIZE;
	*cursor++ = 0x10;
	cursor = vmmfs_platform_x64_pkg_length(cursor, scope_body_length);
	*cursor++ = 0x5c;
	*cursor++ = 0x5f;
	*cursor++ = 0x53;
	*cursor++ = 0x42;
	*cursor++ = 0x5f;
	bcopy(vmmfs_platform_x64_pciroot_aml, cursor,
	    sizeof(vmmfs_platform_x64_pciroot_aml));
	cursor += sizeof(vmmfs_platform_x64_pciroot_aml);
	RB_FOREACH(port, vmmfs_serialport_tree, &serialroot->ports)
		cursor = vmmfs_platform_x64_append_serial(cursor, port);
	lwkt_reltoken(&serialroot->machine->token);
	dsdt_length = (uint32_t)(cursor - table);
	if (dsdt_length > VMMFS_PLATFORM_X64_ACPI_SIZE - 0xe00) {
		kfree(tables, M_VMMFS);
		return EOVERFLOW;
	}
	vmmfs_platform_x64_write32(table, 4, dsdt_length);
	vmmfs_platform_x64_checksum(table, dsdt_length, 9);

	error = vmmfs_platform_x64_write_memory(memory,
	    VMMFS_PLATFORM_X64_ACPI_GPA, tables,
	    VMMFS_PLATFORM_X64_ACPI_SIZE);
	kfree(tables, M_VMMFS);
	return error;
}

int
vmmfs_platform_x64_start(struct vmmfs_platform_x64 *platform,
	vmm_machine_t machine)
{
	int error;

	if (platform == NULL || platform->machine == NULL || machine == NULL)
		return (EINVAL);
	if (platform->runtime_machine != NULL)
		return (EBUSY);
	platform->pm1_status = 0;
	platform->pm1_enable = 0;
	platform->pm1_control = 0;
	error = vmm_machine_trap_pio_read(machine,
	    VMMFS_PLATFORM_X64_DELAY_PORT, VMMFS_PLATFORM_X64_DELAY_SIZE,
	    vmmfs_platform_x64_read, platform, &platform->delay_read);
	if (error != 0)
		return (error);
	error = vmm_machine_trap_pio_write(machine,
	    VMMFS_PLATFORM_X64_DELAY_PORT, VMMFS_PLATFORM_X64_DELAY_SIZE,
	    vmmfs_platform_x64_write, platform, &platform->delay_write);
	if (error != 0)
		goto fail_delay_read;
	error = vmm_machine_trap_pio_read(machine,
	    VMMFS_PLATFORM_X64_ACPI_PORT, VMMFS_PLATFORM_X64_ACPI_SIZE_PIO,
	    vmmfs_platform_x64_read, platform, &platform->acpi_read);
	if (error != 0)
		goto fail_delay_write;
	error = vmm_machine_trap_pio_write(machine,
	    VMMFS_PLATFORM_X64_ACPI_PORT, VMMFS_PLATFORM_X64_ACPI_SIZE_PIO,
	    vmmfs_platform_x64_write, platform, &platform->acpi_write);
	if (error != 0)
		goto fail_acpi_read;
	/*
	 * Zen Linux reads this FCH status register during early CPU setup.
	 * VMMFS has no FCH, so report the architectural all-ones absent value.
	 */
	error = vmm_machine_trap_mmio_read(machine,
	    VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_GPA, sizeof(uint32_t),
	    vmmfs_platform_x64_read, platform, &platform->fch_pm_read);
	if (error != 0)
		goto fail_acpi_write;
	error = vmm_machine_trap_mmio_write(machine,
	    VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_GPA, sizeof(uint32_t),
	    vmmfs_platform_x64_write, platform, &platform->fch_pm_write);
	if (error != 0)
		goto fail_fch_pm_read;
	/*
	 * VMMFS has no userspace monitor to complete unrelated legacy PIO
	 * probes.  Specific devices were registered before this catch-all range,
	 * so an unclaimed scalar read observes an absent device and a write is
	 * discarded.
	 */
	error = vmm_machine_trap_pio_read(machine, 0, 0x10000U,
	    vmmfs_platform_x64_read, platform, &platform->fallback_read);
	if (error != 0)
		goto fail_fch_pm_write;
	error = vmm_machine_trap_pio_write(machine, 0, 0x10000U,
	    vmmfs_platform_x64_write, platform, &platform->fallback_write);
	if (error != 0)
		goto fail_fallback_read;
	platform->tsc_base = rdtsc();
	platform->runtime_machine = machine;
	return (0);

fail_fallback_read:
	(void)vmm_machine_untrap(machine, platform->fallback_read);
	platform->fallback_read = NULL;
fail_fch_pm_write:
	(void)vmm_machine_untrap(machine, platform->fch_pm_write);
	platform->fch_pm_write = NULL;
fail_fch_pm_read:
	(void)vmm_machine_untrap(machine, platform->fch_pm_read);
	platform->fch_pm_read = NULL;
fail_acpi_write:
	(void)vmm_machine_untrap(machine, platform->acpi_write);
	platform->acpi_write = NULL;
fail_acpi_read:
	(void)vmm_machine_untrap(machine, platform->acpi_read);
	platform->acpi_read = NULL;
fail_delay_write:
	(void)vmm_machine_untrap(machine, platform->delay_write);
	platform->delay_write = NULL;
fail_delay_read:
	(void)vmm_machine_untrap(machine, platform->delay_read);
	platform->delay_read = NULL;
	return (error);
}

int
vmmfs_platform_x64_stop(struct vmmfs_platform_x64 *platform)
{
	vmm_machine_t machine;
	vmm_io_t acpi_read;
	vmm_io_t acpi_write;
	vmm_io_t delay_read;
	vmm_io_t delay_write;
	vmm_io_t fch_pm_read;
	vmm_io_t fch_pm_write;
	vmm_io_t fallback_read;
	vmm_io_t fallback_write;
	int error;
	int result;

	if (platform == NULL || platform->machine == NULL)
		return (EINVAL);
	machine = platform->runtime_machine;
	if (machine == NULL)
		return (0);
	acpi_read = platform->acpi_read;
	acpi_write = platform->acpi_write;
	delay_read = platform->delay_read;
	delay_write = platform->delay_write;
	fch_pm_read = platform->fch_pm_read;
	fch_pm_write = platform->fch_pm_write;
	fallback_read = platform->fallback_read;
	fallback_write = platform->fallback_write;
	platform->runtime_machine = NULL;
	platform->acpi_read = NULL;
	platform->acpi_write = NULL;
	platform->delay_read = NULL;
	platform->delay_write = NULL;
	platform->fch_pm_read = NULL;
	platform->fch_pm_write = NULL;
	platform->fallback_read = NULL;
	platform->fallback_write = NULL;
	result = 0;
	if (fallback_write != NULL) {
		error = vmm_machine_untrap(machine, fallback_write);
		if (error != 0)
			result = error;
	}
	if (fallback_read != NULL) {
		error = vmm_machine_untrap(machine, fallback_read);
		if (result == 0)
			result = error;
	}
	if (fch_pm_write != NULL) {
		error = vmm_machine_untrap(machine, fch_pm_write);
		if (result == 0)
			result = error;
	}
	if (fch_pm_read != NULL) {
		error = vmm_machine_untrap(machine, fch_pm_read);
		if (result == 0)
			result = error;
	}
	if (acpi_write != NULL) {
		error = vmm_machine_untrap(machine, acpi_write);
		if (error != 0)
			result = error;
	}
	if (acpi_read != NULL) {
		error = vmm_machine_untrap(machine, acpi_read);
		if (result == 0)
			result = error;
	}
	if (delay_write != NULL) {
		error = vmm_machine_untrap(machine, delay_write);
		if (result == 0)
			result = error;
	}
	if (delay_read != NULL) {
		error = vmm_machine_untrap(machine, delay_read);
		if (result == 0)
			result = error;
	}
	return (result);
}

static int
vmmfs_platform_x64_read(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	struct vmmfs_platform_x64 *platform;
	uint64_t end;

	(void)vcpu;
	platform = argument;
	if (platform == NULL || read == NULL)
		return (ENOENT);
	lwkt_gettoken(&platform->token);
	if (read->address == VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_GPA &&
	    read->width == VMM_IO_WIDTH_32) {
		read->value = UINT32_MAX;
		lwkt_reltoken(&platform->token);
		return (0);
	}
	end = read->address + read->width;
	if (read->address >= VMMFS_PLATFORM_X64_DELAY_PORT &&
	    end <= VMMFS_PLATFORM_X64_DELAY_PORT +
	    VMMFS_PLATFORM_X64_DELAY_SIZE) {
		read->value = 0;
		lwkt_reltoken(&platform->token);
		return (0);
	}
	if (read->address >= VMMFS_PLATFORM_X64_PM1_EVENT_PORT &&
	    end <= VMMFS_PLATFORM_X64_PM1_CONTROL_PORT +
	    VMMFS_PLATFORM_X64_PM1_CONTROL_SIZE) {
		read->value = 0;
		for (unsigned int index = 0; index < read->width; ++index) {
			uint8_t value;

			switch (read->address + index) {
			case VMMFS_PLATFORM_X64_PM1_EVENT_PORT:
				value = platform->pm1_status;
				break;
			case VMMFS_PLATFORM_X64_PM1_EVENT_PORT + 1U:
				value = platform->pm1_status >> 8;
				break;
			case VMMFS_PLATFORM_X64_PM1_EVENT_PORT + 2U:
				value = platform->pm1_enable;
				break;
			case VMMFS_PLATFORM_X64_PM1_EVENT_PORT + 3U:
				value = platform->pm1_enable >> 8;
				break;
			case VMMFS_PLATFORM_X64_PM1_CONTROL_PORT:
				value = platform->pm1_control;
				break;
			default:
				value = platform->pm1_control >> 8;
				break;
			}
				read->value |= (uint64_t)value << (index * 8U);
		}
		lwkt_reltoken(&platform->token);
		return (0);
	}
	if (read->address >= VMMFS_PLATFORM_X64_PM_TIMER_PORT &&
	    end <= VMMFS_PLATFORM_X64_PM_TIMER_LAST + 1U) {
		read->value = vmmfs_platform_x64_pm_timer(platform) >>
		    ((read->address - VMMFS_PLATFORM_X64_PM_TIMER_PORT) * 8U);
		lwkt_reltoken(&platform->token);
		return (0);
	}
	if (read->address >= VMMFS_PLATFORM_X64_ACPI_PORT &&
	    end <= VMMFS_PLATFORM_X64_ACPI_PORT +
	    VMMFS_PLATFORM_X64_ACPI_SIZE_PIO) {
		read->value = 0;
		lwkt_reltoken(&platform->token);
		return (0);
	}
	read->value = UINT64_MAX;
	lwkt_reltoken(&platform->token);
	return (0);
}

static int
vmmfs_platform_x64_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	struct vmmfs_platform_x64 *platform;
	bool power_off;
	bool reset;
	uint64_t end;

	(void)vcpu;
	platform = argument;
	if (platform == NULL || write == NULL)
		return (ENOENT);
	power_off = false;
	reset = false;
	end = write->address + write->width;
	lwkt_gettoken(&platform->token);
	if ((write->address == VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_GPA &&
	    write->width == VMM_IO_WIDTH_32) ||
	    (write->address == VMMFS_PLATFORM_X64_RESET_PORT &&
	    write->width == VMM_IO_WIDTH_8 &&
	    write->value == VMMFS_PLATFORM_X64_RESET_VALUE)) {
		reset = true;
	} else if (write->address >= VMMFS_PLATFORM_X64_PM1_EVENT_PORT &&
	    end <= VMMFS_PLATFORM_X64_PM1_CONTROL_PORT +
	    VMMFS_PLATFORM_X64_PM1_CONTROL_SIZE) {
		for (unsigned int index = 0; index < write->width; ++index) {
			uint8_t value = write->value >> (index * 8U);

			switch (write->address + index) {
			case VMMFS_PLATFORM_X64_PM1_EVENT_PORT:
				platform->pm1_status &= ~(uint16_t)value;
				break;
			case VMMFS_PLATFORM_X64_PM1_EVENT_PORT + 1U:
				platform->pm1_status &= ~((uint16_t)value << 8);
				break;
			case VMMFS_PLATFORM_X64_PM1_EVENT_PORT + 2U:
				platform->pm1_enable =
				    (platform->pm1_enable & 0xff00U) | value;
				break;
			case VMMFS_PLATFORM_X64_PM1_EVENT_PORT + 3U:
				platform->pm1_enable =
				    (platform->pm1_enable & 0x00ffU) |
				    ((uint16_t)value << 8);
				break;
			case VMMFS_PLATFORM_X64_PM1_CONTROL_PORT:
				platform->pm1_control =
				    (platform->pm1_control & 0xff00U) | value;
				break;
			default:
				platform->pm1_control =
				    (platform->pm1_control & 0x00ffU) |
				    ((uint16_t)value << 8);
				break;
			}
		}
		power_off = (platform->pm1_control &
		    (VMMFS_PLATFORM_X64_PM1_SLP_TYPE_MASK |
		    VMMFS_PLATFORM_X64_PM1_SLP_ENABLE)) ==
		    (VMMFS_PLATFORM_X64_PM1_SLP_TYPE_S5 |
		    VMMFS_PLATFORM_X64_PM1_SLP_ENABLE);
	}
	lwkt_reltoken(&platform->token);
	if (power_off) {
		if (vmmfs_machine_stop_request(platform->machine, "guest-s5") != 0)
			vmmfs_events_log(&platform->machine->events,
			    "guest-s5 stop request failed");
	}
	if (reset) {
		if (vmmfs_machine_reset(platform->machine) != 0)
			vmmfs_events_log(&platform->machine->events,
			    "guest reset request failed");
	}
	return (0);
}

static uint32_t
vmmfs_platform_x64_pm_timer(const struct vmmfs_platform_x64 *platform)
{
	uint64_t delta;
	uint64_t frequency;
	uint64_t ticks;

	frequency = tsc_frequency;
	if (frequency == 0)
		return (0);
	delta = rdtsc() - platform->tsc_base;
	ticks = (delta / frequency) * VMMFS_PLATFORM_X64_PM_TIMER_FREQUENCY +
	    ((delta % frequency) * VMMFS_PLATFORM_X64_PM_TIMER_FREQUENCY) /
	    frequency;
	return ((uint32_t)ticks & VMMFS_PLATFORM_X64_PM_TIMER_MASK);
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
	    gpa > memory->size || length > memory->size - gpa)
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
vmmfs_platform_x64_pkg_length(uint8_t *buffer, uint32_t body_length)
{
	uint32_t length;

	/* AML package lengths include the PkgLength encoding itself. */
	length = body_length + 1;
	if (length <= 0x3f) {
		*buffer++ = (uint8_t)length;
		return buffer;
	}
	++length;
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
