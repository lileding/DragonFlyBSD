/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies software irqchip periodic LAPIC timer delivery through the xAPIC
 * IRR and IDT.
 */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include <sys/kvm.h>

#define KVM_LAPIC_PERIODIC_TIMER_MEMORY_SIZE	0x8000U
#define KVM_LAPIC_PERIODIC_TIMER_CODE_OFFSET	0x1000U
#define KVM_LAPIC_PERIODIC_TIMER_HANDLER_OFFSET	0x0200U
#define KVM_LAPIC_PERIODIC_TIMER_RESULT_OFFSET	0x0300U
#define KVM_LAPIC_PERIODIC_TIMER_GDT_OFFSET	0x4000U
#define KVM_LAPIC_PERIODIC_TIMER_IDT_OFFSET	0x5000U
#define KVM_LAPIC_PERIODIC_TIMER_SVR		0x0f0U
#define KVM_LAPIC_PERIODIC_TIMER_LVTT		0x320U
#define KVM_LAPIC_PERIODIC_TIMER_TMICT		0x380U
#define KVM_LAPIC_PERIODIC_TIMER_TDCR		0x3e0U
#define KVM_LAPIC_PERIODIC_TIMER_VECTOR		0x40U

static void
kvm_lapic_periodic_timer_set_segment(struct kvm_segment *segment, uint16_t selector,
    uint8_t type)
{

	bzero(segment, sizeof(*segment));
	segment->selector = selector;
	segment->limit = UINT32_MAX;
	segment->type = type;
	segment->present = 1;
	segment->s = 1;
	segment->db = 1;
	segment->g = 1;
}

static void
kvm_lapic_periodic_timer_set_protected_entry(int vcpu_fd)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	kvm_lapic_periodic_timer_set_segment(&sregs.cs, 0x08, 0x0b);
	kvm_lapic_periodic_timer_set_segment(&sregs.ds, 0x10, 0x03);
	kvm_lapic_periodic_timer_set_segment(&sregs.es, 0x10, 0x03);
	kvm_lapic_periodic_timer_set_segment(&sregs.fs, 0x10, 0x03);
	kvm_lapic_periodic_timer_set_segment(&sregs.gs, 0x10, 0x03);
	kvm_lapic_periodic_timer_set_segment(&sregs.ss, 0x10, 0x03);
	sregs.cr0 = 0x11;
	sregs.cr4 = 0;
	sregs.efer = 0;
	sregs.gdt.base = KVM_LAPIC_PERIODIC_TIMER_GDT_OFFSET;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	sregs.idt.base = KVM_LAPIC_PERIODIC_TIMER_IDT_OFFSET;
	sregs.idt.limit = 256 * sizeof(uint64_t) - 1;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&regs, sizeof(regs));
	regs.rip = KVM_LAPIC_PERIODIC_TIMER_CODE_OFFSET;
	regs.rsp = 0x2ff0;
	regs.rflags = 0x202;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) != 0)
		err(1, "KVM_SET_REGS");
}

static void
kvm_lapic_periodic_timer_write_code(uint8_t *memory)
{
	uint64_t *gdt;
	uint64_t *idt;
	static const uint8_t idle_code[] = {
		0xbf, 0x20, 0x03, 0xe0, 0xfe,	/* mov $0xfee00320,%edi */
		0xb8, 0x40, 0x00, 0x02, 0x00,	/* mov $0x20040,%eax */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0xbf, 0xe0, 0x03, 0xe0, 0xfe,	/* mov $0xfee003e0,%edi */
		0x31, 0xc0,				/* xor %eax,%eax */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0xbf, 0x80, 0x03, 0xe0, 0xfe,	/* mov $0xfee00380,%edi */
		0xb8, 0x40, 0x0d, 0x03, 0x00,	/* mov $0x30d40,%eax */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0xfb,					/* sti */
		0xf4,					/* hlt */
		0xeb, 0xfd				/* jmp hlt */
	};
	static const uint8_t handler_code[] = {
		0xfe, 0x05, 0x00, 0x03, 0x00, 0x00,
						/* incb 0x300 */
		0xb8, 0x00, 0x00, 0x00, 0x00,	/* mov $0,%eax */
		0xbf, 0xb0, 0x00, 0xe0, 0xfe,	/* mov $0xfee000b0,%edi */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0xcf					/* iret */
	};

	gdt = (uint64_t *)(memory + KVM_LAPIC_PERIODIC_TIMER_GDT_OFFSET);
	gdt[1] = 0x00cf9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	idt = (uint64_t *)(memory + KVM_LAPIC_PERIODIC_TIMER_IDT_OFFSET);
	idt[KVM_LAPIC_PERIODIC_TIMER_VECTOR] = KVM_LAPIC_PERIODIC_TIMER_HANDLER_OFFSET |
	    ((uint64_t)0x08 << 16) | ((uint64_t)0x8e << 40);
	bcopy(idle_code, memory + KVM_LAPIC_PERIODIC_TIMER_CODE_OFFSET, sizeof(idle_code));
	bcopy(handler_code, memory + KVM_LAPIC_PERIODIC_TIMER_HANDLER_OFFSET,
	    sizeof(handler_code));
}

static void
kvm_lapic_periodic_timer_run_hlt(int vcpu_fd, struct kvm_run *run)
{
	unsigned int interrupted;

	interrupted = 0;
	while (ioctl(vcpu_fd, KVM_RUN, 0) != 0) {
		if (errno == EINTR && interrupted++ != 10000) {
			usleep(100);
			continue;
		}
		err(1, "KVM_RUN");
	}
	if (run->exit_reason != KVM_EXIT_HLT)
		err(1, "expected KVM_EXIT_HLT, got %u", run->exit_reason);
}

int
main(void)
{
	struct kvm_userspace_memory_region memory_region;
	struct kvm_lapic_state lapic;
	struct kvm_run *run;
	uint8_t *guest_memory;
	void *run_mapping;
	uint32_t value;
	int control_fd;
	int vm_fd;
	int vcpu_fd;
	int run_size;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_IRQCHIP) != 1)
		err(1, "KVM_CAP_IRQCHIP unavailable");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0)
		err(1, "KVM_CREATE_IRQCHIP");
	guest_memory = mmap(NULL, KVM_LAPIC_PERIODIC_TIMER_MEMORY_SIZE,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_LAPIC_PERIODIC_TIMER_MEMORY_SIZE);
	kvm_lapic_periodic_timer_write_code(guest_memory);
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.memory_size = KVM_LAPIC_PERIODIC_TIMER_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	bzero(&lapic, sizeof(lapic));
	if (ioctl(vcpu_fd, KVM_GET_LAPIC, &lapic) != 0)
		err(1, "KVM_GET_LAPIC");
	value = 0x1ffU;
	bcopy(&value, lapic.regs + KVM_LAPIC_PERIODIC_TIMER_SVR, sizeof(value));
	if (ioctl(vcpu_fd, KVM_SET_LAPIC, &lapic) != 0)
		err(1, "KVM_SET_LAPIC");
	kvm_lapic_periodic_timer_set_protected_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		err(1, "KVM_GET_VCPU_MMAP_SIZE");
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;
	kvm_lapic_periodic_timer_run_hlt(vcpu_fd, run);
	usleep(100000);
	kvm_lapic_periodic_timer_run_hlt(vcpu_fd, run);
	if (guest_memory[KVM_LAPIC_PERIODIC_TIMER_RESULT_OFFSET] != 1)
		err(1, "first LAPIC timer handler did not run");
	usleep(100000);
	kvm_lapic_periodic_timer_run_hlt(vcpu_fd, run);
	if (guest_memory[KVM_LAPIC_PERIODIC_TIMER_RESULT_OFFSET] != 2)
		err(1, "periodic LAPIC timer did not rearm");
	if (munmap(run_mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 ||
	    munmap(guest_memory, KVM_LAPIC_PERIODIC_TIMER_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm periodic LAPIC timer: PASS");
	return 0;
}
