/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * PCIe function object -- see vmm_device.h.
 */
#include <sys/types.h>

#include "vmm_device.h"

void
vmm_device_init(struct vmm_device *device, const char *name, int nlen,
    uint64_t id, struct vmm_pcie *pcie, struct vmm_pcie_root *consumer,
    uint32_t bdf)
{
	int i;

	for (i = 0; i < nlen; i++)
		device->imm_name[i] = name[i];
	device->imm_name[nlen] = '\0';
	device->imm_name_len = (size_t)nlen;
	device->imm_id = id;
	device->borrow_imm_pcie = pcie;
	device->borrow_mut_consumer = consumer;
	device->mut_bdf = bdf;
	device->mut_state = VMM_DEVICE_NEW;
}

void
vmm_device_uninit(struct vmm_device *device)
{
	device->imm_name[0] = '\0';
	device->imm_name_len = 0;
	device->imm_id = 0;
	device->borrow_imm_pcie = 0;
	device->borrow_mut_consumer = 0;
	device->mut_bdf = 0;
	device->mut_state = VMM_DEVICE_NEW;
}

int
vmm_device_name_eq(const struct vmm_device *device, const char *name,
    int nlen)
{
	int i;

	if (name == 0 || nlen < 0 || (size_t)nlen != device->imm_name_len)
		return 0;
	for (i = 0; i < nlen; i++) {
		if (device->imm_name[i] != name[i])
			return 0;
	}
	return 1;
}
