/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * User ABI smoke test for KVM_SET_USER_MEMORY_REGION.
 */
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include <linux/kvm.h>

int
main(void)
{
	struct kvm_userspace_memory_region region;
	long page_size;
	void *memory;
	int kvm_fd;
	int vm_fd;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		err(1, "sysconf");
	memory = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (memory == MAP_FAILED)
		err(1, "mmap");
	kvm_fd = open("/dev/kvm", O_RDWR);
	if (kvm_fd < 0)
		err(1, "open /dev/kvm");
	if (ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_READONLY_MEM) != 1)
		err(1, "KVM_CAP_READONLY_MEM");
	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	if (ioctl(vm_fd, KVM_CHECK_EXTENSION, KVM_CAP_READONLY_MEM) != 1)
		err(1, "VM KVM_CAP_READONLY_MEM");
	bzero(&region, sizeof(region));
	region.memory_size = page_size;
	region.userspace_addr = (uintptr_t)memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION map");
	bzero(&region, sizeof(region));
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION unmap");
	bzero(&region, sizeof(region));
	region.memory_size = page_size;
	region.userspace_addr = (uintptr_t)memory;
	region.flags = KVM_MEM_READONLY;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION readonly map");
	bzero(&region, sizeof(region));
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION readonly unmap");
	if (close(vm_fd) != 0)
		err(1, "close vm fd");
	if (close(kvm_fd) != 0)
		err(1, "close kvm fd");
	if (munmap(memory, page_size) != 0)
		err(1, "munmap");
	return 0;
}
