/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies an MMIO ioeventfd doorbell is consumed in kernel.
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

#define KVM_IOEVENT_MEMORY_SIZE	0x4000U
#define KVM_IOEVENT_CODE_OFFSET	0x1000U
#define KVM_IOEVENT_GDT_OFFSET		0x3000U
#define KVM_IOEVENT_ADDRESS		0xfec10000U
#define KVM_IOEVENT_VALUE		0x11223344U

static void
kvm_ioeventfd_set_segment(struct kvm_segment *segment, uint16_t selector,
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
kvm_ioeventfd_set_protected_entry(int vcpu_fd)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	kvm_ioeventfd_set_segment(&sregs.cs, 0x08, 0x0b);
	kvm_ioeventfd_set_segment(&sregs.ds, 0x10, 0x03);
	kvm_ioeventfd_set_segment(&sregs.es, 0x10, 0x03);
	kvm_ioeventfd_set_segment(&sregs.fs, 0x10, 0x03);
	kvm_ioeventfd_set_segment(&sregs.gs, 0x10, 0x03);
	kvm_ioeventfd_set_segment(&sregs.ss, 0x10, 0x03);
	sregs.cr0 = 0x11;
	sregs.cr4 = 0;
	sregs.efer = 0;
	sregs.gdt.base = KVM_IOEVENT_GDT_OFFSET;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&regs, sizeof(regs));
	regs.rip = KVM_IOEVENT_CODE_OFFSET;
	regs.rsp = 0x2ff0;
	regs.rflags = 0x2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) != 0)
		err(1, "KVM_SET_REGS");
}

static void
kvm_ioeventfd_write_code(uint8_t *memory)
{
	uint64_t *gdt;
	static const uint8_t code[] = {
		0xb8, 0x44, 0x33, 0x22, 0x11,	/* mov $VALUE,%eax */
		0xbf, 0x00, 0x00, 0xc1, 0xfe,	/* mov $ADDRESS,%edi */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0xf4					/* hlt */
	};

	gdt = (uint64_t *)(memory + KVM_IOEVENT_GDT_OFFSET);
	gdt[1] = 0x00cf9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	bcopy(code, memory + KVM_IOEVENT_CODE_OFFSET, sizeof(code));
}

int
main(void)
{
	struct kvm_dfly_eventfd eventfd;
	struct kvm_ioeventfd ioevent;
	struct kvm_run *run;
	struct kvm_userspace_memory_region memory_region;
	uint8_t *guest_memory;
	void *run_mapping;
	uint64_t value;
	int control_fd;
	int interrupted;
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
	guest_memory = mmap(NULL, KVM_IOEVENT_MEMORY_SIZE,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_IOEVENT_MEMORY_SIZE);
	kvm_ioeventfd_write_code(guest_memory);
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.memory_size = KVM_IOEVENT_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	kvm_ioeventfd_set_protected_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		err(1, "KVM_GET_VCPU_MMAP_SIZE");
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;
	bzero(&eventfd, sizeof(eventfd));
	eventfd.flags = KVM_DFLY_EVENTFD_NONBLOCK | KVM_DFLY_EVENTFD_CLOEXEC;
	if (ioctl(control_fd, KVM_DFLY_CREATE_EVENTFD, &eventfd) != 0)
		err(1, "KVM_DFLY_CREATE_EVENTFD");
	bzero(&ioevent, sizeof(ioevent));
	ioevent.datamatch = KVM_IOEVENT_VALUE;
	ioevent.addr = KVM_IOEVENT_ADDRESS;
	ioevent.len = sizeof(uint32_t);
	ioevent.fd = eventfd.fd;
	ioevent.flags = KVM_IOEVENTFD_FLAG_DATAMATCH;
	if (ioctl(vm_fd, KVM_IOEVENTFD, &ioevent) != 0)
		err(1, "KVM_IOEVENTFD");
	interrupted = 0;
	while (ioctl(vcpu_fd, KVM_RUN, 0) != 0) {
		if (errno != EINTR || interrupted++ == 10000)
			err(1, "KVM_RUN");
	}
	if (run->exit_reason != KVM_EXIT_HLT)
		err(1, "expected HLT after ioeventfd MMIO, got %u",
		    run->exit_reason);
	if (read(eventfd.fd, &value, sizeof(value)) != sizeof(value))
		err(1, "read ioeventfd");
	if (value != 1)
		err(1, "ioeventfd count %ju", (uintmax_t)value);
	ioevent.flags |= KVM_IOEVENTFD_FLAG_DEASSIGN;
	if (ioctl(vm_fd, KVM_IOEVENTFD, &ioevent) != 0)
		err(1, "KVM_IOEVENTFD deassign");
	if (munmap(run_mapping, run_size) != 0 || close(eventfd.fd) != 0 ||
	    close(vcpu_fd) != 0 || close(vm_fd) != 0 ||
	    munmap(guest_memory, KVM_IOEVENT_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm ioeventfd: PASS");
	return 0;
}
