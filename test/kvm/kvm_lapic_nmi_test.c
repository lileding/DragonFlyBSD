/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies guest-originated NMI delivery through the xAPIC ICR.
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

#define MEMORY_SIZE     0x8000U
#define CODE_OFFSET     0x1000U
#define HANDLER_OFFSET  0x0200U
#define RESULT_OFFSET   0x0300U
#define GDT_OFFSET      0x4000U
#define IDT_OFFSET      0x5000U
#define APIC_SVR        0x0f0U
#define APIC_VECTOR_NMI 2U

static void
set_segment(struct kvm_segment *segment, uint16_t selector, uint8_t type)
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
set_entry(int fd)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	if (ioctl(fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	set_segment(&sregs.cs, 0x08, 0x0b);
	set_segment(&sregs.ds, 0x10, 0x03);
	set_segment(&sregs.es, 0x10, 0x03);
	set_segment(&sregs.fs, 0x10, 0x03);
	set_segment(&sregs.gs, 0x10, 0x03);
	set_segment(&sregs.ss, 0x10, 0x03);
	sregs.cr0 = 0x11;
	sregs.gdt.base = GDT_OFFSET;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	sregs.idt.base = IDT_OFFSET;
	sregs.idt.limit = 256 * sizeof(uint64_t) - 1;
	if (ioctl(fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&regs, sizeof(regs));
	regs.rip = CODE_OFFSET;
	regs.rsp = 0x2ff0;
	regs.rflags = 0x2;
	if (ioctl(fd, KVM_SET_REGS, &regs) != 0)
		err(1, "KVM_SET_REGS");
}

static void
run_hlt(int fd, struct kvm_run *run)
{
	unsigned int interrupted;

	interrupted = 0;
	while (ioctl(fd, KVM_RUN, 0) != 0) {
		if (errno == EINTR && interrupted++ != 10000) {
			usleep(100);
			continue;
		}
		err(1, "KVM_RUN");
	}
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected HLT, got %u", run->exit_reason);
}

int
main(void)
{
	static const uint8_t code[] = {
		0xbf, 0x00, 0x03, 0xe0, 0xfe,
		0xb8, 0x00, 0x04, 0x04, 0x00,
		0x89, 0x07,
		0xf4
	};
	static const uint8_t handler[] = {
		0xc6, 0x05, 0x00, 0x03, 0x00, 0x00, 0x7a,
		0xcf
	};
	struct kvm_userspace_memory_region region;
	struct kvm_lapic_state lapic;
	struct kvm_run *run;
	uint8_t *memory;
	uint64_t *gdt;
	uint64_t *idt;
	void *mapping;
	uint32_t svr;
	int control_fd;
	int vm_fd;
	int vcpu_fd;
	int run_size;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0)
		err(1, "KVM_CREATE_IRQCHIP");
	memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE, -1, 0);
	if (memory == MAP_FAILED)
		err(1, "mmap memory");
	bzero(memory, MEMORY_SIZE);
	bcopy(code, memory + CODE_OFFSET, sizeof(code));
	bcopy(handler, memory + HANDLER_OFFSET, sizeof(handler));
	gdt = (uint64_t *)(memory + GDT_OFFSET);
	gdt[1] = 0x00cf9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	idt = (uint64_t *)(memory + IDT_OFFSET);
	idt[APIC_VECTOR_NMI] = HANDLER_OFFSET | ((uint64_t)0x08 << 16) |
	    ((uint64_t)0x8e << 40);
	bzero(&region, sizeof(region));
	region.slot = 0;
	region.memory_size = MEMORY_SIZE;
	region.userspace_addr = (uintptr_t)memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	bzero(&lapic, sizeof(lapic));
	if (ioctl(vcpu_fd, KVM_GET_LAPIC, &lapic) != 0)
		err(1, "KVM_GET_LAPIC");
	svr = 0x1ffU;
	bcopy(&svr, lapic.regs + APIC_SVR, sizeof(svr));
	if (ioctl(vcpu_fd, KVM_SET_LAPIC, &lapic) != 0)
		err(1, "KVM_SET_LAPIC");
	set_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		errx(1, "KVM_GET_VCPU_MMAP_SIZE");
	mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		vcpu_fd, 0);
	if (mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = mapping;
	run_hlt(vcpu_fd, run);
	if (memory[RESULT_OFFSET] != 0x7a)
		errx(1, "NMI handler did not run");
	if (munmap(mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 || munmap(memory, MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm LAPIC NMI: PASS");
	return 0;
}
