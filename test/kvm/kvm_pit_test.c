/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies the KVM PIT2 ABI and that scalar PIT PIO stays in the kernel.
 */
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>

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

int
main(void)
{
	struct kvm_userspace_memory_region memory_region;
	struct kvm_pit_config pit_config;
	struct kvm_pit_state2 pit_state;
	struct kvm_run *run;
	void *guest_memory;
	void *run_mapping;
	uint8_t *code;
	int control_fd;
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
	guest_memory = mmap(NULL, KVM_PIT_MEMORY_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_PIT_MEMORY_SIZE);
	code = guest_memory;
	code[KVM_PIT_RESET_OFFSET + 0] = 0xb0;
	code[KVM_PIT_RESET_OFFSET + 1] = 0x36;
	code[KVM_PIT_RESET_OFFSET + 2] = 0xe6;
	code[KVM_PIT_RESET_OFFSET + 3] = 0x43;
	code[KVM_PIT_RESET_OFFSET + 4] = 0xb0;
	code[KVM_PIT_RESET_OFFSET + 5] = 0x34;
	code[KVM_PIT_RESET_OFFSET + 6] = 0xe6;
	code[KVM_PIT_RESET_OFFSET + 7] = 0x40;
	code[KVM_PIT_RESET_OFFSET + 8] = 0xb0;
	code[KVM_PIT_RESET_OFFSET + 9] = 0x12;
	code[KVM_PIT_RESET_OFFSET + 10] = 0xe6;
	code[KVM_PIT_RESET_OFFSET + 11] = 0x40;
	code[KVM_PIT_RESET_OFFSET + 12] = 0xe4;
	code[KVM_PIT_RESET_OFFSET + 13] = 0x40;
	code[KVM_PIT_RESET_OFFSET + 14] = 0xf4;
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.guest_phys_addr = KVM_PIT_GPA_BASE;
	memory_region.memory_size = KVM_PIT_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		errx(1, "unexpected KVM_RUN mapping size %d", run_size);
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;
	if (ioctl(vcpu_fd, KVM_RUN, 0) != 0)
		err(1, "KVM_RUN");
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "expected KVM_EXIT_HLT, got %u", run->exit_reason);
	bzero(&pit_state, sizeof(pit_state));
	if (ioctl(vm_fd, KVM_GET_PIT2, &pit_state) != 0)
		err(1, "KVM_GET_PIT2");
	if (pit_state.channels[0].count != 0x1234 ||
	    pit_state.channels[0].rw_mode != 3 ||
	    pit_state.channels[0].mode != 3)
		errx(1, "guest PIT programming was not retained");
	if (munmap(run_mapping, run_size) != 0 || close(vcpu_fd) != 0 ||
	    close(vm_fd) != 0 || munmap(guest_memory, KVM_PIT_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm PIT: PASS");
	return 0;
}
