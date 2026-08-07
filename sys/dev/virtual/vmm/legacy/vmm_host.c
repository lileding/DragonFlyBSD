/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host-local helpers -- see vmm_host.h.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <machine/atomic.h>

#include "vmm_host.h"

static uint32_t vmm_host_atomic_mut_next_cpu;

int
vmm_host_next_cpu(void)
{
	uint32_t n;

	if (ncpus <= 1)
		return 0;
	n = atomic_fetchadd_int(&vmm_host_atomic_mut_next_cpu, 1) %
	    (uint32_t)ncpus;
	return (int)n;
}
