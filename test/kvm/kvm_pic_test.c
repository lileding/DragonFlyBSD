/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies in-kernel x86 PIC PIO, IRQ0 delivery, and KVM IRQCHIP state.
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

#define KVM_PIC_MEMORY_SIZE	0x100000U
#define KVM_PIC_RESET_OFFSET	0xfff0U
#define KVM_PIC_STUB_OFFSET	0x1000U
#define KVM_PIC_HANDLER_OFFSET	0x0200U
#define KVM_PIC_RESULT_OFFSET	0x0300U

static void
kvm_pic_write_code(uint8_t *memory)
{
	static const uint8_t reset_code[] = {
		0xea, 0x00, 0x10, 0x00, 0x00 /* ljmp $0x0000,$0x1000 */
	};
	static const uint8_t stub_code[] = {
		0xfa,			/* cli */
		0xb0, 0x11, 0xe6, 0x20,	/* ICW1: master initialization */
		0xb0, 0x20, 0xe6, 0x21,	/* ICW2: vector base */
		0xb0, 0x04, 0xe6, 0x21,	/* ICW3: slave on IRQ2 */
		0xb0, 0x01, 0xe6, 0x21,	/* ICW4: 8086 mode */
		0xb0, 0xfe, 0xe6, 0x21,	/* unmask IRQ0 */
		0xfb,			/* sti */
		0xf4,			/* hlt */
		0xeb, 0xfd		/* jmp hlt */
	};
	static const uint8_t handler_code[] = {
		0xc6, 0x06, 0x00, 0x03, 0x7a,	/* movb $0x7a, 0x300 */
		0xb0, 0x20, 0xe6, 0x20,		/* master non-specific EOI */
		0xf4				/* hlt */
	};

	/* Real-mode IVT vector 0x20 points to the IRQ0 handler at 0000:0200. */
	memory[0x80] = KVM_PIC_HANDLER_OFFSET & 0xffU;
	memory[0x81] = (KVM_PIC_HANDLER_OFFSET >> 8) & 0xffU;
	memory[0x82] = 0;
	memory[0x83] = 0;
	bcopy(reset_code, memory + KVM_PIC_RESET_OFFSET, sizeof(reset_code));
	bcopy(stub_code, memory + KVM_PIC_STUB_OFFSET, sizeof(stub_code));
	bcopy(handler_code, memory + KVM_PIC_HANDLER_OFFSET,
	    sizeof(handler_code));
}

static void
kvm_pic_set_realmode_entry(int vcpu_fd)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	bzero(&regs, sizeof(regs));
	regs.rip = KVM_PIC_RESET_OFFSET;
	regs.rflags = 0x2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) != 0)
		err(1, "KVM_SET_REGS real-mode entry");
	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS real-mode entry");

	/* Place the reset stub at physical address 0x0000:0xfff0. */
	sregs.cs.base = 0;
	sregs.cs.selector = 0;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS real-mode entry");
}

int
main(void)
{
	struct kvm_userspace_memory_region memory_region;
	struct kvm_irqchip irqchip;
	struct kvm_irq_level irq_line;
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
	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_IRQCHIP) != 1)
		errx(1, "KVM_CAP_IRQCHIP unavailable");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0)
		err(1, "KVM_CREATE_IRQCHIP");

	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_PIC_MASTER;
	irqchip.chip.pic.irq_base = 0x08;
	irqchip.chip.pic.init4 = 1;
	irqchip.chip.pic.imr = 0xff;
	if (ioctl(vm_fd, KVM_SET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_SET_IRQCHIP PIC");
	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_PIC_MASTER;
	if (ioctl(vm_fd, KVM_GET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_GET_IRQCHIP PIC");
	if (irqchip.chip.pic.irq_base != 0x08 || irqchip.chip.pic.init4 != 1 ||
	    irqchip.chip.pic.imr != 0xff)
		errx(1, "KVM PIC state round trip failed");

	guest_memory = mmap(NULL, KVM_PIC_MEMORY_SIZE,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_PIC_MEMORY_SIZE);
	kvm_pic_write_code(guest_memory);
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.memory_size = KVM_PIC_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	kvm_pic_set_realmode_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		errx(1, "unexpected KVM_RUN mapping size %d", run_size);
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;

	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN PIC initialization");
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected HLT after PIC initialization, got %u",
		    run->exit_reason);
	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_PIC_MASTER;
	if (ioctl(vm_fd, KVM_GET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_GET_IRQCHIP programmed PIC");
	if (irqchip.chip.pic.irq_base != 0x20 ||
	    irqchip.chip.pic.imr != 0xfe || irqchip.chip.pic.init4 != 1)
		errx(1, "guest PIC PIO state: base=%#x imr=%#x init=%u",
		    irqchip.chip.pic.irq_base, irqchip.chip.pic.imr,
		    irqchip.chip.pic.init_state);

	irq_line.irq = 0;
	irq_line.level = 1;
	if (ioctl(vm_fd, KVM_IRQ_LINE, &irq_line) != 0)
		err(1, "KVM_IRQ_LINE assert IRQ0");
	irq_line.level = 0;
	if (ioctl(vm_fd, KVM_IRQ_LINE, &irq_line) != 0)
		err(1, "KVM_IRQ_LINE deassert IRQ0");
	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN IRQ0");
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected HLT after IRQ0, got %u", run->exit_reason);
	if (guest_memory[KVM_PIC_RESULT_OFFSET] != 0x7a)
		errx(1, "PIC IRQ0 handler did not run");
	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_PIC_MASTER;
	if (ioctl(vm_fd, KVM_GET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_GET_IRQCHIP after EOI");
	if (irqchip.chip.pic.isr != 0)
		errx(1, "PIC ISR still contains IRQ0 after EOI");

	if (munmap(run_mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 || munmap(guest_memory, KVM_PIC_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm PIC: PASS");
	return 0;
}
