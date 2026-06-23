/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Physical host core -- see vmm_host.h.  Pure; no kernel/VFS deps.
 */
#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif

#include "vmm_host.h"

void
vmm_host_init(struct vmm_host *h)
{
	h->device_count = 0;
}

void
vmm_host_add_device(struct vmm_host *h)
{
	h->device_count++;
}

uint32_t
vmm_host_device_count(const struct vmm_host *h)
{
	return h->device_count;
}
