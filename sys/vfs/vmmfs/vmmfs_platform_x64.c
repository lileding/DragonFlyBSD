/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMMFS x64 platform ABI.  The fixed layout follows Cloud Hypervisor so a
 * direct Linux/DragonFly loader and a CloudHV PVH firmware consume the same
 * machine.  Do not add legacy PM1 or alternate CloudHV PIO aliases here.
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
#include "vmmfs_root.h"
#include "vmmfs_parent.h"
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
#define VMMFS_PLATFORM_X64_POWER_PORT 0x600U
#define VMMFS_PLATFORM_X64_POWER_SIZE 4U
#define VMMFS_PLATFORM_X64_POWER_RESET_VALUE 0x01U
#define VMMFS_PLATFORM_X64_POWER_OFF_VALUE 0x34U
#define VMMFS_PLATFORM_X64_PM_TIMER_PORT 0x608U
#define VMMFS_PLATFORM_X64_I8042_COMMAND_PORT 0x64U
#define VMMFS_PLATFORM_X64_I8042_COMMAND_SIZE 1U
#define VMMFS_PLATFORM_X64_I8042_RESET_VALUE 0xfeU
#define VMMFS_PLATFORM_X64_PM_TIMER_SIZE 4U
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
	const struct vmmfs_serialport_info *);
static void vmmfs_platform_x64_write16(uint8_t *, uint32_t, uint16_t);
static void vmmfs_platform_x64_write32(uint8_t *, uint32_t, uint32_t);
static void vmmfs_platform_x64_write64(uint8_t *, uint32_t, uint64_t);
static void vmmfs_platform_x64_write_gas(uint8_t *, uint32_t, uint8_t,
	uint8_t, uint64_t);

static const uint8_t vmmfs_platform_x64_s5_aml[] = {
	0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x02,
	0x0a, 0x05, 0x0a, 0x05,
};

/*
 * Publish the existing CMOS PIO implementation as the ACPI RTC device.
 * No IRQ resource is advertised until VMMFS implements RTC alarm/periodic
 * interrupt delivery.
 */
static const uint8_t vmmfs_platform_x64_rtc_aml[] = {
	0x5b, 0x82, 0x28, 0x52, 0x54, 0x43, 0x30,
	0x08, 0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x0b,
	0x00, 0x08, 0x5f, 0x55, 0x49, 0x44, 0x00, 0x08,
	0x5f, 0x43, 0x52, 0x53, 0x11, 0x0d, 0x0a, 0x0a,
	0x47, 0x01, 0x70, 0x00, 0x70, 0x00, 0x01, 0x02,
	0x79, 0x00,
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
	 * PCI0 publishes the bus and BAR aperture owned by VMMFS.  ECAM belongs
	 * to the host bridge and is reserved separately as a motherboard resource.
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
	/* DWordMemory: c0000000-e7ffffff. */
	0x87, 0x17, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff, 0xff, 0xe7,
	0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x79, 0x00,
};

/* Reserve ECAM outside PCI0 so MCFG is valid for generic ACPI guests. */
static const uint8_t vmmfs_platform_x64_ecam_aml[] = {
	0x5b, 0x82, 0x3a, 0x45, 0x43, 0x41, 0x4d, 0x08,
	0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x0c,
	0x02, 0x08, 0x5f, 0x55, 0x49, 0x44, 0x00, 0x08,
	0x5f, 0x43, 0x52, 0x53, 0x11, 0x1f, 0x0a, 0x1c,
	0x87, 0x17, 0x00, 0x00, 0x0d, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xe8, 0xff, 0xff,
	0xff, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x79, 0x00,
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

void
vmmfs_platform_x64_fini(struct vmmfs_platform_x64 *platform)
{
	if (platform == NULL)
		return;
	if (platform->runtime_machine != NULL)
		panic("vmmfs_platform_x64_fini: runtime is still active");
	platform->machine = NULL;
	lwkt_token_uninit(&platform->token);
	return;
}

int
vmmfs_platform_x64_prepare(struct vmmfs_platform_x64 *platform,
	struct vmmfs_memory *memory, uint32_t vcpu_count,
	struct vmmfs_pciroot *pciroot,
	struct vmmfs_serialroot *serialroot)
{
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
	    memory->object == NULL || pciroot == NULL || serialroot == NULL ||
	    platform->machine != vmmfs_memory_machine(memory) ||
	    vmmfs_pciroot_machine(pciroot) != vmmfs_memory_machine(memory) ||
	    vmmfs_serialroot_machine(serialroot) != vmmfs_memory_machine(memory) ||
	    vcpu_count == 0 || vcpu_count > UINT8_MAX + 1U ||
	    memory->size < VMMFS_PLATFORM_X64_ACPI_GPA + VMMFS_PLATFORM_X64_ACPI_SIZE)
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
	/*
	 * Hardware-reduced ACPI is the sole VMMFS x64 ABI.  These GAS records
	 * match Cloud Hypervisor: reset/sleep at 0x600 and a 32-bit PM timer at
	 * 0x608.  Legacy PM1 blocks are intentionally absent.
	 */
	vmmfs_platform_x64_write32(table, 112,
	    (1U << 20) | (1U << 10) | (1U << 8));
	vmmfs_platform_x64_write_gas(table, 116, 8, 1,
	    VMMFS_PLATFORM_X64_POWER_PORT);
	table[128] = VMMFS_PLATFORM_X64_POWER_RESET_VALUE;
	table[131] = 3;
	vmmfs_platform_x64_write64(table, 132, VMMFS_PLATFORM_X64_FACS_GPA);
	vmmfs_platform_x64_write64(table, 140, VMMFS_PLATFORM_X64_DSDT_GPA);
	vmmfs_platform_x64_write_gas(table, 208, 32, 3,
	    VMMFS_PLATFORM_X64_PM_TIMER_PORT);
	vmmfs_platform_x64_write_gas(table, 244, 8, 1,
	    VMMFS_PLATFORM_X64_POWER_PORT);
	vmmfs_platform_x64_write_gas(table, 256, 8, 1,
	    VMMFS_PLATFORM_X64_POWER_PORT);
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
	lwkt_gettoken(&platform->machine->token);
	serial_count = (uint32_t)vmmfs_serialroot_port_count(serialroot);
	scope_body_length = 5 + sizeof(vmmfs_platform_x64_pciroot_aml) +
	    sizeof(vmmfs_platform_x64_ecam_aml) +
	    sizeof(vmmfs_platform_x64_rtc_aml) +
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
	bcopy(vmmfs_platform_x64_ecam_aml, cursor,
	    sizeof(vmmfs_platform_x64_ecam_aml));
	cursor += sizeof(vmmfs_platform_x64_ecam_aml);
	bcopy(vmmfs_platform_x64_rtc_aml, cursor,
	    sizeof(vmmfs_platform_x64_rtc_aml));
	cursor += sizeof(vmmfs_platform_x64_rtc_aml);
	for (index = 0; index < serial_count; ++index) {
		struct vmmfs_serialport_info info;

		error = vmmfs_serialroot_port_info(serialroot, index, &info);
		if (error != 0) {
			lwkt_reltoken(&platform->machine->token);
			kfree(tables, M_VMMFS);
			return (error);
		}
		cursor = vmmfs_platform_x64_append_serial(cursor, &info);
	}
	lwkt_reltoken(&platform->machine->token);
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
	platform->power_read = NULL;
	platform->power_write = NULL;
	platform->timer_read = NULL;
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
	    VMMFS_PLATFORM_X64_POWER_PORT, VMMFS_PLATFORM_X64_POWER_SIZE,
	    vmmfs_platform_x64_read, platform, &platform->power_read);
	if (error != 0)
		goto fail_delay_write;
	error = vmm_machine_trap_pio_write(machine,
	    VMMFS_PLATFORM_X64_POWER_PORT, VMMFS_PLATFORM_X64_POWER_SIZE,
	    vmmfs_platform_x64_write, platform, &platform->power_write);
	if (error != 0)
		goto fail_power_read;
	error = vmm_machine_trap_pio_read(machine,
	    VMMFS_PLATFORM_X64_PM_TIMER_PORT,
	    VMMFS_PLATFORM_X64_PM_TIMER_SIZE,
	    vmmfs_platform_x64_read, platform, &platform->timer_read);
	if (error != 0)
		goto fail_power_write;
	/*
	 * Zen Linux reads this FCH status register during early CPU setup.
	 * VMMFS has no FCH, so report the architectural all-ones absent value.
	 */
	error = vmm_machine_trap_mmio_read(machine,
	    VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_GPA, sizeof(uint32_t),
	    vmmfs_platform_x64_read, platform, &platform->fch_pm_read);
	if (error != 0)
		goto fail_timer_read;
	error = vmm_machine_trap_mmio_write(machine,
	    VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_GPA, sizeof(uint32_t),
	    vmmfs_platform_x64_write, platform, &platform->fch_pm_write);
	if (error != 0)
		goto fail_fch_pm_read;
	/*
	 * Preserve only the architectural i8042 reset-control port.  Other
	 * unclaimed PIO exits are completed by the VCPU frontend.
	 */
	error = vmm_machine_trap_pio_read(machine, VMMFS_PLATFORM_X64_I8042_COMMAND_PORT, 1,
	    vmmfs_platform_x64_read, platform, &platform->i8042_read);
	if (error != 0)
		goto fail_fch_pm_write;
	error = vmm_machine_trap_pio_write(machine, VMMFS_PLATFORM_X64_I8042_COMMAND_PORT, 1,
	    vmmfs_platform_x64_write, platform, &platform->i8042_write);
	if (error != 0)
		goto fail_i8042_read;
	platform->tsc_base = rdtsc();
	platform->runtime_machine = machine;
	return (0);

fail_i8042_read:
	(void)vmm_machine_untrap(machine, platform->i8042_read);
	platform->i8042_read = NULL;
fail_fch_pm_write:
	(void)vmm_machine_untrap(machine, platform->fch_pm_write);
	platform->fch_pm_write = NULL;
fail_fch_pm_read:
	(void)vmm_machine_untrap(machine, platform->fch_pm_read);
	platform->fch_pm_read = NULL;
fail_timer_read:
	(void)vmm_machine_untrap(machine, platform->timer_read);
	platform->timer_read = NULL;
fail_power_write:
	(void)vmm_machine_untrap(machine, platform->power_write);
	platform->power_write = NULL;
fail_power_read:
	(void)vmm_machine_untrap(machine, platform->power_read);
	platform->power_read = NULL;
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
	vmm_io_t delay_read;
	vmm_io_t delay_write;
	vmm_io_t fch_pm_read;
	vmm_io_t fch_pm_write;
	vmm_io_t i8042_read;
	vmm_io_t i8042_write;
	vmm_io_t power_read;
	vmm_io_t power_write;
	vmm_io_t timer_read;
	int error;
	int result;

	if (platform == NULL || platform->machine == NULL)
		return (EINVAL);
	machine = platform->runtime_machine;
	if (machine == NULL)
		return (0);
	delay_read = platform->delay_read;
	delay_write = platform->delay_write;
	fch_pm_read = platform->fch_pm_read;
	fch_pm_write = platform->fch_pm_write;
	i8042_read = platform->i8042_read;
	i8042_write = platform->i8042_write;
	power_read = platform->power_read;
	power_write = platform->power_write;
	timer_read = platform->timer_read;
	platform->runtime_machine = NULL;
	platform->delay_read = NULL;
	platform->delay_write = NULL;
	platform->fch_pm_read = NULL;
	platform->fch_pm_write = NULL;
	platform->i8042_read = NULL;
	platform->i8042_write = NULL;
	platform->power_read = NULL;
	platform->power_write = NULL;
	platform->timer_read = NULL;
	result = 0;
	if (i8042_write != NULL) {
		error = vmm_machine_untrap(machine, i8042_write);
		if (error != 0)
			result = error;
	}
	if (i8042_read != NULL) {
		error = vmm_machine_untrap(machine, i8042_read);
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
	if (timer_read != NULL) {
		error = vmm_machine_untrap(machine, timer_read);
		if (result == 0)
			result = error;
	}
	if (power_write != NULL) {
		error = vmm_machine_untrap(machine, power_write);
		if (result == 0)
			result = error;
	}
	if (power_read != NULL) {
		error = vmm_machine_untrap(machine, power_read);
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
	if (read->address >= VMMFS_PLATFORM_X64_POWER_PORT &&
	    end <= VMMFS_PLATFORM_X64_POWER_PORT +
	    VMMFS_PLATFORM_X64_POWER_SIZE) {
		read->value = 0;
		lwkt_reltoken(&platform->token);
		return (0);
	}
	/*
	 * Provide only the i8042 reset-control status bit.  Linux waits for
	 * bit 1 to clear before issuing the architectural 0xfe reset command.
	 * VMMFS does not otherwise emulate a keyboard controller.
	 */
	if (read->address == VMMFS_PLATFORM_X64_I8042_COMMAND_PORT &&
	    read->width == VMM_IO_WIDTH_8) {
		read->value = 0;
		lwkt_reltoken(&platform->token);
		return (0);
	}
	if (read->address == VMMFS_PLATFORM_X64_PM_TIMER_PORT &&
	    read->width == VMM_IO_WIDTH_32) {
		read->value = vmmfs_platform_x64_pm_timer(platform);
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

	(void)vcpu;
	platform = argument;
	if (platform == NULL || write == NULL)
		return (ENOENT);
	power_off = false;
	reset = false;
	lwkt_gettoken(&platform->token);
	if ((write->address == VMMFS_PLATFORM_X64_FCH_PM_S5_RESET_GPA &&
	    write->width == VMM_IO_WIDTH_32) ||
	    (write->address == VMMFS_PLATFORM_X64_POWER_PORT &&
	    write->width == VMM_IO_WIDTH_8 &&
	    write->value == VMMFS_PLATFORM_X64_POWER_RESET_VALUE) ||
	    (write->address == VMMFS_PLATFORM_X64_I8042_COMMAND_PORT &&
	    write->width == VMM_IO_WIDTH_8 &&
	    write->value == VMMFS_PLATFORM_X64_I8042_RESET_VALUE)) {
		reset = true;
	} else if (write->address == VMMFS_PLATFORM_X64_POWER_PORT &&
	    write->width == VMM_IO_WIDTH_8 &&
	    write->value == VMMFS_PLATFORM_X64_POWER_OFF_VALUE) {
		power_off = true;
	}
	lwkt_reltoken(&platform->token);
	if (power_off) {
		if (vmmfs_machine_request_stop(platform->machine, "guest-s5") != 0)
			vmmfs_events_log(&platform->machine->events,
			    VMMFS_MACHINE_EVENT_GUEST_STOP_REQUEST_FAILED, NULL);
	}
	if (reset) {
		if (vmmfs_machine_reset(platform->machine) != 0)
			vmmfs_events_log(&platform->machine->events,
			    VMMFS_MACHINE_EVENT_GUEST_RESET_REQUEST_FAILED, NULL);
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
	const struct vmmfs_serialport_info *port)
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

static void
vmmfs_platform_x64_write_gas(uint8_t *buffer, uint32_t offset,
	uint8_t bit_width, uint8_t access_size, uint64_t address)
{
	buffer[offset] = 1; /* System I/O */
	buffer[offset + 1] = bit_width;
	buffer[offset + 2] = 0;
	buffer[offset + 3] = access_size;
	vmmfs_platform_x64_write64(buffer, offset + 4, address);
}
