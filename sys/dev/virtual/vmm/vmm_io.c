/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core I/O trap registrations -- see vmm.h.
 */
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mman.h>

#include "vmm_backend.h"
#include "vmm_io.h"

static int vmm_io_trap(struct vmm_machine *, enum vmm_io_space,
    enum vmm_io_direction, uint64_t, enum vmm_io_width,
    union vmm_io_handler, void *, vmm_io_t *);

int
vmm_machine_trap_pio_read(vmm_machine_t machine, uint16_t address,
    enum vmm_io_width width, vmm_io_read_handler_t handler, void *argument,
    vmm_io_t *io)
{
	union vmm_io_handler trap;

	trap.read = handler;
	return vmm_io_trap(machine, VMM_IO_PIO, VMM_IO_READ, address, width,
	    trap, argument, io);
}

int
vmm_machine_trap_pio_write(vmm_machine_t machine, uint16_t address,
    enum vmm_io_width width, vmm_io_write_handler_t handler, void *argument,
    vmm_io_t *io)
{
	union vmm_io_handler trap;

	trap.write = handler;
	return vmm_io_trap(machine, VMM_IO_PIO, VMM_IO_WRITE, address, width,
	    trap, argument, io);
}

int
vmm_machine_trap_mmio_read(vmm_machine_t machine, uint64_t address,
    enum vmm_io_width width, vmm_io_read_handler_t handler, void *argument,
    vmm_io_t *io)
{
	union vmm_io_handler trap;

	trap.read = handler;
	return vmm_io_trap(machine, VMM_IO_MMIO, VMM_IO_READ, address, width,
	    trap, argument, io);
}

int
vmm_machine_trap_mmio_write(vmm_machine_t machine, uint64_t address,
    enum vmm_io_width width, vmm_io_write_handler_t handler, void *argument,
    vmm_io_t *io)
{
	union vmm_io_handler trap;

	trap.write = handler;
	return vmm_io_trap(machine, VMM_IO_MMIO, VMM_IO_WRITE, address, width,
	    trap, argument, io);
}

int
vmm_machine_untrap(vmm_machine_t machine, vmm_io_t io)
{

	if (machine == NULL || io == NULL || io->machine != machine)
		return EINVAL;
	lwkt_gettoken(&machine->token);
	TAILQ_REMOVE(&machine->io_list, io, entry);
	io->machine = NULL;
	lwkt_reltoken(&machine->token);
	kfree(io, M_VMM);
	return 0;
}

int
vmm_io_handle_pio(struct vmm_vcpu *vcpu, const struct vmm_cpuexit *exit)
{
	struct vmm_io_read read;
	struct vmm_io_write write;
	uint64_t mask;
	unsigned int size;
	int error;

	if (exit->u.io.str || exit->u.io.rep)
		return ENOENT;
	size = exit->u.io.operand_size;
	if (size != 1 && size != 2 && size != 4)
		return ENOENT;
	if (exit->u.io.in) {
		read.address = exit->u.io.port;
		read.width = (enum vmm_io_width)size;
		read.value = 0;
		error = vmm_io_dispatch_read(vcpu, VMM_IO_PIO, &read);
		if (error == 0) {
			mask = (1ULL << (size * NBBY)) - 1;
			vcpu->state->gprs[VMM_X64_GPR_RAX] =
			    (vcpu->state->gprs[VMM_X64_GPR_RAX] & ~mask) |
			    (read.value & mask);
			vcpu->state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
		}
		return error;
	}
	write.address = exit->u.io.port;
	write.width = (enum vmm_io_width)size;
	write.value = vcpu->state->gprs[VMM_X64_GPR_RAX];
	write.value &= (1ULL << (size * 8)) - 1;
	error = vmm_io_dispatch_write(vcpu, VMM_IO_PIO, &write);
	if (error == 0)
		vcpu->state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
	return error;
}

static int
vmm_io_trap(struct vmm_machine *machine, enum vmm_io_space space,
    enum vmm_io_direction direction, uint64_t address,
    enum vmm_io_width width, union vmm_io_handler handler, void *argument,
    vmm_io_t *io)
{
	struct vmm_io *entry;

	if (machine == NULL || io == NULL ||
	    (width != VMM_IO_WIDTH_8 && width != VMM_IO_WIDTH_16 &&
	    width != VMM_IO_WIDTH_32 && width != VMM_IO_WIDTH_64))
		return EINVAL;
	if ((direction == VMM_IO_READ && handler.read == NULL) ||
	    (direction == VMM_IO_WRITE && handler.write == NULL))
		return EINVAL;
	if (space == VMM_IO_PIO && width == VMM_IO_WIDTH_64)
		return EINVAL;
	*io = NULL;
	entry = kmalloc(sizeof(*entry), M_VMM, M_WAITOK | M_ZERO);
	if (entry == NULL)
		return ENOMEM;
	entry->machine = machine;
	entry->handler = handler;
	entry->argument = argument;
	entry->address = address;
	entry->width = width;
	entry->space = space;
	entry->direction = direction;
	lwkt_gettoken(&machine->token);
	if (machine->destroying) {
		lwkt_reltoken(&machine->token);
		kfree(entry, M_VMM);
		return EBUSY;
	}
	TAILQ_INSERT_TAIL(&machine->io_list, entry, entry);
	lwkt_reltoken(&machine->token);
	*io = entry;
	return 0;
}

int
vmm_io_dispatch_read(struct vmm_vcpu *vcpu, enum vmm_io_space space,
    struct vmm_io_read *read)
{
	struct vmm_machine *machine = vcpu->machine;
	struct vmm_io *entry;
	int error = ENOENT;

	lwkt_gettoken(&machine->token);
	TAILQ_FOREACH(entry, &machine->io_list, entry) {
		if (entry->space != space || entry->direction != VMM_IO_READ ||
		    entry->address != read->address || entry->width != read->width)
			continue;
		error = entry->handler.read(vcpu, entry->argument, read);
		if (error != ENOENT)
			break;
	}
	lwkt_reltoken(&machine->token);
	return error;
}

int
vmm_io_dispatch_write(struct vmm_vcpu *vcpu, enum vmm_io_space space,
    const struct vmm_io_write *write)
{
	struct vmm_machine *machine = vcpu->machine;
	struct vmm_io *entry;
	int error = ENOENT;

	lwkt_gettoken(&machine->token);
	TAILQ_FOREACH(entry, &machine->io_list, entry) {
		if (entry->space != space || entry->direction != VMM_IO_WRITE ||
		    entry->address != write->address || entry->width != write->width)
			continue;
		error = entry->handler.write(vcpu, entry->argument, write);
		if (error != ENOENT)
			break;
	}
	lwkt_reltoken(&machine->token);
	return error;
}
