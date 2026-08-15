/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Manual runtime test for KVM VM fd creation and release.
 */
#include <sys/types.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/kvm.h>

int
main(void)
{
	int control_fd;
	int vm_fd;
	int run_size;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size <= 0)
		err(1, "KVM_GET_VCPU_MMAP_SIZE");
	if (close(vm_fd) != 0 || close(control_fd) != 0)
		err(1, "close");
	puts("kvm vm fd: PASS");
	return 0;
}
