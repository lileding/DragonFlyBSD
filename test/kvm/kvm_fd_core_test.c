/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Regression test for KVM functional file descriptors.  A child keeps a VM,
 * vCPU KVM_RUN mapping, and eventfd alive while it generates a core dump.
 * These descriptors must all expose the real /dev/kvm vnode to the VFS core.
 * This test never enters VMRUN.
 */
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/kvm.h>

static void
kvm_fd_core_child(void)
{
	struct kvm_dfly_eventfd eventfd;
	void *mapping;
	int control_fd;
	int run_size;
	int vcpu_fd;
	int vm_fd;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size <= 0)
		errx(1, "KVM_GET_VCPU_MMAP_SIZE");
	mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	eventfd.initial = 0;
	eventfd.flags = KVM_DFLY_EVENTFD_CLOEXEC;
	eventfd.fd = -1;
	if (ioctl(control_fd, KVM_DFLY_CREATE_EVENTFD, &eventfd) != 0)
		err(1, "KVM_DFLY_CREATE_EVENTFD");

	/* The parent expects SIGABRT after the kernel has written the core. */
	abort();
}

int
main(void)
{
	int status;
	pid_t child;

	child = fork();
	if (child < 0)
		err(1, "fork");
	if (child == 0)
		kvm_fd_core_child();
	if (waitpid(child, &status, 0) != child)
		err(1, "waitpid");
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT)
		errx(1, "child did not terminate with SIGABRT");
	puts("kvm functional fd core: PASS");
	return 0;
}
