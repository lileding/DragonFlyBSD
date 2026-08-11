/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Smallest real KVM execution test: the x86 reset vector contains HLT.
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

#include "../../sys/sys/kvm.h"

#define KVM_HLT_GPA_BASE 0xffff0000ULL
#define KVM_HLT_MEMORY_SIZE 0x10000ULL
#define KVM_HLT_RESET_OFFSET 0xfff0U

int
main(void)
{
	struct kvm_userspace_memory_region memory_region;
	struct kvm_run *run;
	void *guest_memory;
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
	guest_memory = mmap(NULL, KVM_HLT_MEMORY_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_HLT_MEMORY_SIZE);
	((uint8_t *)guest_memory)[KVM_HLT_RESET_OFFSET] = 0xf4;
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.guest_phys_addr = KVM_HLT_GPA_BASE;
	memory_region.memory_size = KVM_HLT_MEMORY_SIZE;
	memory_region.userspace_addr = (uintptr_t)guest_memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory_region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
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
	if (munmap(run_mapping, run_size) != 0)
		err(1, "munmap KVM_RUN");
	if (close(vcpu_fd) != 0)
		err(1, "close vCPU fd");
	if (close(vm_fd) != 0)
		err(1, "close VM fd");
	if (munmap(guest_memory, KVM_HLT_MEMORY_SIZE) != 0)
		err(1, "munmap guest memory");
	if (close(control_fd) != 0)
		err(1, "close control fd");
	puts("kvm HLT: PASS");
	return 0;
}
