/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies protected-mode string PIO completion through the KVM frontend.
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

#define KVM_PIO_STRING_MEMORY_SIZE	0x9000U
#define KVM_PIO_STRING_CODE_GPA		0x1000U
#define KVM_PIO_STRING_GDT_GPA		0x2000U
#define KVM_PIO_STRING_DATA_GPA		0x3000U
#define KVM_PIO_STRING_PML4_GPA		0x4000U
#define KVM_PIO_STRING_PDPT_GPA		0x5000U
#define KVM_PIO_STRING_PD_GPA		0x6000U
#define KVM_PIO_STRING_PT_GPA		0x7000U
#define KVM_PIO_STRING_PORT		0x0500U

#define KVM_X64_CR0_PE			0x00000001ULL
#define KVM_X64_CR0_PG			0x80000000ULL
#define KVM_X64_CR4_PAE			0x00000020ULL
#define KVM_X64_EFER_LME		0x00000100ULL
#define KVM_X64_EFER_LMA		0x00000400ULL
#define KVM_X64_PTE_PRESENT		0x0000000000000001ULL
#define KVM_X64_PTE_WRITE		0x0000000000000002ULL

static void
kvm_pio_string_set_segment(struct kvm_segment *segment, uint16_t selector,
	uint8_t type, uint8_t long_mode)
{

	bzero(segment, sizeof(*segment));
	segment->selector = selector;
	segment->limit = UINT32_MAX;
	segment->type = type;
	segment->present = 1;
	segment->s = 1;
	segment->l = long_mode;
	segment->g = 1;
}

static void
kvm_pio_string_write_memory(uint8_t *memory)
{
	uint64_t *gdt;
	uint64_t *pml4;
	uint64_t *pdpt;
	uint64_t *pd;
	uint64_t *pt;
	unsigned int index;
	static const uint8_t code[] = {
		0x66, 0xba, 0x00, 0x05,		/* mov $PORT,%dx */
		0xbf, 0x00, 0x30, 0x00, 0x00,	/* mov $DATA,%edi */
		0xb9, 0x04, 0x00, 0x00, 0x00,	/* mov $4,%ecx */
		0xfc,					/* cld */
		0xf3, 0x6c,				/* rep insb */
		0xf4,					/* hlt */
	};

	gdt = (uint64_t *)(memory + KVM_PIO_STRING_GDT_GPA);
	gdt[1] = 0x00af9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	bcopy(code, memory + KVM_PIO_STRING_CODE_GPA, sizeof(code));

	pml4 = (uint64_t *)(memory + KVM_PIO_STRING_PML4_GPA);
	pdpt = (uint64_t *)(memory + KVM_PIO_STRING_PDPT_GPA);
	pd = (uint64_t *)(memory + KVM_PIO_STRING_PD_GPA);
	pt = (uint64_t *)(memory + KVM_PIO_STRING_PT_GPA);
	pml4[0] = KVM_PIO_STRING_PDPT_GPA | KVM_X64_PTE_PRESENT |
	    KVM_X64_PTE_WRITE;
	pdpt[0] = KVM_PIO_STRING_PD_GPA | KVM_X64_PTE_PRESENT |
	    KVM_X64_PTE_WRITE;
	pd[0] = KVM_PIO_STRING_PT_GPA | KVM_X64_PTE_PRESENT |
	    KVM_X64_PTE_WRITE;
	for (index = 0; index < KVM_PIO_STRING_MEMORY_SIZE / getpagesize();
	    ++index) {
		pt[index] = (uint64_t)index * getpagesize() |
		    KVM_X64_PTE_PRESENT | KVM_X64_PTE_WRITE;
	}
}

static void
kvm_pio_string_set_entry(int vcpu_fd)
{
	struct kvm_regs registers;
	struct kvm_sregs sregs;

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	kvm_pio_string_set_segment(&sregs.cs, 0x08, 0x0b, 1);
	kvm_pio_string_set_segment(&sregs.ds, 0x10, 0x03, 0);
	kvm_pio_string_set_segment(&sregs.es, 0x10, 0x03, 0);
	kvm_pio_string_set_segment(&sregs.fs, 0x10, 0x03, 0);
	kvm_pio_string_set_segment(&sregs.gs, 0x10, 0x03, 0);
	kvm_pio_string_set_segment(&sregs.ss, 0x10, 0x03, 0);
	sregs.gdt.base = KVM_PIO_STRING_GDT_GPA;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	sregs.cr0 = KVM_X64_CR0_PE | KVM_X64_CR0_PG;
	sregs.cr3 = KVM_PIO_STRING_PML4_GPA;
	sregs.cr4 = KVM_X64_CR4_PAE;
	sregs.efer = KVM_X64_EFER_LME | KVM_X64_EFER_LMA;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&registers, sizeof(registers));
	registers.rip = KVM_PIO_STRING_CODE_GPA;
	registers.rsp = KVM_PIO_STRING_DATA_GPA + 0x800;
	registers.rflags = 0x2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &registers) != 0)
		err(1, "KVM_SET_REGS");
}

int
main(void)
{
	struct kvm_userspace_memory_region memory_region;
	struct kvm_regs registers;
	struct kvm_run *run;
	uint8_t *guest_memory;
	uint8_t value;
	void *run_mapping;
	int control_fd;
	int vm_fd;
	int vcpu_fd;
	int run_size;
	unsigned int index;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	guest_memory = mmap(NULL, KVM_PIO_STRING_MEMORY_SIZE,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_PIO_STRING_MEMORY_SIZE);
	kvm_pio_string_write_memory(guest_memory);
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.memory_size = KVM_PIO_STRING_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	kvm_pio_string_set_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		err(1, "KVM_GET_VCPU_MMAP_SIZE");
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;

	for (index = 0; index < 4; ++index) {
		if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
			err(1, "KVM_RUN PIO %u", index);
		if (run->exit_reason != KVM_EXIT_IO ||
		    run->io.direction != KVM_EXIT_IO_IN ||
		    run->io.port != KVM_PIO_STRING_PORT ||
		    run->io.size != 1 || run->io.count != 1)
			errx(1, "unexpected exit %u reason=%u suberror=%u data=%#jx",
			    index, run->exit_reason, run->internal.suberror,
			    (uintmax_t)run->internal.data[0]);
		value = (uint8_t)(index + 1);
		bcopy(&value, (uint8_t *)run + run->io.data_offset, sizeof(value));
	}
	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN HLT");
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected KVM_EXIT_HLT, got %u", run->exit_reason);
	if (bcmp(guest_memory + KVM_PIO_STRING_DATA_GPA,
	    "\x01\x02\x03\x04", 4) != 0)
		err(1, "unexpected PIO destination");
	if (ioctl(vcpu_fd, KVM_GET_REGS, &registers) != 0)
		err(1, "KVM_GET_REGS");
	if (registers.rcx != 0 || registers.rdi != KVM_PIO_STRING_DATA_GPA + 4)
		err(1, "unexpected string PIO registers");
	if (munmap(run_mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 ||
	    munmap(guest_memory, KVM_PIO_STRING_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm protected string PIO: PASS");
	return 0;
}
