/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm public kernel API.
 */
#ifndef VMM_H
#define VMM_H

#include <sys/types.h>

#ifndef _KERNEL
#include <stdbool.h>
#ifndef CTASSERT
#define CTASSERT(expression) _Static_assert((expression), #expression)
#endif
#endif

struct vmspace;
struct vmm_machine;
struct vmm_vcpu;
struct vmm_io;

/* Opaque runtime handles created and destroyed only through this API. */
typedef struct vmm_machine *vmm_machine_t;
typedef struct vmm_vcpu *vmm_vcpu_t;
typedef struct vmm_io *vmm_io_t;

enum vmm_io_width {
	VMM_IO_WIDTH_8 = 1,
	VMM_IO_WIDTH_16 = 2,
	VMM_IO_WIDTH_32 = 4,
	VMM_IO_WIDTH_64 = 8,
};

enum vmm_memory_exit_mode {
	VMM_MEMORY_EXIT_RAW,
	VMM_MEMORY_EXIT_EMULATE
};

#if defined(__x86_64__)
#include "x64/vmm_x64.h"
#else
#error "vmm has no public API for this architecture"
#endif

/*
 * Creates a machine using vmspace as its guest GPA address space.
 * vmspace must be initialized and private to the guest.  VMM marks its pmap
 * threadable and converts it for nested paging.  The caller retains ownership,
 * keeps it valid until vmm_machine_destroy() returns, and must not reuse it as
 * a host vmspace.  Returns EBUSY while the module drains.
 */
int vmm_machine_create(struct vmspace *vmspace, vmm_machine_t *machine);

/*
 * Gives VMM ownership of this machine's interrupt-controller model.  It must
 * be called before creating any vCPU and cannot be undone.  The selected
 * backend uses hardware acceleration when available, otherwise it provides
 * an in-kernel software interrupt controller.
 */
int vmm_machine_create_irqchip(vmm_machine_t machine);

/* Reports whether the selected backend can create an in-kernel irqchip. */
bool vmm_irqchip_available(void);

/*
 * Delivers one x86 MSI message through the machine interrupt controller.
 * Hardware-backed irqchips deliver directly when possible.  The software
 * irqchip accepts the same fixed, physical-destination MSI form.
 */
int vmm_machine_raise_msi(vmm_machine_t machine, uint64_t address,
	uint32_t data);

/*
 * Raises one edge-triggered guest GSI through the machine interrupt
 * controller.  The GSI is neither a host IRQ nor an APIC vector.  Backends
 * without an in-kernel GSI input return ENOTSUP.
 */
int vmm_machine_raise_irq(vmm_machine_t machine, uint32_t gsi);

/*
 * Drives one guest GSI input to level.  This is the level-sensitive form of
 * vmm_machine_raise_irq(); callers must deassert an asserted input.  It is
 * required for virtual INTx and other level-triggered IOAPIC sources.
 */
int vmm_machine_set_irq(vmm_machine_t machine, uint32_t gsi, bool level);

struct vmm_io_write {
	uint64_t address;
	enum vmm_io_width width;
	uint64_t value;
};

struct vmm_io_read {
	uint64_t address;
	enum vmm_io_width width;
	uint64_t value;
};

/*
 * A trap handler consumes one scalar guest access by returning zero.
 * A read handler must set read->value before returning.  Returning ENOENT
 * declines this registration; if no registration consumes the access, VMM
 * leaves the VM exit visible to the vCPU caller.  Handlers run synchronously
 * under the machine token and must neither block nor call back into machine.
 */
typedef int (*vmm_io_read_handler_t)(vmm_vcpu_t, void *,
	struct vmm_io_read *);

typedef int (*vmm_io_write_handler_t)(vmm_vcpu_t, void *,
	const struct vmm_io_write *);

/*
 * Traps scalar PIO reads fully contained in [base, base + size).  PIO accepts
 * only 8-, 16-, and 32-bit accesses.  size may describe the entire 64 KiB PIO
 * space.  A successful handler supplies the result and resumes the guest at
 * the next instruction.
 */
int vmm_machine_trap_pio_read(vmm_machine_t machine, uint16_t base,
	uint32_t size, vmm_io_read_handler_t handler, void *argument,
	vmm_io_t *io);

/*
 * Traps scalar PIO writes fully contained in [base, base + size).  A
 * successful handler avoids a caller round trip and resumes the guest at the
 * next instruction.
 */
int vmm_machine_trap_pio_write(vmm_machine_t machine, uint16_t base,
	uint32_t size, vmm_io_write_handler_t handler, void *argument,
	vmm_io_t *io);

/*
 * Traps scalar MMIO reads fully contained in [base, base + size).  A
 * successful handler supplies the result while VMM completes the decoded
 * scalar instruction.
 */
int vmm_machine_trap_mmio_read(vmm_machine_t machine, uint64_t base,
	uint64_t size, vmm_io_read_handler_t handler, void *argument,
	vmm_io_t *io);

/*
 * Traps scalar MMIO writes fully contained in [base, base + size).  VMM
 * completes registered writes in kernel; unsupported memory instructions
 * remain visible to the vCPU caller.
 */
int vmm_machine_trap_mmio_write(vmm_machine_t machine, uint64_t base,
	uint64_t size, vmm_io_write_handler_t handler, void *argument,
	vmm_io_t *io);

/*
 * Removes one I/O read or write trap.  machine must own io.  The caller must
 * stop using io; an in-flight handler is serialized by the machine token.
 */
int vmm_machine_untrap(vmm_machine_t machine, vmm_io_t io);

/*
 * Creates a vCPU using caller-owned architectural state.  state must remain
 * valid until vmm_vcpu_destroy(), and machine must remain valid throughout.
 * Callers must not change state while the vCPU runs.
 */
int vmm_vcpu_create(vmm_machine_t machine, struct vmm_cpustate *state,
	vmm_vcpu_t *vcpu);

/*
 * Selects how unresolved guest-memory exits reach the caller.  RAW returns
 * the hardware exit; EMULATE decodes scalar MMIO fragments for completion.
 * Returns EBUSY while vmm_vcpu_run() is active.
 */
int vmm_vcpu_set_memory_exit_mode(vmm_vcpu_t vcpu,
	enum vmm_memory_exit_mode mode);

/*
 * Replaces the vCPU's exact CPUID override table.  A specific subleaf entry
 * takes precedence over a leaf-only entry.  Entries not present continue to
 * use backend policy.  The vCPU must not be running.
 */
int vmm_vcpu_set_cpuid(vmm_vcpu_t vcpu,
	const struct vmm_cpuid_entry *entries, size_t entry_count);

/*
 * Runs a vCPU until it exits to the caller.  The backend resolves known
 * architectural exits and registered I/O in kernel; VMM resolves nested page
 * faults through the machine vmspace.  On success, *reason is valid until the
 * next vmm_vcpu_run() call or vmm_vcpu_destroy(), and state is current.
 * Returns EBUSY when another caller is running it.
 */
int vmm_vcpu_run(vmm_vcpu_t vcpu, struct vmm_cpuexit **reason);

/*
 * Acknowledges the write fragment returned as VMM_CPUEXIT_MEMORY.  The vCPU
 * must be stopped at that fragment.  VMM completes the decoded instruction
 * on the next vmm_vcpu_run() call.
 */
int vmm_vcpu_complete_mmio_write(vmm_vcpu_t vcpu);

/*
 * Supplies the bytes read for the fragment returned as VMM_CPUEXIT_MEMORY.
 * size must equal the exit width.  The vCPU must be stopped at that fragment;
 * VMM completes the decoded instruction on the next vmm_vcpu_run() call.
 */
int vmm_vcpu_complete_mmio_read(vmm_vcpu_t vcpu, const void *data,
	size_t size);

/*
 * Queues one architectural event for the next vCPU run.  The vCPU must not be
 * running.  At most one event can wait for entry; the event is committed only
 * after the backend has loaded architectural state and confirmed VM entry.
 * Returns EBUSY while an event is already pending.
 */
int vmm_vcpu_inject(vmm_vcpu_t vcpu, const struct vmm_cpuevent *event);

/*
 * Requests that a running vCPU return promptly without changing guest state.
 * Returns EALREADY when the vCPU is not running.
 */
int vmm_vcpu_kick(vmm_vcpu_t vcpu);

/*
 * Destroys a non-running vCPU; returns EBUSY while vmm_vcpu_run() is active.
 */
int vmm_vcpu_destroy(vmm_vcpu_t vcpu);

/*
 * Destroys a machine after every vCPU has been destroyed.  This removes VMM's
 * pmap CPU association but does not otherwise manage vmspace.
 * Returns EBUSY while a vCPU exists or is running.
 */
int vmm_machine_destroy(vmm_machine_t machine);

#endif /* VMM_H */
