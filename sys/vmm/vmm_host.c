/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical host core -- see vmm_host.h.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <machine/atomic.h>

#include "vmm_host.h"

void
vmm_host_init(struct vmm_host *h)
{
	h->mut_device_count = 0;
	h->atomic_mut_next_cpu = 0;
}

void
vmm_host_add_device(struct vmm_host *h)
{
	h->mut_device_count++;
}

uint32_t
vmm_host_device_count(const struct vmm_host *h)
{
	return h->mut_device_count;
}

int
vmm_host_next_cpu(struct vmm_host *h)
{
	uint32_t n;

	if (ncpus <= 1)
		return 0;
	n = atomic_fetchadd_int(&h->atomic_mut_next_cpu, 1) % (uint32_t)ncpus;
	return (int)n;
}
