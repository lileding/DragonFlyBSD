/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Runtime ABI smoke test for a KVM vCPU file descriptor.
 * This test never enters VMRUN: immediate_exit exercises the KVM_RUN path.
 */
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include <linux/kvm.h>

#define KVM_TEST_CPUID_ENTRIES	256
#define KVM_TEST_LAPIC_VERSION	0x030U
#define KVM_TEST_LAPIC_TPR		0x080U
#define KVM_TEST_LAPIC_PPR		0x0a0U
#define KVM_TEST_LAPIC_LDR		0x0d0U
#define KVM_TEST_LAPIC_DFR		0x0e0U
#define KVM_TEST_LAPIC_SVR		0x0f0U
#define KVM_TEST_LAPIC_LVT_TIMER	0x320U
#define KVM_TEST_LAPIC_LVT_THERMAL	0x330U
#define KVM_TEST_LAPIC_LVT_PERF	0x340U
#define KVM_TEST_LAPIC_LVT0		0x350U
#define KVM_TEST_LAPIC_LVT1		0x360U
#define KVM_TEST_LAPIC_LVT_ERROR	0x370U
#define KVM_TEST_LAPIC_LVT_MASKED	0x00010000U
#define KVM_TEST_LAPIC_LVT_EXTINT	0x00000700U

#define KVM_TEST_STEP(message) do { \
	puts(message); \
	fflush(stdout); \
} while (0)

static void
kvm_test_cpuid(int control_fd, int vcpu_fd)
{
	struct kvm_dfly_buffer request;
	struct kvm_cpuid2 *cpuid;
	size_t length;

	length = sizeof(*cpuid) + KVM_TEST_CPUID_ENTRIES *
	    sizeof(cpuid->entries[0]);
	cpuid = calloc(1, length);
	if (cpuid == NULL)
		err(1, "calloc cpuid");
	bzero(&request, sizeof(request));
	request.data = (uintptr_t)cpuid;
	request.length = length;
	if (ioctl(control_fd, KVM_DFLY_GET_SUPPORTED_CPUID, &request) != 0)
		err(1, "KVM_DFLY_GET_SUPPORTED_CPUID");
	if (cpuid->nent == 0 || request.length > length)
		errx(1, "invalid supported CPUID response");
	request.length = sizeof(*cpuid) + cpuid->nent * sizeof(cpuid->entries[0]);
	if (ioctl(vcpu_fd, KVM_DFLY_SET_CPUID2, &request) != 0)
		err(1, "KVM_DFLY_SET_CPUID2");
	free(cpuid);
}

static void
kvm_test_msrs(int vcpu_fd)
{
	struct {
		struct kvm_msrs msrs;
		struct kvm_msr_entry entries[2];
	} data;
	struct kvm_dfly_buffer request;

	bzero(&data, sizeof(data));
	data.msrs.nmsrs = 2;
	data.entries[0].index = 0x00000010U;
	data.entries[0].data = 0;
	data.entries[1].index = 0x00000277U;
	data.entries[1].data = 0x0007040600070406ULL;
	bzero(&request, sizeof(request));
	request.data = (uintptr_t)&data;
	request.length = sizeof(data);
	if (ioctl(vcpu_fd, KVM_DFLY_SET_MSRS, &request) != 2)
		err(1, "KVM_DFLY_SET_MSRS");
	data.entries[0].data = 0;
	data.entries[1].data = 0;
	if (ioctl(vcpu_fd, KVM_DFLY_GET_MSRS, &request) != 2)
		err(1, "KVM_DFLY_GET_MSRS");
	if (data.entries[0].data != 0 ||
	    data.entries[1].data != 0x0007040600070406ULL)
		errx(1, "KVM MSR round trip mismatch");
}

static void
kvm_test_msr_index_list(int control_fd)
{
	struct kvm_dfly_buffer request;
	struct kvm_msr_list *list;
	size_t length;
	uint32_t count;

	bzero(&request, sizeof(request));
	list = calloc(1, sizeof(*list));
	if (list == NULL)
		err(1, "calloc MSR list header");
	request.data = (uintptr_t)list;
	request.length = sizeof(*list);
	errno = 0;
	if (ioctl(control_fd, KVM_DFLY_GET_MSR_INDEX_LIST, &request) != -1 ||
	    errno != E2BIG || list->nmsrs == 0)
		errx(1, "KVM_DFLY_GET_MSR_INDEX_LIST size query");
	count = list->nmsrs;
	free(list);
	length = sizeof(*list) + count * sizeof(list->indices[0]);
	list = calloc(1, length);
	if (list == NULL)
		err(1, "calloc MSR list");
	request.data = (uintptr_t)list;
	request.length = length;
	if (ioctl(control_fd, KVM_DFLY_GET_MSR_INDEX_LIST, &request) != 0)
		err(1, "KVM_DFLY_GET_MSR_INDEX_LIST");
	if (list->nmsrs == 0 || request.length != length)
		errx(1, "invalid MSR list response");
	free(list);
}

static void
kvm_test_msr_feature_index_list(int control_fd)
{
	struct kvm_dfly_buffer request;
	struct kvm_msr_list list;

	bzero(&list, sizeof(list));
	bzero(&request, sizeof(request));
	request.data = (uintptr_t)&list;
	request.length = sizeof(list);
	if (ioctl(control_fd, KVM_DFLY_GET_MSR_FEATURE_INDEX_LIST,
	    &request) != 0)
		err(1, "KVM_DFLY_GET_MSR_FEATURE_INDEX_LIST");
	if (list.nmsrs != 0 || request.length != sizeof(list))
		errx(1, "invalid KVM feature MSR list response");
}

static void
kvm_test_xcrs(int control_fd, int vcpu_fd)
{
	struct kvm_xcrs xcrs;

	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_XCRS) != 1)
		errx(1, "KVM_CAP_XCRS not available");
	bzero(&xcrs, sizeof(xcrs));
	xcrs.nr_xcrs = 1;
	xcrs.xcrs[0].xcr = 0;
	xcrs.xcrs[0].value = 1;
	if (ioctl(vcpu_fd, KVM_SET_XCRS, &xcrs) != 0)
		err(1, "KVM_SET_XCRS");
	bzero(&xcrs, sizeof(xcrs));
	if (ioctl(vcpu_fd, KVM_GET_XCRS, &xcrs) != 0)
		err(1, "KVM_GET_XCRS");
	if (xcrs.nr_xcrs != 1 || xcrs.xcrs[0].xcr != 0 ||
	    xcrs.xcrs[0].value != 1)
		errx(1, "KVM XCR round trip mismatch");
}

static void
kvm_test_mp_state(int vcpu_fd)
{
	struct kvm_mp_state state;

	state.mp_state = KVM_MP_STATE_RUNNABLE;
	if (ioctl(vcpu_fd, KVM_SET_MP_STATE, &state) != 0)
		err(1, "KVM_SET_MP_STATE");
	state.mp_state = UINT32_MAX;
	if (ioctl(vcpu_fd, KVM_GET_MP_STATE, &state) != 0)
		err(1, "KVM_GET_MP_STATE");
	if (state.mp_state != KVM_MP_STATE_RUNNABLE)
		errx(1, "unexpected KVM MP state");
}

static void
kvm_test_lapic_reset(int vcpu_fd)
{
	static const size_t lvt_registers[] = {
		KVM_TEST_LAPIC_LVT_TIMER,
		KVM_TEST_LAPIC_LVT_THERMAL,
		KVM_TEST_LAPIC_LVT_PERF,
		KVM_TEST_LAPIC_LVT0,
		KVM_TEST_LAPIC_LVT1,
		KVM_TEST_LAPIC_LVT_ERROR,
	};
	struct kvm_lapic_state state;
	size_t index;
	uint32_t value;

	bzero(&state, sizeof(state));
	if (ioctl(vcpu_fd, KVM_GET_LAPIC, &state) != 0)
		err(1, "KVM_GET_LAPIC reset state");
	bcopy(state.regs + KVM_TEST_LAPIC_VERSION, &value, sizeof(value));
	if (value != 0x00140014U)
		errx(1, "KVM LAPIC version %#x", value);
	bcopy(state.regs + KVM_TEST_LAPIC_TPR, &value, sizeof(value));
	if (value != 0)
		errx(1, "KVM LAPIC reset TPR %#x", value);
	bcopy(state.regs + KVM_TEST_LAPIC_PPR, &value, sizeof(value));
	if (value != 0)
		errx(1, "KVM LAPIC reset PPR %#x", value);
	bcopy(state.regs + KVM_TEST_LAPIC_LDR, &value, sizeof(value));
	if (value != 0)
		errx(1, "KVM LAPIC reset LDR %#x", value);
	bcopy(state.regs + KVM_TEST_LAPIC_DFR, &value, sizeof(value));
	if (value != UINT32_MAX)
		errx(1, "KVM LAPIC reset DFR %#x", value);
	bcopy(state.regs + KVM_TEST_LAPIC_SVR, &value, sizeof(value));
	if (value != 0xffU)
		errx(1, "KVM LAPIC reset SVR %#x", value);
	for (index = 0; index < sizeof(lvt_registers) / sizeof(lvt_registers[0]);
	    ++index) {
		bcopy(state.regs + lvt_registers[index], &value, sizeof(value));
		if (value != (lvt_registers[index] == KVM_TEST_LAPIC_LVT0 ?
		    KVM_TEST_LAPIC_LVT_EXTINT : KVM_TEST_LAPIC_LVT_MASKED))
			errx(1, "KVM LAPIC reset LVT %#zx is %#x",
			    lvt_registers[index], value);
	}
}

static void
kvm_test_lapic(int vcpu_fd)
{
	struct kvm_lapic_state state;
	struct kvm_vapic_addr vapic;

	bzero(&state, sizeof(state));
	state.regs[0x80] = 0x20;
	if (ioctl(vcpu_fd, KVM_SET_LAPIC, &state) != 0)
		err(1, "KVM_SET_LAPIC");
	bzero(&state, sizeof(state));
	if (ioctl(vcpu_fd, KVM_GET_LAPIC, &state) != 0)
		err(1, "KVM_GET_LAPIC");
	if (state.regs[0x80] != 0x20)
		errx(1, "KVM LAPIC TPR round trip mismatch");
	bzero(&vapic, sizeof(vapic));
	vapic.vapic_addr = 0x1000;
	if (ioctl(vcpu_fd, KVM_SET_VAPIC_ADDR, &vapic) != 0)
		err(1, "KVM_SET_VAPIC_ADDR");
	vapic.vapic_addr = 1;
	errno = 0;
	if (ioctl(vcpu_fd, KVM_SET_VAPIC_ADDR, &vapic) != -1 ||
	    errno != EINVAL)
		errx(1, "KVM_SET_VAPIC_ADDR alignment check");
}

static void
kvm_test_clock(int vm_fd)
{
	struct kvm_clock_data clock;
	uint64_t requested;

	bzero(&clock, sizeof(clock));
	if (ioctl(vm_fd, KVM_GET_CLOCK, &clock) != 0)
		err(1, "KVM_GET_CLOCK initial");
	requested = clock.clock + 1000000000ULL;
	clock.clock = requested;
	clock.flags = KVM_CLOCK_REALTIME;
	if (ioctl(vm_fd, KVM_SET_CLOCK, &clock) != 0)
		err(1, "KVM_SET_CLOCK");
	bzero(&clock, sizeof(clock));
	if (ioctl(vm_fd, KVM_GET_CLOCK, &clock) != 0)
		err(1, "KVM_GET_CLOCK updated");
	if (clock.clock < requested ||
	    (clock.flags & KVM_CLOCK_REALTIME) == 0)
		errx(1, "KVM clock round trip mismatch");
}

static void
kvm_test_irqchip(int vm_fd)
{
	struct kvm_irqchip irqchip;
	struct kvm_irq_level line;
	uint32_t pin;

	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_PIC_MASTER;
	irqchip.chip.pic.last_irr = 0x5a;
	irqchip.chip.pic.irq_base = 0x08;
	irqchip.chip.pic.init4 = 1;
	if (ioctl(vm_fd, KVM_SET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_SET_IRQCHIP PIC");
	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_PIC_MASTER;
	if (ioctl(vm_fd, KVM_GET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_GET_IRQCHIP PIC");
	if (irqchip.chip.pic.last_irr != 0x5a ||
	    irqchip.chip.pic.irq_base != 0x08 || irqchip.chip.pic.init4 != 1)
		errx(1, "KVM PIC round trip mismatch");

	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_IOAPIC;
	irqchip.chip.ioapic.base_address = 0xfec00000ULL;
	irqchip.chip.ioapic.ioregsel = 0x11;
	irqchip.chip.ioapic.id = 1;
	irqchip.chip.ioapic.irr = 1U << 4;
	for (pin = 0; pin < KVM_IOAPIC_NUM_PINS; ++pin)
		irqchip.chip.ioapic.redirtbl[pin].bits = 0x00010000ULL;
	irqchip.chip.ioapic.redirtbl[4].bits = 0x00010051ULL;
	if (ioctl(vm_fd, KVM_SET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_SET_IRQCHIP IOAPIC");
	bzero(&irqchip, sizeof(irqchip));
	irqchip.chip_id = KVM_IRQCHIP_IOAPIC;
	if (ioctl(vm_fd, KVM_GET_IRQCHIP, &irqchip) != 0)
		err(1, "KVM_GET_IRQCHIP IOAPIC");
	if (irqchip.chip.ioapic.base_address != 0xfec00000ULL ||
	    irqchip.chip.ioapic.ioregsel != 0x11 ||
	    irqchip.chip.ioapic.id != 1 ||
	    irqchip.chip.ioapic.irr != (1U << 4) ||
	    irqchip.chip.ioapic.redirtbl[4].bits != 0x00010051ULL)
		errx(1, "KVM IOAPIC round trip mismatch");

	line.irq = 4;
	line.level = 1;
	if (ioctl(vm_fd, KVM_IRQ_LINE, &line) != 0)
		err(1, "KVM_IRQ_LINE assert");
	line.level = 0;
	if (ioctl(vm_fd, KVM_IRQ_LINE, &line) != 0)
		err(1, "KVM_IRQ_LINE deassert");
}

static void
kvm_test_extended_state(int control_fd, int vcpu_fd)
{
	struct kvm_debugregs debugregs;
	struct kvm_fpu fpu;
	struct kvm_vcpu_events events;
	struct kvm_xsave xsave;

	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_DEBUGREGS) != 1)
		errx(1, "KVM_CAP_DEBUGREGS not available");
	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE) != 1)
		errx(1, "KVM_CAP_XSAVE not available");
	bzero(&fpu, sizeof(fpu));
	fpu.fcw = 0x037f;
	fpu.mxcsr = 0x1f80;
	fpu.fpr[0][0] = 0xa5;
	fpu.xmm[1][0] = 0x5a;
	if (ioctl(vcpu_fd, KVM_SET_FPU, &fpu) != 0)
		err(1, "KVM_SET_FPU");
	bzero(&fpu, sizeof(fpu));
	if (ioctl(vcpu_fd, KVM_GET_FPU, &fpu) != 0)
		err(1, "KVM_GET_FPU");
	if (fpu.fcw != 0x037f || fpu.mxcsr != 0x1f80 ||
	    fpu.fpr[0][0] != 0xa5 || fpu.xmm[1][0] != 0x5a)
		errx(1, "KVM FPU round trip mismatch");
	bzero(&xsave, sizeof(xsave));
	if (ioctl(vcpu_fd, KVM_GET_XSAVE, &xsave) != 0)
		err(1, "KVM_GET_XSAVE");
	if (ioctl(vcpu_fd, KVM_SET_XSAVE, &xsave) != 0)
		err(1, "KVM_SET_XSAVE");
	bzero(&debugregs, sizeof(debugregs));
	debugregs.db[0] = 0x1122334455667788ULL;
	debugregs.dr6 = 0xffff0ff0;
	debugregs.dr7 = 0x400;
	if (ioctl(vcpu_fd, KVM_SET_DEBUGREGS, &debugregs) != 0)
		err(1, "KVM_SET_DEBUGREGS");
	bzero(&debugregs, sizeof(debugregs));
	if (ioctl(vcpu_fd, KVM_GET_DEBUGREGS, &debugregs) != 0)
		err(1, "KVM_GET_DEBUGREGS");
	if (debugregs.db[0] != 0x1122334455667788ULL ||
	    debugregs.dr6 != 0xffff0ff0 || debugregs.dr7 != 0x400)
		errx(1, "KVM debug-register round trip mismatch");
	bzero(&events, sizeof(events));
	if (ioctl(vcpu_fd, KVM_SET_VCPU_EVENTS, &events) != 0)
		err(1, "KVM_SET_VCPU_EVENTS");
	if (ioctl(vcpu_fd, KVM_GET_VCPU_EVENTS, &events) != 0)
		err(1, "KVM_GET_VCPU_EVENTS");
}

static void
kvm_test_irqfd(int control_fd, int vm_fd)
{
	struct {
		struct kvm_irq_routing routing;
		struct kvm_irq_routing_entry entries[1];
	} routes;
	struct kvm_dfly_buffer route_buffer;
	struct kvm_dfly_eventfd eventfd;
	struct kvm_irqfd irqfd;
	uint64_t value;

	bzero(&routes, sizeof(routes));
	routes.routing.nr = 1;
	routes.entries[0].gsi = 24;
	routes.entries[0].type = KVM_IRQ_ROUTING_MSI;
	routes.entries[0].u.msi.address_lo = 0xfee00000U;
	routes.entries[0].u.msi.data = 0x51;
	bzero(&route_buffer, sizeof(route_buffer));
	route_buffer.data = (uintptr_t)&routes;
	route_buffer.length = sizeof(routes);
	if (ioctl(vm_fd, KVM_DFLY_SET_GSI_ROUTING, &route_buffer) != 0)
		err(1, "KVM_DFLY_SET_GSI_ROUTING");
	bzero(&eventfd, sizeof(eventfd));
	eventfd.flags = KVM_DFLY_EVENTFD_CLOEXEC;
	if (ioctl(control_fd, KVM_DFLY_CREATE_EVENTFD, &eventfd) != 0)
		err(1, "KVM_DFLY_CREATE_EVENTFD irqfd");
	bzero(&irqfd, sizeof(irqfd));
	irqfd.fd = eventfd.fd;
	irqfd.gsi = 24;
	if (ioctl(vm_fd, KVM_IRQFD, &irqfd) != 0)
		err(1, "KVM_IRQFD bind");
	value = 1;
	if (write(eventfd.fd, &value, sizeof(value)) != sizeof(value))
		err(1, "write irqfd");
	irqfd.flags = KVM_IRQFD_FLAG_DEASSIGN;
	if (ioctl(vm_fd, KVM_IRQFD, &irqfd) != 0)
		err(1, "KVM_IRQFD deassign");
	if (close(eventfd.fd) != 0)
		err(1, "close irqfd");
}

int
main(void)
{
	struct kvm_regs regs;
	struct kvm_dfly_eventfd eventfd;
	struct kvm_ioeventfd ioevent;
	struct kvm_msi msi;
	struct kvm_run *run;
	void *mapping;
	int control_fd;
	int vm_fd;
	int vcpu_fd;
	int run_size;
	uint64_t identity_address;
	uint64_t tss_address;

	control_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (control_fd < 0)
		err(1, "open /dev/kvm");
	KVM_TEST_STEP("step: control");
	if (ioctl(control_fd, KVM_GET_API_VERSION) != KVM_API_VERSION)
		err(1, "KVM_GET_API_VERSION");
	if (ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_SET_TSS_ADDR) != 1 ||
	    ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_EXT_CPUID) != 1 ||
	    ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_MP_STATE) != 1 ||
	    ioctl(control_fd, KVM_CHECK_EXTENSION,
	    KVM_CAP_SET_IDENTITY_MAP_ADDR) != 1 ||
	    ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT) != 1 ||
	    ioctl(control_fd, KVM_CHECK_EXTENSION, KVM_CAP_NR_MEMSLOTS) != 32)
		errx(1, "missing implemented KVM capability");
	vm_fd = ioctl(control_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0)
		err(1, "KVM_CREATE_VM");
	KVM_TEST_STEP("step: vm");
	identity_address = 0xfffbc000ULL;
	if (ioctl(vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &identity_address) != 0)
		err(1, "KVM_SET_IDENTITY_MAP_ADDR");
	tss_address = 0xfffbd000ULL;
	if (ioctl(vm_fd, KVM_SET_TSS_ADDR, tss_address) != 0)
		err(1, "KVM_SET_TSS_ADDR");
	KVM_TEST_STEP("step: legacy-mm-state");
	kvm_test_clock(vm_fd);
	KVM_TEST_STEP("step: clock");
	if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) != 0)
		err(1, "KVM_CREATE_IRQCHIP");
	KVM_TEST_STEP("step: irqchip");
	kvm_test_irqchip(vm_fd);
	KVM_TEST_STEP("step: irqchip-state");
	run_size = ioctl(vm_fd, KVM_GET_VCPU_MMAP_SIZE);
	if (run_size != 2 * getpagesize())
		errx(1, "unexpected KVM_RUN mapping size %d", run_size);
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0)
		err(1, "KVM_CREATE_VCPU");
	KVM_TEST_STEP("step: vcpu");
	mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    vcpu_fd, 0);
	if (mapping == MAP_FAILED)
		err(1, "mmap KVM_RUN");
	KVM_TEST_STEP("step: run-map");
	run = mapping;
	bzero(&regs, sizeof(regs));
	regs.rax = 0x123456789abcdef0ULL;
	regs.rip = 0xfff0;
	regs.rflags = 0x2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) != 0)
		err(1, "KVM_SET_REGS");
	bzero(&regs, sizeof(regs));
	if (ioctl(vcpu_fd, KVM_GET_REGS, &regs) != 0)
		err(1, "KVM_GET_REGS");
	if (regs.rax != 0x123456789abcdef0ULL)
		errx(1, "KVM register round trip mismatch");
	KVM_TEST_STEP("step: registers");
	kvm_test_cpuid(control_fd, vcpu_fd);
	KVM_TEST_STEP("step: cpuid");
	kvm_test_msr_index_list(control_fd);
	KVM_TEST_STEP("step: msr-index-list");
	kvm_test_msr_feature_index_list(control_fd);
	KVM_TEST_STEP("step: msr-feature-index-list");
	kvm_test_msrs(vcpu_fd);
	KVM_TEST_STEP("step: msrs");
	kvm_test_xcrs(control_fd, vcpu_fd);
	KVM_TEST_STEP("step: xcrs");
	kvm_test_mp_state(vcpu_fd);
	KVM_TEST_STEP("step: mp-state");
	kvm_test_lapic_reset(vcpu_fd);
	KVM_TEST_STEP("step: lapic-reset");
	kvm_test_lapic(vcpu_fd);
	KVM_TEST_STEP("step: lapic");
	kvm_test_extended_state(control_fd, vcpu_fd);
	KVM_TEST_STEP("step: extended-state");
	bzero(&eventfd, sizeof(eventfd));
	eventfd.flags = KVM_DFLY_EVENTFD_NONBLOCK | KVM_DFLY_EVENTFD_CLOEXEC;
	if (ioctl(control_fd, KVM_DFLY_CREATE_EVENTFD, &eventfd) != 0)
		err(1, "KVM_DFLY_CREATE_EVENTFD");
	bzero(&ioevent, sizeof(ioevent));
	ioevent.datamatch = 0x5a;
	ioevent.addr = 0x1234;
	ioevent.len = 1;
	ioevent.fd = eventfd.fd;
	ioevent.flags = KVM_IOEVENTFD_FLAG_PIO |
	    KVM_IOEVENTFD_FLAG_DATAMATCH;
	if (ioctl(vm_fd, KVM_IOEVENTFD, &ioevent) != 0)
		err(1, "KVM_IOEVENTFD bind");
	ioevent.flags |= KVM_IOEVENTFD_FLAG_DEASSIGN;
	if (ioctl(vm_fd, KVM_IOEVENTFD, &ioevent) != 0)
		err(1, "KVM_IOEVENTFD deassign");
	ioevent.len = 0;
	ioevent.flags &= ~(KVM_IOEVENTFD_FLAG_DATAMATCH |
	    KVM_IOEVENTFD_FLAG_DEASSIGN);
	if (ioctl(vm_fd, KVM_IOEVENTFD, &ioevent) != 0)
		err(1, "KVM_IOEVENTFD any-length bind");
	ioevent.flags |= KVM_IOEVENTFD_FLAG_DEASSIGN;
	if (ioctl(vm_fd, KVM_IOEVENTFD, &ioevent) != 0)
		err(1, "KVM_IOEVENTFD any-length deassign");
	if (close(eventfd.fd) != 0)
		err(1, "close ioeventfd");
	KVM_TEST_STEP("step: ioeventfd");
	bzero(&msi, sizeof(msi));
	msi.address_lo = 0xfee00000U;
	msi.data = 0x50;
	if (ioctl(vm_fd, KVM_SIGNAL_MSI, &msi) != 0)
		err(1, "KVM_SIGNAL_MSI");
	KVM_TEST_STEP("step: msi");
	kvm_test_irqfd(control_fd, vm_fd);
	KVM_TEST_STEP("step: irqfd");
	run->immediate_exit = 1;
	errno = 0;
	if (ioctl(vcpu_fd, KVM_RUN, 0) != -1 || errno != EINTR)
		err(1, "KVM_RUN immediate_exit");
	if (close(vm_fd) != 0)
		err(1, "close VM fd");
	if (ioctl(vcpu_fd, KVM_GET_REGS, &regs) != 0)
		err(1, "KVM vCPU after VM fd close");
	KVM_TEST_STEP("step: parent-close");
	if (munmap(mapping, run_size) != 0)
		err(1, "munmap KVM_RUN");
	KVM_TEST_STEP("step: unmap");
	if (close(vcpu_fd) != 0)
		err(1, "close vCPU fd");
	KVM_TEST_STEP("step: vcpu-close");
	if (close(control_fd) != 0)
		err(1, "close control fd");
	KVM_TEST_STEP("step: control-close");
	puts("kvm vcpu ABI: PASS");
	return 0;
}
