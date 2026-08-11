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

static int vmm_io_trap(struct vmm_machine *, enum vmm_io_space, uint64_t,
    enum vmm_io_width, vmm_io_handler_t, void *, vmm_io_t *);
static int vmm_io_dispatch(struct vmm_vcpu *, enum vmm_io_space,
    const struct vmm_io_write *);
static int vmm_io_decode_mmio_write(struct vmm_vcpu *,
    const struct vmm_cpuexit *, struct vmm_io_write *);

int
vmm_machine_trap_pio_write(vmm_machine_t machine, uint16_t address,
    enum vmm_io_width width, vmm_io_handler_t handler, void *argument,
    vmm_io_t *io)
{

	return vmm_io_trap(machine, VMM_IO_PIO, address, width, handler,
	    argument, io);
}

int
vmm_machine_trap_mmio_write(vmm_machine_t machine, uint64_t address,
    enum vmm_io_width width, vmm_io_handler_t handler, void *argument,
    vmm_io_t *io)
{

	return vmm_io_trap(machine, VMM_IO_MMIO, address, width, handler,
	    argument, io);
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
	struct vmm_io_write write;
	unsigned int size;
	int error;

	if (exit->u.io.in || exit->u.io.str || exit->u.io.rep)
		return ENOENT;
	size = exit->u.io.operand_size;
	if (size != 1 && size != 2 && size != 4)
		return ENOENT;
	vcpu->backend_ops->vcpu_getstate(vcpu);
	write.address = exit->u.io.port;
	write.width = (enum vmm_io_width)size;
	write.value = vcpu->state->gprs[VMM_X64_GPR_RAX];
	write.value &= (1ULL << (size * 8)) - 1;
	error = vmm_io_dispatch(vcpu, VMM_IO_PIO, &write);
	if (error == 0)
		vcpu->state->gprs[VMM_X64_GPR_RIP] = exit->u.io.npc;
	return error;
}

int
vmm_io_handle_mmio(struct vmm_vcpu *vcpu, const struct vmm_cpuexit *exit)
{
	struct vmm_io_write write;
	int error;

	if (exit->u.mem.prot != PROT_WRITE)
		return ENOENT;
	vcpu->backend_ops->vcpu_getstate(vcpu);
	error = vmm_io_decode_mmio_write(vcpu, exit, &write);
	if (error != 0)
		return error;
	if (write.width != VMM_IO_WIDTH_64)
		write.value &= (1ULL << (write.width * 8)) - 1;
	error = vmm_io_dispatch(vcpu, VMM_IO_MMIO, &write);
	if (error == 0)
		vcpu->state->gprs[VMM_X64_GPR_RIP] += exit->u.mem.inst_len;
	return error;
}

static int
vmm_io_trap(struct vmm_machine *machine, enum vmm_io_space space,
    uint64_t address, enum vmm_io_width width, vmm_io_handler_t handler,
    void *argument, vmm_io_t *io)
{
	struct vmm_io *entry;

	if (machine == NULL || handler == NULL || io == NULL ||
	    (width != VMM_IO_WIDTH_8 && width != VMM_IO_WIDTH_16 &&
	    width != VMM_IO_WIDTH_32 && width != VMM_IO_WIDTH_64))
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

static int
vmm_io_dispatch(struct vmm_vcpu *vcpu, enum vmm_io_space space,
    const struct vmm_io_write *write)
{
	struct vmm_machine *machine = vcpu->machine;
	struct vmm_io *entry;
	int error = ENOENT;

	lwkt_gettoken(&machine->token);
	TAILQ_FOREACH(entry, &machine->io_list, entry) {
		if (entry->space != space || entry->address != write->address ||
		    entry->width != write->width)
			continue;
		error = entry->handler(entry->argument, write);
		if (error != ENOENT)
			break;
	}
	lwkt_reltoken(&machine->token);
	return error;
}

static int
vmm_io_decode_mmio_write(struct vmm_vcpu *vcpu,
    const struct vmm_cpuexit *exit, struct vmm_io_write *write)
{
	const uint8_t *bytes = exit->u.mem.inst_bytes;
	unsigned int length = exit->u.mem.inst_len;
	unsigned int offset = 0;
	unsigned int size = 4;
	unsigned int reg;
	uint8_t rex = 0;
	uint8_t opcode;
	uint8_t modrm = 0;

	if (length == 0 || length > sizeof(exit->u.mem.inst_bytes))
		return ENOENT;
	for (;;) {
		if (offset == length)
			return ENOENT;
		if (bytes[offset] == 0x66) {
			size = 2;
			++offset;
			continue;
		}
		if (bytes[offset] == 0x67 || bytes[offset] == 0xf0) {
			++offset;
			continue;
		}
		if (bytes[offset] >= 0x40 && bytes[offset] <= 0x4f) {
			rex = bytes[offset++];
			continue;
		}
		break;
	}
	opcode = bytes[offset++];
	if ((opcode == 0x88 || opcode == 0x89 || opcode == 0xc6 ||
	    opcode == 0xc7) && offset == length)
		return ENOENT;
	if (opcode == 0x88 || opcode == 0x89 || opcode == 0xc6 ||
	    opcode == 0xc7) {
		modrm = bytes[offset++];
		if ((modrm & 0xc0U) == 0xc0U)
			return ENOENT;
	}
	if (opcode == 0x88)
		size = 1;
	else if (opcode == 0x89 || opcode == 0xc7) {
		if ((rex & 0x08U) != 0)
			size = 8;
	} else if (opcode == 0xa2)
		size = 1;
	else if (opcode == 0xa3 && (rex & 0x08U) != 0)
		size = 8;
	else if (opcode != 0xa3 && opcode != 0xc6)
		return ENOENT;
	write->address = exit->u.mem.gpa;
	write->width = (enum vmm_io_width)size;
	if (opcode == 0x88 || opcode == 0x89) {
		reg = ((modrm >> 3) & 7U) | ((rex & 0x04U) != 0 ? 8U : 0U);
		write->value = reg < 16 ? vcpu->state->gprs[reg] : 0;
		if (opcode == 0x88 && rex == 0 && reg >= 4 && reg <= 7)
			write->value >>= 8;
		return 0;
	}
	if (opcode == 0xa2 || opcode == 0xa3) {
		write->value = vcpu->state->gprs[VMM_X64_GPR_RAX];
		return 0;
	}
	if (opcode == 0xc6) {
		write->width = VMM_IO_WIDTH_8;
		write->value = bytes[length - 1];
		return 0;
	}
	if (size == 2) {
		if (length < 2)
			return ENOENT;
		write->value = bytes[length - 2] |
		    ((uint64_t)bytes[length - 1] << 8);
	} else {
		if (length < 4)
			return ENOENT;
		write->value = bytes[length - 4] |
		    ((uint64_t)bytes[length - 3] << 8) |
		    ((uint64_t)bytes[length - 2] << 16) |
		    ((uint64_t)bytes[length - 1] << 24);
		if (size == 8)
			write->value = (uint64_t)(int64_t)(int32_t)write->value;
	}
	return 0;
}
