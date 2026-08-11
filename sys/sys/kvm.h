/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly KVM-compatible control ABI.
 */
#ifndef _SYS_KVM_H_
#define _SYS_KVM_H_

#include <sys/ioccom.h>
#include <sys/types.h>

/*
 * Keep the Linux KVM API version and command numbers.  DragonFly encodes
 * ioctl directions with its native _IO* macros; DragonFly QEMU builds must
 * include this header instead of Linux's ioctl definitions.
 */
#define KVM_API_VERSION	12
#define KVMIO			0xAE

#define KVM_GET_API_VERSION	_IO(KVMIO, 0x00)
#define KVM_CREATE_VM		_IO(KVMIO, 0x01)
#define KVM_CHECK_EXTENSION	_IO(KVMIO, 0x03)
#define KVM_GET_VCPU_MMAP_SIZE	_IO(KVMIO, 0x04)

/* x86 VM and vCPU control commands used by the initial QEMU frontend. */
#define KVM_CREATE_VCPU		_IO(KVMIO, 0x41)
#define KVM_RUN			_IO(KVMIO, 0x80)

/* KVM_RUN exit codes implemented by this frontend. */
#define KVM_EXIT_UNKNOWN		0
#define KVM_EXIT_EXCEPTION		1
#define KVM_EXIT_IO			2
#define KVM_EXIT_HLT			5
#define KVM_EXIT_MMIO			6
#define KVM_EXIT_IRQ_WINDOW_OPEN	7
#define KVM_EXIT_SHUTDOWN		8
#define KVM_EXIT_FAIL_ENTRY		9
#define KVM_EXIT_INTR			10
#define KVM_EXIT_INTERNAL_ERROR	17

#define KVM_INTERNAL_ERROR_EMULATION	1

#define KVM_EXIT_IO_IN			0
#define KVM_EXIT_IO_OUT		1

/* The second mapped page carries data for KVM_EXIT_IO. */
#define KVM_PIO_PAGE_OFFSET		1

struct kvm_run {
	uint8_t request_interrupt_window;
	uint8_t immediate_exit;
	uint8_t padding1[6];
	uint32_t exit_reason;
	uint8_t ready_for_interrupt_injection;
	uint8_t if_flag;
	uint16_t flags;
	uint64_t cr8;
	uint64_t apic_base;
	union {
		struct {
			uint64_t hardware_exit_reason;
		} hw;
		struct {
			uint64_t hardware_entry_failure_reason;
			uint32_t cpu;
		} fail_entry;
		struct {
			uint32_t exception;
			uint32_t error_code;
		} ex;
		struct {
			uint8_t direction;
			uint8_t size;
			uint16_t port;
			uint32_t count;
			uint64_t data_offset;
		} io;
		struct {
			uint64_t phys_addr;
			uint8_t data[8];
			uint32_t len;
			uint8_t is_write;
		} mmio;
		struct {
			uint32_t suberror;
			uint32_t ndata;
			uint64_t data[16];
		} internal;
		uint8_t padding[256];
	} u;
};

struct kvm_regs {
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rsi;
	uint64_t rdi;
	uint64_t rsp;
	uint64_t rbp;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rip;
	uint64_t rflags;
};

struct kvm_segment {
	uint64_t base;
	uint32_t limit;
	uint16_t selector;
	uint8_t type;
	uint8_t present;
	uint8_t dpl;
	uint8_t db;
	uint8_t s;
	uint8_t l;
	uint8_t g;
	uint8_t avl;
	uint8_t unusable;
	uint8_t padding;
};

struct kvm_dtable {
	uint64_t base;
	uint16_t limit;
	uint16_t padding[3];
};

#define KVM_NR_INTERRUPTS	256

struct kvm_sregs {
	struct kvm_segment cs;
	struct kvm_segment ds;
	struct kvm_segment es;
	struct kvm_segment fs;
	struct kvm_segment gs;
	struct kvm_segment ss;
	struct kvm_segment tr;
	struct kvm_segment ldt;
	struct kvm_dtable gdt;
	struct kvm_dtable idt;
	uint64_t cr0;
	uint64_t cr2;
	uint64_t cr3;
	uint64_t cr4;
	uint64_t cr8;
	uint64_t efer;
	uint64_t apic_base;
	uint64_t interrupt_bitmap[(KVM_NR_INTERRUPTS + 63) / 64];
};

struct kvm_cpuid_entry2 {
	uint32_t function;
	uint32_t index;
	uint32_t flags;
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t padding[3];
};

#define KVM_CPUID_FLAG_SIGNIFCANT_INDEX	0x00000001U

struct kvm_cpuid2 {
	uint32_t nent;
	uint32_t padding;
	struct kvm_cpuid_entry2 entries[];
};

struct kvm_msr_entry {
	uint32_t index;
	uint32_t reserved;
	uint64_t data;
};

struct kvm_msrs {
	uint32_t nmsrs;
	uint32_t pad;
	struct kvm_msr_entry entries[];
};

struct kvm_msr_list {
	uint32_t nmsrs;
	uint32_t indices[];
};

/* x87 and SSE state used by KVM_GET_FPU and KVM_SET_FPU. */
struct kvm_fpu {
	uint8_t fpr[8][16];
	uint16_t fcw;
	uint16_t fsw;
	uint8_t ftwx;
	uint8_t pad1;
	uint16_t last_opcode;
	uint64_t last_ip;
	uint64_t last_dp;
	uint8_t xmm[16][16];
	uint32_t mxcsr;
	uint32_t pad2;
};

/* Extended-state image.  The first 512 bytes are the FXSAVE base area. */
struct kvm_xsave {
	uint32_t region[1024];
};

struct kvm_xcr {
	uint32_t xcr;
	uint32_t reserved;
	uint64_t value;
};

#define KVM_MAX_XCRS	16

struct kvm_xcrs {
	uint32_t nr_xcrs;
	uint32_t flags;
	struct kvm_xcr xcrs[KVM_MAX_XCRS];
	uint64_t padding[16];
};

/* Architectural event state used by KVM_GET_VCPU_EVENTS and SET. */
struct kvm_vcpu_events {
	struct {
		uint8_t injected;
		uint8_t nr;
		uint8_t has_error_code;
		uint8_t pending;
		uint32_t error_code;
	} exception;
	struct {
		uint8_t injected;
		uint8_t nr;
		uint8_t soft;
		uint8_t shadow;
	} interrupt;
	struct {
		uint8_t injected;
		uint8_t pending;
		uint8_t masked;
		uint8_t pad;
	} nmi;
	uint32_t sipi_vector;
	uint32_t flags;
	struct {
		uint8_t smm;
		uint8_t pending;
		uint8_t smm_inside_nmi;
		uint8_t latched_init;
	} smi;
	struct {
		uint8_t pending;
	} triple_fault;
	uint8_t reserved[26];
	uint8_t exception_has_payload;
	uint64_t exception_payload;
};

struct kvm_debugregs {
	uint64_t db[4];
	uint64_t dr6;
	uint64_t dr7;
	uint64_t flags;
	uint64_t reserved[9];
};

#ifdef _KERNEL
CTASSERT(sizeof(struct kvm_fpu) == 416);
CTASSERT(sizeof(struct kvm_xsave) == 4096);
CTASSERT(sizeof(struct kvm_debugregs) == 128);
#else
_Static_assert(sizeof(struct kvm_fpu) == 416, "kvm_fpu ABI");
_Static_assert(sizeof(struct kvm_xsave) == 4096, "kvm_xsave ABI");
_Static_assert(sizeof(struct kvm_debugregs) == 128, "kvm_debugregs ABI");
#endif

#define KVM_MP_STATE_RUNNABLE		0
#define KVM_MP_STATE_UNINITIALIZED	1
#define KVM_MP_STATE_INIT_RECEIVED	2
#define KVM_MP_STATE_HALTED		3
#define KVM_MP_STATE_SIPI_RECEIVED	4
#define KVM_MP_STATE_STOPPED		5
#define KVM_MP_STATE_CHECK_STOP		6
#define KVM_MP_STATE_OPERATING		7
#define KVM_MP_STATE_LOAD		8
#define KVM_MP_STATE_AP_RESET_HOLD	9
#define KVM_MP_STATE_SUSPENDED		10

struct kvm_mp_state {
	uint32_t mp_state;
};

#define KVM_IOEVENTFD_FLAG_DATAMATCH	(1U << 0)
#define KVM_IOEVENTFD_FLAG_PIO		(1U << 1)
#define KVM_IOEVENTFD_FLAG_DEASSIGN	(1U << 2)
#define KVM_IOEVENTFD_VALID_FLAG_MASK	(KVM_IOEVENTFD_FLAG_DATAMATCH | \
	KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DEASSIGN)

struct kvm_ioeventfd {
	uint64_t datamatch;
	uint64_t addr;
	uint32_t len;
	int32_t fd;
	uint32_t flags;
	uint8_t pad[36];
};

#define KVM_IRQ_ROUTING_IRQCHIP	1
#define KVM_IRQ_ROUTING_MSI		2

struct kvm_irq_routing_irqchip {
	uint32_t irqchip;
	uint32_t pin;
};

struct kvm_irq_routing_msi {
	uint32_t address_lo;
	uint32_t address_hi;
	uint32_t data;
	union {
		uint32_t pad;
		uint32_t devid;
	};
};

struct kvm_irq_routing_entry {
	uint32_t gsi;
	uint32_t type;
	uint32_t flags;
	uint32_t pad;
	union {
		struct kvm_irq_routing_irqchip irqchip;
		struct kvm_irq_routing_msi msi;
		uint32_t pad[8];
	} u;
};

struct kvm_irq_routing {
	uint32_t nr;
	uint32_t flags;
	struct kvm_irq_routing_entry entries[];
};

#define KVM_IRQFD_FLAG_DEASSIGN	(1U << 0)
#define KVM_IRQFD_FLAG_RESAMPLE	(1U << 1)

struct kvm_irqfd {
	uint32_t fd;
	uint32_t gsi;
	uint32_t flags;
	uint32_t resamplefd;
	uint8_t pad[16];
};

#define KVM_MSI_VALID_DEVID	(1U << 0)

struct kvm_msi {
	uint32_t address_lo;
	uint32_t address_hi;
	uint32_t data;
	uint32_t flags;
	uint32_t devid;
	uint8_t pad[12];
};

/*
 * DragonFly's generic ioctl entry copies exactly IOCPARM_LEN(command) bytes.
 * Variable-length Linux KVM payloads therefore use this fixed-size descriptor;
 * data is a user virtual address and length is the complete payload size.
 */
struct kvm_dfly_buffer {
	uint64_t data;
	uint32_t length;
	uint32_t reserved;
};

#define KVM_GET_SUPPORTED_CPUID	_IOWR(KVMIO, 0x05, struct kvm_cpuid2)
#define KVM_GET_MSR_INDEX_LIST		_IOWR(KVMIO, 0x02, struct kvm_msr_list)
#define KVM_GET_REGS			_IOR(KVMIO, 0x81, struct kvm_regs)
#define KVM_SET_REGS			_IOW(KVMIO, 0x82, struct kvm_regs)
#define KVM_GET_SREGS			_IOR(KVMIO, 0x83, struct kvm_sregs)
#define KVM_SET_SREGS			_IOW(KVMIO, 0x84, struct kvm_sregs)
#define KVM_GET_MSRS			_IOWR(KVMIO, 0x88, struct kvm_msrs)
#define KVM_SET_MSRS			_IOW(KVMIO, 0x89, struct kvm_msrs)
#define KVM_GET_FPU			_IOR(KVMIO, 0x8c, struct kvm_fpu)
#define KVM_SET_FPU			_IOW(KVMIO, 0x8d, struct kvm_fpu)
#define KVM_SET_CPUID2		_IOW(KVMIO, 0x90, struct kvm_cpuid2)
#define KVM_GET_VCPU_EVENTS		_IOR(KVMIO, 0x9f, struct kvm_vcpu_events)
#define KVM_SET_VCPU_EVENTS		_IOW(KVMIO, 0xa0, struct kvm_vcpu_events)
#define KVM_GET_DEBUGREGS		_IOR(KVMIO, 0xa1, struct kvm_debugregs)
#define KVM_SET_DEBUGREGS		_IOW(KVMIO, 0xa2, struct kvm_debugregs)
#define KVM_GET_XSAVE			_IOR(KVMIO, 0xa4, struct kvm_xsave)
#define KVM_SET_XSAVE			_IOW(KVMIO, 0xa5, struct kvm_xsave)
#define KVM_GET_XCRS			_IOR(KVMIO, 0xa6, struct kvm_xcrs)
#define KVM_SET_XCRS			_IOW(KVMIO, 0xa7, struct kvm_xcrs)
#define KVM_GET_MP_STATE		_IOR(KVMIO, 0x98, struct kvm_mp_state)
#define KVM_SET_MP_STATE		_IOW(KVMIO, 0x99, struct kvm_mp_state)
#define KVM_SET_TSS_ADDR		_IO(KVMIO, 0x47)
#define KVM_SET_IDENTITY_MAP_ADDR	_IOW(KVMIO, 0x48, uint64_t)
#define KVM_CREATE_IRQCHIP		_IO(KVMIO, 0x60)
#define KVM_SET_GSI_ROUTING		_IOW(KVMIO, 0x6a, struct kvm_irq_routing)
#define KVM_IRQFD			_IOW(KVMIO, 0x76, struct kvm_irqfd)
#define KVM_IOEVENTFD			_IOW(KVMIO, 0x79, struct kvm_ioeventfd)
#define KVM_SIGNAL_MSI			_IOW(KVMIO, 0xa5, struct kvm_msi)

/* DragonFly indirection for Linux KVM's flexible-array CPUID/MSR payloads. */
#define KVM_DFLY_GET_SUPPORTED_CPUID \
	_IOWR('K', 0x02, struct kvm_dfly_buffer)
#define KVM_DFLY_SET_CPUID2 \
	_IOW('K', 0x03, struct kvm_dfly_buffer)
#define KVM_DFLY_GET_MSRS \
	_IOWR('K', 0x04, struct kvm_dfly_buffer)
#define KVM_DFLY_SET_MSRS \
	_IOW('K', 0x05, struct kvm_dfly_buffer)
#define KVM_DFLY_SET_GSI_ROUTING \
	_IOW('K', 0x06, struct kvm_dfly_buffer)
#define KVM_DFLY_GET_MSR_INDEX_LIST \
	_IOWR('K', 0x07, struct kvm_dfly_buffer)
#define KVM_DFLY_GET_MSR_FEATURE_INDEX_LIST \
	_IOWR('K', 0x08, struct kvm_dfly_buffer)

struct kvm_userspace_memory_region {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};

#define KVM_MEM_LOG_DIRTY_PAGES	(1U << 0)
#define KVM_MEM_READONLY		(1U << 1)
#define KVM_SET_USER_MEMORY_REGION \
	_IOW(KVMIO, 0x46, struct kvm_userspace_memory_region)

/*
 * DragonFly extension used by the QEMU port to create an event counter fd.
 * It will become the backing primitive for KVM_IOEVENTFD and KVM_IRQFD.
 */
#define KVM_DFLY_EVENTFD_NONBLOCK	0x00000001U
#define KVM_DFLY_EVENTFD_CLOEXEC	0x00000002U
#define KVM_DFLY_EVENTFD_VALID_FLAGS	(KVM_DFLY_EVENTFD_NONBLOCK | \
	KVM_DFLY_EVENTFD_CLOEXEC)

struct kvm_dfly_eventfd {
	uint64_t initial;
	uint32_t flags;
	int32_t fd;
};

#define KVM_DFLY_CREATE_EVENTFD \
	_IOWR('K', 0x01, struct kvm_dfly_eventfd)

/* KVM_CHECK_EXTENSION values used by the initial QEMU KVM probe. */
#define KVM_CAP_IRQCHIP		0
#define KVM_CAP_USER_MEMORY	3
#define KVM_CAP_SET_TSS_ADDR	4
#define KVM_CAP_EXT_CPUID		7
#define KVM_CAP_NR_VCPUS		9
#define KVM_CAP_NR_MEMSLOTS		10
#define KVM_CAP_MP_STATE		14
#define KVM_CAP_DESTROY_MEMORY_REGION_WORKS	21
#define KVM_CAP_JOIN_MEMORY_REGIONS_WORKS	30
#define KVM_CAP_IRQ_ROUTING		25
#define KVM_CAP_IRQFD		32
#define KVM_CAP_IOEVENTFD		36
#define KVM_CAP_SET_IDENTITY_MAP_ADDR	37
#define KVM_CAP_INTERNAL_ERROR_DATA	40
#define KVM_CAP_VCPU_EVENTS		41
#define KVM_CAP_DEBUGREGS		50
#define KVM_CAP_XSAVE			55
#define KVM_CAP_XCRS			56
#define KVM_CAP_MAX_VCPUS		66
#define KVM_CAP_SIGNAL_MSI		77
#define KVM_CAP_IOEVENTFD_ANY_LENGTH	122
#define KVM_CAP_IMMEDIATE_EXIT	136

#endif /* _SYS_KVM_H_ */
