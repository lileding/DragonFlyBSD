/*
 * Minimal x86-64 Linux guest probe for the dfvmm CPU topology contract.
 *
 * This has no libc dependency.  It verifies that dfvmm describes two
 * independent cores with one thread each and does not publish x2APIC before
 * its full platform ABI exists.
 */

typedef unsigned int uint32_t;
typedef unsigned long size_t;

#define LINUX_SYS_WRITE	1L
#define LINUX_SYS_EXIT		60L

#define CPUID_HTT		0x10000000U
#define CPUID_X2APIC		0x00200000U
#define CPUID_TOPOLOGY_SMT	0x00000100U
#define CPUID_TOPOLOGY_CORE	0x00000201U

static long linux_syscall3(long number, long arg1, long arg2, long arg3);
static void guest_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]);
static int guest_topology_valid(void);
static void guest_write(const char *message, size_t length);

void
_start(void)
{
	static const char ok[] =
	    "DFVMM_CPU_TOPOLOGY_OK cores=2 threads=1 x2apic=0\n";
	static const char bad[] = "DFVMM_CPU_TOPOLOGY_BAD\n";
	int status;

	status = guest_topology_valid() ? 0 : 1;
	if (status == 0)
		guest_write(ok, sizeof(ok) - 1);
	else
		guest_write(bad, sizeof(bad) - 1);
	(void)linux_syscall3(LINUX_SYS_EXIT, status, 0, 0);
	for (;;)
		;
}

static int
guest_topology_valid(void)
{
	uint32_t regs[4];

	guest_cpuid(0, 0, regs);
	if (regs[0] < 0x1fU)
		return 0;
	guest_cpuid(1, 0, regs);
	if (((regs[1] >> 16) & 0xffU) != 2 ||
	    (regs[3] & CPUID_HTT) == 0 ||
	    (regs[2] & CPUID_X2APIC) != 0)
		return 0;
	guest_cpuid(0x0bU, 0, regs);
	if (regs[0] != 0 || regs[1] != 1 || regs[2] != CPUID_TOPOLOGY_SMT ||
	    regs[3] != 0)
		return 0;
	guest_cpuid(0x0bU, 1, regs);
	if (regs[0] != 1 || regs[1] != 2 || regs[2] != CPUID_TOPOLOGY_CORE ||
	    regs[3] != 0)
		return 0;
	guest_cpuid(0x1fU, 0, regs);
	if (regs[0] != 0 || regs[1] != 1 || regs[2] != CPUID_TOPOLOGY_SMT ||
	    regs[3] != 0)
		return 0;
	guest_cpuid(0x1fU, 1, regs);
	return regs[0] == 1 && regs[1] == 2 &&
	    regs[2] == CPUID_TOPOLOGY_CORE && regs[3] == 0;
}

static void
guest_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4])
{
	__asm__ __volatile__("cpuid"
	    : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
	    : "a"(leaf), "c"(subleaf));
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

static void
guest_write(const char *message, size_t length)
{
	while (length != 0) {
		long written;

		written = linux_syscall3(LINUX_SYS_WRITE, 1, (long)message,
		    (long)length);
		if (written <= 0)
			return;
		message += written;
		length -= (size_t)written;
	}
}
