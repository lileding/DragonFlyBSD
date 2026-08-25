/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly support used by the imported AMD SVM backend.
 *
 * These wrappers deliberately preserve the small OS boundary used by the
 * original backend while binding all allocation and CPU operations to vmm.
 */
#ifndef VMM_SVM_OS_H
#define VMM_SVM_OS_H

#include <sys/globaldata.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/thread2.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>
#include <vm/vm_page2.h>

#include <machine/cpu.h>
#include <machine/pcb.h>
#include <machine/pmap.h>
#include <machine/cpufunc.h>
#include <machine/globaldata.h>
#include <machine/npx.h>
#include <machine/segments.h>

#include "../../vmm_internal.h"

typedef struct globaldata os_cpu_t;
typedef struct lock os_mtx_t;
typedef vm_offset_t vaddr_t;
typedef vm_paddr_t paddr_t;
typedef uint64_t gpaddr_t;

typedef struct {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
} cpuid_desc_t;

#define __cacheline_aligned __cachealign
#define __diagused __debugvar
#define __arraycount(array) (sizeof(array) / sizeof((array)[0]))

#define OS_ASSERT KKASSERT
#define OS_MAXCPUS SMP_MAXCPU
#define OS_CPU_FOREACH(cpu) \
	for (int index = 0; index < ncpus && (cpu = globaldata_find(index)); index++)
#define OS_IPI_FUNC(function) void function(void *arg)

#define os_mem_zalloc(size) kmalloc(size, M_VMM, M_WAITOK | M_ZERO)
#define os_mem_free(ptr, size) kfree(ptr, M_VMM)
#define os_printf kprintf
#define os_mtx_init(lock) lockinit(lock, "vmm_svm", 0, 0)
#define os_mtx_destroy(lock) lockuninit(lock)
#define os_mtx_lock(lock) lockmgr(lock, LK_EXCLUSIVE)
#define os_mtx_unlock(lock) lockmgr(lock, LK_RELEASE)
#define os_preempt_disable() crit_enter()
#define os_preempt_enable() crit_exit()
#define os_curcpu_number() mycpuid
#define os_cpu_number(cpu) ((cpu)->gd_cpuid)
#define os_vmspace_pmap(vmspace) vmspace_pmap(vmspace)
#define os_vmspace_pdirpa(vmspace) (vtophys(vmspace_pmap(vmspace)->pm_pml4))
#define uimin(left, right) ((u_int)(left) < (u_int)(right) ? \
	(u_int)(left) : (u_int)(right))
#define x86_get_cpuid(leaf, desc) do_cpuid(leaf, (uint32_t *)(desc))
#define x86_get_cpuid2(leaf, subleaf, desc) \
	cpuid_count(leaf, subleaf, (uint32_t *)(desc))
#define x86_get_dr0() rdr0()
#define x86_get_dr1() rdr1()
#define x86_get_dr2() rdr2()
#define x86_get_dr3() rdr3()
#define x86_get_dr6() rdr6()
#define x86_get_dr7() rdr7()
#define x86_set_dr0(value) load_dr0(value)
#define x86_set_dr1(value) load_dr1(value)
#define x86_set_dr2(value) load_dr2(value)
#define x86_set_dr3(value) load_dr3(value)
#define x86_set_dr6(value) load_dr6(value)
#define x86_set_dr7(value) load_dr7(value)
#define x86_save_fpu(area, mask) \
	({ fpusave((union savefpu *)(area), mask); load_cr0(rcr0() | CR0_TS); })
#define x86_restore_fpu(area, mask) \
	({ __asm volatile("clts" ::: "memory"); fpurstor((union savefpu *)(area), mask); })
#define x86_xsave_features npx_xcr0_mask
#define x86_fpu_mxcsr_mask npx_mxcsr_mask

static inline uint64_t
x86_get_xcr(uint32_t xcr)
{
	uint32_t low;
	uint32_t high;

	__asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(xcr));
	return low | ((uint64_t)high << 32);
}

static inline void
x86_set_xcr(uint32_t xcr, uint64_t value)
{
	uint32_t low;
	uint32_t high;

	low = value;
	high = value >> 32;
	__asm volatile("xsetbv" : : "a"(low), "d"(high), "c"(xcr) : "memory");
}

static inline void
x86_curthread_save_dbregs(uint64_t *drs)
{
	struct pcb *pcb;

	if (curthread->td_lwp == NULL)
		return;
	pcb = curthread->td_lwp->lwp_thread->td_pcb;
	if (__predict_true(!(pcb->pcb_flags & PCB_DBREGS)))
		return;
	drs[VMM_X64_DR_DR0] = rdr0();
	drs[VMM_X64_DR_DR1] = rdr1();
	drs[VMM_X64_DR_DR2] = rdr2();
	drs[VMM_X64_DR_DR3] = rdr3();
	drs[VMM_X64_DR_DR6] = rdr6();
	drs[VMM_X64_DR_DR7] = rdr7();
}

static inline void
x86_curthread_restore_dbregs(uint64_t *drs)
{
	struct pcb *pcb;

	if (curthread->td_lwp == NULL)
		return;
	pcb = curthread->td_lwp->lwp_thread->td_pcb;
	if (__predict_true(!(pcb->pcb_flags & PCB_DBREGS)))
		return;
	load_dr0(drs[VMM_X64_DR_DR0]);
	load_dr1(drs[VMM_X64_DR_DR1]);
	load_dr2(drs[VMM_X64_DR_DR2]);
	load_dr3(drs[VMM_X64_DR_DR3]);
	load_dr6(drs[VMM_X64_DR_DR6]);
	load_dr7(drs[VMM_X64_DR_DR7]);
}

static inline void *
os_pagemem_zalloc(size_t size)
{
	return (void *)kmem_alloc(kernel_map, roundup(size, PAGE_SIZE),
	    VM_SUBSYS_UNKNOWN);
}

static inline void
os_pagemem_free(void *address, size_t size)
{
	kmem_free(kernel_map, (vm_offset_t)address, roundup(size, PAGE_SIZE));
}

static inline vm_paddr_t
os_pa_zalloc(void)
{
	struct vm_page *page;

	page = vm_page_alloczwq(0,
	    VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	return VM_PAGE_TO_PHYS(page);
}

static inline void
os_pa_free(vm_paddr_t address)
{
	vm_page_freezwq(PHYS_TO_VM_PAGE(address));
}

static inline int
os_contigpa_zalloc(vm_paddr_t *physical, vm_offset_t *virtual, size_t npages)
{
	void *address;

	address = contigmalloc(npages * PAGE_SIZE, M_VMM, M_WAITOK | M_ZERO,
	    0, ~0UL, PAGE_SIZE, 0);
	if (address == NULL)
		return ENOMEM;
	*virtual = (vm_offset_t)address;
	*physical = vtophys(address);
	return 0;
}

static inline void
os_contigpa_free(vm_paddr_t physical __unused, vm_offset_t virtual,
    size_t npages)
{
	contigfree((void *)virtual, npages * PAGE_SIZE, M_VMM);
}

/*
 * VMMFS executes guests in kernel-resident P_SYSTEM LWPs.  Those workers
 * must participate in ordinary user scheduling, but never return to
 * userland to consume an AST signal or profiling tick owned by another LWP.
 * Keep all host-interrupt, scheduler, and TLB-safety work as VM-entry
 * barriers; leave return-to-user-only ASTs to an ordinary user LWP.
 */
static inline uint32_t
os_vmrun_entry_pending(void)
{
	struct lwp *lp;
	uint32_t pending;

	pending = mycpu->gd_reqflags;
	lp = curthread->td_lwp;
	if (lp != NULL && (lp->lwp_proc->p_flags & P_SYSTEM) != 0) {
		pending &= RQF_IDLECHECK_MASK | RQF_AST_USER_RESCHED |
		    RQF_AST_LWKT_RESCHED | RQF_XINVLTLB;
	} else {
		pending &= RQF_HVM_MASK;
	}
	return pending;
}

static inline uint32_t
os_vmrun_return_pending(void)
{
	struct lwp *lp;
	uint32_t pending;

	pending = mycpu->gd_reqflags;
	lp = curthread->td_lwp;
	if (lp != NULL && (lp->lwp_proc->p_flags & P_SYSTEM) != 0) {
		pending &= RQF_IDLECHECK_MASK | RQF_AST_USER_RESCHED |
		    RQF_AST_LWKT_RESCHED;
	} else {
		pending &= RQF_HVM_MASK & ~RQF_XINVLTLB;
	}
	return pending;
}

static inline bool
os_return_needed(void)
{
	if (__predict_false(os_vmrun_return_pending() != 0))
		return true;
	if (__predict_false(curthread->td_lwp != NULL &&
	    (curthread->td_lwp->lwp_mpflags & LWP_MP_URETMASK)))
		return true;
	return false;
}

static inline void
os_ipi_broadcast(void (*function)(void *), void *arg)
{
	cpumask_t mask;
	int cpu;

	for (cpu = 0; cpu < ncpus; cpu++) {
		CPUMASK_ASSBIT(mask, cpu);
		lwkt_cpusync_simple(mask, function, arg);
	}
}

#endif /* VMM_SVM_OS_H */
