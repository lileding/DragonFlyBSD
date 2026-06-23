/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The mem config object -- see vmm_mem.h.  Pure; no kernel/VFS deps.
 */
#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#include "vmm_parse.h"
#include "vmm_mem.h"

#define VMM_MEM_ALIGN	(2ull * 1024 * 1024)	/* large-page granularity */

#ifdef _KERNEL
struct vmm_mem_backing {
	struct vm_object *object;
	uint64_t bytes;
	vm_pindex_t wired_pages;
};
#endif

int
vmm_mem_parse(struct vmm_mem *m, const char *buf, size_t len)
{
	size_t tl;
	const char *t = vmm_trim(buf, len, &tl);
	uint64_t mult, v;
	size_t dlen;
	char last;

#ifdef _KERNEL
	if (m->backing != NULL)
		return 0;
#endif
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
	m->bytes = v;
	return 1;
}

size_t
vmm_mem_format(const struct vmm_mem *m, char *out, size_t cap)
{
	return m->bytes == 0 ? 0 : vmm_write_decimal(m->bytes, out, cap);
}

int
vmm_mem_is_set(const struct vmm_mem *m)
{
	return m->bytes != 0;
}

#ifdef _KERNEL
static void
vmm_mem_unwire(struct vmm_mem_backing *b)
{
	vm_page_t pg;
	vm_pindex_t i;

	if (b->object == NULL || b->wired_pages == 0)
		return;

	vm_object_hold(b->object);
	for (i = 0; i < b->wired_pages; i++) {
		pg = vm_page_lookup_busy_wait(b->object, i, FALSE, "vmmmem");
		if (pg == NULL)
			continue;
		vm_page_unwire(pg, 0);
		vm_page_wakeup(pg);
	}
	vm_object_drop(b->object);
	b->wired_pages = 0;
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
	if (m->backing != NULL)
		return 0;

	b = kmalloc(sizeof(*b), M_TEMP, M_WAITOK | M_ZERO);
	b->bytes = m->bytes;
	pages = OFF_TO_IDX(round_page64(b->bytes));
	b->object = vm_object_allocate(OBJT_DEFAULT, pages);
	if (b->object == NULL) {
		error = ENOMEM;
		goto fail;
	}
	vm_object_set_flag(b->object, OBJ_NOSPLIT);

	for (i = 0; i < pages; i++) {
		pg = vm_page_grab(b->object, i,
		    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO |
		    VM_ALLOC_NULL_OK);
		if (pg == NULL) {
			error = ENOMEM;
			goto fail;
		}
		vm_page_wire(pg);
		vm_page_wakeup(pg);
		b->wired_pages++;
	}

	m->backing = b;
	return 0;

fail:
	vmm_mem_unwire(b);
	if (b->object != NULL)
		vm_object_deallocate(b->object);
	kfree(b, M_TEMP);
	return error;
}

void
vmm_mem_release(struct vmm_mem *m)
{
	struct vmm_mem_backing *b = m->backing;

	if (b == NULL)
		return;
	m->backing = NULL;
	vmm_mem_unwire(b);
	vm_object_deallocate(b->object);
	kfree(b, M_TEMP);
}

struct vm_object *
vmm_mem_object(struct vmm_mem *m)
{
	if (m->backing == NULL)
		return NULL;
	return m->backing->object;
}
#endif
