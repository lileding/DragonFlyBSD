/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies external scalar PIO exits and completion through the KVM frontend.
 */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include <linux/kvm.h>

#define KVM_PIO_MEMORY_SIZE	0x4000U
#define KVM_PIO_CODE_OFFSET	0x1000U
#define KVM_PIO_GDT_OFFSET	0x3000U
#define KVM_PIO_PORT		0x0500U
#define KVM_PIO_WRITE_VALUE	0x11223344U
#define KVM_PIO_READ_VALUE	0x55667788U

static void
kvm_pio_set_segment(struct kvm_segment *segment, uint16_t selector,
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
kvm_pio_set_protected_entry(int vcpu_fd)
{
	struct kvm_regs registers;
	struct kvm_sregs sregs;

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	kvm_pio_set_segment(&sregs.cs, 0x08, 0x0b);
	kvm_pio_set_segment(&sregs.ds, 0x10, 0x03);
	kvm_pio_set_segment(&sregs.es, 0x10, 0x03);
	kvm_pio_set_segment(&sregs.fs, 0x10, 0x03);
	kvm_pio_set_segment(&sregs.gs, 0x10, 0x03);
	kvm_pio_set_segment(&sregs.ss, 0x10, 0x03);
	sregs.cr0 = 0x11;
	sregs.gdt.base = KVM_PIO_GDT_OFFSET;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&registers, sizeof(registers));
	registers.rip = KVM_PIO_CODE_OFFSET;
	registers.rsp = 0x2000;
	registers.rflags = 0x2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &registers) != 0)
		err(1, "KVM_SET_REGS");
}

static void
kvm_pio_write_code(uint8_t *memory)
{
	uint64_t *gdt;
	static const uint8_t code[] = {
		0xb8, 0x44, 0x33, 0x22, 0x11,	/* mov $WRITE_VALUE,%eax */
		0x66, 0xba, 0x00, 0x05,		/* mov $PIO_PORT,%dx */
		0xe8, 0x04, 0x00, 0x00, 0x00,	/* call output */
		0xed,					/* inl (%dx),%eax */
		0x89, 0xc3,				/* mov %eax,%ebx */
		0xf4,					/* hlt */
		0xef,					/* output: outl %eax,(%dx) */
		0xc3,					/* ret */
	};

	gdt = (uint64_t *)(memory + KVM_PIO_GDT_OFFSET);
	gdt[1] = 0x00cf9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	bcopy(code, memory + KVM_PIO_CODE_OFFSET, sizeof(code));
}

static void
kvm_pio_expect(int vcpu_fd, struct kvm_run *run, uint8_t direction,
	uint32_t expected)
{
	uint32_t observed;

	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN");
	if (run->exit_reason != KVM_EXIT_IO)
		errx(1, "expected KVM_EXIT_IO, got %u", run->exit_reason);
	if (run->io.direction != direction || run->io.port != KVM_PIO_PORT ||
	    run->io.size != sizeof(expected) || run->io.count != 1)
		errx(1, "unexpected PIO exit");
	if (direction == KVM_EXIT_IO_OUT) {
		bcopy((uint8_t *)run + run->io.data_offset, &observed,
		    sizeof(observed));
		if (observed != expected)
			errx(1, "PIO write %#x", observed);
	}
}

static void
kvm_pio_complete_read(struct kvm_run *run, uint32_t value)
{

	bcopy(&value, (uint8_t *)run + run->io.data_offset, sizeof(value));
}

int
main(void)
{
	struct kvm_userspace_memory_region memory_region;
	struct kvm_regs registers;
	struct kvm_run *run;
	uint8_t *guest_memory;
	void *run_mapping;
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
	guest_memory = mmap(NULL, KVM_PIO_MEMORY_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_PIO_MEMORY_SIZE);
	kvm_pio_write_code(guest_memory);
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.memory_size = KVM_PIO_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	kvm_pio_set_protected_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		errx(1, "unexpected KVM_RUN mapping size %d", run_size);
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;

	kvm_pio_expect(vcpu_fd, run, KVM_EXIT_IO_OUT, KVM_PIO_WRITE_VALUE);
	kvm_pio_expect(vcpu_fd, run, KVM_EXIT_IO_IN, 0);
	kvm_pio_complete_read(run, KVM_PIO_READ_VALUE);
	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN after PIO read");
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected KVM_EXIT_HLT, got %u", run->exit_reason);
	if (ioctl(vcpu_fd, KVM_GET_REGS, &registers) != 0)
		err(1, "KVM_GET_REGS");
	if ((uint32_t)registers.rbx != KVM_PIO_READ_VALUE)
		errx(1, "PIO read %#x", (uint32_t)registers.rbx);
	if (munmap(run_mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 ||
	    munmap(guest_memory, KVM_PIO_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm PIO: PASS");
	return 0;
}
