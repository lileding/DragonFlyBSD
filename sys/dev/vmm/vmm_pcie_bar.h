/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One pathless shared-memory BAR capability.
 */
#ifndef VMM_PCIE_BAR_H
#define VMM_PCIE_BAR_H

#include <sys/types.h>

struct file;
struct vm_object;

struct vmm_pcie_bar {
	/* The device owns both references until provider detach. */
	struct file	*own_mut_fp;
	/* The device keeps the BAR backing alive for guest NPT mappings. */
	struct vm_object *own_mut_backing_object;
	uint64_t	imm_size;
	uint32_t	imm_flags;
};

int	vmm_pcie_bar_create(struct vmm_pcie_bar *bar, uint64_t size,
	    uint32_t flags);
void	vmm_pcie_bar_revoke(struct vmm_pcie_bar *bar);
void	vmm_pcie_bar_destroy(struct vmm_pcie_bar *bar);
/* Caller holds the parent fabric token_registry. */
int	vmm_pcie_bar_snapshot(struct vmm_pcie_bar *bar,
	    struct vm_object **objectp, uint64_t *sizep);
/* Caller holds an object reference obtained from vmm_pcie_bar_snapshot(). */
int	vmm_pcie_bar_object_read32(struct vm_object *object, uint64_t size,
	    uint64_t offset, uint32_t *valuep);
int	vmm_pcie_bar_mmap_active(void);

#endif /* VMM_PCIE_BAR_H */
