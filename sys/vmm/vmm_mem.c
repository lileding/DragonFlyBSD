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
#include "vmm_loader_x86.h"
#include "vmm_mem.h"
#include "vmm_pcie_layout.h"

struct vmm_mem_backing {
	struct vm_object *own_mut_object;
	struct vmspace *own_mut_boot_vmspace;
	struct vmspace *own_mut_run_vmspace;
	uint64_t imm_bytes;
	uint64_t imm_vmspace_max;
};

static int	vmm_mem_fault_vmspace(struct vmm_mem *m, uint64_t gpa,
		    int prot);

static void
vmm_mem_pmap_del_all_cpus(struct vmspace *vmspace)
{
#ifdef _KERNEL_VIRTUAL
	(void)vmspace;
#else
	pmap_del_all_cpus(vmspace);
#endif
}

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
vmm_mem_gpa_page_inside(uint64_t bytes, uint64_t gpa)
{
	uint64_t page;

	page = trunc_page(gpa);
	return page < bytes && bytes - page >= PAGE_SIZE &&
	    (page < VMM_PCIE_MMIO_BASE || page >= VMM_PCIE_MMIO_END) &&
	    (page < VMM_PCIE_ECAM_BASE ||
	     page >= VMM_PCIE_ECAM_END) &&
	    (page < VMM_X86_LAPIC_MMIO_GPA ||
	     page >= VMM_X86_LAPIC_MMIO_GPA + VMM_X86_LAPIC_MMIO_SIZE);
}

#ifndef _KERNEL_VIRTUAL
static int
vmm_mem_map_ram_object(struct vmspace *vm, struct vm_object *object,
    uint64_t bytes)
{
	struct vmm_mem_hole {
		uint64_t start;
		uint64_t end;
	};
	struct vmm_mem_map_segment {
		vm_offset_t start;
		vm_offset_t end;
		vm_ooffset_t offset;
	} seg[4];
	const struct vmm_mem_hole holes[] = {
		{ VMM_PCIE_MMIO_BASE, VMM_PCIE_MMIO_END },
		{ VMM_PCIE_ECAM_BASE, VMM_PCIE_ECAM_END },
		{ VMM_X86_LAPIC_MMIO_GPA,
		  VMM_X86_LAPIC_MMIO_GPA + VMM_X86_LAPIC_MMIO_SIZE },
	};
	vm_map_t map = &vm->vm_map;
	vm_size_t size = round_page64(bytes);
	vm_prot_t prot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
	unsigned int i;
	unsigned int nseg = 0;
	uint64_t start;

	start = 0;
	for (i = 0; i < sizeof(holes) / sizeof(holes[0]) && start < size; i++) {
		uint64_t end = holes[i].start < size ? holes[i].start : size;

		if (end > start) {
			seg[nseg].start = start;
			seg[nseg].end = end;
			seg[nseg].offset = start;
			nseg++;
		}
		if (size <= holes[i].end) {
			start = size;
			break;
		}
		start = holes[i].end;
	}
	if (start < size) {
		seg[nseg].start = start;
		seg[nseg].end = size;
		seg[nseg].offset = start;
		nseg++;
	}
	for (i = 0; i < nseg; i++) {
		int count;
		int rv;

		count = vm_map_entry_reserve(MAP_RESERVE_COUNT);
		vm_map_lock(map);
		/*
		 * vm_map_insert() consumes this reference on success.  On failure
		 * the caller remains responsible for dropping it.
		 */
		vmm_mem_object_ref(object);
		vm_object_hold(object);
		rv = vm_map_insert(map, &count, object, NULL, seg[i].offset,
		    NULL, seg[i].start, seg[i].end, VM_MAPTYPE_NORMAL,
		    VM_SUBSYS_MMAP, prot, prot, 0);
		vm_object_drop(object);
		vm_map_unlock(map);
		vm_map_entry_release(count);
		if (rv != 0) {
			vm_object_deallocate(object);
			return ENOMEM;
		}
	}
	return 0;
}
#endif

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
	b->imm_vmspace_max = bytes > VMM_PCIE_ECAM_END ? bytes :
	    VMM_PCIE_ECAM_END;
	size = round_page64(b->imm_bytes);
	b->own_mut_object = default_pager_alloc(NULL, size, VM_PROT_DEFAULT, 0);
	if (b->own_mut_object == NULL) {
		error = ENOMEM;
		goto fail;
	}
	vm_object_set_flag(b->own_mut_object, OBJ_NOSPLIT);
	/*
	 * GPA is the VA in this boot vmspace.  We map the declared RAM
	 * object once at GPA 0 and let loader mmap faults or guest NPFs
	 * populate pages on demand; this is not an eager reservation of every
	 * guest page.
	 */
#ifndef _KERNEL_VIRTUAL
	b->own_mut_boot_vmspace = vmspace_alloc(0, b->imm_vmspace_max);
	if (b->own_mut_boot_vmspace == NULL) {
		error = ENOMEM;
		goto fail;
	}
	pmap_maybethreaded(vmspace_pmap(b->own_mut_boot_vmspace));
	error = vmm_mem_map_ram_object(b->own_mut_boot_vmspace,
	    b->own_mut_object, size);
	if (error)
		goto fail;
#endif

	*backingp = b;
	return 0;

fail:
	if (b->own_mut_boot_vmspace != NULL) {
		vmm_mem_pmap_del_all_cpus(b->own_mut_boot_vmspace);
		vmspace_rel(b->own_mut_boot_vmspace);
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

int
vmm_mem_start_run(struct vmm_mem *m)
{
	struct vmm_mem_backing *b;
	struct vmspace *run_vmspace;

	if (m == NULL)
		return EINVAL;
	b = m->own_mut_backing;
	if (b == NULL || b->own_mut_boot_vmspace == NULL)
		return EINVAL;
	if (b->own_mut_run_vmspace != NULL)
		return EBUSY;
	run_vmspace = vmspace_fork(b->own_mut_boot_vmspace, NULL, NULL);
	if (run_vmspace == NULL)
		return ENOMEM;
	pmap_maybethreaded(vmspace_pmap(run_vmspace));
	b->own_mut_run_vmspace = run_vmspace;
	return 0;
}

int
vmm_mem_reset_run(struct vmm_mem *m)
{
	struct vmm_mem_backing *b;
	struct vmspace *old_vmspace;
	struct vmspace *run_vmspace;

	if (m == NULL)
		return EINVAL;
	b = m->own_mut_backing;
	if (b == NULL || b->own_mut_boot_vmspace == NULL ||
	    b->own_mut_run_vmspace == NULL)
		return EINVAL;
	run_vmspace = vmspace_fork(b->own_mut_boot_vmspace, NULL, NULL);
	if (run_vmspace == NULL)
		return ENOMEM;
	pmap_maybethreaded(vmspace_pmap(run_vmspace));
	old_vmspace = b->own_mut_run_vmspace;
	b->own_mut_run_vmspace = run_vmspace;
	vmm_mem_pmap_del_all_cpus(old_vmspace);
	vmspace_rel(old_vmspace);
	return 0;
}

struct vmm_mem_backing *
vmm_mem_detach(struct vmm_mem *m)
{
	struct vmm_mem_backing *b;

	if (m == NULL)
		return NULL;
	b = m->own_mut_backing;
	m->own_mut_backing = NULL;
	return b;
}

void
vmm_mem_release_backing(struct vmm_mem_backing *b)
{
	if (b == NULL)
		return;
	if (b->own_mut_run_vmspace != NULL) {
		vmm_mem_pmap_del_all_cpus(b->own_mut_run_vmspace);
		vmspace_rel(b->own_mut_run_vmspace);
	}
	if (b->own_mut_boot_vmspace != NULL) {
		vmm_mem_pmap_del_all_cpus(b->own_mut_boot_vmspace);
		vmspace_rel(b->own_mut_boot_vmspace);
	}
	vm_object_deallocate(b->own_mut_object);
	kfree(b, M_TEMP);
}

int
vmm_mem_snapshot(struct vmm_mem *m, struct vm_object **objectp,
    uint64_t *bytesp)
{
	struct vmm_mem_backing *b;

	if (objectp != NULL)
		*objectp = NULL;
	if (bytesp != NULL)
		*bytesp = 0;
	if (m == NULL || objectp == NULL || bytesp == NULL)
		return EINVAL;
	b = m->own_mut_backing;
	if (b == NULL || b->own_mut_object == NULL || b->imm_bytes == 0)
		return EINVAL;
	vmm_mem_object_ref(b->own_mut_object);
	*objectp = b->own_mut_object;
	*bytesp = b->imm_bytes;
	return 0;
}

struct vmspace *
vmm_mem_borrow_vmspace(struct vmm_mem *m)
{
	if (m == NULL || m->own_mut_backing == NULL)
		return NULL;
	return m->own_mut_backing->own_mut_run_vmspace;
}

#ifdef _KERNEL_VIRTUAL
int
vmm_mem_map_object(struct vmm_mem *m, uint64_t gpa, uint64_t size,
    struct vm_object *object)
{

	(void)m;
	(void)gpa;
	(void)size;
	(void)object;
	return EOPNOTSUPP;
}

void
vmm_mem_unmap_object(struct vmm_mem *m, uint64_t gpa, uint64_t size)
{

	(void)m;
	(void)gpa;
	(void)size;
}
#else
int
vmm_mem_map_object(struct vmm_mem *m, uint64_t gpa, uint64_t size,
    struct vm_object *object)
{
	struct vmm_mem_backing *b;
	vm_map_t map;
	vm_prot_t prot;
	int count;
	int error;

	if (m == NULL || object == NULL || size == 0 ||
	    gpa != trunc_page(gpa) || size != round_page64(size))
		return EINVAL;
	b = m->own_mut_backing;
	if (b == NULL || b->own_mut_run_vmspace == NULL ||
	    gpa >= b->imm_vmspace_max || size > b->imm_vmspace_max - gpa)
		return EINVAL;
	map = &b->own_mut_run_vmspace->vm_map;
	prot = VM_PROT_READ | VM_PROT_WRITE;
	count = vm_map_entry_reserve(MAP_RESERVE_COUNT);
	vm_map_lock(map);
	/* vm_map_insert() consumes this reference on success. */
	vmm_mem_object_ref(object);
	vm_object_hold(object);
	error = vm_map_insert(map, &count, object, NULL, 0, NULL, gpa,
	    gpa + size, VM_MAPTYPE_NORMAL, VM_SUBSYS_MMAP, prot, prot, 0);
	vm_object_drop(object);
	vm_map_unlock(map);
	vm_map_entry_release(count);
	if (error != 0) {
		vm_object_deallocate(object);
		return EBUSY;
	}
	return 0;
}

void
vmm_mem_unmap_object(struct vmm_mem *m, uint64_t gpa, uint64_t size)
{
	struct vmm_mem_backing *b;

	if (m == NULL || size == 0 || gpa != trunc_page(gpa) ||
	    size != round_page64(size))
		return;
	b = m->own_mut_backing;
	if (b == NULL || b->own_mut_run_vmspace == NULL ||
	    gpa >= b->imm_vmspace_max || size > b->imm_vmspace_max - gpa)
		return;
	(void)vm_map_remove(&b->own_mut_run_vmspace->vm_map, gpa, gpa + size);
}
#endif

int
vmm_mem_fault_gpa(struct vmm_mem *m, uint64_t gpa, int prot)
{
	struct vmm_mem_backing *b;

	if (m == NULL || m->own_mut_backing == NULL)
		return EINVAL;
	b = m->own_mut_backing;
	if (!vmm_mem_gpa_page_inside(b->imm_bytes, gpa))
		return EINVAL;
	return vmm_mem_fault_vmspace(m, gpa, prot);
}

int
vmm_mem_fault_object_gpa(struct vmm_mem *m, uint64_t gpa, int prot)
{

	return vmm_mem_fault_vmspace(m, gpa, prot);
}

int
vmm_mem_read_gpa(struct vmm_mem *m, uint64_t gpa, void *buf, size_t len)
{
	struct vmm_mem_backing *b;
	uint8_t *dst = buf;
	void *pmap_handle;
	vm_paddr_t pa;
	uint64_t page_gpa;
	size_t chunk;
	size_t off;
	int error;

	if (m == NULL || buf == NULL)
		return EINVAL;
	b = m->own_mut_backing;
	if (b == NULL || b->own_mut_run_vmspace == NULL)
		return EINVAL;
	while (len != 0) {
		page_gpa = trunc_page(gpa);
		if (!vmm_mem_gpa_page_inside(b->imm_bytes, page_gpa))
			return EINVAL;
		off = (size_t)(gpa - page_gpa);
		chunk = PAGE_SIZE - off;
		if (chunk > len)
			chunk = len;
		error = vm_fault(&b->own_mut_run_vmspace->vm_map, page_gpa,
		    VM_PROT_READ, VM_FAULT_NORMAL);
		if (error)
			return error;
		pmap_handle = NULL;
		pa = pmap_extract(vmspace_pmap(b->own_mut_run_vmspace),
		    page_gpa, &pmap_handle);
		if (pa == 0) {
			pmap_extract_done(pmap_handle);
			return EFAULT;
		}
		bcopy((const void *)(PHYS_TO_DMAP(pa) + off), dst, chunk);
		pmap_extract_done(pmap_handle);
		gpa += chunk;
		dst += chunk;
		len -= chunk;
	}
	return 0;
}

static int
vmm_mem_fault_vmspace(struct vmm_mem *m, uint64_t gpa, int prot)
{
	struct vmm_mem_backing *b;
	const int valid_prot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
	int flags;

	if (m == NULL)
		return EINVAL;
	b = m->own_mut_backing;
	if (b == NULL || b->own_mut_run_vmspace == NULL)
		return EINVAL;
	if ((prot & valid_prot) == 0 || (prot & ~valid_prot) != 0)
		return EINVAL;
	if (trunc_page(gpa) >= b->imm_vmspace_max ||
	    trunc_page(gpa) > b->imm_vmspace_max - PAGE_SIZE)
		return EINVAL;
	flags = (prot & VM_PROT_WRITE) ? VM_FAULT_DIRTY : VM_FAULT_NORMAL;
	return vm_fault(&b->own_mut_run_vmspace->vm_map, trunc_page(gpa),
	    (vm_prot_t)prot, flags);
}
