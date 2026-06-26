/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The mem object -- see vmm_mem.h.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include "vmm_parse.h"
#include "vmm_mem.h"

struct vmm_mem_backing {
	struct vm_object *own_mut_object;
	struct vmspace *own_mut_vmspace;
	uint64_t imm_bytes;
};

int
vmm_mem_parse(struct vmm_mem *m, const char *buf, size_t len)
{
	size_t tl;
	const char *t = vmm_trim(buf, len, &tl);
	uint64_t mult, v;
	size_t dlen;
	char last;

	if (m->own_mut_backing != NULL)
		return 0;
	if (tl == 0)
		return 0;
	last = t[tl - 1];
	if (last == 'K' || last == 'k') {
		mult = 1024;
		dlen = tl - 1;
	} else if (last == 'M' || last == 'm') {
		mult = 1024 * 1024;
		dlen = tl - 1;
	} else if (last == 'G' || last == 'g') {
		mult = 1024 * 1024 * 1024;
		dlen = tl - 1;
	} else if (last >= '0' && last <= '9') {
		mult = 1;
		dlen = tl;
	} else {
		return 0;
	}
	if (!vmm_parse_decimal(t, dlen, &v))
		return 0;
	if (v > ((uint64_t)-1) / mult)
		return 0;
	v *= mult;
	if (v == 0 || v > VMM_MEM_MAX || (v % VMM_MEM_ALIGN) != 0)
		return 0;
	m->mut_bytes = v;
	return 1;
}

size_t
vmm_mem_format(const struct vmm_mem *m, char *out, size_t cap)
{
	return m->mut_bytes == 0 ? 0 : vmm_write_decimal(m->mut_bytes, out, cap);
}

int
vmm_mem_is_set(const struct vmm_mem *m)
{
	return m->mut_bytes != 0;
}

static void
vmm_mem_object_ref(struct vm_object *object)
{
	vm_object_hold(object);
	vm_object_reference_locked(object);
	vm_object_drop(object);
}

static int
vmm_mem_map_object(struct vmspace *vm, struct vm_object *object,
    uint64_t bytes)
{
	vm_map_t map = &vm->vm_map;
	vm_offset_t start = 0;
	vm_size_t size = round_page64(bytes);
	vm_prot_t prot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
	int count;
	int rv;

	count = vm_map_entry_reserve(MAP_RESERVE_COUNT);
	vm_map_lock(map);
	vmm_mem_object_ref(object);
	vm_object_hold(object);
	rv = vm_map_insert(map, &count, object, NULL, 0, NULL, start,
	    start + size, VM_MAPTYPE_NORMAL, VM_SUBSYS_MMAP, prot, prot, 0);
	vm_object_drop(object);
	vm_map_unlock(map);
	vm_map_entry_release(count);
	if (rv != 0) {
		vm_object_deallocate(object);
		return ENOMEM;
	}
	return 0;
}

int
vmm_mem_prepare(uint64_t bytes, struct vmm_mem_backing **backingp)
{
	struct vmm_mem_backing *b;
	uint64_t size;
	int error = 0;

	if (backingp == NULL)
		return EINVAL;
	*backingp = NULL;
	if (bytes == 0 || bytes > VMM_MEM_MAX ||
	    (bytes % VMM_MEM_ALIGN) != 0)
		return EINVAL;

	b = kmalloc(sizeof(*b), M_TEMP, M_WAITOK | M_ZERO);
	b->imm_bytes = bytes;
	size = round_page64(b->imm_bytes);
	b->own_mut_object = default_pager_alloc(NULL, size, VM_PROT_DEFAULT, 0);
	if (b->own_mut_object == NULL) {
		error = ENOMEM;
		goto fail;
	}
	vm_object_set_flag(b->own_mut_object, OBJ_NOSPLIT);
	b->own_mut_vmspace = vmspace_alloc(0, size);
	if (b->own_mut_vmspace == NULL) {
		error = ENOMEM;
		goto fail;
	}
	pmap_maybethreaded(vmspace_pmap(b->own_mut_vmspace));
	error = vmm_mem_map_object(b->own_mut_vmspace, b->own_mut_object, size);
	if (error)
		goto fail;

	*backingp = b;
	return 0;

fail:
	if (b->own_mut_vmspace != NULL) {
		pmap_del_all_cpus(b->own_mut_vmspace);
		vmspace_rel(b->own_mut_vmspace);
	}
	if (b->own_mut_object != NULL)
		vm_object_deallocate(b->own_mut_object);
	kfree(b, M_TEMP);
	return error;
}

int
vmm_mem_publish(struct vmm_mem *m, struct vmm_mem_backing *backing)
{
	if (m == NULL || backing == NULL)
		return EINVAL;
	if (m->own_mut_backing != NULL)
		return EBUSY;
	if (m->mut_bytes != backing->imm_bytes)
		return EINVAL;
	m->own_mut_backing = backing;
	return 0;
}

struct vmm_mem_backing *
vmm_mem_detach(struct vmm_mem *m)
{
	struct vmm_mem_backing *b = m->own_mut_backing;

	m->own_mut_backing = NULL;
	return b;
}

void
vmm_mem_release_backing(struct vmm_mem_backing *b)
{
	if (b == NULL)
		return;
	if (b->own_mut_vmspace != NULL) {
		pmap_del_all_cpus(b->own_mut_vmspace);
		vmspace_rel(b->own_mut_vmspace);
	}
	vm_object_deallocate(b->own_mut_object);
	kfree(b, M_TEMP);
}

int
vmm_mem_snapshot(struct vmm_mem *m, struct vm_object **objectp,
    uint64_t *bytesp)
{
	struct vmm_mem_backing *b;

	if (m == NULL || objectp == NULL || bytesp == NULL)
		return EINVAL;
	*objectp = NULL;
	*bytesp = 0;
	b = m->own_mut_backing;
	if (b == NULL || b->own_mut_object == NULL || b->imm_bytes == 0)
		return EINVAL;
	vmm_mem_object_ref(b->own_mut_object);
	*objectp = b->own_mut_object;
	*bytesp = b->imm_bytes;
	return 0;
}

struct vmspace *
vmm_mem_vmspace(struct vmm_mem *m)
{
	if (m->own_mut_backing == NULL)
		return NULL;
	return m->own_mut_backing->own_mut_vmspace;
}

int
vmm_mem_fault_gpa(struct vmm_mem *m, uint64_t gpa, int prot)
{
	struct vmm_mem_backing *b = m->own_mut_backing;
	int flags;

	if (b == NULL || b->own_mut_vmspace == NULL)
		return EINVAL;
	if (gpa >= b->imm_bytes)
		return EINVAL;
	flags = (prot & VM_PROT_WRITE) ? VM_FAULT_DIRTY : VM_FAULT_NORMAL;
	return vm_fault(&b->own_mut_vmspace->vm_map, trunc_page(gpa),
	    (vm_prot_t)prot, flags);
}
