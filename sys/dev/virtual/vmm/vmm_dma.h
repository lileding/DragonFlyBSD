/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-run DMA authority for vPCIe providers.
 */
#ifndef VMM_DMA_H
#define VMM_DMA_H

#include <sys/types.h>

#include "vmm_mem.h"

struct file;
struct vmm_dma_cap;

struct vmm_dma {
	/* The serialized machine taskqueue owns every field in this object. */
	struct vmspace		*own_mut_vmspace;
	uint64_t		mut_generation;
	uint64_t		imm_aperture_size;
	unsigned int		imm_range_count;
	struct vmm_mem_dma_range
				imm_ranges[VMM_MEM_DMA_MAX_RANGES];
};

void	vmm_dma_init(struct vmm_dma *dma);
void	vmm_dma_uninit(struct vmm_dma *dma);
int	vmm_dma_start(struct vmm_dma *dma, struct vmm_mem *mem);
void	vmm_dma_stop(struct vmm_dma *dma);
int	vmm_dma_cap_create(struct vmm_dma *dma, struct vmm_dma_cap **capp);
void	vmm_dma_cap_revoke(struct vmm_dma_cap *cap);
struct file *vmm_dma_cap_file_hold(struct vmm_dma_cap *cap);
uint64_t vmm_dma_cap_generation(const struct vmm_dma_cap *cap);
unsigned int vmm_dma_cap_ranges(const struct vmm_dma_cap *cap,
	    struct vmm_mem_dma_range *ranges, unsigned int range_cap);
int	vmm_dma_mmap_active(void);

#endif /* VMM_DMA_H */
