/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Core vPCIe fabric -- see vmm_pcie.h.
 */
#include <sys/errno.h>
#include <sys/types.h>

#include "vmm_pcie.h"

static int	vmm_pcie_device_cmp(struct vmm_device *left,
		    struct vmm_device *right);
static struct vmm_device *vmm_pcie_device_find_name_locked(
		    struct vmm_pcie *pcie, const char *name, int nlen);

RB_GENERATE(vmm_pcie_device_tree, vmm_device, own_mut_registry_entry,
    vmm_pcie_device_cmp);

void
vmm_pcie_init(struct vmm_pcie *pcie)
{
	lwkt_token_init(&pcie->token_registry, "vmmpcie");
	RB_INIT(&pcie->mut_devices);
	pcie->mut_next_device_id = 1;
	vmm_pcie_root_init(&pcie->own_mut_host_root, pcie, 0);
}

void
vmm_pcie_uninit(struct vmm_pcie *pcie)
{
	lwkt_gettoken(&pcie->token_registry);
	RB_INIT(&pcie->mut_devices);
	pcie->mut_next_device_id = 0;
	lwkt_reltoken(&pcie->token_registry);
	vmm_pcie_root_uninit(&pcie->own_mut_host_root);
}

struct vmm_pcie_root *
vmm_pcie_host_root(struct vmm_pcie *pcie)
{
	return &pcie->own_mut_host_root;
}

int
vmm_pcie_device_create(struct vmm_pcie *pcie,
    struct vmm_pcie_root *consumer, const char *name, int nlen,
    struct vmm_device *device)
{
	uint32_t bdf;
	int error;

	if (consumer == 0 || consumer->borrow_imm_pcie != pcie ||
	    name == 0 || nlen <= 0 || nlen > VMM_DEVICE_NAME_MAX ||
	    device == 0)
		return EINVAL;
	lwkt_gettoken(&pcie->token_registry);
	if (vmm_pcie_device_find_name_locked(pcie, name, nlen) != 0) {
		error = EEXIST;
		goto out;
	}
	if (pcie->mut_next_device_id == 0) {
		error = EOVERFLOW;
		goto out;
	}
	bdf = vmm_pcie_root_bdf_alloc_locked(consumer);
	if (bdf == 0) {
		error = ENOSPC;
		goto out;
	}
	vmm_device_init(device, name, nlen, pcie->mut_next_device_id++, pcie,
	    consumer, bdf);
	if (RB_INSERT(vmm_pcie_device_tree, &pcie->mut_devices, device) != 0) {
		vmm_pcie_root_bdf_release_locked(consumer, bdf);
		vmm_device_uninit(device);
		error = EEXIST;
		goto out;
	}
	error = 0;
out:
	lwkt_reltoken(&pcie->token_registry);
	return error;
}

int
vmm_pcie_device_destroy(struct vmm_pcie *pcie, struct vmm_device *device)
{
	int error;

	if (device == 0 || device->borrow_imm_pcie != pcie)
		return EINVAL;
	lwkt_gettoken(&pcie->token_registry);
	if (vmm_pcie_device_find_name_locked(pcie, device->imm_name,
	    (int)device->imm_name_len) != device) {
		error = ENOENT;
		goto out;
	}
	if (device->mut_state != VMM_DEVICE_NEW) {
		error = EBUSY;
		goto out;
	}
	RB_REMOVE(vmm_pcie_device_tree, &pcie->mut_devices, device);
	vmm_pcie_root_bdf_release_locked(device->borrow_mut_consumer,
	    device->mut_bdf);
	vmm_device_uninit(device);
	error = 0;
out:
	lwkt_reltoken(&pcie->token_registry);
	return error;
}

int
vmm_pcie_device_move(struct vmm_pcie *pcie, struct vmm_device *device,
    struct vmm_pcie_root *consumer)
{
	uint32_t bdf;
	int error;

	if (device == 0 || device->borrow_imm_pcie != pcie ||
	    consumer == 0 || consumer->borrow_imm_pcie != pcie)
		return EINVAL;
	lwkt_gettoken(&pcie->token_registry);
	if (vmm_pcie_device_find_name_locked(pcie, device->imm_name,
	    (int)device->imm_name_len) != device) {
		error = ENOENT;
		goto out;
	}
	if (device->mut_state != VMM_DEVICE_NEW) {
		error = EBUSY;
		goto out;
	}
	if (device->borrow_mut_consumer == consumer) {
		error = 0;
		goto out;
	}
	bdf = vmm_pcie_root_bdf_alloc_locked(consumer);
	if (bdf == 0) {
		error = ENOSPC;
		goto out;
	}
	vmm_pcie_root_bdf_release_locked(device->borrow_mut_consumer,
	    device->mut_bdf);
	device->borrow_mut_consumer = consumer;
	device->mut_bdf = bdf;
	error = 0;
out:
	lwkt_reltoken(&pcie->token_registry);
	return error;
}

struct vmm_device *
vmm_pcie_device_find(struct vmm_pcie *pcie,
    const struct vmm_pcie_root *consumer, const char *name, int nlen)
{
	struct vmm_device *device;

	if (pcie == 0 || consumer == 0 || consumer->borrow_imm_pcie != pcie)
		return 0;
	lwkt_gettoken(&pcie->token_registry);
	device = vmm_pcie_device_find_name_locked(pcie, name, nlen);
	if (device != 0 && device->borrow_mut_consumer != consumer)
		device = 0;
	lwkt_reltoken(&pcie->token_registry);
	return device;
}

struct vmm_device *
vmm_pcie_device_find_name(struct vmm_pcie *pcie, const char *name, int nlen)
{
	struct vmm_device *device;

	if (pcie == 0)
		return 0;
	lwkt_gettoken(&pcie->token_registry);
	device = vmm_pcie_device_find_name_locked(pcie, name, nlen);
	lwkt_reltoken(&pcie->token_registry);
	return device;
}

int
vmm_pcie_device_attached_to(const struct vmm_device *device,
    const struct vmm_pcie_root *consumer)
{
	struct vmm_pcie *pcie;
	int attached;

	if (device == 0 || consumer == 0)
		return 0;
	pcie = device->borrow_imm_pcie;
	if (pcie == 0 || consumer->borrow_imm_pcie != pcie)
		return 0;
	lwkt_gettoken(&pcie->token_registry);
	attached = device->borrow_mut_consumer == consumer;
	lwkt_reltoken(&pcie->token_registry);
	return attached;
}

struct vmm_pcie_root *
vmm_pcie_device_consumer(const struct vmm_device *device)
{
	struct vmm_pcie *pcie;
	struct vmm_pcie_root *consumer;

	if (device == 0 || device->borrow_imm_pcie == 0)
		return 0;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	consumer = device->borrow_mut_consumer;
	lwkt_reltoken(&pcie->token_registry);
	return consumer;
}

uint32_t
vmm_pcie_device_bdf(const struct vmm_device *device)
{
	struct vmm_pcie *pcie;
	uint32_t bdf;

	if (device == 0 || device->borrow_imm_pcie == 0)
		return 0;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	bdf = device->mut_bdf;
	lwkt_reltoken(&pcie->token_registry);
	return bdf;
}

static int
vmm_pcie_device_cmp(struct vmm_device *left, struct vmm_device *right)
{
	unsigned int i;

	for (i = 0; i < sizeof(left->imm_name); i++) {
		if (left->imm_name[i] < right->imm_name[i])
			return -1;
		if (left->imm_name[i] > right->imm_name[i])
			return 1;
		if (left->imm_name[i] == '\0')
			return 0;
	}
	return 0;
}

static struct vmm_device *
vmm_pcie_device_find_name_locked(struct vmm_pcie *pcie, const char *name,
    int nlen)
{
	struct vmm_device *device;

	if (name == 0 || nlen < 0 || nlen > VMM_DEVICE_NAME_MAX)
		return 0;
	device = RB_ROOT(&pcie->mut_devices);
	while (device != 0) {
		int cmp;
		int i;

		for (i = 0; i < nlen; i++) {
			if (name[i] != device->imm_name[i])
				break;
		}
		if (i == nlen) {
			if (device->imm_name[i] == '\0')
				return device;
			cmp = -1;
		} else {
			cmp = name[i] < device->imm_name[i] ? -1 : 1;
		}
		device = cmp < 0 ? RB_LEFT(device, own_mut_registry_entry) :
		    RB_RIGHT(device, own_mut_registry_entry);
	}
	return 0;
}
