/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The mem object -- see vmm_mem.h.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include "vmm_parse.h"
#include "vmm_mem.h"

#define VMM_MEM_ALIGN	(2ull * 1024 * 1024)	/* large-page granularity */

struct vmm_mem_backing {
	struct vm_object *own_mut_object;
	uint64_t imm_bytes;
	vm_pindex_t mut_wired_pages;
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
	if (v == 0 || (v % VMM_MEM_ALIGN) != 0)
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
vmm_mem_unwire(struct vmm_mem_backing *b)
{
	vm_page_t pg;
	vm_pindex_t i;

	if (b->own_mut_object == NULL || b->mut_wired_pages == 0)
		return;

	vm_object_hold(b->own_mut_object);
	for (i = 0; i < b->mut_wired_pages; i++) {
		pg = vm_page_lookup_busy_wait(b->own_mut_object, i, FALSE,
		    "vmmmem");
		if (pg == NULL)
			continue;
		vm_page_unwire(pg, 0);
		vm_page_wakeup(pg);
	}
	vm_object_drop(b->own_mut_object);
	b->mut_wired_pages = 0;
}

int
vmm_mem_prepare(struct vmm_mem *m)
{
	struct vmm_mem_backing *b;
	vm_page_t pg;
	vm_pindex_t i, pages;
	int error = 0;

	if (!vmm_mem_is_set(m))
		return EINVAL;
	if (m->own_mut_backing != NULL)
		return 0;

	b = kmalloc(sizeof(*b), M_TEMP, M_WAITOK | M_ZERO);
	b->imm_bytes = m->mut_bytes;
	pages = OFF_TO_IDX(round_page64(b->imm_bytes));
	b->own_mut_object = vm_object_allocate(OBJT_DEFAULT, pages);
	if (b->own_mut_object == NULL) {
		error = ENOMEM;
		goto fail;
	}
	vm_object_set_flag(b->own_mut_object, OBJ_NOSPLIT);

	for (i = 0; i < pages; i++) {
		pg = vm_page_grab(b->own_mut_object, i,
		    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO |
		    VM_ALLOC_NULL_OK);
		if (pg == NULL) {
			error = ENOMEM;
			goto fail;
		}
		vm_page_wire(pg);
		vm_page_wakeup(pg);
		b->mut_wired_pages++;
	}

	m->own_mut_backing = b;
	return 0;

fail:
	vmm_mem_unwire(b);
	if (b->own_mut_object != NULL)
		vm_object_deallocate(b->own_mut_object);
	kfree(b, M_TEMP);
	return error;
}

void
vmm_mem_release(struct vmm_mem *m)
{
	vmm_mem_release_backing(vmm_mem_detach(m));
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
	vmm_mem_unwire(b);
	vm_object_deallocate(b->own_mut_object);
	kfree(b, M_TEMP);
}

struct vm_object *
vmm_mem_object(struct vmm_mem *m)
{
	if (m->own_mut_backing == NULL)
		return NULL;
	return m->own_mut_backing->own_mut_object;
}
