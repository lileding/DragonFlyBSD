/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs x86 CMOS/RTC platform device.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/param.h>
#include <sys/systm.h>

#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_rtc.h"

#define VMMFS_RTC_INDEX_PORT 0x70U
#define VMMFS_RTC_DATA_PORT 0x71U

#define VMMFS_RTC_SECONDS 0x00U
#define VMMFS_RTC_MINUTES 0x02U
#define VMMFS_RTC_HOURS 0x04U
#define VMMFS_RTC_DAY_OF_WEEK 0x06U
#define VMMFS_RTC_DAY_OF_MONTH 0x07U
#define VMMFS_RTC_MONTH 0x08U
#define VMMFS_RTC_YEAR 0x09U
#define VMMFS_RTC_REGISTER_A 0x0aU
#define VMMFS_RTC_REGISTER_B 0x0bU
#define VMMFS_RTC_REGISTER_C 0x0cU
#define VMMFS_RTC_REGISTER_D 0x0dU
#define VMMFS_RTC_CENTURY 0x32U

#define VMMFS_RTC_REGISTER_A_DEFAULT 0x26U
#define VMMFS_RTC_REGISTER_A_UIP 0x80U
#define VMMFS_RTC_REGISTER_B_BINARY 0x04U
#define VMMFS_RTC_REGISTER_B_24H 0x02U
#define VMMFS_RTC_REGISTER_D_VALID 0x80U

static int vmmfs_rtc_read(vmm_vcpu_t, void *, struct vmm_io_read *);
static int vmmfs_rtc_write(vmm_vcpu_t, void *,
	const struct vmm_io_write *);
static uint8_t vmmfs_rtc_value(struct vmmfs_rtc *);
static uint8_t vmmfs_rtc_encode(const struct vmmfs_rtc *, unsigned int);
static bool vmmfs_rtc_leap_year(unsigned int);

int
vmmfs_rtc_init(struct vmmfs_machine *machine, struct vmmfs_rtc *rtc)
{
	if (machine == NULL || rtc == NULL)
		return (EINVAL);
	bzero(rtc, sizeof(*rtc));
	rtc->machine = machine;
	rtc->register_a = VMMFS_RTC_REGISTER_A_DEFAULT;
	rtc->register_b = VMMFS_RTC_REGISTER_B_24H;
	return (0);
}

int
vmmfs_rtc_fini(struct vmmfs_rtc *rtc)
{
	if (rtc == NULL)
		return (EINVAL);
	if (rtc->runtime_machine != NULL)
		return (EBUSY);
	rtc->machine = NULL;
	return (0);
}

int
vmmfs_rtc_start(struct vmmfs_rtc *rtc, vmm_machine_t machine)
{
	int error;

	if (rtc == NULL || rtc->machine == NULL || machine == NULL)
		return (EINVAL);
	if (rtc->runtime_machine != NULL)
		return (EBUSY);
	error = vmm_machine_trap_pio_read(machine, VMMFS_RTC_INDEX_PORT, 2,
	    vmmfs_rtc_read, rtc, &rtc->read_io);
	if (error != 0)
		return (error);
	error = vmm_machine_trap_pio_write(machine, VMMFS_RTC_INDEX_PORT, 2,
	    vmmfs_rtc_write, rtc, &rtc->write_io);
	if (error != 0) {
		(void)vmm_machine_untrap(machine, rtc->read_io);
		rtc->read_io = NULL;
		return (error);
	}
	rtc->runtime_machine = machine;
	return (0);
}

int
vmmfs_rtc_stop(struct vmmfs_rtc *rtc)
{
	vmm_machine_t machine;
	vmm_io_t read_io;
	vmm_io_t write_io;
	int error;
	int result;

	if (rtc == NULL || rtc->machine == NULL)
		return (EINVAL);
	machine = rtc->runtime_machine;
	if (machine == NULL)
		return (0);
	read_io = rtc->read_io;
	write_io = rtc->write_io;
	rtc->runtime_machine = NULL;
	rtc->read_io = NULL;
	rtc->write_io = NULL;
	result = 0;
	if (write_io != NULL) {
		error = vmm_machine_untrap(machine, write_io);
		if (error != 0)
			result = error;
	}
	if (read_io != NULL) {
		error = vmm_machine_untrap(machine, read_io);
		if (result == 0)
			result = error;
	}
	return (result);
}

static int
vmmfs_rtc_read(vmm_vcpu_t vcpu, void *argument, struct vmm_io_read *read)
{
	struct vmmfs_rtc *rtc;

	(void)vcpu;
	rtc = argument;
	if (rtc == NULL || read == NULL || read->width != VMM_IO_WIDTH_8 ||
	    (read->address != VMMFS_RTC_INDEX_PORT &&
	    read->address != VMMFS_RTC_DATA_PORT))
		return (ENOENT);
	if (read->address == VMMFS_RTC_INDEX_PORT)
		read->value = rtc->index | rtc->nmi_disabled;
	else
		read->value = vmmfs_rtc_value(rtc);
	return (0);
}

static int
vmmfs_rtc_write(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	struct vmmfs_rtc *rtc;
	uint8_t value;

	(void)vcpu;
	rtc = argument;
	if (rtc == NULL || write == NULL || write->width != VMM_IO_WIDTH_8 ||
	    (write->address != VMMFS_RTC_INDEX_PORT &&
	    write->address != VMMFS_RTC_DATA_PORT))
		return (ENOENT);
	value = (uint8_t)write->value;
	if (write->address == VMMFS_RTC_INDEX_PORT) {
		rtc->index = value & 0x7fU;
		rtc->nmi_disabled = value & 0x80U;
		return (0);
	}
	switch (rtc->index) {
	case VMMFS_RTC_REGISTER_A:
		rtc->register_a = value & ~VMMFS_RTC_REGISTER_A_UIP;
		break;
	case VMMFS_RTC_REGISTER_B:
		rtc->register_b = value;
		break;
	case VMMFS_RTC_REGISTER_C:
	case VMMFS_RTC_REGISTER_D:
		break;
	default:
		rtc->ram[rtc->index] = value;
		break;
	}
	return (0);
}

static uint8_t
vmmfs_rtc_value(struct vmmfs_rtc *rtc)
{
	static const unsigned int days_in_month[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};
	uint64_t elapsed;
	uint64_t epoch_days;
	uint64_t week_days;
	unsigned int day;
	unsigned int month;
	unsigned int year;
	unsigned int value;

	switch (rtc->index) {
	case VMMFS_RTC_REGISTER_A:
		return (rtc->register_a & ~VMMFS_RTC_REGISTER_A_UIP);
	case VMMFS_RTC_REGISTER_B:
		return (rtc->register_b);
	case VMMFS_RTC_REGISTER_C:
		value = rtc->register_c;
		rtc->register_c = 0;
		return ((uint8_t)value);
	case VMMFS_RTC_REGISTER_D:
		return (VMMFS_RTC_REGISTER_D_VALID);
	default:
		break;
	}
	elapsed = time_second < 0 ? 0 : (uint64_t)time_second;
	epoch_days = elapsed / 86400U;
	week_days = epoch_days;
	elapsed %= 86400U;
	year = 1970;
	while (epoch_days >= 365U + vmmfs_rtc_leap_year(year)) {
		epoch_days -= 365U + vmmfs_rtc_leap_year(year);
		++year;
	}
	month = 0;
	while (epoch_days >= days_in_month[month] +
	    (month == 1 && vmmfs_rtc_leap_year(year))) {
		epoch_days -= days_in_month[month] +
		    (month == 1 && vmmfs_rtc_leap_year(year));
		++month;
	}
	day = (unsigned int)epoch_days + 1U;
	switch (rtc->index) {
	case VMMFS_RTC_SECONDS:
		value = (unsigned int)(elapsed % 60U);
		break;
	case VMMFS_RTC_MINUTES:
		value = (unsigned int)((elapsed / 60U) % 60U);
		break;
	case VMMFS_RTC_HOURS:
		value = (unsigned int)(elapsed / 3600U);
		break;
	case VMMFS_RTC_DAY_OF_WEEK:
		value = (unsigned int)(((week_days + 4U) % 7U) + 1U);
		break;
	case VMMFS_RTC_DAY_OF_MONTH:
		value = day;
		break;
	case VMMFS_RTC_MONTH:
		value = month + 1U;
		break;
	case VMMFS_RTC_YEAR:
		value = year % 100U;
		break;
	case VMMFS_RTC_CENTURY:
		value = year / 100U;
		break;
	default:
		return (rtc->ram[rtc->index]);
	}
	return (vmmfs_rtc_encode(rtc, value));
}

static uint8_t
vmmfs_rtc_encode(const struct vmmfs_rtc *rtc, unsigned int value)
{
	if ((rtc->register_b & VMMFS_RTC_REGISTER_B_BINARY) != 0)
		return ((uint8_t)value);
	return (bin2bcd(value));
}

static bool
vmmfs_rtc_leap_year(unsigned int year)
{
	return (year % 4U == 0 && (year % 100U != 0 || year % 400U == 0));
}
