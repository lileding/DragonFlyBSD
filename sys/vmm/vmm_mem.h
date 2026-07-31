/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The mem object: desired guest memory size plus kernel backing object.
 * FS presentation: vmmfs_mem.c.
 *
 * Types (uint64_t/size_t) come from the includer.
 */
#ifndef VMM_MEM_H
#define VMM_MEM_H

/*
 * The current pc64 backend maps one guest-RAM vm_object at GPA 0 in a
 * DragonFly machine vmspace.  This follows NVMM's VM/pmap mechanism, but the
 * dfvmm control model has no user-provided hmapping table: fd3 writes the same
 * object that the vCPU backend later faults through the machine vmspace.
 *
 * Keep the cap at the config boundary so start workers never accept a memory
 * size the backend cannot map.
 */
#define VMM_MEM_ALIGN	(2ull * 1024 * 1024)
#define VMM_MEM_MAX	(127ull * 1024 * (1ull << 30))
#define VMM_MEM_DMA_MAX_RANGES	4U

struct vmm_mem_dma_range {
	uint64_t	raw_gpa;
	uint64_t	imm_size;
};

struct vmm_mem {
	/*
	 * Lifecycle:
	 * vmmfs serializes config writes before start.  vmm_machine's command
	 * queue publishes/detaches own_mut_backing and releases it only after
	 * all vCPUs stop.  Each backing owns a loader-written boot vmspace and
	 * the current vCPU runtime vmspace.  A runtime is a COW fork of boot;
	 * guest reset discards only that runtime and forks boot again.  Guest
	 * pages are allocated lazily by loader mmap faults or by vCPU
	 * nested-page-fault handling.
	 */
	uint64_t	mut_bytes;		/* 0 = unset */
	struct vmm_mem_backing *own_mut_backing;
};

/*
 * Parse number[KkMmGg], > 0, <= VMM_MEM_MAX, VMM_MEM_ALIGN aligned.
 * 1 = updated, 0 = reject.
 */
int	vmm_mem_parse(struct vmm_mem *m, const char *buf, size_t len);
size_t	vmm_mem_format(const struct vmm_mem *m, char *out, size_t cap);
int	vmm_mem_is_set(const struct vmm_mem *m);

struct vm_object;
struct vmspace;
int	vmm_mem_prepare(uint64_t bytes, struct vmm_mem_backing **backingp);
int	vmm_mem_publish(struct vmm_mem *m, struct vmm_mem_backing *backing);
/* Fork the loader-complete boot vmspace for the first vCPU run. */
int	vmm_mem_start_run(struct vmm_mem *m);
/* Replace the stopped runtime vmspace with a fresh COW fork of boot. */
int	vmm_mem_reset_run(struct vmm_mem *m);
struct vmm_mem_backing *vmm_mem_detach(struct vmm_mem *m);
void	vmm_mem_release_backing(struct vmm_mem_backing *b);
/*
 * On success, *objectp owns one vm_object reference and must later call
 * vm_object_deallocate().
 */
int	vmm_mem_snapshot(struct vmm_mem *m, struct vm_object **objectp,
	    uint64_t *bytesp);
/*
 * Borrowed current runtime only, for vCPU backend lifetime.  The parent
 * machine keeps its backing alive until all active vCPUs have exited.
 */
struct vmspace *vmm_mem_borrow_vmspace(struct vmm_mem *m);
/*
 * On success, *vmspacep owns one vmspace reference.  The returned ranges
 * describe every guest-RAM GPA interval and exclude architectural MMIO holes.
 */
int	vmm_mem_dma_snapshot(struct vmm_mem *m, struct vmspace **vmspacep,
	    uint64_t *aperture_sizep, struct vmm_mem_dma_range *ranges,
	    unsigned int *range_countp);
/* Insert or remove a non-RAM object in the current run vmspace at a fixed GPA. */
int	vmm_mem_map_object(struct vmm_mem *m, uint64_t gpa, uint64_t size,
	    struct vm_object *object, uint64_t object_offset);
void	vmm_mem_unmap_object(struct vmm_mem *m, uint64_t gpa, uint64_t size);
/*
 * Called only by an active vCPU backend; memory detach waits for all vCPUs to
 * exit before releasing the backing.
 */
int	vmm_mem_fault_gpa(struct vmm_mem *m, uint64_t gpa, int prot);
/* Fault a non-RAM object that is already mapped in the current run vmspace. */
int	vmm_mem_fault_object_gpa(struct vmm_mem *m, uint64_t gpa, int prot);
int	vmm_mem_read_gpa(struct vmm_mem *m, uint64_t gpa, void *buf,
	    size_t len);

#endif /* VMM_MEM_H */
