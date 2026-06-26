/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Userland boundary tests for the kernel mem config and backing object.
 */
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm/vm.h"
#include "vm/vm_map.h"
#include "vm/vm_object.h"
#include "vmm_mem.h"

static int failures;
int vmm_test_vm_fault_calls;
vm_map_t vmm_test_vm_fault_map;
vm_offset_t vmm_test_vm_fault_addr;
vm_prot_t vmm_test_vm_fault_prot;
int vmm_test_vm_fault_flags;
int vmm_test_vm_fault_result;
int vmm_test_default_pager_alloc_fail;
vm_size_t vmm_test_default_pager_alloc_size;
vm_prot_t vmm_test_default_pager_alloc_prot;
int vmm_test_default_pager_alloc_flags;
int vmm_test_vmspace_alloc_fail;
int vmm_test_vm_map_insert_result;
int vmm_test_vm_map_insert_calls;
int vmm_test_vm_object_free_count;
int vmm_test_vmspace_free_count;
vm_offset_t vmm_test_vmspace_alloc_min;
vm_offset_t vmm_test_vmspace_alloc_max;
int vmm_test_pmap_maybethreaded_calls;
struct pmap *vmm_test_pmap_maybethreaded_pmap;
int vmm_test_pmap_del_all_cpus_calls;
struct vmspace *vmm_test_pmap_del_all_cpus_vmspace;
int vmm_test_pmap_del_all_cpus_before_vmspace_rel;

static void
fail(const char *name)
{
	fprintf(stderr, "FAIL: %s\n", name);
	failures++;
}

static void
expect_parse(const char *name, const char *input, uint64_t initial,
    uint64_t want, int ok)
{
	struct vmm_mem mem;
	int got;

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = initial;
	got = vmm_mem_parse(&mem, input, strlen(input));
	if (got != ok) {
		fail(name);
		return;
	}
	if (ok && mem.mut_bytes != want)
		fail(name);
	if (!ok && mem.mut_bytes != initial)
		fail(name);
}

static void
expect_format(const char *name, uint64_t bytes, const char *want)
{
	struct vmm_mem mem;
	char buf[64];
	size_t n;

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = bytes;
	memset(buf, 0xa5, sizeof(buf));
	n = vmm_mem_format(&mem, buf, sizeof(buf));
	if (n != strlen(want) || memcmp(buf, want, n) != 0)
		fail(name);
}

static void
expect_format_reject(const char *name, uint64_t bytes, size_t cap)
{
	struct vmm_mem mem;
	char buf[64];

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = bytes;
	if (vmm_mem_format(&mem, buf, cap) != 0)
		fail(name);
}

static void
expect_backing_reject(void)
{
	struct vmm_mem mem;
	struct vmm_mem_backing *backing;

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = VMM_MEM_ALIGN;
	backing = (struct vmm_mem_backing *)(uintptr_t)1;
	mem.own_mut_backing = backing;
	if (vmm_mem_parse(&mem, "4M", 2) != 0)
		fail("backing rejects config change");
	if (mem.mut_bytes != VMM_MEM_ALIGN || mem.own_mut_backing != backing)
		fail("backing preserves config");
}

static void
reset_fault_trace(int result)
{
	vmm_test_vm_fault_calls = 0;
	vmm_test_vm_fault_map = NULL;
	vmm_test_vm_fault_addr = 0;
	vmm_test_vm_fault_prot = 0;
	vmm_test_vm_fault_flags = 0;
	vmm_test_vm_fault_result = result;
}

static void
reset_vm_trace(void)
{
	vmm_test_default_pager_alloc_fail = 0;
	vmm_test_default_pager_alloc_size = 0;
	vmm_test_default_pager_alloc_prot = 0;
	vmm_test_default_pager_alloc_flags = 0;
	vmm_test_vmspace_alloc_fail = 0;
	vmm_test_vm_map_insert_result = 0;
	vmm_test_vm_map_insert_calls = 0;
	vmm_test_vm_object_free_count = 0;
	vmm_test_vmspace_free_count = 0;
	vmm_test_vmspace_alloc_min = 0;
	vmm_test_vmspace_alloc_max = 0;
	vmm_test_pmap_maybethreaded_calls = 0;
	vmm_test_pmap_maybethreaded_pmap = NULL;
	vmm_test_pmap_del_all_cpus_calls = 0;
	vmm_test_pmap_del_all_cpus_vmspace = NULL;
	vmm_test_pmap_del_all_cpus_before_vmspace_rel = 0;
}

static void
expect_prepare_reject(const char *name, uint64_t bytes)
{
	struct vmm_mem_backing *backing;

	backing = (struct vmm_mem_backing *)(uintptr_t)1;
	if (vmm_mem_prepare(bytes, &backing) != EINVAL || backing != NULL)
		fail(name);
}

static void
expect_publish_mismatch(void)
{
	struct vmm_mem mem;
	struct vmm_mem_backing *backing;

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = VMM_MEM_ALIGN * 2;
	backing = NULL;
	if (vmm_mem_prepare(VMM_MEM_ALIGN, &backing) != 0 || backing == NULL) {
		fail("prepare mismatch backing");
		return;
	}
	if (vmm_mem_publish(&mem, backing) != EINVAL ||
	    mem.own_mut_backing != NULL) {
		fail("publish mismatch");
	}
	vmm_mem_release_backing(backing);
}

static void
expect_prepare_failure_cleanup(const char *name, int pager_fail,
    int vmspace_fail, int map_insert_result, int want_object_frees,
    int want_vmspace_frees, int want_map_calls, int want_pmap_del_calls)
{
	struct vmm_mem_backing *backing;
	int error;

	reset_vm_trace();
	vmm_test_default_pager_alloc_fail = pager_fail;
	vmm_test_vmspace_alloc_fail = vmspace_fail;
	vmm_test_vm_map_insert_result = map_insert_result;
	backing = (struct vmm_mem_backing *)(uintptr_t)1;
	error = vmm_mem_prepare(VMM_MEM_ALIGN, &backing);
	if (error != ENOMEM || backing != NULL)
		fail(name);
	if (vmm_test_vm_object_free_count != want_object_frees ||
	    vmm_test_vmspace_free_count != want_vmspace_frees ||
	    vmm_test_vm_map_insert_calls != want_map_calls ||
	    vmm_test_pmap_del_all_cpus_calls != want_pmap_del_calls) {
		fail(name);
	}
	if (want_pmap_del_calls != 0 &&
	    !vmm_test_pmap_del_all_cpus_before_vmspace_rel)
		fail(name);
	reset_vm_trace();
}

static void
expect_fault_result(struct vmm_mem *mem, const char *name, uint64_t gpa,
    int prot, int want, int want_calls, vm_map_t want_map,
    vm_offset_t want_addr, int want_flags)
{
	int got;

	reset_fault_trace(want);
	got = vmm_mem_fault_gpa(mem, gpa, prot);
	if (got != want)
		fail(name);
	if (vmm_test_vm_fault_calls != want_calls)
		fail(name);
	if (want_calls != 0 &&
	    (vmm_test_vm_fault_map != want_map ||
	     vmm_test_vm_fault_addr != want_addr ||
	     vmm_test_vm_fault_prot != prot ||
	     vmm_test_vm_fault_flags != want_flags)) {
		fail(name);
	}
}

static void
expect_backing_lifecycle(void)
{
	struct vmm_mem mem;
	struct vmm_mem_backing *backing;
	struct vmm_mem_backing *detached;
	struct vm_object *object;
	struct vmspace *vmspace;
	uint64_t bytes;

	reset_vm_trace();
	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = VMM_MEM_ALIGN;
	backing = NULL;
	if (vmm_mem_prepare(mem.mut_bytes, &backing) != 0 || backing == NULL) {
		fail("prepare backing");
		return;
	}
	if (vmm_test_vm_map_insert_calls != 1)
		fail("prepare maps object once");
	if (vmm_test_default_pager_alloc_size != VMM_MEM_ALIGN ||
	    vmm_test_default_pager_alloc_prot != VM_PROT_DEFAULT ||
	    vmm_test_default_pager_alloc_flags != 0)
		fail("prepare allocates default-pager RAM object");
	if (vmm_mem_publish(&mem, backing) != 0) {
		fail("publish backing");
		vmm_mem_release_backing(backing);
		return;
	}
	if (vmm_mem_publish(&mem, backing) != EBUSY)
		fail("publish busy");
	vmspace = vmm_mem_borrow_vmspace(&mem);
	if (vmspace == NULL)
		fail("borrow vmspace");
	if (vmm_test_vmspace_alloc_min != 0 ||
	    vmm_test_vmspace_alloc_max != VMM_MEM_ALIGN)
		fail("prepare allocates machine vmspace over GPA range");
	if (vmm_test_pmap_maybethreaded_calls != 1 ||
	    (vmspace != NULL &&
	     vmm_test_pmap_maybethreaded_pmap != &vmspace->vm_pmap))
		fail("prepare marks machine pmap threaded");
	if (vmspace != NULL &&
	    (vmspace->vm_map.mapped_start != 0 ||
	     vmspace->vm_map.mapped_end != VMM_MEM_ALIGN ||
	     vmspace->vm_map.mapped_offset != 0 ||
	     vmspace->vm_map.mapped_prot !=
	     (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) ||
	     vmspace->vm_map.mapped_maxprot !=
	     (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE))) {
		fail("mapped vmspace range");
	}

	object = NULL;
	bytes = 0;
	if (vmm_mem_snapshot(&mem, &object, &bytes) != 0 ||
	    object == NULL || bytes != VMM_MEM_ALIGN) {
		fail("snapshot backing");
	}
	if (vmspace != NULL && object != vmspace->vm_map.mapped_object)
		fail("snapshot returns mapped backing object");
	if (object != NULL && (object->flags & OBJ_NOSPLIT) == 0)
		fail("backing object is nosplit");
	if (object != NULL && object->refs != 3)
		fail("snapshot owns temporary object reference");
	if (object != NULL) {
		vm_object_deallocate(object);
		if (object->refs != 2)
			fail("snapshot drops only temporary object reference");
	}

	expect_fault_result(&mem, "fault read", 0, VM_PROT_READ, 0, 1,
	    &vmspace->vm_map, 0, VM_FAULT_NORMAL);
	expect_fault_result(&mem, "fault write", PAGE_SIZE, VM_PROT_WRITE, 0,
	    1, &vmspace->vm_map, PAGE_SIZE, VM_FAULT_DIRTY);
	expect_fault_result(&mem, "fault read write", PAGE_SIZE,
	    VM_PROT_READ | VM_PROT_WRITE, 0, 1, &vmspace->vm_map, PAGE_SIZE,
	    VM_FAULT_DIRTY);
	expect_fault_result(&mem, "fault exec", VMM_MEM_ALIGN - 1,
	    VM_PROT_EXECUTE, 0, 1, &vmspace->vm_map, VMM_MEM_ALIGN - PAGE_SIZE,
	    VM_FAULT_NORMAL);
	expect_fault_result(&mem, "fault propagates vm fault", 0,
	    VM_PROT_READ, ENOMEM, 1, &vmspace->vm_map, 0, VM_FAULT_NORMAL);
	expect_fault_result(&mem, "fault bad prot", 0, 0, EINVAL, 0, NULL, 0, 0);
	expect_fault_result(&mem, "fault invalid prot bit", 0,
	    VM_PROT_READ | 0x80, EINVAL, 0, NULL, 0, 0);
	expect_fault_result(&mem, "fault past end", VMM_MEM_ALIGN,
	    VM_PROT_READ, EINVAL, 0, NULL, 0, 0);

	detached = vmm_mem_detach(&mem);
	if (detached != backing || vmm_mem_borrow_vmspace(&mem) != NULL)
		fail("detach backing");
	if (vmm_mem_detach(NULL) != NULL)
		fail("detach null");
	object = (struct vm_object *)(uintptr_t)1;
	bytes = 1;
	if (vmm_mem_snapshot(&mem, &object, &bytes) != EINVAL)
		fail("snapshot detached");
	if (object != NULL || bytes != 0)
		fail("snapshot detached clears outputs");
	object = (struct vm_object *)(uintptr_t)1;
	bytes = 1;
	if (vmm_mem_snapshot(NULL, &object, &bytes) != EINVAL)
		fail("snapshot null mem");
	if (object != NULL || bytes != 0)
		fail("snapshot null mem clears outputs");
	if (vmm_mem_fault_gpa(&mem, 0, VM_PROT_READ) != EINVAL)
		fail("fault detached");
	if (vmm_mem_fault_gpa(NULL, 0, VM_PROT_READ) != EINVAL)
		fail("fault null");
	vmm_mem_release_backing(detached);
	if (vmm_test_vm_object_free_count != 1 ||
	    vmm_test_vmspace_free_count != 1)
		fail("release frees backing exactly once");
	if (vmm_test_pmap_del_all_cpus_calls != 1 ||
	    vmm_test_pmap_del_all_cpus_vmspace != vmspace ||
	    !vmm_test_pmap_del_all_cpus_before_vmspace_rel)
		fail("release removes pmap cpus before vmspace release");
	vmm_mem_release_backing(NULL);
}

static void
expect_large_prepare_is_lazy(void)
{
	struct vmm_mem_backing *backing;

	reset_vm_trace();
	reset_fault_trace(0);
	backing = NULL;
	if (vmm_mem_prepare(VMM_MEM_MAX, &backing) != 0 || backing == NULL) {
		fail("prepare max backing");
		return;
	}
	if (vmm_test_default_pager_alloc_size != VMM_MEM_MAX ||
	    vmm_test_vmspace_alloc_min != 0 ||
	    vmm_test_vmspace_alloc_max != VMM_MEM_MAX ||
	    vmm_test_vm_map_insert_calls != 1 ||
	    vmm_test_vm_fault_calls != 0) {
		fail("prepare max backing is lazy");
	}
	vmm_mem_release_backing(backing);
	if (vmm_test_vm_object_free_count != 1 ||
	    vmm_test_vmspace_free_count != 1 ||
	    vmm_test_pmap_del_all_cpus_calls != 1) {
		fail("release max backing");
	}
}

int
main(void)
{
	char max_bytes[32];
	char over_max_bytes[32];

	snprintf(max_bytes, sizeof(max_bytes), "%ju",
	    (uintmax_t)VMM_MEM_MAX);
	snprintf(over_max_bytes, sizeof(over_max_bytes), "%ju",
	    (uintmax_t)(VMM_MEM_MAX + VMM_MEM_ALIGN));

	expect_parse("2M", "2M", 0, 2ull * 1024 * 1024, 1);
	expect_parse("lowercase suffix and trim", " \t4m\n", 0,
	    4ull * 1024 * 1024, 1);
	expect_parse("bytes", "2097152", 0, 2ull * 1024 * 1024, 1);
	expect_parse("1G", "1G", 0, 1ull << 30, 1);
	expect_parse("max bytes", max_bytes, 0, VMM_MEM_MAX, 1);
	expect_parse("zero rejected", "0", VMM_MEM_ALIGN, VMM_MEM_ALIGN, 0);
	expect_parse("empty rejected", " \n", VMM_MEM_ALIGN, VMM_MEM_ALIGN, 0);
	expect_parse("unaligned 1M rejected", "1M", VMM_MEM_ALIGN,
	    VMM_MEM_ALIGN, 0);
	expect_parse("unaligned 3M rejected", "3M", VMM_MEM_ALIGN,
	    VMM_MEM_ALIGN, 0);
	expect_parse("bad suffix rejected", "2T", VMM_MEM_ALIGN,
	    VMM_MEM_ALIGN, 0);
	expect_parse("overflow decimal rejected",
	    "18446744073709551616", VMM_MEM_ALIGN, VMM_MEM_ALIGN, 0);
	expect_parse("over max rejected", over_max_bytes, VMM_MEM_ALIGN,
	    VMM_MEM_ALIGN, 0);
	expect_prepare_reject("prepare zero rejected", 0);
	expect_prepare_reject("prepare unaligned rejected", VMM_MEM_ALIGN - 1);
	expect_prepare_reject("prepare over max rejected",
	    VMM_MEM_MAX + VMM_MEM_ALIGN);
	if (vmm_mem_prepare(VMM_MEM_ALIGN, NULL) != EINVAL)
		fail("prepare null backing pointer");
	expect_prepare_failure_cleanup("prepare pager failure cleanup", 1, 0,
	    0, 0, 0, 0, 0);
	expect_prepare_failure_cleanup("prepare vmspace failure cleanup", 0, 1,
	    0, 1, 0, 0, 0);
	expect_prepare_failure_cleanup("prepare map failure cleanup", 0, 0, 1,
	    1, 1, 1, 1);
	expect_publish_mismatch();

	expect_format("format 2M", 2ull * 1024 * 1024, "2097152\n");
	expect_format_reject("format unset", 0, sizeof(max_bytes));
	expect_format_reject("format small buffer", 2ull * 1024 * 1024, 4);
	expect_backing_reject();
	expect_backing_lifecycle();
	expect_large_prepare_is_lazy();

	if (failures != 0)
		return 1;
	printf("PASS: mem config and backing\n");
	return 0;
}
