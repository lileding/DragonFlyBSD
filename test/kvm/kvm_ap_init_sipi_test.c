/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies that a secondary vCPU waits for SIPI, then runs a real-mode
 * bootstrap vector after the BSP issues INIT followed by SIPI through ICR.
 */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include <sys/kvm.h>

#define MEMORY_SIZE     0x8000U
#define BSP_CODE        0x1000U
#define AP_VECTOR       0x02U
#define AP_CODE         ((uint32_t)AP_VECTOR << 12)
#define RESULT          0x0300U
#define GDT             0x4000U

struct ap_thread {
	int fd;
	struct kvm_run *run;
	int error;
};

static void
set_segment(struct kvm_segment *segment, uint16_t selector, uint8_t type)
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
set_bsp_state(int fd)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	if (ioctl(fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	set_segment(&sregs.cs, 0x08, 0x0b);
	set_segment(&sregs.ds, 0x10, 0x03);
	set_segment(&sregs.es, 0x10, 0x03);
	set_segment(&sregs.fs, 0x10, 0x03);
	set_segment(&sregs.gs, 0x10, 0x03);
	set_segment(&sregs.ss, 0x10, 0x03);
	sregs.cr0 = 0x11;
	sregs.gdt.base = GDT;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	if (ioctl(fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&regs, sizeof(regs));
	regs.rip = BSP_CODE;
	regs.rsp = 0x2ff0;
	regs.rflags = 0x2;
	if (ioctl(fd, KVM_SET_REGS, &regs) != 0)
		err(1, "KVM_SET_REGS");
}

static void *
run_ap(void *argument)
{
	struct ap_thread *ap = argument;
	unsigned int interrupted;

	interrupted = 0;
	while (ioctl(ap->fd, KVM_RUN, 0) != 0) {
		if (errno == EINTR && interrupted++ != 10000) {
			usleep(100);
			continue;
		}
		ap->error = errno;
		return NULL;
	}
	if (ap->run->exit_reason != KVM_EXIT_HLT)
		ap->error = EPROTO;
	return NULL;
}

static void
run_bsp_hlt(int fd, struct kvm_run *run)
{
	unsigned int interrupted;

	interrupted = 0;
	while (ioctl(fd, KVM_RUN, 0) != 0) {
		if (errno == EINTR && interrupted++ != 10000) {
			usleep(100);
			continue;
		}
		err(1, "KVM_RUN BSP");
	}
	if (run->exit_reason != KVM_EXIT_HLT)
		errx(1, "BSP exit %u", run->exit_reason);
}

int
main(void)
{
	static const uint8_t bsp_code[] = {
		0xbf, 0x10, 0x03, 0xe0, 0xfe,
		0xb8, 0x00, 0x00, 0x00, 0x01,
		0x89, 0x07,
		0xbf, 0x00, 0x03, 0xe0, 0xfe,
		0xb8, 0x00, 0x05, 0x00, 0x00,
		0x89, 0x07,
		0xb8, 0x02, 0x06, 0x00, 0x00,
		0x89, 0x07,
		0xfb, 0xf4
	};
	static const uint8_t ap_code[] = {
		0xc6, 0x06, 0x00, 0x03, 0x7a, 0xf4
	};
	struct kvm_userspace_memory_region region;
	struct kvm_lapic_state lapic;
	struct ap_thread ap;
	struct kvm_run *bsp_run;
	uint8_t *memory;
	void *bsp_mapping;
	void *ap_mapping;
	uint64_t *gdt;
	pthread_t thread;
	int control_fd;
	int vm_fd;
	int bsp_fd;
	int ap_fd;
	int run_size;
	uint32_t svr;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0)
		err(1, "KVM_CREATE_IRQCHIP");
	memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE, -1, 0);
	if (memory == MAP_FAILED)
		err(1, "mmap memory");
	bzero(memory, MEMORY_SIZE);
	bcopy(bsp_code, memory + BSP_CODE, sizeof(bsp_code));
	bcopy(ap_code, memory + AP_CODE, sizeof(ap_code));
	gdt = (uint64_t *)(memory + GDT);
	gdt[1] = 0x00cf9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	bzero(&region, sizeof(region));
	region.slot = 0;
	region.memory_size = MEMORY_SIZE;
	region.userspace_addr = (uintptr_t)memory;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) != 0)
		err(1, "KVM_SET_USER_MEMORY_REGION");
	bsp_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	ap_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 1);
	if (bsp_fd < 0 || ap_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	bzero(&lapic, sizeof(lapic));
	if (ioctl(bsp_fd, KVM_GET_LAPIC, &lapic) != 0)
		err(1, "KVM_GET_LAPIC");
	svr = 0x1ffU;
	bcopy(&svr, lapic.regs + 0x0f0, sizeof(svr));
	if (ioctl(bsp_fd, KVM_SET_LAPIC, &lapic) != 0)
		err(1, "KVM_SET_LAPIC");
	set_bsp_state(bsp_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		errx(1, "KVM_GET_VCPU_MMAP_SIZE");
	bsp_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		bsp_fd, 0);
	ap_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		ap_fd, 0);
	if (bsp_mapping == MAP_FAILED || ap_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	bzero(&ap, sizeof(ap));
	ap.fd = ap_fd;
	ap.run = ap_mapping;
	if (pthread_create(&thread, NULL, run_ap, &ap) != 0)
		err(1, "pthread_create");
	usleep(10000);
	bsp_run = bsp_mapping;
	run_bsp_hlt(bsp_fd, bsp_run);
	if (pthread_join(thread, NULL) != 0)
		err(1, "pthread_join");
	if (ap.error != 0)
		errno = ap.error, err(1, "KVM_RUN AP");
	if (memory[RESULT] != 0x7a)
		errx(1, "AP did not execute SIPI vector");
	if (munmap(bsp_mapping, run_size) != 0 ||
	    munmap(ap_mapping, run_size) != 0 || close(bsp_fd) != 0 ||
	    close(ap_fd) != 0 || close(vm_fd) != 0 ||
	    munmap(memory, MEMORY_SIZE) != 0 || close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm AP INIT/SIPI: PASS");
	return 0;
}
