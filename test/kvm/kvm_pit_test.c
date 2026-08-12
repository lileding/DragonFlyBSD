/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies the KVM PIT2 ABI, scalar PIT PIO, and one-shot IRQ0 delivery.
 */
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include <linux/kvm.h>

#define KVM_PIT_GPA_BASE	0xffff0000ULL
#define KVM_PIT_MEMORY_SIZE	0x10000ULL
#define KVM_PIT_RESET_OFFSET	0xfff0U
#define KVM_PIT_LOW_MEMORY_SIZE	0x10000U
#define KVM_PIT_STUB_OFFSET	0x1000U
#define KVM_PIT_HANDLER_OFFSET	0x0200U
#define KVM_PIT_RESULT_OFFSET	0x0300U

static void
kvm_pit_write_irq0_handler(uint8_t *memory)
{
	static const uint8_t stub_code[] = {
		0xfa,					/* cli */
		0xb0, 0x11, 0xe6, 0x20,	/* ICW1: master initialization */
		0xb0, 0x20, 0xe6, 0x21,	/* ICW2: vector base */
		0xb0, 0x04, 0xe6, 0x21,	/* ICW3: slave on IRQ2 */
		0xb0, 0x01, 0xe6, 0x21,	/* ICW4: 8086 mode */
		0xb0, 0xfe, 0xe6, 0x21,	/* unmask IRQ0 */
		0xb0, 0x30, 0xe6, 0x43,	/* PIT ch0: lo/hi, mode 0 */
		0xb0, 0xff, 0xe6, 0x40,	/* PIT count low */
		0xb0, 0xff, 0xe6, 0x40,	/* PIT count high: about 55ms */
		0xfb,					/* sti */
		0xf4,					/* hlt */
		0xeb, 0xfd				/* jmp hlt */
	};
	static const uint8_t handler_code[] = {
		0xc6, 0x06, 0x00, 0x03, 0x7a,	/* movb $0x7a, 0x300 */
		0xb0, 0x20, 0xe6, 0x20,		/* master non-specific EOI */
		0xf4					/* hlt */
	};

	/* Real-mode IVT vector 0x20 points to the IRQ0 handler at 0000:0200. */
	memory[0x80] = KVM_PIT_HANDLER_OFFSET & 0xffU;
	memory[0x81] = (KVM_PIT_HANDLER_OFFSET >> 8) & 0xffU;
	memory[0x82] = 0;
	memory[0x83] = 0;
	bcopy(stub_code, memory + KVM_PIT_STUB_OFFSET, sizeof(stub_code));
	bcopy(handler_code, memory + KVM_PIT_HANDLER_OFFSET,
	    sizeof(handler_code));
}

int
main(void)
{
	struct kvm_userspace_memory_region memory_region;
	struct kvm_irqchip irqchip;
	struct kvm_pit_config pit_config;
	struct kvm_pit_state2 pit_state;
	struct kvm_run *run;
	uint8_t *guest_low_memory;
	void *guest_memory;
	void *run_mapping;
	uint8_t *code;
	int control_fd;
	int interrupted;
	int vm_fd;
	int vcpu_fd;
	int run_size;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_PIT2) != 1 ||
	    ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_PIT_STATE2) != 1)
		errx(1, "KVM PIT2 capabilities unavailable");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0)
		err(1, "KVM_CREATE_IRQCHIP");
	/* QEMU creates idle vCPUs before its in-kernel PIT. */
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	bzero(&pit_config, sizeof(pit_config));
	if (ioctl(vm_fd, KVM_CREATE_PIT2, &pit_config) != 0)
		err(1, "KVM_CREATE_PIT2");
	bzero(&pit_state, sizeof(pit_state));
	pit_state.channels[0].count = 0x3456;
	pit_state.channels[0].rw_mode = 3;
	pit_state.channels[0].mode = 3;
	pit_state.channels[0].gate = 1;
	if (ioctl(vm_fd, KVM_SET_PIT2, &pit_state) != 0)
		err(1, "KVM_SET_PIT2");
	bzero(&pit_state, sizeof(pit_state));
	if (ioctl(vm_fd, KVM_GET_PIT2, &pit_state) != 0)
		err(1, "KVM_GET_PIT2 after set");
	if (pit_state.channels[0].count != 0x3456 ||
	    pit_state.channels[0].rw_mode != 3 ||
	    pit_state.channels[0].mode != 3 ||
	    pit_state.channels[0].gate != 1)
		errx(1, "KVM PIT2 state round trip failed");
	/* Keep channel 0 disarmed until the guest writes its full count. */
	pit_state.channels[0].count = 0;
	pit_state.channels[0].count_load_time = 0;
	if (ioctl(vm_fd, KVM_SET_PIT2, &pit_state) != 0)
		err(1, "KVM_SET_PIT2 disarm");
	guest_memory = mmap(NULL, KVM_PIT_MEMORY_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_PIT_MEMORY_SIZE);
	guest_low_memory = mmap(NULL, KVM_PIT_LOW_MEMORY_SIZE,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_low_memory == MAP_FAILED)
		err(1, "mmap low guest memory");
	bzero(guest_low_memory, KVM_PIT_LOW_MEMORY_SIZE);
	kvm_pit_write_irq0_handler(guest_low_memory);
	code = guest_memory;
	static const uint8_t reset_code[] = {
		0xea, 0x00, 0x10, 0x00, 0x00	/* ljmp $0x0000,$0x1000 */
	};
	bcopy(reset_code, code + KVM_PIT_RESET_OFFSET, sizeof(reset_code));
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.guest_phys_addr = KVM_PIT_GPA_BASE;
	memory_region.memory_size = KVM_PIT_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 1;
	memory_region.memory_size = KVM_PIT_LOW_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_low_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION low memory");
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		errx(1, "unexpected KVM_RUN mapping size %d", run_size);
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;
	interrupted = 0;
	while (ioctl(vcpu_fd, KVM_RUN, 0) != 0) {
		if (errno != EINTR || interrupted++ == 10000)
			err(1, "KVM_RUN");
	}
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected KVM_EXIT_HLT, got %u", run->exit_reason);
	bzero(&pit_state, sizeof(pit_state));
	if (ioctl(vm_fd, KVM_GET_PIT2, &pit_state) != 0)
		err(1, "KVM_GET_PIT2");
	if (pit_state.channels[0].count != 0xffff ||
	    pit_state.channels[0].rw_mode != 3 ||
	    pit_state.channels[0].mode != 0)
		errx(1, "guest PIT programming was not retained");
	/* Let the guest-programmed one-shot expire before entering HLT again. */
	usleep(100000);
	interrupted = 0;
	while (ioctl(vcpu_fd, KVM_RUN, 0) != 0) {
		if (errno != EINTR || interrupted++ == 10000)
			err(1, "KVM_RUN PIT IRQ0");
	}
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected HLT after PIT IRQ0, got %u", run->exit_reason);
	if (guest_low_memory[KVM_PIT_RESULT_OFFSET] != 0x7a)
		errx(1, "PIT IRQ0 handler did not run");
	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_PIC_MASTER;
	if (ioctl(vm_fd, KVM_GET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_GET_IRQCHIP after PIT EOI");
	if (irqchip.chip.pic.isr != 0)
		errx(1, "PIC ISR still contains IRQ0 after PIT EOI");
	if (munmap(run_mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 || munmap(guest_memory, KVM_PIT_MEMORY_SIZE) != 0 ||
	    munmap(guest_low_memory, KVM_PIT_LOW_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm PIT: PASS");
	return 0;
}
