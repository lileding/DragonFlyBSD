/*
 * Minimal x86-64 Linux user program for the dfvmm time-domain diagnostic.
 *
 * It deliberately has no libc or Linux headers: linux_initrd_rootfs_build.sh
 * cross-links it as a static ELF and places it in the test initrd.  The probe
 * obtains I/O privilege, then measures the hardware guest TSC against the
 * ACPI PM timer advertised by the dfvmm FADT.
 */

typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long int int64_t;
typedef unsigned long size_t;

#define LINUX_SYS_WRITE	1L
#define LINUX_SYS_NANOSLEEP	35L
#define LINUX_SYS_IOPL		172L
#define LINUX_SYS_EXIT		60L

#define VMM_PM_TIMER_PORT	0x408U
#define VMM_PM_TIMER_MASK	0x00ffffffU
#define VMM_PM_TIMER_FREQ	3579545ULL

struct linux_timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

static long
linux_syscall1(long number, long arg1)
{
	register long rax __asm__("rax") = number;
	register long rdi __asm__("rdi") = arg1;

	__asm__ __volatile__("syscall"
	    : "+a"(rax)
	    : "D"(rdi)
	    : "rcx", "r11", "memory");
	return rax;
}

static long
linux_syscall2(long number, long arg1, long arg2)
{
	register long rax __asm__("rax") = number;
	register long rdi __asm__("rdi") = arg1;
	register long rsi __asm__("rsi") = arg2;

	__asm__ __volatile__("syscall"
	    : "+a"(rax)
	    : "D"(rdi), "S"(rsi)
	    : "rcx", "r11", "memory");
	return rax;
}

static long
linux_syscall3(long number, long arg1, long arg2, long arg3)
{
	register long rax __asm__("rax") = number;
	register long rdi __asm__("rdi") = arg1;
	register long rsi __asm__("rsi") = arg2;
	register long rdx __asm__("rdx") = arg3;

	__asm__ __volatile__("syscall"
	    : "+a"(rax)
	    : "D"(rdi), "S"(rsi), "d"(rdx)
	    : "rcx", "r11", "memory");
	return rax;
}

static uint64_t
guest_rdtsc(void)
{
	uint32_t lo;
	uint32_t hi;

	__asm__ __volatile__("lfence; rdtsc"
	    : "=a"(lo), "=d"(hi)
	    :
	    : "memory");
	return ((uint64_t)hi << 32) | lo;
}

static uint32_t
guest_pm_timer(void)
{
	uint32_t value;
	uint32_t port = VMM_PM_TIMER_PORT;

	__asm__ __volatile__("inl %w1, %0"
	    : "=a"(value)
	    : "d"(port)
	    : "memory");
	return value & VMM_PM_TIMER_MASK;
}

static size_t
append_text(char *buf, size_t off, const char *text)
{
	while (*text != '\0')
		buf[off++] = *text++;
	return off;
}

static size_t
append_u64(char *buf, size_t off, uint64_t value)
{
	char digits[20];
	size_t count = 0;

	if (value == 0) {
		buf[off++] = '0';
		return off;
	}
	while (value != 0) {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	}
	while (count != 0)
		buf[off++] = digits[--count];
	return off;
}

static void
write_line(const char *text, size_t len)
{
	while (len != 0) {
		long written;

		written = linux_syscall3(LINUX_SYS_WRITE, 1, (long)text, (long)len);
		if (written <= 0)
			return;
		text += written;
		len -= (size_t)written;
	}
}

static void
write_iopl_error(long error)
{
	char line[96];
	size_t len = 0;

	len = append_text(line, len, "DFVMM_TSC_PM_PROBE_ERROR iopl=");
	if (error < 0) {
		line[len++] = '-';
		error = -error;
	}
	len = append_u64(line, len, (uint64_t)error);
	line[len++] = '\n';
	write_line(line, len);
}

void
vmm_probe_main(void)
{
	struct linux_timespec delay = { 0, 500000000 };
	char line[320];
	uint64_t start_tsc;
	uint64_t end_tsc;
	uint64_t delta_tsc;
	uint64_t tsc_hz;
	uint32_t start_pm;
	uint32_t end_pm;
	uint32_t delta_pm;
	long error;
	size_t len = 0;

	error = linux_syscall1(LINUX_SYS_IOPL, 3);
	if (error < 0) {
		write_iopl_error(error);
		return;
	}

	start_tsc = guest_rdtsc();
	start_pm = guest_pm_timer();
	(void)linux_syscall2(LINUX_SYS_NANOSLEEP, (long)&delay, 0);
	end_tsc = guest_rdtsc();
	end_pm = guest_pm_timer();
	delta_tsc = end_tsc - start_tsc;
	delta_pm = (end_pm - start_pm) & VMM_PM_TIMER_MASK;
	if (delta_pm != 0) {
		tsc_hz = (delta_tsc / delta_pm) * VMM_PM_TIMER_FREQ +
		    (delta_tsc % delta_pm) * VMM_PM_TIMER_FREQ / delta_pm;
	} else {
		tsc_hz = 0;
	}

	len = append_text(line, len, "DFVMM_TSC_PM_PROBE");
	len = append_text(line, len, " start_tsc=");
	len = append_u64(line, len, start_tsc);
	len = append_text(line, len, " end_tsc=");
	len = append_u64(line, len, end_tsc);
	len = append_text(line, len, " delta_tsc=");
	len = append_u64(line, len, delta_tsc);
	len = append_text(line, len, " start_pm=");
	len = append_u64(line, len, start_pm);
	len = append_text(line, len, " end_pm=");
	len = append_u64(line, len, end_pm);
	len = append_text(line, len, " delta_pm=");
	len = append_u64(line, len, delta_pm);
	len = append_text(line, len, " tsc_hz_from_pm=");
	len = append_u64(line, len, tsc_hz);
	line[len++] = '\n';
	write_line(line, len);
}

/*
 * Linux enters an ELF image with a 16-byte-aligned stack.  A normal SysV
 * function expects the return address to have shifted it by eight bytes, so
 * align here and use call before entering the C implementation.
 */
__asm__(
    ".text\n"
    ".global _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "\tandq $-16, %rsp\n"
    "\tcall vmm_probe_main\n"
    "\tmovq $60, %rax\n"
    "\txorl %edi, %edi\n"
    "\tsyscall\n"
    "\tud2\n"
    ".size _start, .-_start\n");
