/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies in-kernel x86 IOAPIC MMIO and fixed GSI delivery from a 32-bit
 * protected-mode guest.
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

#include <linux/kvm.h>

#define KVM_IOAPIC_MEMORY_SIZE	0x4000U
#define KVM_IOAPIC_CODE_OFFSET	0x1000U
#define KVM_IOAPIC_HANDLER_OFFSET	0x0200U
#define KVM_IOAPIC_RESULT_OFFSET	0x0300U
#define KVM_IOAPIC_IDT_OFFSET		0x1800U
#define KVM_IOAPIC_GDT_OFFSET	0x3000U
#define KVM_IOAPIC_ADDRESS	0xfec00000U
#define KVM_IOAPIC_VERSION	0x00170011U
#define KVM_IOAPIC_GSI		4U
#define KVM_IOAPIC_VECTOR		0x41U
#define KVM_IOAPIC_SVR		0x0f0U

static void
kvm_ioapic_set_segment(struct kvm_segment *segment, uint16_t selector,
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
kvm_ioapic_set_protected_entry(int vcpu_fd)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	kvm_ioapic_set_segment(&sregs.cs, 0x08, 0x0b);
	kvm_ioapic_set_segment(&sregs.ds, 0x10, 0x03);
	kvm_ioapic_set_segment(&sregs.es, 0x10, 0x03);
	kvm_ioapic_set_segment(&sregs.fs, 0x10, 0x03);
	kvm_ioapic_set_segment(&sregs.gs, 0x10, 0x03);
	kvm_ioapic_set_segment(&sregs.ss, 0x10, 0x03);
	sregs.cr0 = 0x11;
	sregs.cr4 = 0;
	sregs.efer = 0;
	sregs.gdt.base = KVM_IOAPIC_GDT_OFFSET;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	sregs.idt.base = KVM_IOAPIC_IDT_OFFSET;
	sregs.idt.limit = 256 * sizeof(uint64_t) - 1;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&regs, sizeof(regs));
	regs.rip = KVM_IOAPIC_CODE_OFFSET;
	regs.rsp = 0x2ff0;
	regs.rflags = 0x202;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) != 0)
		err(1, "KVM_SET_REGS");
}

static void
kvm_ioapic_write_code(uint8_t *memory)
{
	uint64_t *gdt;
	uint64_t *idt;
	static const uint8_t code[] = {
		0xbf, 0x00, 0x00, 0xc0, 0xfe,	/* mov $0xfec00000,%edi */
		0x31, 0xdb,				/* xor %ebx,%ebx */
		0x89, 0x1f,				/* mov %ebx,(%edi) */
		0x8b, 0x5f, 0x10,			/* mov 0x10(%edi),%ebx */
		0xb8, 0x01, 0x00, 0x00, 0x00,	/* mov $1,%eax */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0x8b, 0x47, 0x10,			/* mov 0x10(%edi),%eax */
		0xfb,					/* sti */
		0xf4,					/* hlt */
		0xeb, 0xfd				/* jmp hlt */
	};
	static const uint8_t handler[] = {
		0xc6, 0x05, 0x00, 0x03, 0x00, 0x00, 0x7a,
						/* movb $0x7a,0x300 */
		0xb8, 0x00, 0x00, 0x00, 0x00,	/* mov $0,%eax */
		0xbf, 0xb0, 0x00, 0xe0, 0xfe,	/* mov $0xfee000b0,%edi */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0xf4					/* hlt */
	};

	gdt = (uint64_t *)(memory + KVM_IOAPIC_GDT_OFFSET);
	gdt[1] = 0x00cf9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	idt = (uint64_t *)(memory + KVM_IOAPIC_IDT_OFFSET);
	idt[KVM_IOAPIC_VECTOR] = KVM_IOAPIC_HANDLER_OFFSET |
	    ((uint64_t)0x08 << 16) | ((uint64_t)0x8e << 40);
	bcopy(code, memory + KVM_IOAPIC_CODE_OFFSET, sizeof(code));
	bcopy(handler, memory + KVM_IOAPIC_HANDLER_OFFSET, sizeof(handler));
}

static void
kvm_ioapic_run(int vcpu_fd)
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
}

int
main(void)
{
	struct kvm_userspace_memory_region memory_region;
	struct kvm_irqchip irqchip;
	struct kvm_irq_level irq_line;
	struct kvm_lapic_state lapic;
	struct kvm_regs regs;
	struct kvm_run *run;
	uint8_t *guest_memory;
	void *run_mapping;
	int control_fd;
	int vm_fd;
	int vcpu_fd;
	int run_size;
	uint32_t value;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_IRQCHIP) != 1)
		errx(1, "KVM_CAP_IRQCHIP unavailable");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0)
		err(1, "KVM_CREATE_IRQCHIP");
	guest_memory = mmap(NULL, KVM_IOAPIC_MEMORY_SIZE,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_IOAPIC_MEMORY_SIZE);
	kvm_ioapic_write_code(guest_memory);
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.memory_size = KVM_IOAPIC_MEMORY_SIZE;
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
	bcopy(&value, lapic.regs + KVM_IOAPIC_SVR, sizeof(value));
	if (ioctl(vcpu_fd, KVM_SET_LAPIC, &lapic) != 0)
		err(1, "KVM_SET_LAPIC");
	kvm_ioapic_set_protected_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		err(1, "KVM_GET_VCPU_MMAP_SIZE");
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;
	kvm_ioapic_run(vcpu_fd);
	if (run->exit_reason != KVM_EXIT_HLT)
		err(1, "expected KVM_EXIT_HLT, got %u", run->exit_reason);
	if (ioctl(vcpu_fd, KVM_GET_REGS, &regs) != 0)
		err(1, "KVM_GET_REGS");
	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_IOAPIC;
	if (ioctl(vm_fd, KVM_GET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_GET_IRQCHIP");
	if (irqchip.chip.ioapic.ioregsel != 1)
		errx(1, "IOAPIC selector %#x", irqchip.chip.ioapic.ioregsel);
	if ((uint32_t)regs.rbx != 0x01000000U)
		errx(1, "IOAPIC id %#x", (uint32_t)regs.rbx);
	if ((uint32_t)regs.rax != KVM_IOAPIC_VERSION)
		errx(1, "IOAPIC version %#x", (uint32_t)regs.rax);
	irqchip.chip.ioapic.redirtbl[KVM_IOAPIC_GSI].bits =
	    KVM_IOAPIC_VECTOR;
	if (ioctl(vm_fd, KVM_SET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_SET_IRQCHIP redirection");
	irq_line.irq = KVM_IOAPIC_GSI;
	irq_line.level = 1;
	if (ioctl(vm_fd, KVM_IRQ_LINE, &irq_line) != 0)
		err(1, "KVM_IRQ_LINE assert");
	irq_line.level = 0;
	if (ioctl(vm_fd, KVM_IRQ_LINE, &irq_line) != 0)
		err(1, "KVM_IRQ_LINE deassert");
	kvm_ioapic_run(vcpu_fd);
	if (run->exit_reason != KVM_EXIT_HLT)
		err(1, "expected HLT after GSI, got %u", run->exit_reason);
	if (guest_memory[KVM_IOAPIC_RESULT_OFFSET] != 0x7a)
		err(1, "IOAPIC GSI handler did not run");
	if (munmap(run_mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 ||
	    munmap(guest_memory, KVM_IOAPIC_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm IOAPIC: PASS");
	return 0;
}
