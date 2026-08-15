/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * x86 MMIO instruction emulation for the VMM core.
 *
 * Copyright (c) 2018-2021 Maxime Villard, m00nbsd.net
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The decoder and instruction semantics derive from the NVMM memory-assist
 * implementation.  This VMM adaptation retains execution context across one
 * or more external MMIO fragments instead of invoking a synchronous callback.
 */
#include <sys/errno.h>
#include <sys/bitops.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/systm.h>

#include <machine/pmap.h>
#include <machine/psl.h>
#include <machine/specialreg.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_param.h>

#include "../vmm_backend.h"
#include "../vmm_io.h"
#include "../vmm_machine.h"
#include "../vmm_vcpu.h"
#include "vmm_x64.h"
#include "vmm_x64_emul.h"

#define VMM_X64_EMUL_MAX_ACCESS	8
#define __cacheline_aligned	__attribute__((__aligned__(64)))
#define VMM_EMUL_PROT_READ	0x01
#define VMM_EMUL_PROT_WRITE	0x02
#define VMM_EMUL_PROT_EXEC	0x04
#define VMM_EMUL_PROT_USER	0x08
#define VMM_EMUL_PROT_ALL	(VMM_EMUL_PROT_READ | VMM_EMUL_PROT_WRITE | \
	VMM_EMUL_PROT_EXEC | VMM_EMUL_PROT_USER)
#define VMM_X64_PTE_PRESENT	0x0000000000000001ULL
#define VMM_X64_PTE_WRITE	0x0000000000000002ULL
#define VMM_X64_PTE_USER	0x0000000000000004ULL
#define VMM_X64_PTE_LARGE	0x0000000000000080ULL
#define VMM_X64_PTE_NX		0x8000000000000000ULL
#define VMM_X64_PTE_FRAME	0x000ffffffffff000ULL
#define VMM_X64_PTE_2M_FRAME	0x000fffffffe00000ULL
#define VMM_X64_PTE_1G_FRAME	0x000fffffc0000000ULL

struct vmm_x64_emul_access {
	uint64_t gpa;
	uint64_t value;
	size_t size;
	bool write;
};

struct vmm_x64_emul {
	/* State is restored before replaying a partially completed instruction. */
	struct vmm_cpustate state;
	struct vmm_cpuexit exit;
	struct vmm_x64_emul_access access[VMM_X64_EMUL_MAX_ACCESS];
	unsigned int access_count;
	unsigned int completed_count;
	unsigned int next_access;
	int error;
	bool active;
	bool blocked;
};

struct vmm_x64_memory {
	struct vmm_vcpu *vcpu;
	uint64_t gpa;
	bool write;
	size_t size;
	uint8_t *data;
};

static int vmm_x64_emul_execute(struct vmm_vcpu *);
static void vmm_x64_emul_memory_access(struct vmm_vcpu *,
	struct vmm_x64_memory *);
static int read_guest_memory(struct vmm_machine *, struct vmm_vcpu *,
	uint64_t, uint8_t *, size_t);
static int write_guest_memory(struct vmm_machine *, struct vmm_vcpu *,
	uint64_t, uint8_t *, size_t);

int
vmm_x64_emul_init(struct vmm_vcpu *vcpu)
{

	vcpu->emul = kmalloc(sizeof(*vcpu->emul), M_VMM, M_WAITOK | M_ZERO);
	return vcpu->emul == NULL ? ENOMEM : 0;
}

void
vmm_x64_emul_uninit(struct vmm_vcpu *vcpu)
{

	if (vcpu->emul != NULL)
		kfree(vcpu->emul, M_VMM);
	vcpu->emul = NULL;
}

int
vmm_x64_emul_memory(struct vmm_vcpu *vcpu, const struct vmm_cpuexit *exit)
{
	struct vmm_x64_emul *emul = vcpu->emul;

	if (emul == NULL || emul->active)
		return EBUSY;
	emul->state = *vcpu->state;
	emul->exit = *exit;
	emul->access_count = 0;
	emul->completed_count = 0;
	emul->next_access = 0;
	emul->error = 0;
	emul->active = true;
	emul->blocked = false;
	return vmm_x64_emul_execute(vcpu);
}

int
vmm_x64_emul_resume(struct vmm_vcpu *vcpu)
{
	struct vmm_x64_emul *emul = vcpu->emul;

	if (emul == NULL || !emul->active)
		return ENOENT;
	if (emul->completed_count != emul->access_count)
		return EBUSY;
	*vcpu->state = emul->state;
	emul->next_access = 0;
	emul->blocked = false;
	return vmm_x64_emul_execute(vcpu);
}

int
vmm_x64_emul_complete_write(struct vmm_vcpu *vcpu)
{
	struct vmm_x64_emul *emul = vcpu->emul;
	struct vmm_x64_emul_access *access;

	if (emul == NULL || !emul->active || !emul->blocked || emul->error != 0 ||
	    emul->completed_count == emul->access_count)
		return EINVAL;
	access = &emul->access[emul->completed_count];
	if (!access->write)
		return EINVAL;
	++emul->completed_count;
	return 0;
}

int
vmm_x64_emul_complete_read(struct vmm_vcpu *vcpu, const void *data,
	size_t size)
{
	struct vmm_x64_emul *emul = vcpu->emul;
	struct vmm_x64_emul_access *access;

	if (emul == NULL || data == NULL || !emul->active || !emul->blocked ||
	    emul->error != 0 ||
	    emul->completed_count == emul->access_count)
		return EINVAL;
	access = &emul->access[emul->completed_count];
	if (access->write || access->size != size)
		return EINVAL;
	bcopy(data, &access->value, size);
	++emul->completed_count;
	return 0;
}

#define DISASSEMBLER_BUG()\
	do {\
		return -1;\
	} while (0)

static int
vmm_x64_emul_read_ram(struct vmm_machine *machine, uint64_t gpa,
	void *data, size_t size)
{
	void *pmap_handle;
	vm_paddr_t pa;
	uint64_t page_gpa;
	size_t chunk;
	size_t offset;
	int error;

	if (machine->vmspace == NULL || data == NULL || size == 0)
		return EFAULT;
	while (size != 0) {
		page_gpa = trunc_page(gpa);
		offset = (size_t)(gpa - page_gpa);
		chunk = PAGE_SIZE - offset;
		if (chunk > size)
			chunk = size;
		error = vm_fault(&machine->vmspace->vm_map, page_gpa,
		    VM_PROT_READ, VM_FAULT_NORMAL);
		if (error != KERN_SUCCESS)
			return EFAULT;
		pmap_handle = NULL;
		pa = pmap_extract(&machine->vmspace->vm_pmap, page_gpa,
		    &pmap_handle);
		if (pa == 0) {
			pmap_extract_done(pmap_handle);
			return EFAULT;
		}
		bcopy((const void *)(PHYS_TO_DMAP(pa) + offset), data, chunk);
		pmap_extract_done(pmap_handle);
		gpa += chunk;
		data = (uint8_t *)data + chunk;
		size -= chunk;
	}
	return 0;
}

static int
vmm_x64_emul_translate(struct vmm_vcpu *vcpu, uint64_t gva, uint64_t *gpa,
	uint64_t *prot)
{
	struct vmm_cpustate *state = vcpu->state;
	uint64_t entry;
	uint64_t table;

	if (gpa == NULL || prot == NULL)
		return EINVAL;
	*prot = VMM_EMUL_PROT_ALL;
	if ((state->crs[VMM_X64_CR_CR0] & CR0_PG) == 0) {
		*gpa = gva;
		return 0;
	}
	if ((state->msrs[VMM_X64_MSR_EFER] & EFER_LMA) == 0)
		return EOPNOTSUPP;
	if ((gva >> 48) != 0 && (gva >> 48) != 0xffffULL)
		return EFAULT;
	table = state->crs[VMM_X64_CR_CR3] & VMM_X64_PTE_FRAME;
	if (vmm_x64_emul_read_ram(vcpu->machine,
	    table + (((gva >> 39) & 0x1ffULL) * sizeof(entry)), &entry,
	    sizeof(entry)) != 0 || (entry & VMM_X64_PTE_PRESENT) == 0)
		return EFAULT;
	if ((entry & VMM_X64_PTE_USER) == 0)
		*prot &= ~VMM_EMUL_PROT_USER;
	if ((entry & VMM_X64_PTE_WRITE) == 0)
		*prot &= ~VMM_EMUL_PROT_WRITE;
	if ((entry & VMM_X64_PTE_NX) != 0)
		*prot &= ~VMM_EMUL_PROT_EXEC;
	table = entry & VMM_X64_PTE_FRAME;
	if (vmm_x64_emul_read_ram(vcpu->machine,
	    table + (((gva >> 30) & 0x1ffULL) * sizeof(entry)), &entry,
	    sizeof(entry)) != 0 || (entry & VMM_X64_PTE_PRESENT) == 0)
		return EFAULT;
	if ((entry & VMM_X64_PTE_USER) == 0)
		*prot &= ~VMM_EMUL_PROT_USER;
	if ((entry & VMM_X64_PTE_WRITE) == 0)
		*prot &= ~VMM_EMUL_PROT_WRITE;
	if ((entry & VMM_X64_PTE_NX) != 0)
		*prot &= ~VMM_EMUL_PROT_EXEC;
	if ((entry & VMM_X64_PTE_LARGE) != 0) {
		*gpa = (entry & VMM_X64_PTE_1G_FRAME) |
		    (gva & ((1ULL << 30) - 1));
		return 0;
	}
	table = entry & VMM_X64_PTE_FRAME;
	if (vmm_x64_emul_read_ram(vcpu->machine,
	    table + (((gva >> 21) & 0x1ffULL) * sizeof(entry)), &entry,
	    sizeof(entry)) != 0 || (entry & VMM_X64_PTE_PRESENT) == 0)
		return EFAULT;
	if ((entry & VMM_X64_PTE_USER) == 0)
		*prot &= ~VMM_EMUL_PROT_USER;
	if ((entry & VMM_X64_PTE_WRITE) == 0)
		*prot &= ~VMM_EMUL_PROT_WRITE;
	if ((entry & VMM_X64_PTE_NX) != 0)
		*prot &= ~VMM_EMUL_PROT_EXEC;
	if ((entry & VMM_X64_PTE_LARGE) != 0) {
		*gpa = (entry & VMM_X64_PTE_2M_FRAME) |
		    (gva & ((1ULL << 21) - 1));
		return 0;
	}
	table = entry & VMM_X64_PTE_FRAME;
	if (vmm_x64_emul_read_ram(vcpu->machine,
	    table + (((gva >> 12) & 0x1ffULL) * sizeof(entry)), &entry,
	    sizeof(entry)) != 0 || (entry & VMM_X64_PTE_PRESENT) == 0)
		return EFAULT;
	if ((entry & VMM_X64_PTE_USER) == 0)
		*prot &= ~VMM_EMUL_PROT_USER;
	if ((entry & VMM_X64_PTE_WRITE) == 0)
		*prot &= ~VMM_EMUL_PROT_WRITE;
	if ((entry & VMM_X64_PTE_NX) != 0)
		*prot &= ~VMM_EMUL_PROT_EXEC;
	*gpa = (entry & VMM_X64_PTE_FRAME) | (gva & PAGE_MASK);
	return 0;
}

int
vmm_x64_translate(struct vmm_vcpu *vcpu, uint64_t gva, uint64_t *gpa)
{
	uint64_t prot;

	if (vcpu == NULL || gpa == NULL)
		return EINVAL;
	return vmm_x64_emul_translate(vcpu, gva, gpa, &prot);
}

size_t
vmm_x64_fetch_instruction(struct vmm_vcpu *vcpu, uint8_t *data,
	size_t size)
{
	struct vmm_cpustate *state;
	uint64_t gpa;
	uint64_t gva;
	uint64_t prot;
	size_t chunk;
	size_t fetched;
	int error;

	if (vcpu == NULL || data == NULL || size == 0)
		return 0;
	state = vcpu->state;
	if (state->gprs[VMM_X64_GPR_RIP] >
	    UINT64_MAX - state->segs[VMM_X64_SEG_CS].base)
		return 0;
	gva = state->segs[VMM_X64_SEG_CS].base +
	    state->gprs[VMM_X64_GPR_RIP];
	fetched = 0;
	while (size != 0) {
		error = vmm_x64_emul_translate(vcpu, gva, &gpa, &prot);
		if (error != 0 || (prot & VMM_EMUL_PROT_EXEC) == 0)
			break;
		chunk = MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK));
		error = vmm_x64_emul_read_ram(vcpu->machine, gpa, data, chunk);
		if (error != 0)
			break;
		fetched += chunk;
		if (size != chunk && gva > UINT64_MAX - chunk)
			break;
		gva += chunk;
		data += chunk;
		size -= chunk;
	}
	return fetched;
}

static int
vmm_x64_emul_gpa_access(struct vmm_vcpu *vcpu, uint64_t gpa, uint8_t *data,
	size_t size, bool write)
{
	struct vmm_x64_emul *emul = vcpu->emul;
	void *pmap_handle;
	vm_paddr_t pa;
	uint64_t page_gpa;
	size_t chunk;
	size_t offset;
	int error;

	while (size != 0) {
		if (emul->blocked) {
			if (!write)
				bzero(data, size);
			return 0;
		}
		page_gpa = trunc_page(gpa);
		offset = (size_t)(gpa - page_gpa);
		chunk = PAGE_SIZE - offset;
		if (chunk > size)
			chunk = size;
		error = vm_fault(&vcpu->machine->vmspace->vm_map, page_gpa,
		    write ? VM_PROT_WRITE : VM_PROT_READ,
		    write ? VM_FAULT_DIRTY : VM_FAULT_NORMAL);
		if (error != KERN_SUCCESS) {
			struct vmm_x64_memory memory = {
				.vcpu = vcpu,
				.gpa = gpa,
				.write = write,
				.size = chunk,
				.data = data,
			};

			vmm_x64_emul_memory_access(vcpu, &memory);
		} else {
			pmap_handle = NULL;
			pa = pmap_extract(&vcpu->machine->vmspace->vm_pmap,
			    page_gpa, &pmap_handle);
			if (pa == 0) {
				pmap_extract_done(pmap_handle);
				return EFAULT;
			}
			if (write)
				bcopy(data, (void *)(PHYS_TO_DMAP(pa) + offset), chunk);
			else
				bcopy((const void *)(PHYS_TO_DMAP(pa) + offset), data,
				    chunk);
			pmap_extract_done(pmap_handle);
		}
		if (emul->blocked)
			return 0;
		gpa += chunk;
		data += chunk;
		size -= chunk;
	}
	return 0;
}

static bool
is_long_mode(struct vmm_cpustate *state)
{

	return (state->msrs[VMM_X64_MSR_EFER] & EFER_LMA) != 0;
}

static bool
is_64bit(struct vmm_cpustate *state)
{

	return state->segs[VMM_X64_SEG_CS].attrib.l != 0;
}

static bool
is_32bit(struct vmm_cpustate *state)
{

	return state->segs[VMM_X64_SEG_CS].attrib.l == 0 &&
	    state->segs[VMM_X64_SEG_CS].attrib.def != 0;
}

static bool
is_16bit(struct vmm_cpustate *state)
{

	return state->segs[VMM_X64_SEG_CS].attrib.l == 0 &&
	    state->segs[VMM_X64_SEG_CS].attrib.def == 0;
}

static int
segment_check(struct vmm_segment *segment, uint64_t gva, size_t size)
{
	uint64_t limit;

	if (segment->attrib.p == 0)
		return EFAULT;
	limit = (uint64_t)segment->limit + 1;
	if (segment->attrib.g != 0)
		limit *= PAGE_SIZE;
	return gva + size <= limit ? 0 : EFAULT;
}

static void
segment_apply(struct vmm_segment *segment, uint64_t *gva)
{

	*gva += segment->base;
}

static uint64_t
size_to_mask(size_t size)
{
	switch (size) {
	case 1:
		return 0x00000000000000ffULL;
	case 2:
		return 0x000000000000ffffULL;
	case 4:
		return 0x00000000ffffffffULL;
	default:
		return 0xffffffffffffffffULL;
	}
}

static uint64_t
rep_get_cnt(struct vmm_cpustate *state, size_t address_size)
{

	return state->gprs[VMM_X64_GPR_RCX] & size_to_mask(address_size);
}

static void
rep_set_cnt(struct vmm_cpustate *state, size_t address_size, uint64_t count)
{
	uint64_t mask = size_to_mask(address_size);

	state->gprs[VMM_X64_GPR_RCX] &= ~mask;
	state->gprs[VMM_X64_GPR_RCX] |= count;
}

static int
read_guest_memory(struct vmm_machine *machine, struct vmm_vcpu *vcpu,
	uint64_t gva, uint8_t *data, size_t size)
{
	uint64_t gpa;
	uint64_t prot;
	int error;

	(void)machine;
	while (size != 0) {
		error = vmm_x64_emul_translate(vcpu, gva, &gpa, &prot);
		if (error != 0 || (prot & VMM_EMUL_PROT_READ) == 0)
			return EFAULT;
		error = vmm_x64_emul_gpa_access(vcpu, gpa, data,
		    MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK)), false);
		if (error != 0)
			return error;
		if (vcpu->emul->blocked)
			return 0;
		gva += MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK));
		data += MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK));
		size -= MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK));
	}
	return 0;
}

static int
write_guest_memory(struct vmm_machine *machine, struct vmm_vcpu *vcpu,
	uint64_t gva, uint8_t *data, size_t size)
{
	uint64_t gpa;
	uint64_t prot;
	int error;

	(void)machine;
	while (size != 0) {
		error = vmm_x64_emul_translate(vcpu, gva, &gpa, &prot);
		if (error != 0 || (prot & VMM_EMUL_PROT_WRITE) == 0)
			return EFAULT;
		error = vmm_x64_emul_gpa_access(vcpu, gpa, data,
		    MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK)), true);
		if (error != 0)
			return error;
		if (vcpu->emul->blocked)
			return 0;
		gva += MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK));
		data += MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK));
		size -= MIN(size, PAGE_SIZE - (size_t)(gpa & PAGE_MASK));
	}
	return 0;
}

static void
vmm_x64_emul_memory_access(struct vmm_vcpu *vcpu,
	struct vmm_x64_memory *memory)
{
	struct vmm_x64_emul *emul = vcpu->emul;
	struct vmm_x64_emul_access *access;
	struct vmm_io_write write;
	uint64_t value;
	unsigned int index;
	int error;

	if (emul->blocked)
		return;
	if (memory->size != VMM_IO_WIDTH_8 &&
	    memory->size != VMM_IO_WIDTH_16 &&
	    memory->size != VMM_IO_WIDTH_32 &&
	    memory->size != VMM_IO_WIDTH_64) {
		emul->error = EOPNOTSUPP;
		emul->blocked = true;
		return;
	}
	index = emul->next_access++;
	if (index < emul->completed_count) {
		access = &emul->access[index];
		if (access->gpa != memory->gpa || access->size != memory->size ||
		    access->write != memory->write) {
			emul->error = EPROTO;
			emul->blocked = true;
			return;
		}
		if (!memory->write)
			bcopy(&access->value, memory->data, memory->size);
		return;
	}
	if (index != emul->access_count || index == VMM_X64_EMUL_MAX_ACCESS) {
		emul->error = E2BIG;
		emul->blocked = true;
		return;
	}
	access = &emul->access[index];
	access->gpa = memory->gpa;
	access->size = memory->size;
	access->write = memory->write;
	access->value = 0;
	if (memory->write)
		bcopy(memory->data, &access->value, memory->size);
	++emul->access_count;
	value = access->value;
	error = ENOENT;
	if (vcpu->backend_ops->vcpu_mmio != NULL) {
		error = vcpu->backend_ops->vcpu_mmio(vcpu, memory->gpa,
		    memory->size, memory->write, &value);
	}
	if (error == 0) {
		access->value = value;
		if (!memory->write)
			bcopy(&access->value, memory->data, memory->size);
		++emul->completed_count;
		return;
	}
	if (error != ENOENT) {
		emul->error = error;
		emul->blocked = true;
		return;
	}
	if (memory->write) {
		write.address = memory->gpa;
		write.width = (enum vmm_io_width)memory->size;
		write.value = access->value;
		error = vmm_io_dispatch(vcpu, VMM_IO_MMIO, &write);
		if (error == 0) {
			++emul->completed_count;
			return;
		}
		if (error != ENOENT) {
			emul->error = error;
			emul->blocked = true;
			return;
		}
	}
	emul->blocked = true;
	vcpu->exit = emul->exit;
	vcpu->exit.reason = VMM_CPUEXIT_MEMORY;
	vcpu->exit.u.mem.gpa = memory->gpa;
	vcpu->exit.u.mem.prot = memory->write ? VM_PROT_WRITE : VM_PROT_READ;
	vcpu->exit.u.mem.width = (enum vmm_io_width)memory->size;
	vcpu->exit.u.mem.value = access->value;
}

/* -------------------------------------------------------------------------- */

struct x86_emul {
	bool readreg;
	bool backprop;
	bool notouch;
	void (*func)(struct vmm_vcpu *, struct vmm_x64_memory *);
};

static void x86_func_or(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_and(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_xchg(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_sub(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_xor(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_cmp(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_test(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_mov(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_stos(struct vmm_vcpu *, struct vmm_x64_memory *);
static void x86_func_lods(struct vmm_vcpu *, struct vmm_x64_memory *);

static const struct x86_emul x86_emul_or = {
	.readreg = true,
	.func = x86_func_or
};

static const struct x86_emul x86_emul_and = {
	.readreg = true,
	.func = x86_func_and
};

static const struct x86_emul x86_emul_xchg = {
	.readreg = true,
	.backprop = true,
	.func = x86_func_xchg
};

static const struct x86_emul x86_emul_sub = {
	.readreg = true,
	.func = x86_func_sub
};

static const struct x86_emul x86_emul_xor = {
	.readreg = true,
	.func = x86_func_xor
};

static const struct x86_emul x86_emul_cmp = {
	.notouch = true,
	.func = x86_func_cmp
};

static const struct x86_emul x86_emul_test = {
	.notouch = true,
	.func = x86_func_test
};

static const struct x86_emul x86_emul_mov = {
	.func = x86_func_mov
};

static const struct x86_emul x86_emul_stos = {
	.func = x86_func_stos
};

static const struct x86_emul x86_emul_lods = {
	.func = x86_func_lods
};

/* Legacy prefixes. */
#define LEG_LOCK	0xF0
#define LEG_REPN	0xF2
#define LEG_REP		0xF3
#define LEG_OVR_CS	0x2E
#define LEG_OVR_SS	0x36
#define LEG_OVR_DS	0x3E
#define LEG_OVR_ES	0x26
#define LEG_OVR_FS	0x64
#define LEG_OVR_GS	0x65
#define LEG_OPR_OVR	0x66
#define LEG_ADR_OVR	0x67

struct x86_legpref {
	bool opr_ovr:1;
	bool adr_ovr:1;
	bool rep:1;
	bool repn:1;
	int8_t seg;
};

struct x86_rexpref {
	bool b:1;
	bool x:1;
	bool r:1;
	bool w:1;
	bool present:1;
};

struct x86_reg {
	int num;	/* NVMM GPR state index */
	uint64_t mask;
};

struct x86_dualreg {
	int reg1;
	int reg2;
};

enum x86_disp_type {
	DISP_NONE,
	DISP_0,
	DISP_1,
	DISP_2,
	DISP_4
};

struct x86_disp {
	enum x86_disp_type type;
	uint64_t data; /* 4 bytes, but can be sign-extended */
};

struct x86_regmodrm {
	uint8_t mod:2;
	uint8_t reg:3;
	uint8_t rm:3;
};

struct x86_immediate {
	uint64_t data;
};

struct x86_sib {
	uint8_t scale;
	const struct x86_reg *idx;
	const struct x86_reg *bas;
};

enum x86_store_type {
	STORE_NONE,
	STORE_REG,
	STORE_DUALREG,
	STORE_IMM,
	STORE_SIB,
	STORE_DMO
};

struct x86_store {
	enum x86_store_type type;
	union {
		const struct x86_reg *reg;
		struct x86_dualreg dualreg;
		struct x86_immediate imm;
		struct x86_sib sib;
		uint64_t dmo;
	} u;
	struct x86_disp disp;
	int hardseg;
};

struct x86_instr {
	uint8_t len;
	struct x86_legpref legpref;
	struct x86_rexpref rexpref;
	struct x86_regmodrm regmodrm;
	uint8_t operand_size;
	uint8_t address_size;
	uint64_t zeroextend_mask;

	const struct x86_opcode *opcode;
	const struct x86_emul *emul;

	struct x86_store src;
	struct x86_store dst;
	struct x86_store *strm;
};

struct x86_decode_fsm {
	/* vcpu */
	bool is64bit;
	bool is32bit;
	bool is16bit;

	/* fsm */
	int (*fn)(struct x86_decode_fsm *, struct x86_instr *);
	uint8_t *buf;
	uint8_t *end;
};

struct x86_opcode {
	bool valid:1;
	bool regmodrm:1;
	bool regtorm:1;
	bool dmo:1;
	bool todmo:1;
	bool movs:1;
	bool stos:1;
	bool lods:1;
	bool szoverride:1;
	bool group1:1;
	bool group3:1;
	bool group11:1;
	bool immediate:1;
	uint8_t defsize;
	uint8_t flags;
	const struct x86_emul *emul;
};

struct x86_group_entry {
	const struct x86_emul *emul;
};

#define OPSIZE_BYTE 0x01
#define OPSIZE_WORD 0x02 /* 2 bytes */
#define OPSIZE_DOUB 0x04 /* 4 bytes */
#define OPSIZE_QUAD 0x08 /* 8 bytes */

#define FLAG_imm8	0x01
#define FLAG_immz	0x02
#define FLAG_ze		0x04

static const struct x86_group_entry group1[8] __cacheline_aligned = {
	[1] = { .emul = &x86_emul_or },
	[4] = { .emul = &x86_emul_and },
	[6] = { .emul = &x86_emul_xor },
	[7] = { .emul = &x86_emul_cmp }
};

static const struct x86_group_entry group3[8] __cacheline_aligned = {
	[0] = { .emul = &x86_emul_test },
	[1] = { .emul = &x86_emul_test }
};

static const struct x86_group_entry group11[8] __cacheline_aligned = {
	[0] = { .emul = &x86_emul_mov }
};

static const struct x86_opcode primary_opcode_table[256] __cacheline_aligned = {
	/*
	 * Group1
	 */
	[0x80] = {
		/* Eb, Ib */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.group1 = true,
		.immediate = true,
		.emul = NULL /* group1 */
	},
	[0x81] = {
		/* Ev, Iz */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.group1 = true,
		.immediate = true,
		.flags = FLAG_immz,
		.emul = NULL /* group1 */
	},
	[0x83] = {
		/* Ev, Ib */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.group1 = true,
		.immediate = true,
		.flags = FLAG_imm8,
		.emul = NULL /* group1 */
	},

	/*
	 * Group3
	 */
	[0xF6] = {
		/* Eb, Ib */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.group3 = true,
		.immediate = true,
		.emul = NULL /* group3 */
	},
	[0xF7] = {
		/* Ev, Iz */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.group3 = true,
		.immediate = true,
		.flags = FLAG_immz,
		.emul = NULL /* group3 */
	},

	/*
	 * Group11
	 */
	[0xC6] = {
		/* Eb, Ib */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.group11 = true,
		.immediate = true,
		.emul = NULL /* group11 */
	},
	[0xC7] = {
		/* Ev, Iz */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.group11 = true,
		.immediate = true,
		.flags = FLAG_immz,
		.emul = NULL /* group11 */
	},

	/*
	 * OR
	 */
	[0x08] = {
		/* Eb, Gb */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_or
	},
	[0x09] = {
		/* Ev, Gv */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_or
	},
	[0x0A] = {
		/* Gb, Eb */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_or
	},
	[0x0B] = {
		/* Gv, Ev */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_or
	},

	/*
	 * AND
	 */
	[0x20] = {
		/* Eb, Gb */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_and
	},
	[0x21] = {
		/* Ev, Gv */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_and
	},
	[0x22] = {
		/* Gb, Eb */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_and
	},
	[0x23] = {
		/* Gv, Ev */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_and
	},

	/*
	 * SUB
	 */
	[0x28] = {
		/* Eb, Gb */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_sub
	},
	[0x29] = {
		/* Ev, Gv */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_sub
	},
	[0x2A] = {
		/* Gb, Eb */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_sub
	},
	[0x2B] = {
		/* Gv, Ev */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_sub
	},

	/*
	 * XOR
	 */
	[0x30] = {
		/* Eb, Gb */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_xor
	},
	[0x31] = {
		/* Ev, Gv */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_xor
	},
	[0x32] = {
		/* Gb, Eb */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_xor
	},
	[0x33] = {
		/* Gv, Ev */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_xor
	},

	/*
	 * XCHG
	 */
	[0x86] = {
		/* Eb, Gb */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_xchg
	},
	[0x87] = {
		/* Ev, Gv */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_xchg
	},

	/*
	 * MOV
	 */
	[0x88] = {
		/* Eb, Gb */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_mov
	},
	[0x89] = {
		/* Ev, Gv */
		.valid = true,
		.regmodrm = true,
		.regtorm = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_mov
	},
	[0x8A] = {
		/* Gb, Eb */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_mov
	},
	[0x8B] = {
		/* Gv, Ev */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_mov
	},
	[0xA0] = {
		/* AL, Ob */
		.valid = true,
		.dmo = true,
		.todmo = false,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_mov
	},
	[0xA1] = {
		/* rAX, Ov */
		.valid = true,
		.dmo = true,
		.todmo = false,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_mov
	},
	[0xA2] = {
		/* Ob, AL */
		.valid = true,
		.dmo = true,
		.todmo = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_mov
	},
	[0xA3] = {
		/* Ov, rAX */
		.valid = true,
		.dmo = true,
		.todmo = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_mov
	},

	/*
	 * MOVS
	 */
	[0xA4] = {
		/* Yb, Xb */
		.valid = true,
		.movs = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = NULL /* assist_mem_double_movs */
	},
	[0xA5] = {
		/* Yv, Xv */
		.valid = true,
		.movs = true,
		.szoverride = true,
		.defsize = -1,
		.emul = NULL /* assist_mem_double_movs */
	},

	/*
	 * STOS
	 */
	[0xAA] = {
		/* Yb, AL */
		.valid = true,
		.stos = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_stos
	},
	[0xAB] = {
		/* Yv, rAX */
		.valid = true,
		.stos = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_stos
	},

	/*
	 * LODS
	 */
	[0xAC] = {
		/* AL, Xb */
		.valid = true,
		.lods = true,
		.szoverride = false,
		.defsize = OPSIZE_BYTE,
		.emul = &x86_emul_lods
	},
	[0xAD] = {
		/* rAX, Xv */
		.valid = true,
		.lods = true,
		.szoverride = true,
		.defsize = -1,
		.emul = &x86_emul_lods
	},
};

static const struct x86_opcode secondary_opcode_table[256] __cacheline_aligned = {
	/*
	 * MOVZX
	 */
	[0xB6] = {
		/* Gv, Eb */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = true,
		.defsize = OPSIZE_BYTE,
		.flags = FLAG_ze,
		.emul = &x86_emul_mov
	},
	[0xB7] = {
		/* Gv, Ew */
		.valid = true,
		.regmodrm = true,
		.regtorm = false,
		.szoverride = true,
		.defsize = OPSIZE_WORD,
		.flags = FLAG_ze,
		.emul = &x86_emul_mov
	},
};

static const struct x86_reg gpr_map__rip = { VMM_X64_GPR_RIP, 0xFFFFFFFFFFFFFFFF };

/* [REX-present][enc][opsize] */
static const struct x86_reg gpr_map__special[2][4][8] __cacheline_aligned = {
	[false] = {
		/* No REX prefix. */
		[0b00] = {
			[0] = { VMM_X64_GPR_RAX, 0x000000000000FF00 }, /* AH */
			[1] = { VMM_X64_GPR_RSP, 0x000000000000FFFF }, /* SP */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RSP, 0x00000000FFFFFFFF }, /* ESP */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { -1, 0 },
		},
		[0b01] = {
			[0] = { VMM_X64_GPR_RCX, 0x000000000000FF00 }, /* CH */
			[1] = { VMM_X64_GPR_RBP, 0x000000000000FFFF }, /* BP */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RBP, 0x00000000FFFFFFFF },	/* EBP */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { -1, 0 },
		},
		[0b10] = {
			[0] = { VMM_X64_GPR_RDX, 0x000000000000FF00 }, /* DH */
			[1] = { VMM_X64_GPR_RSI, 0x000000000000FFFF }, /* SI */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RSI, 0x00000000FFFFFFFF }, /* ESI */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { -1, 0 },
		},
		[0b11] = {
			[0] = { VMM_X64_GPR_RBX, 0x000000000000FF00 }, /* BH */
			[1] = { VMM_X64_GPR_RDI, 0x000000000000FFFF }, /* DI */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RDI, 0x00000000FFFFFFFF }, /* EDI */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { -1, 0 },
		}
	},
	[true] = {
		/* Has REX prefix. */
		[0b00] = {
			[0] = { VMM_X64_GPR_RSP, 0x00000000000000FF }, /* SPL */
			[1] = { VMM_X64_GPR_RSP, 0x000000000000FFFF }, /* SP */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RSP, 0x00000000FFFFFFFF }, /* ESP */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_RSP, 0xFFFFFFFFFFFFFFFF }, /* RSP */
		},
		[0b01] = {
			[0] = { VMM_X64_GPR_RBP, 0x00000000000000FF }, /* BPL */
			[1] = { VMM_X64_GPR_RBP, 0x000000000000FFFF }, /* BP */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RBP, 0x00000000FFFFFFFF }, /* EBP */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_RBP, 0xFFFFFFFFFFFFFFFF }, /* RBP */
		},
		[0b10] = {
			[0] = { VMM_X64_GPR_RSI, 0x00000000000000FF }, /* SIL */
			[1] = { VMM_X64_GPR_RSI, 0x000000000000FFFF }, /* SI */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RSI, 0x00000000FFFFFFFF }, /* ESI */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_RSI, 0xFFFFFFFFFFFFFFFF }, /* RSI */
		},
		[0b11] = {
			[0] = { VMM_X64_GPR_RDI, 0x00000000000000FF }, /* DIL */
			[1] = { VMM_X64_GPR_RDI, 0x000000000000FFFF }, /* DI */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RDI, 0x00000000FFFFFFFF }, /* EDI */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_RDI, 0xFFFFFFFFFFFFFFFF }, /* RDI */
		}
	}
};

/* [depends][enc][size] */
static const struct x86_reg gpr_map[2][8][8] __cacheline_aligned = {
	[false] = {
		/* Not extended. */
		[0b000] = {
			[0] = { VMM_X64_GPR_RAX, 0x00000000000000FF }, /* AL */
			[1] = { VMM_X64_GPR_RAX, 0x000000000000FFFF }, /* AX */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RAX, 0x00000000FFFFFFFF }, /* EAX */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_RAX, 0xFFFFFFFFFFFFFFFF }, /* RAX */
		},
		[0b001] = {
			[0] = { VMM_X64_GPR_RCX, 0x00000000000000FF }, /* CL */
			[1] = { VMM_X64_GPR_RCX, 0x000000000000FFFF }, /* CX */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RCX, 0x00000000FFFFFFFF }, /* ECX */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_RCX, 0xFFFFFFFFFFFFFFFF }, /* RCX */
		},
		[0b010] = {
			[0] = { VMM_X64_GPR_RDX, 0x00000000000000FF }, /* DL */
			[1] = { VMM_X64_GPR_RDX, 0x000000000000FFFF }, /* DX */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RDX, 0x00000000FFFFFFFF }, /* EDX */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_RDX, 0xFFFFFFFFFFFFFFFF }, /* RDX */
		},
		[0b011] = {
			[0] = { VMM_X64_GPR_RBX, 0x00000000000000FF }, /* BL */
			[1] = { VMM_X64_GPR_RBX, 0x000000000000FFFF }, /* BX */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_RBX, 0x00000000FFFFFFFF }, /* EBX */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_RBX, 0xFFFFFFFFFFFFFFFF }, /* RBX */
		},
		[0b100] = {
			[0] = { -1, 0 }, /* SPECIAL */
			[1] = { -1, 0 }, /* SPECIAL */
			[2] = { -1, 0 },
			[3] = { -1, 0 }, /* SPECIAL */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { -1, 0 }, /* SPECIAL */
		},
		[0b101] = {
			[0] = { -1, 0 }, /* SPECIAL */
			[1] = { -1, 0 }, /* SPECIAL */
			[2] = { -1, 0 },
			[3] = { -1, 0 }, /* SPECIAL */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { -1, 0 }, /* SPECIAL */
		},
		[0b110] = {
			[0] = { -1, 0 }, /* SPECIAL */
			[1] = { -1, 0 }, /* SPECIAL */
			[2] = { -1, 0 },
			[3] = { -1, 0 }, /* SPECIAL */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { -1, 0 }, /* SPECIAL */
		},
		[0b111] = {
			[0] = { -1, 0 }, /* SPECIAL */
			[1] = { -1, 0 }, /* SPECIAL */
			[2] = { -1, 0 },
			[3] = { -1, 0 }, /* SPECIAL */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { -1, 0 }, /* SPECIAL */
		},
	},
	[true] = {
		/* Extended. */
		[0b000] = {
			[0] = { VMM_X64_GPR_R8, 0x00000000000000FF }, /* R8B */
			[1] = { VMM_X64_GPR_R8, 0x000000000000FFFF }, /* R8W */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_R8, 0x00000000FFFFFFFF }, /* R8D */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_R8, 0xFFFFFFFFFFFFFFFF }, /* R8 */
		},
		[0b001] = {
			[0] = { VMM_X64_GPR_R9, 0x00000000000000FF }, /* R9B */
			[1] = { VMM_X64_GPR_R9, 0x000000000000FFFF }, /* R9W */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_R9, 0x00000000FFFFFFFF }, /* R9D */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_R9, 0xFFFFFFFFFFFFFFFF }, /* R9 */
		},
		[0b010] = {
			[0] = { VMM_X64_GPR_R10, 0x00000000000000FF }, /* R10B */
			[1] = { VMM_X64_GPR_R10, 0x000000000000FFFF }, /* R10W */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_R10, 0x00000000FFFFFFFF }, /* R10D */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_R10, 0xFFFFFFFFFFFFFFFF }, /* R10 */
		},
		[0b011] = {
			[0] = { VMM_X64_GPR_R11, 0x00000000000000FF }, /* R11B */
			[1] = { VMM_X64_GPR_R11, 0x000000000000FFFF }, /* R11W */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_R11, 0x00000000FFFFFFFF }, /* R11D */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_R11, 0xFFFFFFFFFFFFFFFF }, /* R11 */
		},
		[0b100] = {
			[0] = { VMM_X64_GPR_R12, 0x00000000000000FF }, /* R12B */
			[1] = { VMM_X64_GPR_R12, 0x000000000000FFFF }, /* R12W */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_R12, 0x00000000FFFFFFFF }, /* R12D */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_R12, 0xFFFFFFFFFFFFFFFF }, /* R12 */
		},
		[0b101] = {
			[0] = { VMM_X64_GPR_R13, 0x00000000000000FF }, /* R13B */
			[1] = { VMM_X64_GPR_R13, 0x000000000000FFFF }, /* R13W */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_R13, 0x00000000FFFFFFFF }, /* R13D */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_R13, 0xFFFFFFFFFFFFFFFF }, /* R13 */
		},
		[0b110] = {
			[0] = { VMM_X64_GPR_R14, 0x00000000000000FF }, /* R14B */
			[1] = { VMM_X64_GPR_R14, 0x000000000000FFFF }, /* R14W */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_R14, 0x00000000FFFFFFFF }, /* R14D */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_R14, 0xFFFFFFFFFFFFFFFF }, /* R14 */
		},
		[0b111] = {
			[0] = { VMM_X64_GPR_R15, 0x00000000000000FF }, /* R15B */
			[1] = { VMM_X64_GPR_R15, 0x000000000000FFFF }, /* R15W */
			[2] = { -1, 0 },
			[3] = { VMM_X64_GPR_R15, 0x00000000FFFFFFFF }, /* R15D */
			[4] = { -1, 0 },
			[5] = { -1, 0 },
			[6] = { -1, 0 },
			[7] = { VMM_X64_GPR_R15, 0xFFFFFFFFFFFFFFFF }, /* R15 */
		},
	}
};

/* [enc] */
static const int gpr_dual_reg1_rm[8] __cacheline_aligned = {
	[0b000] = VMM_X64_GPR_RBX, /* BX (+SI) */
	[0b001] = VMM_X64_GPR_RBX, /* BX (+DI) */
	[0b010] = VMM_X64_GPR_RBP, /* BP (+SI) */
	[0b011] = VMM_X64_GPR_RBP, /* BP (+DI) */
	[0b100] = VMM_X64_GPR_RSI, /* SI */
	[0b101] = VMM_X64_GPR_RDI, /* DI */
	[0b110] = VMM_X64_GPR_RBP, /* BP */
	[0b111] = VMM_X64_GPR_RBX, /* BX */
};

static int
node_overflow(struct x86_decode_fsm *fsm, struct x86_instr *instr __unused)
{
	fsm->fn = NULL;
	return -1;
}

static int
fsm_read(struct x86_decode_fsm *fsm, uint8_t *bytes, size_t n)
{
	if (fsm->buf + n > fsm->end) {
		return -1;
	}
	memcpy(bytes, fsm->buf, n);
	return 0;
}

static inline void
fsm_advance(struct x86_decode_fsm *fsm, size_t n,
    int (*fn)(struct x86_decode_fsm *, struct x86_instr *))
{
	fsm->buf += n;
	if (fsm->buf > fsm->end) {
		fsm->fn = node_overflow;
	} else {
		fsm->fn = fn;
	}
}

static const struct x86_reg *
resolve_special_register(struct x86_instr *instr, uint8_t enc, size_t regsize)
{
	enc &= 0b11;
	if (regsize == 8) {
		/* May be 64bit without REX */
		return &gpr_map__special[1][enc][regsize-1];
	}
	return &gpr_map__special[instr->rexpref.present][enc][regsize-1];
}

/*
 * Special node, for MOVS. Fake two displacements of zero on the source and
 * destination registers.
 */
static int
node_movs(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	size_t adrsize;

	adrsize = instr->address_size;

	/* DS:RSI */
	instr->src.type = STORE_REG;
	instr->src.u.reg = &gpr_map__special[1][2][adrsize-1];
	instr->src.disp.type = DISP_0;

	/* ES:RDI, force ES */
	instr->dst.type = STORE_REG;
	instr->dst.u.reg = &gpr_map__special[1][3][adrsize-1];
	instr->dst.disp.type = DISP_0;
	instr->dst.hardseg = VMM_X64_SEG_ES;

	fsm_advance(fsm, 0, NULL);

	return 0;
}

/*
 * Special node, for STOS and LODS. Fake a displacement of zero on the
 * destination register.
 */
static int
node_stlo(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	const struct x86_opcode *opcode = instr->opcode;
	struct x86_store *stlo, *streg;
	size_t adrsize, regsize;

	adrsize = instr->address_size;
	regsize = instr->operand_size;

	if (opcode->stos) {
		streg = &instr->src;
		stlo = &instr->dst;
	} else {
		streg = &instr->dst;
		stlo = &instr->src;
	}

	streg->type = STORE_REG;
	streg->u.reg = &gpr_map[0][0][regsize-1]; /* ?AX */

	stlo->type = STORE_REG;
	if (opcode->stos) {
		/* ES:RDI, force ES */
		stlo->u.reg = &gpr_map__special[1][3][adrsize-1];
		stlo->hardseg = VMM_X64_SEG_ES;
	} else {
		/* DS:RSI */
		stlo->u.reg = &gpr_map__special[1][2][adrsize-1];
	}
	stlo->disp.type = DISP_0;

	fsm_advance(fsm, 0, NULL);

	return 0;
}

static int
node_dmo(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	const struct x86_opcode *opcode = instr->opcode;
	struct x86_store *stdmo, *streg;
	size_t adrsize, regsize;

	adrsize = instr->address_size;
	regsize = instr->operand_size;

	if (opcode->todmo) {
		streg = &instr->src;
		stdmo = &instr->dst;
	} else {
		streg = &instr->dst;
		stdmo = &instr->src;
	}

	streg->type = STORE_REG;
	streg->u.reg = &gpr_map[0][0][regsize-1]; /* ?AX */

	stdmo->type = STORE_DMO;
	if (fsm_read(fsm, (uint8_t *)&stdmo->u.dmo, adrsize) == -1) {
		return -1;
	}
	fsm_advance(fsm, adrsize, NULL);

	return 0;
}

static inline uint64_t
sign_extend(uint64_t val, int size)
{
	if (size == 1) {
		if (val & __BIT(7))
			val |= 0xFFFFFFFFFFFFFF00;
	} else if (size == 2) {
		if (val & __BIT(15))
			val |= 0xFFFFFFFFFFFF0000;
	} else if (size == 4) {
		if (val & __BIT(31))
			val |= 0xFFFFFFFF00000000;
	}
	return val;
}

static int
node_immediate(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	const struct x86_opcode *opcode = instr->opcode;
	struct x86_store *store;
	uint8_t immsize;
	size_t sesize = 0;

	/* The immediate is the source */
	store = &instr->src;
	immsize = instr->operand_size;

	if (opcode->flags & FLAG_imm8) {
		sesize = immsize;
		immsize = 1;
	} else if ((opcode->flags & FLAG_immz) && (immsize == 8)) {
		sesize = immsize;
		immsize = 4;
	}

	store->type = STORE_IMM;
	if (fsm_read(fsm, (uint8_t *)&store->u.imm.data, immsize) == -1) {
		return -1;
	}
	fsm_advance(fsm, immsize, NULL);

	if (sesize != 0) {
		store->u.imm.data = sign_extend(store->u.imm.data, sesize);
	}

	return 0;
}

static int
node_disp(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	const struct x86_opcode *opcode = instr->opcode;
	uint64_t data = 0;
	size_t n;

	if (instr->strm->disp.type == DISP_1) {
		n = 1;
	} else if (instr->strm->disp.type == DISP_2) {
		n = 2;
	} else if (instr->strm->disp.type == DISP_4) {
		n = 4;
	} else {
		DISASSEMBLER_BUG();
	}

	if (fsm_read(fsm, (uint8_t *)&data, n) == -1) {
		return -1;
	}

	if (__predict_true(fsm->is64bit)) {
		data = sign_extend(data, n);
	}

	instr->strm->disp.data = data;

	if (opcode->immediate) {
		fsm_advance(fsm, n, node_immediate);
	} else {
		fsm_advance(fsm, n, NULL);
	}

	return 0;
}

/*
 * Special node to handle 16bit addressing encoding, which can reference two
 * registers at once.
 */
static int
node_dual(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	int reg1, reg2;

	reg1 = gpr_dual_reg1_rm[instr->regmodrm.rm];

	if (instr->regmodrm.rm == 0b000 ||
	    instr->regmodrm.rm == 0b010) {
		reg2 = VMM_X64_GPR_RSI;
	} else if (instr->regmodrm.rm == 0b001 ||
	    instr->regmodrm.rm == 0b011) {
		reg2 = VMM_X64_GPR_RDI;
	} else {
		DISASSEMBLER_BUG();
	}

	instr->strm->type = STORE_DUALREG;
	instr->strm->u.dualreg.reg1 = reg1;
	instr->strm->u.dualreg.reg2 = reg2;

	if (instr->strm->disp.type == DISP_NONE) {
		DISASSEMBLER_BUG();
	} else if (instr->strm->disp.type == DISP_0) {
		/* Indirect register addressing mode */
		if (instr->opcode->immediate) {
			fsm_advance(fsm, 1, node_immediate);
		} else {
			fsm_advance(fsm, 1, NULL);
		}
	} else {
		fsm_advance(fsm, 1, node_disp);
	}

	return 0;
}

static const struct x86_reg *
get_register_idx(struct x86_instr *instr, uint8_t index)
{
	uint8_t enc = index;
	const struct x86_reg *reg;
	size_t regsize;

	regsize = instr->address_size;
	reg = &gpr_map[instr->rexpref.x][enc][regsize-1];

	if (reg->num == -1) {
		reg = resolve_special_register(instr, enc, regsize);
	}

	return reg;
}

static const struct x86_reg *
get_register_bas(struct x86_instr *instr, uint8_t base)
{
	uint8_t enc = base;
	const struct x86_reg *reg;
	size_t regsize;

	regsize = instr->address_size;
	reg = &gpr_map[instr->rexpref.b][enc][regsize-1];
	if (reg->num == -1) {
		reg = resolve_special_register(instr, enc, regsize);
	}

	return reg;
}

static int
node_sib(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	const struct x86_opcode *opcode;
	uint8_t scale, index, base;
	bool noindex, nobase;
	uint8_t byte;

	if (fsm_read(fsm, &byte, sizeof(byte)) == -1) {
		return -1;
	}

	scale = ((byte & 0b11000000) >> 6);
	index = ((byte & 0b00111000) >> 3);
	base  = ((byte & 0b00000111) >> 0);

	opcode = instr->opcode;

	noindex = false;
	nobase = false;

	if (index == 0b100 && !instr->rexpref.x) {
		/* Special case: the index is null */
		noindex = true;
	}

	if (instr->regmodrm.mod == 0b00 && base == 0b101) {
		/* Special case: the base is null + disp32 */
		instr->strm->disp.type = DISP_4;
		nobase = true;
	}

	instr->strm->type = STORE_SIB;
	instr->strm->u.sib.scale = (1 << scale);
	if (!noindex)
		instr->strm->u.sib.idx = get_register_idx(instr, index);
	if (!nobase)
		instr->strm->u.sib.bas = get_register_bas(instr, base);

	/* May have a displacement, or an immediate */
	if (instr->strm->disp.type == DISP_1 ||
	    instr->strm->disp.type == DISP_2 ||
	    instr->strm->disp.type == DISP_4) {
		fsm_advance(fsm, 1, node_disp);
	} else if (opcode->immediate) {
		fsm_advance(fsm, 1, node_immediate);
	} else {
		fsm_advance(fsm, 1, NULL);
	}

	return 0;
}

static const struct x86_reg *
get_register_reg(struct x86_instr *instr)
{
	uint8_t enc = instr->regmodrm.reg;
	const struct x86_reg *reg;
	size_t regsize;

	regsize = instr->operand_size;

	reg = &gpr_map[instr->rexpref.r][enc][regsize-1];
	if (reg->num == -1) {
		reg = resolve_special_register(instr, enc, regsize);
	}

	return reg;
}

static const struct x86_reg *
get_register_rm(struct x86_instr *instr)
{
	uint8_t enc = instr->regmodrm.rm;
	const struct x86_reg *reg;
	size_t regsize;

	if (instr->strm->disp.type == DISP_NONE) {
		regsize = instr->operand_size;
	} else {
		/* Indirect access, the size is that of the address. */
		regsize = instr->address_size;
	}

	reg = &gpr_map[instr->rexpref.b][enc][regsize-1];
	if (reg->num == -1) {
		reg = resolve_special_register(instr, enc, regsize);
	}

	return reg;
}

static inline bool
has_sib(struct x86_instr *instr)
{
	return (instr->address_size != 2 && /* no SIB in 16bit addressing */
	    instr->regmodrm.mod != 0b11 &&
	    instr->regmodrm.rm == 0b100);
}

static inline bool
is_rip_relative(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	return (fsm->is64bit && /* RIP-relative only in 64bit mode */
	    instr->regmodrm.mod == 0b00 &&
	    instr->regmodrm.rm == 0b101);
}

static inline bool
is_disp32_only(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	return (!fsm->is64bit && /* no disp32-only in 64bit mode */
	    instr->address_size != 2 && /* no disp32-only in 16bit addressing */
	    instr->regmodrm.mod == 0b00 &&
	    instr->regmodrm.rm == 0b101);
}

static inline bool
is_disp16_only(struct x86_decode_fsm *fsm __unused, struct x86_instr *instr)
{
	return (instr->address_size == 2 && /* disp16-only only in 16bit addr */
	    instr->regmodrm.mod == 0b00 &&
	    instr->regmodrm.rm == 0b110);
}

static inline bool
is_dual(struct x86_decode_fsm *fsm __unused, struct x86_instr *instr)
{
	return (instr->address_size == 2 &&
	    instr->regmodrm.mod != 0b11 &&
	    instr->regmodrm.rm <= 0b011);
}

static enum x86_disp_type
get_disp_type(struct x86_instr *instr)
{
	switch (instr->regmodrm.mod) {
	case 0b00:	/* indirect */
		return DISP_0;
	case 0b01:	/* indirect+1 */
		return DISP_1;
	case 0b10:	/* indirect+{2,4} */
		if (__predict_false(instr->address_size == 2)) {
			return DISP_2;
		}
		return DISP_4;
	case 0b11:	/* direct */
	default:	/* llvm */
		return DISP_NONE;
	}
	__unreachable();
}

static int
node_regmodrm(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	struct x86_store *strg, *strm;
	const struct x86_opcode *opcode;
	const struct x86_reg *reg;
	uint8_t byte;

	if (fsm_read(fsm, &byte, sizeof(byte)) == -1) {
		return -1;
	}

	opcode = instr->opcode;

	instr->regmodrm.rm  = ((byte & 0b00000111) >> 0);
	instr->regmodrm.reg = ((byte & 0b00111000) >> 3);
	instr->regmodrm.mod = ((byte & 0b11000000) >> 6);

	if (opcode->regtorm) {
		strg = &instr->src;
		strm = &instr->dst;
	} else { /* RM to REG */
		strm = &instr->src;
		strg = &instr->dst;
	}

	/* Save for later use. */
	instr->strm = strm;

	/*
	 * Special cases: Groups. The REG field of REGMODRM is the index in
	 * the group. op1 gets overwritten in the Immediate node, if any.
	 */
	if (opcode->group1) {
		if (group1[instr->regmodrm.reg].emul == NULL) {
			return -1;
		}
		instr->emul = group1[instr->regmodrm.reg].emul;
	} else if (opcode->group3) {
		if (group3[instr->regmodrm.reg].emul == NULL) {
			return -1;
		}
		instr->emul = group3[instr->regmodrm.reg].emul;
	} else if (opcode->group11) {
		if (group11[instr->regmodrm.reg].emul == NULL) {
			return -1;
		}
		instr->emul = group11[instr->regmodrm.reg].emul;
	}

	if (!opcode->immediate) {
		reg = get_register_reg(instr);
		if (reg == NULL) {
			return -1;
		}
		strg->type = STORE_REG;
		strg->u.reg = reg;
	}

	/* The displacement applies to RM. */
	strm->disp.type = get_disp_type(instr);

	if (has_sib(instr)) {
		/* Overwrites RM */
		fsm_advance(fsm, 1, node_sib);
		return 0;
	}

	if (is_rip_relative(fsm, instr)) {
		/* Overwrites RM */
		strm->type = STORE_REG;
		strm->u.reg = &gpr_map__rip;
		strm->disp.type = DISP_4;
		fsm_advance(fsm, 1, node_disp);
		return 0;
	}

	if (is_disp32_only(fsm, instr)) {
		/* Overwrites RM */
		strm->type = STORE_REG;
		strm->u.reg = NULL;
		strm->disp.type = DISP_4;
		fsm_advance(fsm, 1, node_disp);
		return 0;
	}

	if (__predict_false(is_disp16_only(fsm, instr))) {
		/* Overwrites RM */
		strm->type = STORE_REG;
		strm->u.reg = NULL;
		strm->disp.type = DISP_2;
		fsm_advance(fsm, 1, node_disp);
		return 0;
	}

	if (__predict_false(is_dual(fsm, instr))) {
		/* Overwrites RM */
		fsm_advance(fsm, 0, node_dual);
		return 0;
	}

	reg = get_register_rm(instr);
	if (reg == NULL) {
		return -1;
	}
	strm->type = STORE_REG;
	strm->u.reg = reg;

	if (strm->disp.type == DISP_NONE) {
		/* Direct register addressing mode */
		if (opcode->immediate) {
			fsm_advance(fsm, 1, node_immediate);
		} else {
			fsm_advance(fsm, 1, NULL);
		}
	} else if (strm->disp.type == DISP_0) {
		/* Indirect register addressing mode */
		if (opcode->immediate) {
			fsm_advance(fsm, 1, node_immediate);
		} else {
			fsm_advance(fsm, 1, NULL);
		}
	} else {
		fsm_advance(fsm, 1, node_disp);
	}

	return 0;
}

static size_t
get_operand_size(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	const struct x86_opcode *opcode = instr->opcode;
	int opsize;

	/* Get the opsize */
	if (!opcode->szoverride) {
		opsize = opcode->defsize;
	} else if (instr->rexpref.present && instr->rexpref.w) {
		opsize = 8;
	} else {
		if (!fsm->is16bit) {
			if (instr->legpref.opr_ovr) {
				opsize = 2;
			} else {
				opsize = 4;
			}
		} else { /* 16bit */
			if (instr->legpref.opr_ovr) {
				opsize = 4;
			} else {
				opsize = 2;
			}
		}
	}

	return opsize;
}

static size_t
get_address_size(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	if (fsm->is64bit) {
		if (__predict_false(instr->legpref.adr_ovr)) {
			return 4;
		}
		return 8;
	}

	if (fsm->is32bit) {
		if (__predict_false(instr->legpref.adr_ovr)) {
			return 2;
		}
		return 4;
	}

	/* 16bit. */
	if (__predict_false(instr->legpref.adr_ovr)) {
		return 4;
	}
	return 2;
}

static int
node_primary_opcode(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	const struct x86_opcode *opcode;
	uint8_t byte;

	if (fsm_read(fsm, &byte, sizeof(byte)) == -1) {
		return -1;
	}

	opcode = &primary_opcode_table[byte];
	if (__predict_false(!opcode->valid)) {
		return -1;
	}

	instr->opcode = opcode;
	instr->emul = opcode->emul;
	instr->operand_size = get_operand_size(fsm, instr);
	instr->address_size = get_address_size(fsm, instr);

	if (fsm->is64bit && (instr->operand_size == 4)) {
		/* Zero-extend to 64 bits. */
		instr->zeroextend_mask = ~size_to_mask(4);
	}

	if (opcode->regmodrm) {
		fsm_advance(fsm, 1, node_regmodrm);
	} else if (opcode->dmo) {
		/* Direct-Memory Offsets */
		fsm_advance(fsm, 1, node_dmo);
	} else if (opcode->stos || opcode->lods) {
		fsm_advance(fsm, 1, node_stlo);
	} else if (opcode->movs) {
		fsm_advance(fsm, 1, node_movs);
	} else {
		return -1;
	}

	return 0;
}

static int
node_secondary_opcode(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	const struct x86_opcode *opcode;
	uint8_t byte;

	if (fsm_read(fsm, &byte, sizeof(byte)) == -1) {
		return -1;
	}

	opcode = &secondary_opcode_table[byte];
	if (__predict_false(!opcode->valid)) {
		return -1;
	}

	instr->opcode = opcode;
	instr->emul = opcode->emul;
	instr->operand_size = get_operand_size(fsm, instr);
	instr->address_size = get_address_size(fsm, instr);

	if (fsm->is64bit && (instr->operand_size == 4)) {
		/* Zero-extend to 64 bits. */
		instr->zeroextend_mask = ~size_to_mask(4);
	}

	if (opcode->flags & FLAG_ze) {
		/*
		 * Compute the mask for zero-extend. Update the operand size,
		 * we move fewer bytes.
		 */
		instr->zeroextend_mask |= size_to_mask(instr->operand_size);
		instr->zeroextend_mask &= ~size_to_mask(opcode->defsize);
		instr->operand_size = opcode->defsize;
	}

	if (opcode->regmodrm) {
		fsm_advance(fsm, 1, node_regmodrm);
	} else {
		return -1;
	}

	return 0;
}

static int
node_main(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	uint8_t byte;

#define ESCAPE	0x0F
#define VEX_1	0xC5
#define VEX_2	0xC4
#define XOP	0x8F

	if (fsm_read(fsm, &byte, sizeof(byte)) == -1) {
		return -1;
	}

	/*
	 * We don't take XOP. It is AMD-specific, and it was removed shortly
	 * after being introduced.
	 */
	if (byte == ESCAPE) {
		fsm_advance(fsm, 1, node_secondary_opcode);
	} else if (!instr->rexpref.present) {
		if (byte == VEX_1) {
			return -1;
		} else if (byte == VEX_2) {
			return -1;
		} else {
			fsm->fn = node_primary_opcode;
		}
	} else {
		fsm->fn = node_primary_opcode;
	}

	return 0;
}

static int
node_rex_prefix(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	struct x86_rexpref *rexpref = &instr->rexpref;
	uint8_t byte;
	size_t n = 0;

	if (fsm_read(fsm, &byte, sizeof(byte)) == -1) {
		return -1;
	}

	if (byte >= 0x40 && byte <= 0x4F) {
		if (__predict_false(!fsm->is64bit)) {
			return -1;
		}
		rexpref->b = ((byte & 0x1) != 0);
		rexpref->x = ((byte & 0x2) != 0);
		rexpref->r = ((byte & 0x4) != 0);
		rexpref->w = ((byte & 0x8) != 0);
		rexpref->present = true;
		n = 1;
	}

	fsm_advance(fsm, n, node_main);
	return 0;
}

static int
node_legacy_prefix(struct x86_decode_fsm *fsm, struct x86_instr *instr)
{
	uint8_t byte;

	if (fsm_read(fsm, &byte, sizeof(byte)) == -1) {
		return -1;
	}

	if (byte == LEG_OPR_OVR) {
		instr->legpref.opr_ovr = 1;
	} else if (byte == LEG_OVR_DS) {
		instr->legpref.seg = VMM_X64_SEG_DS;
	} else if (byte == LEG_OVR_ES) {
		instr->legpref.seg = VMM_X64_SEG_ES;
	} else if (byte == LEG_REP) {
		instr->legpref.rep = 1;
	} else if (byte == LEG_OVR_GS) {
		instr->legpref.seg = VMM_X64_SEG_GS;
	} else if (byte == LEG_OVR_FS) {
		instr->legpref.seg = VMM_X64_SEG_FS;
	} else if (byte == LEG_ADR_OVR) {
		instr->legpref.adr_ovr = 1;
	} else if (byte == LEG_OVR_CS) {
		instr->legpref.seg = VMM_X64_SEG_CS;
	} else if (byte == LEG_OVR_SS) {
		instr->legpref.seg = VMM_X64_SEG_SS;
	} else if (byte == LEG_REPN) {
		instr->legpref.repn = 1;
	} else if (byte == LEG_LOCK) {
		/* ignore */
	} else {
		/* not a legacy prefix */
		fsm_advance(fsm, 0, node_rex_prefix);
		return 0;
	}

	fsm_advance(fsm, 1, node_legacy_prefix);
	return 0;
}

static int
x86_decode(uint8_t *inst_bytes, size_t inst_len, struct x86_instr *instr,
    struct vmm_cpustate *state)
{
	struct x86_decode_fsm fsm;
	int ret;

	memset(instr, 0, sizeof(*instr));
	instr->legpref.seg = -1;
	instr->src.hardseg = -1;
	instr->dst.hardseg = -1;

	fsm.is64bit = is_64bit(state);
	fsm.is32bit = is_32bit(state);
	fsm.is16bit = is_16bit(state);

	fsm.fn = node_legacy_prefix;
	fsm.buf = inst_bytes;
	fsm.end = inst_bytes + inst_len;

	while (fsm.fn != NULL) {
		ret = (*fsm.fn)(&fsm, instr);
		if (ret == -1)
			return -1;
	}

	instr->len = fsm.buf - inst_bytes;

	return 0;
}

/* -------------------------------------------------------------------------- */

#define EXEC_INSTR(sz, instr)						\
static uint##sz##_t							\
exec_##instr##sz(uint##sz##_t op1, uint##sz##_t op2, uint64_t *rflags)	\
{									\
	uint##sz##_t res;						\
	__asm __volatile (						\
		#instr"	%2, %3;"					\
		"mov	%3, %1;"					\
		"pushfq;"						\
		"popq	%0"						\
	    : "=r" (*rflags), "=r" (res)				\
	    : "r" (op1), "r" (op2));					\
	return res;							\
}

#define EXEC_DISPATCHER(instr)						\
static uint64_t								\
exec_##instr(uint64_t op1, uint64_t op2, uint64_t *rflags, size_t opsize) \
{									\
	switch (opsize) {						\
	case 1:								\
		return exec_##instr##8(op1, op2, rflags);		\
	case 2:								\
		return exec_##instr##16(op1, op2, rflags);		\
	case 4:								\
		return exec_##instr##32(op1, op2, rflags);		\
	default:							\
		return exec_##instr##64(op1, op2, rflags);		\
	}								\
}

/* SUB: ret = op1 - op2 */
#define PSL_SUB_MASK	(PSL_V|PSL_C|PSL_Z|PSL_N|PSL_PF|PSL_AF)
EXEC_INSTR(8, sub)
EXEC_INSTR(16, sub)
EXEC_INSTR(32, sub)
EXEC_INSTR(64, sub)
EXEC_DISPATCHER(sub)

/* OR:  ret = op1 | op2 */
#define PSL_OR_MASK	(PSL_V|PSL_C|PSL_Z|PSL_N|PSL_PF)
EXEC_INSTR(8, or)
EXEC_INSTR(16, or)
EXEC_INSTR(32, or)
EXEC_INSTR(64, or)
EXEC_DISPATCHER(or)

/* AND: ret = op1 & op2 */
#define PSL_AND_MASK	(PSL_V|PSL_C|PSL_Z|PSL_N|PSL_PF)
EXEC_INSTR(8, and)
EXEC_INSTR(16, and)
EXEC_INSTR(32, and)
EXEC_INSTR(64, and)
EXEC_DISPATCHER(and)

/* XOR: ret = op1 ^ op2 */
#define PSL_XOR_MASK	(PSL_V|PSL_C|PSL_Z|PSL_N|PSL_PF)
EXEC_INSTR(8, xor)
EXEC_INSTR(16, xor)
EXEC_INSTR(32, xor)
EXEC_INSTR(64, xor)
EXEC_DISPATCHER(xor)

/* -------------------------------------------------------------------------- */

/*
 * Emulation functions. We don't care about the order of the operands, except
 * for SUB, CMP and TEST. For these ones we look at mem->write to determine who
 * is op1 and who is op2.
 */

static void
x86_func_or(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *gprs = vcpu->state->gprs;
	uint64_t *retval = (uint64_t *)mem->data;
	const bool write = mem->write;
	uint64_t *op1, op2, fl, ret;

	op1 = (uint64_t *)mem->data;
	op2 = 0;

	/* Fetch the value to be OR'ed (op2). */
	mem->data = (uint8_t *)&op2;
	mem->write = false;
	vmm_x64_emul_memory_access(vcpu, mem);

	/* Perform the OR. */
	ret = exec_or(*op1, op2, &fl, mem->size);

	if (write) {
		/* Write back the result. */
		mem->data = (uint8_t *)&ret;
		mem->write = true;
		vmm_x64_emul_memory_access(vcpu, mem);
	} else {
		/* Return data to the caller. */
		*retval = ret;
	}

	gprs[VMM_X64_GPR_RFLAGS] &= ~PSL_OR_MASK;
	gprs[VMM_X64_GPR_RFLAGS] |= (fl & PSL_OR_MASK);
}

static void
x86_func_and(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *gprs = vcpu->state->gprs;
	uint64_t *retval = (uint64_t *)mem->data;
	const bool write = mem->write;
	uint64_t *op1, op2, fl, ret;

	op1 = (uint64_t *)mem->data;
	op2 = 0;

	/* Fetch the value to be AND'ed (op2). */
	mem->data = (uint8_t *)&op2;
	mem->write = false;
	vmm_x64_emul_memory_access(vcpu, mem);

	/* Perform the AND. */
	ret = exec_and(*op1, op2, &fl, mem->size);

	if (write) {
		/* Write back the result. */
		mem->data = (uint8_t *)&ret;
		mem->write = true;
		vmm_x64_emul_memory_access(vcpu, mem);
	} else {
		/* Return data to the caller. */
		*retval = ret;
	}

	gprs[VMM_X64_GPR_RFLAGS] &= ~PSL_AND_MASK;
	gprs[VMM_X64_GPR_RFLAGS] |= (fl & PSL_AND_MASK);
}

static void
x86_func_xchg(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *op1, op2;

	op1 = (uint64_t *)mem->data;
	op2 = 0;

	/* Fetch op2. */
	mem->data = (uint8_t *)&op2;
	mem->write = false;
	vmm_x64_emul_memory_access(vcpu, mem);

	/* Write op1 in op2. */
	mem->data = (uint8_t *)op1;
	mem->write = true;
	vmm_x64_emul_memory_access(vcpu, mem);

	/* Write op2 in op1. */
	*op1 = op2;
}

static void
x86_func_sub(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *gprs = vcpu->state->gprs;
	uint64_t *retval = (uint64_t *)mem->data;
	const bool write = mem->write;
	uint64_t *op1, *op2, fl, ret;
	uint64_t tmp;
	bool memop1;

	memop1 = !mem->write;
	op1 = memop1 ? &tmp : (uint64_t *)mem->data;
	op2 = memop1 ? (uint64_t *)mem->data : &tmp;

	/* Fetch the value to be SUB'ed (op1 or op2). */
	mem->data = (uint8_t *)&tmp;
	mem->write = false;
	vmm_x64_emul_memory_access(vcpu, mem);

	/* Perform the SUB. */
	ret = exec_sub(*op1, *op2, &fl, mem->size);

	if (write) {
		/* Write back the result. */
		mem->data = (uint8_t *)&ret;
		mem->write = true;
		vmm_x64_emul_memory_access(vcpu, mem);
	} else {
		/* Return data to the caller. */
		*retval = ret;
	}

	gprs[VMM_X64_GPR_RFLAGS] &= ~PSL_SUB_MASK;
	gprs[VMM_X64_GPR_RFLAGS] |= (fl & PSL_SUB_MASK);
}

static void
x86_func_xor(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *gprs = vcpu->state->gprs;
	uint64_t *retval = (uint64_t *)mem->data;
	const bool write = mem->write;
	uint64_t *op1, op2, fl, ret;

	op1 = (uint64_t *)mem->data;
	op2 = 0;

	/* Fetch the value to be XOR'ed (op2). */
	mem->data = (uint8_t *)&op2;
	mem->write = false;
	vmm_x64_emul_memory_access(vcpu, mem);

	/* Perform the XOR. */
	ret = exec_xor(*op1, op2, &fl, mem->size);

	if (write) {
		/* Write back the result. */
		mem->data = (uint8_t *)&ret;
		mem->write = true;
		vmm_x64_emul_memory_access(vcpu, mem);
	} else {
		/* Return data to the caller. */
		*retval = ret;
	}

	gprs[VMM_X64_GPR_RFLAGS] &= ~PSL_XOR_MASK;
	gprs[VMM_X64_GPR_RFLAGS] |= (fl & PSL_XOR_MASK);
}

static void
x86_func_cmp(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *gprs = vcpu->state->gprs;
	uint64_t *op1, *op2, fl;
	uint64_t tmp;
	bool memop1;

	memop1 = !mem->write;
	op1 = memop1 ? &tmp : (uint64_t *)mem->data;
	op2 = memop1 ? (uint64_t *)mem->data : &tmp;

	/* Fetch the value to be CMP'ed (op1 or op2). */
	mem->data = (uint8_t *)&tmp;
	mem->write = false;
	vmm_x64_emul_memory_access(vcpu, mem);

	/* Perform the CMP. */
	exec_sub(*op1, *op2, &fl, mem->size);

	gprs[VMM_X64_GPR_RFLAGS] &= ~PSL_SUB_MASK;
	gprs[VMM_X64_GPR_RFLAGS] |= (fl & PSL_SUB_MASK);
}

static void
x86_func_test(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *gprs = vcpu->state->gprs;
	uint64_t *op1, *op2, fl;
	uint64_t tmp;
	bool memop1;

	memop1 = !mem->write;
	op1 = memop1 ? &tmp : (uint64_t *)mem->data;
	op2 = memop1 ? (uint64_t *)mem->data : &tmp;

	/* Fetch the value to be TEST'ed (op1 or op2). */
	mem->data = (uint8_t *)&tmp;
	mem->write = false;
	vmm_x64_emul_memory_access(vcpu, mem);

	/* Perform the TEST. */
	exec_and(*op1, *op2, &fl, mem->size);

	gprs[VMM_X64_GPR_RFLAGS] &= ~PSL_AND_MASK;
	gprs[VMM_X64_GPR_RFLAGS] |= (fl & PSL_AND_MASK);
}

static void
x86_func_mov(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	/*
	 * Nothing special, just move without emulation.
	 */
	vmm_x64_emul_memory_access(vcpu, mem);
}

static void
x86_func_stos(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *gprs = vcpu->state->gprs;

	/*
	 * Just move, and update RDI.
	 */
	vmm_x64_emul_memory_access(vcpu, mem);

	if (gprs[VMM_X64_GPR_RFLAGS] & PSL_D) {
		gprs[VMM_X64_GPR_RDI] -= mem->size;
	} else {
		gprs[VMM_X64_GPR_RDI] += mem->size;
	}
}

static void
x86_func_lods(struct vmm_vcpu *vcpu, struct vmm_x64_memory *mem)
{
	uint64_t *gprs = vcpu->state->gprs;

	/*
	 * Just move, and update RSI.
	 */
	vmm_x64_emul_memory_access(vcpu, mem);

	if (gprs[VMM_X64_GPR_RFLAGS] & PSL_D) {
		gprs[VMM_X64_GPR_RSI] -= mem->size;
	} else {
		gprs[VMM_X64_GPR_RSI] += mem->size;
	}
}

/* -------------------------------------------------------------------------- */

static inline uint64_t
gpr_read_address(struct x86_instr *instr, struct vmm_cpustate *state, int gpr)
{
	uint64_t val;

	val = state->gprs[gpr];
	val &= size_to_mask(instr->address_size);

	return val;
}

static int
store_to_gva(struct vmm_cpustate *state, struct x86_instr *instr,
    struct x86_store *store, uint64_t *gvap, size_t size)
{
	struct x86_sib *sib;
	uint64_t gva = 0;
	uint64_t reg;
	int ret, seg;

	if (store->type == STORE_SIB) {
		sib = &store->u.sib;
		if (sib->bas != NULL)
			gva += gpr_read_address(instr, state, sib->bas->num);
		if (sib->idx != NULL) {
			reg = gpr_read_address(instr, state, sib->idx->num);
			gva += sib->scale * reg;
		}
	} else if (store->type == STORE_REG) {
		if (store->u.reg == NULL) {
			/* The base is null. Happens with disp32-only and
			 * disp16-only. */
		} else {
			gva = gpr_read_address(instr, state, store->u.reg->num);
			if (store->u.reg == &gpr_map__rip) {
				/* RIP-relative addressing: the base is the
				 * start of the next instruction. */
				gva += instr->len;
			}
		}
	} else if (store->type == STORE_DUALREG) {
		gva = gpr_read_address(instr, state, store->u.dualreg.reg1) +
		    gpr_read_address(instr, state, store->u.dualreg.reg2);
	} else {
		gva = store->u.dmo;
	}

	if (store->disp.type != DISP_NONE) {
		gva += store->disp.data;
	}

	if (store->hardseg != -1) {
		seg = store->hardseg;
	} else {
		if (__predict_false(instr->legpref.seg != -1)) {
			seg = instr->legpref.seg;
		} else {
			seg = VMM_X64_SEG_DS;
		}
	}

	if (__predict_true(is_long_mode(state))) {
		if (seg == VMM_X64_SEG_GS || seg == VMM_X64_SEG_FS) {
			segment_apply(&state->segs[seg], &gva);
		}
	} else {
		ret = segment_check(&state->segs[seg], gva, size);
		if (ret == -1)
			return -1;
		segment_apply(&state->segs[seg], &gva);
	}

	*gvap = gva;
	return 0;
}

/*
 * Double memory operand, MOVS only.
 */
static int
assist_mem_double_movs(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct x86_instr *instr)
{
	struct vmm_cpustate *state = vcpu->state;
	uint8_t data[8];
	uint64_t gva;
	size_t size;
	int ret;

	size = instr->operand_size;

	/* Source. */
	ret = store_to_gva(state, instr, &instr->src, &gva, size);
	if (ret == -1)
		return -1;
	ret = read_guest_memory(mach, vcpu, gva, data, size);
	if (ret == -1)
		return -1;

	/* Destination. */
	ret = store_to_gva(state, instr, &instr->dst, &gva, size);
	if (ret == -1)
		return -1;
	ret = write_guest_memory(mach, vcpu, gva, data, size);
	if (ret == -1)
		return -1;

	if (state->gprs[VMM_X64_GPR_RFLAGS] & PSL_D) {
		state->gprs[VMM_X64_GPR_RSI] -= size;
		state->gprs[VMM_X64_GPR_RDI] -= size;
	} else {
		state->gprs[VMM_X64_GPR_RSI] += size;
		state->gprs[VMM_X64_GPR_RDI] += size;
	}

	return 0;
}

/*
 * Single memory operand, covers most instructions.
 */
static int
assist_mem_single(struct vmm_machine *mach, struct vmm_vcpu *vcpu,
    struct x86_instr *instr)
{
	struct vmm_cpustate *state = vcpu->state;
	struct vmm_cpuexit *exit = &vcpu->emul->exit;
	struct vmm_x64_memory mem;
	uint8_t membuf[8];
	uint64_t val;

	memset(membuf, 0, sizeof(membuf));

	mem.vcpu = vcpu;
	mem.gpa = exit->u.mem.gpa;
	mem.size = instr->operand_size;
	mem.data = membuf;

	/* Determine the direction. */
	switch (instr->src.type) {
	case STORE_REG:
		if (instr->src.disp.type != DISP_NONE) {
			/* Indirect access. */
			mem.write = false;
		} else {
			/* Direct access. */
			mem.write = true;
		}
		break;
	case STORE_DUALREG:
		if (instr->src.disp.type == DISP_NONE) {
			DISASSEMBLER_BUG();
		}
		mem.write = false;
		break;
	case STORE_IMM:
		mem.write = true;
		break;
	case STORE_SIB:
		mem.write = false;
		break;
	case STORE_DMO:
		mem.write = false;
		break;
	default:
		DISASSEMBLER_BUG();
	}

	if (mem.write) {
		switch (instr->src.type) {
		case STORE_REG:
			/* The instruction was "reg -> mem". Fetch the register
			 * in membuf. */
			if (__predict_false(instr->src.disp.type != DISP_NONE)) {
				DISASSEMBLER_BUG();
			}
			val = state->gprs[instr->src.u.reg->num];
			val = __SHIFTOUT(val, instr->src.u.reg->mask);
			memcpy(mem.data, &val, mem.size);
			break;
		case STORE_IMM:
			/* The instruction was "imm -> mem". Fetch the immediate
			 * in membuf. */
			memcpy(mem.data, &instr->src.u.imm.data, mem.size);
			break;
		default:
			DISASSEMBLER_BUG();
		}
	} else if (instr->emul->readreg) {
		/* The instruction was "mem -> reg", but the value of the
		 * register matters for the emul func. Fetch it in membuf. */
		if (__predict_false(instr->dst.type != STORE_REG)) {
			DISASSEMBLER_BUG();
		}
		if (__predict_false(instr->dst.disp.type != DISP_NONE)) {
			DISASSEMBLER_BUG();
		}
		val = state->gprs[instr->dst.u.reg->num];
		val = __SHIFTOUT(val, instr->dst.u.reg->mask);
		memcpy(mem.data, &val, mem.size);
	}

	(*instr->emul->func)(vcpu, &mem);

	if (instr->emul->notouch) {
		/* We're done. */
		return 0;
	}

	if (!mem.write) {
		/* The instruction was "mem -> reg". The emul func has filled
		 * membuf with the memory content. Install membuf in the
		 * register. */
		if (__predict_false(instr->dst.type != STORE_REG)) {
			DISASSEMBLER_BUG();
		}
		if (__predict_false(instr->dst.disp.type != DISP_NONE)) {
			DISASSEMBLER_BUG();
		}
		memcpy(&val, membuf, sizeof(uint64_t));
		val = __SHIFTIN(val, instr->dst.u.reg->mask);
		state->gprs[instr->dst.u.reg->num] &= ~instr->dst.u.reg->mask;
		state->gprs[instr->dst.u.reg->num] |= val;
		state->gprs[instr->dst.u.reg->num] &= ~instr->zeroextend_mask;
	} else if (instr->emul->backprop) {
		/* The instruction was "reg -> mem", but the memory must be
		 * back-propagated to the register. Install membuf in the
		 * register. */
		if (__predict_false(instr->src.type != STORE_REG)) {
			DISASSEMBLER_BUG();
		}
		if (__predict_false(instr->src.disp.type != DISP_NONE)) {
			DISASSEMBLER_BUG();
		}
		memcpy(&val, membuf, sizeof(uint64_t));
		val = __SHIFTIN(val, instr->src.u.reg->mask);
		state->gprs[instr->src.u.reg->num] &= ~instr->src.u.reg->mask;
		state->gprs[instr->src.u.reg->num] |= val;
		state->gprs[instr->src.u.reg->num] &= ~instr->zeroextend_mask;
	}

	return 0;
}

static int
vmm_x64_emul_execute(struct vmm_vcpu *vcpu)
{
	struct vmm_x64_emul *emul = vcpu->emul;
	struct vmm_cpustate *state = vcpu->state;
	struct x86_instr instruction;
	uint64_t count;
	int error;

	if (emul->exit.u.mem.inst_len == 0 ||
	    emul->exit.u.mem.inst_len > sizeof(emul->exit.u.mem.inst_bytes)) {
		emul->active = false;
		return EOPNOTSUPP;
	}
	error = x86_decode(emul->exit.u.mem.inst_bytes,
	    emul->exit.u.mem.inst_len, &instruction, state);
	if (error != 0) {
		emul->active = false;
		return EOPNOTSUPP;
	}
	count = 0;
	if (instruction.legpref.rep || instruction.legpref.repn) {
		count = rep_get_cnt(state, instruction.address_size);
		if (count == 0) {
			state->gprs[VMM_X64_GPR_RIP] += instruction.len;
			emul->active = false;
			return 0;
		}
	}
	if (instruction.opcode->movs)
		error = assist_mem_double_movs(vcpu->machine, vcpu, &instruction);
	else
		error = assist_mem_single(vcpu->machine, vcpu, &instruction);
	if (error != 0 || emul->error != 0) {
		emul->active = false;
		return emul->error != 0 ? emul->error : EOPNOTSUPP;
	}
	if (emul->blocked) {
		/* The architectural instruction remains at its entry boundary. */
		*state = emul->state;
		return EINPROGRESS;
	}
	if (instruction.legpref.rep || instruction.legpref.repn) {
		--count;
		rep_set_cnt(state, instruction.address_size, count);
		if (count == 0 || (instruction.legpref.repn &&
		    (state->gprs[VMM_X64_GPR_RFLAGS] & PSL_Z) != 0))
			state->gprs[VMM_X64_GPR_RIP] += instruction.len;
	} else {
		state->gprs[VMM_X64_GPR_RIP] += instruction.len;
	}
	emul->active = false;
	emul->access_count = 0;
	emul->completed_count = 0;
	return 0;
}
