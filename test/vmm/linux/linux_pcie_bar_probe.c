/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Linux guest probe for one direct vPCIe BAR mapping.
 */
#include <stdint.h>

#define VMM_PCIE_BAR_INITIAL_VALUE	0x11223344U
#define VMM_PCIE_BAR_GUEST_VALUE	0x55667788U

#define LINUX_SYS_WRITE	1
#define LINUX_SYS_OPEN	2
#define LINUX_SYS_CLOSE	3
#define LINUX_SYS_LSEEK	8
#define LINUX_SYS_MMAP	9
#define LINUX_SYS_EXIT	60

#define LINUX_O_RDWR	2
#define LINUX_SEEK_SET	0
#define LINUX_PROT_READ	1
#define LINUX_PROT_WRITE	2
#define LINUX_MAP_SHARED	1

#define LINUX_PCI_COMMAND_OFFSET	4
#define LINUX_PCI_COMMAND_MEMORY	0x0002U

static long linux_syscall1(long number, long arg1);
static long linux_syscall3(long number, long arg1, long arg2, long arg3);
static long linux_mmap(long address, long length, long prot, long flags,
    long fd, long offset);
static void linux_write(const char *message, long length);
static int probe(void);

void
_start(void)
{
	long status;

	status = probe();
	(void)linux_syscall1(LINUX_SYS_EXIT, status);
	for (;;)
		;
}

static int
probe(void)
{
	static const char config[] =
	    "/sys/bus/pci/devices/0000:00:01.0/config";
	static const char resource[] =
	    "/sys/bus/pci/devices/0000:00:01.0/resource0";
	static const char config_ok[] = "DFVMM_PCIE_CONFIG_MEMORY_OK\n";
	static const char read_ok[] = "DFVMM_PCIE_BAR_GUEST_READ_OK\n";
	static const char write_ok[] = "DFVMM_PCIE_BAR_GUEST_WRITE_OK\n";
	static const char failed[] = "DFVMM_PCIE_BAR_GUEST_BAD\n";
	volatile uint32_t *bar;
	uint16_t command;
	long config_fd;
	long fd;
	long mapped;

	command = LINUX_PCI_COMMAND_MEMORY;
	config_fd = linux_syscall3(LINUX_SYS_OPEN, (long)(uintptr_t)config,
	    LINUX_O_RDWR, 0);
	if (config_fd < 0) {
		linux_write(failed, sizeof(failed) - 1);
		return 1;
	}
	if (linux_syscall3(LINUX_SYS_LSEEK, config_fd,
	    LINUX_PCI_COMMAND_OFFSET, LINUX_SEEK_SET) !=
	    LINUX_PCI_COMMAND_OFFSET ||
	    linux_syscall3(LINUX_SYS_WRITE, config_fd,
	    (long)(uintptr_t)&command, sizeof(command)) != sizeof(command) ||
	    linux_syscall1(LINUX_SYS_CLOSE, config_fd) != 0) {
		linux_write(failed, sizeof(failed) - 1);
		return 1;
	}
	linux_write(config_ok, sizeof(config_ok) - 1);
	fd = linux_syscall3(LINUX_SYS_OPEN, (long)(uintptr_t)resource,
	    LINUX_O_RDWR, 0);
	if (fd < 0) {
		linux_write(failed, sizeof(failed) - 1);
		return 1;
	}
	mapped = linux_mmap(0, 4096, LINUX_PROT_READ | LINUX_PROT_WRITE,
	    LINUX_MAP_SHARED, fd, 0);
	(void)linux_syscall1(LINUX_SYS_CLOSE, fd);
	if (mapped < 0) {
		linux_write(failed, sizeof(failed) - 1);
		return 1;
	}
	bar = (volatile uint32_t *)(uintptr_t)mapped;
	if (*bar != VMM_PCIE_BAR_INITIAL_VALUE) {
		linux_write(failed, sizeof(failed) - 1);
		return 1;
	}
	linux_write(read_ok, sizeof(read_ok) - 1);
	*bar = VMM_PCIE_BAR_GUEST_VALUE;
	if (*bar != VMM_PCIE_BAR_GUEST_VALUE) {
		linux_write(failed, sizeof(failed) - 1);
		return 1;
	}
	linux_write(write_ok, sizeof(write_ok) - 1);
	return 0;
}

static long
linux_syscall1(long number, long arg1)
{
	long result;

	__asm __volatile("syscall" : "=a"(result) : "a"(number), "D"(arg1) :
	    "rcx", "r11", "memory");
	return result;
}

static long
linux_syscall3(long number, long arg1, long arg2, long arg3)
{
	long result;

	__asm __volatile("syscall" : "=a"(result) : "a"(number), "D"(arg1),
	    "S"(arg2), "d"(arg3) : "rcx", "r11", "memory");
	return result;
}

static long
linux_mmap(long address, long length, long prot, long flags, long fd,
    long offset)
{
	register long arg4 __asm("r10") = flags;
	register long arg5 __asm("r8") = fd;
	register long arg6 __asm("r9") = offset;
	long result;

	__asm __volatile("syscall" : "=a"(result) : "a"(LINUX_SYS_MMAP),
	    "D"(address), "S"(length), "d"(prot), "r"(arg4), "r"(arg5),
	    "r"(arg6) : "rcx", "r11", "memory");
	return result;
}

static void
linux_write(const char *message, long length)
{

	(void)linux_syscall3(LINUX_SYS_WRITE, 1, (long)(uintptr_t)message, length);
}
