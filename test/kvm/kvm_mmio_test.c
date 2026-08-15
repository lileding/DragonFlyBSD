/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies external scalar MMIO exits and completion through the KVM frontend.
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

#include <sys/kvm.h>

#define KVM_MMIO_MEMORY_SIZE	0x4000U
#define KVM_MMIO_CODE_OFFSET	0x1000U
#define KVM_MMIO_GDT_OFFSET	0x3000U
#define KVM_MMIO_BOUNDARY_MEMORY_SIZE	0x1000U
#define KVM_MMIO_BOUNDARY_CODE_OFFSET	(KVM_MMIO_BOUNDARY_MEMORY_SIZE - 2U)
#define KVM_MMIO_ADDRESS	0xfec10000U
#define KVM_MMIO_WRITE_VALUE	0x11223344U
#define KVM_MMIO_READ_VALUE	0x55667788U
#define KVM_MMIO_RMW_VALUE	(KVM_MMIO_READ_VALUE | 0x10U)

static void
kvm_mmio_set_segment(struct kvm_segment *segment, uint16_t selector,
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
kvm_mmio_set_protected_entry(int vcpu_fd)
{
	struct kvm_regs registers;
	struct kvm_sregs sregs;

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	kvm_mmio_set_segment(&sregs.cs, 0x08, 0x0b);
	kvm_mmio_set_segment(&sregs.ds, 0x10, 0x03);
	kvm_mmio_set_segment(&sregs.es, 0x10, 0x03);
	kvm_mmio_set_segment(&sregs.fs, 0x10, 0x03);
	kvm_mmio_set_segment(&sregs.gs, 0x10, 0x03);
	kvm_mmio_set_segment(&sregs.ss, 0x10, 0x03);
	sregs.cr0 = 0x11;
	sregs.gdt.base = KVM_MMIO_GDT_OFFSET;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&registers, sizeof(registers));
	registers.rip = KVM_MMIO_CODE_OFFSET;
	registers.rflags = 0x2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &registers) != 0)
		err(1, "KVM_SET_REGS");
}

static void
kvm_mmio_write_code(uint8_t *memory)
{
	uint64_t *gdt;
	static const uint8_t code[] = {
		0xbf, 0x00, 0x00, 0xc1, 0xfe,	/* mov $MMIO_ADDRESS,%edi */
		0xb8, 0x44, 0x33, 0x22, 0x11,	/* mov $WRITE_VALUE,%eax */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0x8b, 0x1f,				/* mov (%edi),%ebx */
		0x83, 0x0f, 0x10,			/* orl $0x10,(%edi) */
		0xf4,					/* hlt */
	};

	gdt = (uint64_t *)(memory + KVM_MMIO_GDT_OFFSET);
	gdt[1] = 0x00cf9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	bcopy(code, memory + KVM_MMIO_CODE_OFFSET, sizeof(code));
}

static void
kvm_mmio_expect_write(int vcpu_fd, struct kvm_run *run, uint32_t expected)
{
	uint32_t observed;

	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN");
	if (run->exit_reason != KVM_EXIT_MMIO)
		errx(1, "expected KVM_EXIT_MMIO, got %u", run->exit_reason);
	if (run->mmio.phys_addr != KVM_MMIO_ADDRESS || run->mmio.len != 4 ||
	    run->mmio.is_write == 0)
		errx(1, "unexpected MMIO write exit");
	bcopy(run->mmio.data, &observed, sizeof(observed));
	if (observed != expected)
		errx(1, "MMIO write %#x", observed);
}

static void
kvm_mmio_expect_read(int vcpu_fd, struct kvm_run *run)
{

	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN");
	if (run->exit_reason != KVM_EXIT_MMIO)
		errx(1, "expected KVM_EXIT_MMIO, got %u", run->exit_reason);
	if (run->mmio.phys_addr != KVM_MMIO_ADDRESS || run->mmio.len != 4 ||
	    run->mmio.is_write != 0)
		errx(1, "unexpected MMIO read exit");
}

static void
kvm_mmio_complete_read(struct kvm_run *run, uint32_t value)
{

	bcopy(&value, run->mmio.data, sizeof(value));
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
	guest_memory = mmap(NULL, KVM_MMIO_MEMORY_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_MMIO_MEMORY_SIZE);
	kvm_mmio_write_code(guest_memory);
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.memory_size = KVM_MMIO_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	kvm_mmio_set_protected_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		err(1, "KVM_GET_VCPU_MMAP_SIZE");
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;

	kvm_mmio_expect_write(vcpu_fd, run, KVM_MMIO_WRITE_VALUE);
	kvm_mmio_expect_read(vcpu_fd, run);
	kvm_mmio_complete_read(run, KVM_MMIO_READ_VALUE);
	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN after MMIO read");
	if (run->exit_reason != KVM_EXIT_MMIO || run->mmio.is_write != 0)
		errx(1, "expected MMIO RMW read, got %u", run->exit_reason);
	kvm_mmio_complete_read(run, KVM_MMIO_READ_VALUE);
	kvm_mmio_expect_write(vcpu_fd, run, KVM_MMIO_RMW_VALUE);
	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN after MMIO RMW write");
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected KVM_EXIT_HLT, got %u", run->exit_reason);
	if (ioctl(vcpu_fd, KVM_GET_REGS, &registers) != 0)
		err(1, "KVM_GET_REGS");
	if ((uint32_t)registers.rbx != KVM_MMIO_READ_VALUE)
		errx(1, "MMIO read %#x", (uint32_t)registers.rbx);

	/*
	 * Leave only the page containing the two-byte MMIO write mapped.  A
	 * VMX frontend must not require a readable next page merely to decode it.
	 */
	memory_region.memory_size = KVM_MMIO_BOUNDARY_MEMORY_SIZE;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION boundary");
	guest_memory[KVM_MMIO_BOUNDARY_CODE_OFFSET] = 0x89;
	guest_memory[KVM_MMIO_BOUNDARY_CODE_OFFSET + 1] = 0x07;
	registers.rax = KVM_MMIO_WRITE_VALUE;
	registers.rdi = KVM_MMIO_ADDRESS;
	registers.rip = KVM_MMIO_BOUNDARY_CODE_OFFSET;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &registers) != 0)
		err(1, "KVM_SET_REGS boundary");
	kvm_mmio_expect_write(vcpu_fd, run, KVM_MMIO_WRITE_VALUE);

	if (munmap(run_mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 ||
	    munmap(guest_memory, KVM_MMIO_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm MMIO: PASS");
	return 0;
}
