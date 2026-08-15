/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verifies end-to-end irqfd MSI delivery to a running guest vCPU.
 */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include <sys/kvm.h>

#define KVM_IRQFD_MEMORY_SIZE		0x8000U
#define KVM_IRQFD_CODE_OFFSET		0x1000U
#define KVM_IRQFD_HANDLER_OFFSET	0x0200U
#define KVM_IRQFD_RESULT_OFFSET		0x0300U
#define KVM_IRQFD_GATE_OFFSET		0x0301U
#define KVM_IRQFD_GDT_OFFSET		0x4000U
#define KVM_IRQFD_IDT_OFFSET		0x5000U
#define KVM_IRQFD_SVR			0x0f0U
#define KVM_IRQFD_GSI			24U
#define KVM_IRQFD_VECTOR		0x40U

struct kvm_irqfd_run_task {
	int vcpu_fd;
	int error;
};

static void
kvm_irqfd_set_segment(struct kvm_segment *segment, uint16_t selector,
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
kvm_irqfd_set_protected_entry(int vcpu_fd)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		err(1, "KVM_GET_SREGS");
	kvm_irqfd_set_segment(&sregs.cs, 0x08, 0x0b);
	kvm_irqfd_set_segment(&sregs.ds, 0x10, 0x03);
	kvm_irqfd_set_segment(&sregs.es, 0x10, 0x03);
	kvm_irqfd_set_segment(&sregs.fs, 0x10, 0x03);
	kvm_irqfd_set_segment(&sregs.gs, 0x10, 0x03);
	kvm_irqfd_set_segment(&sregs.ss, 0x10, 0x03);
	sregs.cr0 = 0x11;
	sregs.cr4 = 0;
	sregs.efer = 0;
	sregs.gdt.base = KVM_IRQFD_GDT_OFFSET;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
	sregs.idt.base = KVM_IRQFD_IDT_OFFSET;
	sregs.idt.limit = 256 * sizeof(uint64_t) - 1;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		err(1, "KVM_SET_SREGS");
	bzero(&regs, sizeof(regs));
	regs.rip = KVM_IRQFD_CODE_OFFSET;
	regs.rsp = 0x2ff0;
	regs.rflags = 0x202;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) != 0)
		err(1, "KVM_SET_REGS");
}

static void
kvm_irqfd_write_code(uint8_t *memory)
{
	uint64_t *gdt;
	uint64_t *idt;
	static const uint8_t idle_code[] = {
		0xc6, 0x05, 0x01, 0x03, 0x00, 0x00, 0x01,
						/* movb $1,0x301 */
		0xfb,					/* sti */
		0xeb, 0xfe				/* jmp . */
	};
	static const uint8_t handler_code[] = {
		0xc6, 0x05, 0x00, 0x03, 0x00, 0x00, 0x7a,
						/* movb $0x7a,0x300 */
		0xb8, 0x00, 0x00, 0x00, 0x00,	/* mov $0,%eax */
		0xbf, 0xb0, 0x00, 0xe0, 0xfe,	/* mov $0xfee000b0,%edi */
		0x89, 0x07,				/* mov %eax,(%edi) */
		0xf4					/* hlt */
	};

	gdt = (uint64_t *)(memory + KVM_IRQFD_GDT_OFFSET);
	gdt[1] = 0x00cf9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	idt = (uint64_t *)(memory + KVM_IRQFD_IDT_OFFSET);
	idt[KVM_IRQFD_VECTOR] = KVM_IRQFD_HANDLER_OFFSET |
	    ((uint64_t)0x08 << 16) | ((uint64_t)0x8e << 40);
	bcopy(idle_code, memory + KVM_IRQFD_CODE_OFFSET, sizeof(idle_code));
	bcopy(handler_code, memory + KVM_IRQFD_HANDLER_OFFSET,
	    sizeof(handler_code));
}

static void *
kvm_irqfd_run_thread(void *argument)
{
	struct kvm_irqfd_run_task *task;
	unsigned int interrupted;

	task = argument;
	interrupted = 0;
	while (ioctl(task->vcpu_fd, KVM_RUN, 0) != 0) {
		if (errno == EINTR && interrupted++ != 10000) {
			usleep(100);
			continue;
		}
		task->error = errno;
		return NULL;
	}
	return NULL;
}

static int
kvm_irqfd_wait_for_value(volatile uint8_t *value, uint8_t expected)
{
	unsigned int attempt;

	for (attempt = 0; attempt < 10000; ++attempt) {
		if (*value == expected)
			return 0;
		usleep(100);
	}
	return ETIMEDOUT;
}

int
main(void)
{
	struct {
		struct kvm_irq_routing routing;
		struct kvm_irq_routing_entry entries[1];
	} routes;
	struct kvm_dfly_buffer route_buffer;
	struct kvm_dfly_eventfd eventfd;
	struct kvm_irqfd irqfd;
	struct kvm_lapic_state lapic;
	struct kvm_msi recovery_msi;
	struct kvm_run *run;
	struct kvm_userspace_memory_region memory_region;
	struct kvm_irqfd_run_task task;
	pthread_t thread;
	uint8_t *guest_memory;
	volatile uint8_t *gate;
	volatile uint8_t *result;
	void *run_mapping;
	uint64_t event_value;
	uint32_t value;
	int control_fd;
	int error;
	int vm_fd;
	int vcpu_fd;
	int run_size;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_IRQCHIP) != 1)
		err(1, "KVM_CAP_IRQCHIP unavailable");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0)
		err(1, "KVM_CREATE_IRQCHIP");
	guest_memory = mmap(NULL, KVM_IRQFD_MEMORY_SIZE,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (guest_memory == MAP_FAILED)
		err(1, "mmap guest memory");
	bzero(guest_memory, KVM_IRQFD_MEMORY_SIZE);
	gate = guest_memory + KVM_IRQFD_GATE_OFFSET;
	result = guest_memory + KVM_IRQFD_RESULT_OFFSET;
	kvm_irqfd_write_code(guest_memory);
	bzero(&memory_region, sizeof(memory_region));
	memory_region.slot = 0;
	memory_region.memory_size = KVM_IRQFD_MEMORY_SIZE;
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
	bcopy(&value, lapic.regs + KVM_IRQFD_SVR, sizeof(value));
	if (ioctl(vcpu_fd, KVM_SET_LAPIC, &lapic) != 0)
		err(1, "KVM_SET_LAPIC");
	kvm_irqfd_set_protected_entry(vcpu_fd);
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		err(1, "KVM_GET_VCPU_MMAP_SIZE");
	run_mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (run_mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	run = run_mapping;

	bzero(&routes, sizeof(routes));
	routes.routing.nr = 1;
	routes.entries[0].gsi = KVM_IRQFD_GSI;
	routes.entries[0].type = KVM_IRQ_ROUTING_MSI;
	routes.entries[0].u.msi.address_lo = 0xfee00000U;
	routes.entries[0].u.msi.data = KVM_IRQFD_VECTOR;
	bzero(&route_buffer, sizeof(route_buffer));
	route_buffer.data = (uintptr_t)&routes;
	route_buffer.length = sizeof(routes);
	if (ioctl(vm_fd, KVM_DFLY_SET_GSI_ROUTING, &route_buffer) != 0)
		err(1, "KVM_DFLY_SET_GSI_ROUTING");
	bzero(&eventfd, sizeof(eventfd));
	eventfd.flags = KVM_DFLY_EVENTFD_CLOEXEC;
	if (ioctl(control_fd, KVM_DFLY_CREATE_EVENTFD, &eventfd) != 0)
		err(1, "KVM_DFLY_CREATE_EVENTFD");
	bzero(&irqfd, sizeof(irqfd));
	irqfd.fd = eventfd.fd;
	irqfd.gsi = KVM_IRQFD_GSI;
	if (ioctl(vm_fd, KVM_IRQFD, &irqfd) != 0)
		err(1, "KVM_IRQFD");

	bzero(&task, sizeof(task));
	task.vcpu_fd = vcpu_fd;
	error = pthread_create(&thread, NULL, kvm_irqfd_run_thread, &task);
	if (error != 0)
		errc(1, error, "pthread_create");
	if (kvm_irqfd_wait_for_value(gate, 1) != 0)
		err(1, "guest did not enter interruptible loop");
	event_value = 1;
	if (write(eventfd.fd, &event_value, sizeof(event_value)) !=
	    sizeof(event_value))
		err(1, "write irqfd");
	if (kvm_irqfd_wait_for_value(result, 0x7a) != 0) {
		bzero(&recovery_msi, sizeof(recovery_msi));
		recovery_msi.address_lo = 0xfee00000U;
		recovery_msi.data = KVM_IRQFD_VECTOR;
		if (ioctl(vm_fd, KVM_SIGNAL_MSI, &recovery_msi) != 0)
			err(1, "KVM_SIGNAL_MSI recovery");
		if (pthread_join(thread, NULL) != 0)
			err(1, "pthread_join recovery");
		err(1, "irqfd MSI handler did not run");
	}
	if (pthread_join(thread, NULL) != 0)
		err(1, "pthread_join");
	if (task.error != 0)
		errc(1, task.error, "KVM_RUN irqfd");
	if (run->exit_reason != KVM_EXIT_HLT)
		err(1, "expected HLT after irqfd MSI, got %u", run->exit_reason);
	irqfd.flags = KVM_IRQFD_FLAG_DEASSIGN;
	if (ioctl(vm_fd, KVM_IRQFD, &irqfd) != 0)
		err(1, "KVM_IRQFD deassign");
	if (munmap(run_mapping, run_size) != 0 || close(eventfd.fd) != 0 ||
	    close(vcpu_fd) != 0 || close(vm_fd) != 0 ||
	    munmap(guest_memory, KVM_IRQFD_MEMORY_SIZE) != 0 ||
	    close(control_fd) != 0)
		err(1, "cleanup");
	puts("kvm irqfd: PASS");
	return 0;
}
