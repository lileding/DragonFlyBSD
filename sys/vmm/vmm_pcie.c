/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Core vPCIe fabric -- see vmm_pcie.h.
 */
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <vm/vm_object.h>

#include "vmm_machine.h"
#include "vmm_mem.h"
#include "vmm_pcie.h"
#include "vmm_pcie_bar.h"
#include "vmm_pcie_config.h"
#include "vmm_pcie_layout.h"
#include "vmm_pcie_user.h"

#define VMM_PCIE_MSIX_TABLE_ADDRESS_LOW	0U
#define VMM_PCIE_MSIX_TABLE_ADDRESS_HIGH	4U
#define VMM_PCIE_MSIX_TABLE_DATA		8U
#define VMM_PCIE_MSIX_TABLE_VECTOR_CONTROL	12U
#define VMM_PCIE_MSI_ADDRESS_BASE		0xfee00000U
#define VMM_PCIE_MSI_ADDRESS_DEST_MASK	0x000ff000U
#define VMM_PCIE_MSI_ADDRESS_CONTROL_MASK	0x0000000cU
#define VMM_PCIE_MSI_DATA_VECTOR_MASK		0x000000ffU
#define VMM_PCIE_MSI_DATA_DELIVERY_MASK	0x00000700U
#define VMM_PCIE_MSI_DATA_ALLOWED		(VMM_PCIE_MSI_DATA_VECTOR_MASK | \
	 VMM_PCIE_MSI_DATA_DELIVERY_MASK)

static int	vmm_pcie_device_cmp(struct vmm_device *left,
		    struct vmm_device *right);
static struct vmm_device *vmm_pcie_device_find_name_locked(
		    struct vmm_pcie *pcie, const char *name, int nlen);
static int	vmm_pcie_device_busy_locked(struct vmm_pcie *pcie);
static void	vmm_pcie_device_clear_registered_locked(
		    struct vmm_device *device, struct vmm_pcie_bar *bars,
		    struct vmm_pcie_config **configp);
static int	vmm_pcie_bar_range_valid(uint64_t gpa, uint64_t size);
static int	vmm_pcie_range_overlaps(uint64_t a_gpa, uint64_t a_size,
		    uint64_t b_gpa, uint64_t b_size);
static int	vmm_pcie_root_bar_overlap_locked(struct vmm_pcie_root *root,
		    const struct vmm_device *skip_device, unsigned int skip_index,
		    uint64_t gpa, uint64_t size);

RB_GENERATE(vmm_pcie_device_tree, vmm_device, own_mut_registry_entry,
    vmm_pcie_device_cmp);

void
vmm_pcie_init(struct vmm_pcie *pcie)
{

	lwkt_token_init(&pcie->token_registry, "vmmpcie");
	RB_INIT(&pcie->mut_devices);
	pcie->mut_next_device_id = 1;
	pcie->mut_next_root_id = 1;
	pcie->mut_closing = 0;
	vmm_pcie_root_init(&pcie->own_mut_host_root, pcie, NULL);
}

void
vmm_pcie_uninit(struct vmm_pcie *pcie)
{

	lwkt_gettoken(&pcie->token_registry);
	if (!RB_EMPTY(&pcie->mut_devices)) {
		lwkt_reltoken(&pcie->token_registry);
		return;
	}
	pcie->mut_next_device_id = 0;
	pcie->mut_next_root_id = 0;
	pcie->mut_closing = 1;
	lwkt_reltoken(&pcie->token_registry);
	vmm_pcie_root_uninit(&pcie->own_mut_host_root);
}

int
vmm_pcie_begin_shutdown(struct vmm_pcie *pcie)
{
	int error;

	if (pcie == NULL)
		return EINVAL;
	lwkt_gettoken(&pcie->token_registry);
	if (pcie->mut_closing || vmm_pcie_device_busy_locked(pcie)) {
		error = EBUSY;
	} else {
		pcie->mut_closing = 1;
		error = 0;
	}
	lwkt_reltoken(&pcie->token_registry);
	return error;
}

void
vmm_pcie_cancel_shutdown(struct vmm_pcie *pcie)
{

	if (pcie == NULL)
		return;
	lwkt_gettoken(&pcie->token_registry);
	pcie->mut_closing = 0;
	lwkt_reltoken(&pcie->token_registry);
}

uint64_t
vmm_pcie_root_id_alloc(struct vmm_pcie *pcie)
{
	uint64_t id;

	if (pcie == NULL)
		return 0;
	lwkt_gettoken(&pcie->token_registry);
	id = pcie->mut_next_root_id;
	if (id != 0 && id != (uint64_t)-1)
		pcie->mut_next_root_id++;
	else
		id = 0;
	lwkt_reltoken(&pcie->token_registry);
	return id;
}

struct vmm_pcie_root *
vmm_pcie_host_root(struct vmm_pcie *pcie)
{

	return &pcie->own_mut_host_root;
}

int
vmm_pcie_device_create(struct vmm_pcie *pcie,
    struct vmm_pcie_root *root, const char *name, int nlen,
    struct vmm_device *device)
{
	uint32_t bdf;
	int error;

	if (root == NULL || root->borrow_imm_pcie != pcie || name == NULL ||
	    nlen <= 0 || nlen > VMM_DEVICE_NAME_MAX || device == NULL)
		return EINVAL;
	lwkt_gettoken(&pcie->token_registry);
	if (pcie->mut_closing) {
		error = EBUSY;
		goto out;
	}
	if (vmm_pcie_device_find_name_locked(pcie, name, nlen) != NULL) {
		error = EEXIST;
		goto out;
	}
	if (pcie->mut_next_device_id == 0) {
		error = EOVERFLOW;
		goto out;
	}
	bdf = vmm_pcie_root_bdf_alloc_locked(root);
	if (bdf == 0) {
		error = ENOSPC;
		goto out;
	}
	vmm_device_init(device, name, nlen, pcie->mut_next_device_id++, pcie,
	    root, bdf);
	if (RB_INSERT(vmm_pcie_device_tree, &pcie->mut_devices, device) != NULL) {
		vmm_pcie_root_bdf_release_locked(root, bdf);
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

	if (device == NULL || device->borrow_imm_pcie != pcie)
		return EINVAL;
	lwkt_gettoken(&pcie->token_registry);
	if (vmm_pcie_device_find_name_locked(pcie, device->imm_name,
	    (int)device->imm_name_len) != device) {
		error = ENOENT;
		goto out;
	}
	if (device->borrow_mut_provider != NULL ||
	    device->borrow_mut_offload != NULL || device->mut_registered) {
		error = EBUSY;
		goto out;
	}
	RB_REMOVE(vmm_pcie_device_tree, &pcie->mut_devices, device);
	vmm_pcie_root_bdf_release_locked(device->borrow_mut_root,
	    device->mut_bdf);
	vmm_device_uninit(device);
	error = 0;
out:
	lwkt_reltoken(&pcie->token_registry);
	return error;
}

int
vmm_pcie_device_move(struct vmm_pcie *pcie, struct vmm_device *device,
    struct vmm_pcie_root *root)
{
	uint32_t bdf;
	int error;

	if (device == NULL || device->borrow_imm_pcie != pcie || root == NULL ||
	    root->borrow_imm_pcie != pcie)
		return EINVAL;
	lwkt_gettoken(&pcie->token_registry);
	if (pcie->mut_closing) {
		error = EBUSY;
		goto out;
	}
	if (vmm_pcie_device_find_name_locked(pcie, device->imm_name,
	    (int)device->imm_name_len) != device) {
		error = ENOENT;
		goto out;
	}
	if (device->borrow_mut_provider != NULL ||
	    device->borrow_mut_offload != NULL || device->mut_registered) {
		error = EBUSY;
		goto out;
	}
	if (device->borrow_mut_root == root) {
		error = 0;
		goto out;
	}
	bdf = vmm_pcie_root_bdf_alloc_locked(root);
	if (bdf == 0) {
		error = ENOSPC;
		goto out;
	}
	vmm_pcie_root_bdf_release_locked(device->borrow_mut_root,
	    device->mut_bdf);
	device->borrow_mut_root = root;
	device->mut_bdf = bdf;
	if (device->mut_attachment_generation != (uint64_t)-1)
		device->mut_attachment_generation++;
	error = 0;
out:
	lwkt_reltoken(&pcie->token_registry);
	return error;
}

struct vmm_device *
vmm_pcie_device_find(struct vmm_pcie *pcie,
    const struct vmm_pcie_root *root, const char *name, int nlen)
{
	struct vmm_device *device;

	if (pcie == NULL || root == NULL || root->borrow_imm_pcie != pcie)
		return NULL;
	lwkt_gettoken(&pcie->token_registry);
	device = vmm_pcie_device_find_name_locked(pcie, name, nlen);
	if (device != NULL && device->borrow_mut_root != root)
		device = NULL;
	lwkt_reltoken(&pcie->token_registry);
	return device;
}

struct vmm_device *
vmm_pcie_device_find_name(struct vmm_pcie *pcie, const char *name, int nlen)
{
	struct vmm_device *device;

	if (pcie == NULL)
		return NULL;
	lwkt_gettoken(&pcie->token_registry);
	device = vmm_pcie_device_find_name_locked(pcie, name, nlen);
	lwkt_reltoken(&pcie->token_registry);
	return device;
}

int
vmm_pcie_device_at_root(const struct vmm_device *device,
    const struct vmm_pcie_root *root)
{
	struct vmm_pcie *pcie;
	int attached;

	if (device == NULL || root == NULL)
		return 0;
	pcie = device->borrow_imm_pcie;
	if (pcie == NULL || root->borrow_imm_pcie != pcie)
		return 0;
	lwkt_gettoken(&pcie->token_registry);
	attached = device->borrow_mut_root == root;
	lwkt_reltoken(&pcie->token_registry);
	return attached;
}

struct vmm_pcie_root *
vmm_pcie_device_root(const struct vmm_device *device)
{
	struct vmm_pcie *pcie;
	struct vmm_pcie_root *root;

	if (device == NULL || device->borrow_imm_pcie == NULL)
		return NULL;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	root = device->borrow_mut_root;
	lwkt_reltoken(&pcie->token_registry);
	return root;
}

uint32_t
vmm_pcie_device_bdf(const struct vmm_device *device)
{
	struct vmm_pcie *pcie;
	uint32_t bdf;

	if (device == NULL || device->borrow_imm_pcie == NULL)
		return 0;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	bdf = device->mut_bdf;
	lwkt_reltoken(&pcie->token_registry);
	return bdf;
}

int
vmm_pcie_device_provider_attach(struct vmm_device *device,
    struct vmm_pcie_user *provider)
{
	struct vmm_pcie *pcie;
	int error;

	if (device == NULL || provider == NULL ||
	    device->borrow_imm_pcie == NULL)
		return EINVAL;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	if (pcie->mut_closing || device->borrow_mut_provider != NULL ||
	    device->mut_registered) {
		error = EBUSY;
	} else if (device->mut_attachment_generation == (uint64_t)-1) {
		error = EOVERFLOW;
	} else {
		device->borrow_mut_provider = provider;
		device->mut_attachment_generation++;
		error = 0;
	}
	lwkt_reltoken(&pcie->token_registry);
	return error;
}

int
vmm_pcie_device_provider_register(struct vmm_device *device,
    struct vmm_pcie_user *provider, const struct vmm_pcie_abi_register *request,
    struct vmm_pcie_abi_registered *response, struct file **bar_fps,
    unsigned int *bar_countp)
{
	struct vmm_pcie_bar bars[VMM_PCIE_ABI_MAX_BARS];
	struct vmm_pcie_config *config;
	struct vmm_pcie *pcie;
	uint32_t bar_fd_mask;
	unsigned int bar_count;
	unsigned int i;
	int error;

	if (device == NULL || provider == NULL || request == NULL ||
	    response == NULL || bar_fps == NULL || bar_countp == NULL ||
	    device->borrow_imm_pcie == NULL ||
	    vmm_pcie_abi_validate(request, sizeof(*request)) != 0)
		return EINVAL;
	__builtin_memset(bars, 0, sizeof(bars));
	config = NULL;
	error = vmm_pcie_config_create(&config, request);
	if (error != 0)
		return error;
	bar_count = 0;
	bar_fd_mask = 0;
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		uint64_t size;
		uint32_t flags;

		size = le64toh(request->bar[i].le_size);
		flags = le32toh(request->bar[i].le_flags);
		if (size == 0)
			continue;
		error = vmm_pcie_bar_create(&bars[i], size, flags);
		if (error != 0)
			goto fail;
		bar_fps[bar_count++] = bars[i].own_mut_fp;
		bar_fd_mask |= 1U << i;
	}

	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	if (pcie->mut_closing || device->borrow_mut_provider != provider ||
	    device->mut_registered) {
		error = EBUSY;
		lwkt_reltoken(&pcie->token_registry);
		goto fail;
	}
	if (device->mut_attachment_generation == (uint64_t)-1) {
		error = EOVERFLOW;
		lwkt_reltoken(&pcie->token_registry);
		goto fail;
	}
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		device->own_mut_bars[i] = bars[i];
		__builtin_memset(&bars[i], 0, sizeof(bars[i]));
	}
	device->own_mut_config = config;
	config = NULL;
	device->mut_vendor_id = le16toh(request->le_vendor_id);
	device->mut_device_id = le16toh(request->le_device_id);
	device->mut_subsystem_vendor_id = le16toh(request->le_subsystem_vendor_id);
	device->mut_subsystem_device_id = le16toh(request->le_subsystem_device_id);
	device->mut_class_code = le32toh(request->le_class_code);
	device->mut_msix_vectors = le16toh(request->le_msix_vectors);
	device->mut_revision = request->revision;
	device->mut_registered = 1;
	device->mut_attachment_generation++;
	__builtin_memset(response, 0, sizeof(*response));
	response->header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	response->header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	response->header.le_type = htole16(VMM_PCIE_ABI_MSG_REGISTERED);
	response->header.le_size = htole32(sizeof(*response));
	response->header.le_sequence = request->header.le_sequence;
	response->le_device_id = htole64(device->imm_id);
	response->le_consumer_id = htole64(device->borrow_mut_root->imm_id);
	response->le_attachment_generation = htole64(
	    device->mut_attachment_generation);
	response->le_bdf = htole32(device->mut_bdf);
	response->le_bar_fd_mask = htole32(bar_fd_mask);
	response->le_msix_vectors = htole16(device->mut_msix_vectors);
	lwkt_reltoken(&pcie->token_registry);
	*bar_countp = bar_count;
	return 0;

fail:
	vmm_pcie_config_destroy(config);
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++)
		vmm_pcie_bar_destroy(&bars[i]);
	return error;
}

void
vmm_pcie_device_provider_detach(struct vmm_device *device,
    struct vmm_pcie_user *provider)
{
	struct vmm_pcie_bar bars[VMM_PCIE_ABI_MAX_BARS];
	struct vmm_pcie_bar_mapping mappings[VMM_PCIE_ABI_MAX_BARS];
	struct vmm_pcie_config *config;
	struct vmm_pcie *pcie;
	struct vmm_pcie_root *root;
	unsigned int i;

	if (device == NULL || provider == NULL ||
	    device->borrow_imm_pcie == NULL)
		return;
	__builtin_memset(bars, 0, sizeof(bars));
	__builtin_memset(mappings, 0, sizeof(mappings));
	config = NULL;
	root = NULL;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	if (device->borrow_mut_provider == provider) {
		root = device->borrow_mut_root;
		vmm_pcie_device_bar_mappings_take_locked(device, mappings);
		vmm_pcie_device_clear_registered_locked(device, bars, &config);
		device->borrow_mut_provider = NULL;
		if (device->mut_attachment_generation != (uint64_t)-1)
			device->mut_attachment_generation++;
	}
	lwkt_reltoken(&pcie->token_registry);
	/*
	 * This is PCIe hot-unplug, not a machine lifecycle command.  vm_map_remove
	 * invalidates the tracked run pmap before the device drops its BAR references.
	 * A BAR fd already sent to the provider retains its own capability reference;
	 * P4 does not force-revoke that fd or its existing user mappings.  Later
	 * guest BAR access is an NPF for the guest OS to handle.
	 */
	vmm_pcie_root_bar_mappings_unmap(root, mappings);
	vmm_pcie_config_destroy(config);
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++)
		vmm_pcie_bar_destroy(&bars[i]);
}

int
vmm_pcie_device_provider_msix(struct vmm_device *device,
    struct vmm_pcie_user *provider, const struct vmm_pcie_abi_msix *message)
{
	struct vmm_pcie_root *root;
	struct vmm_machine *machine;
	struct vm_object *bar_object;
	struct vmm_pcie *pcie;
	uint64_t attachment_generation;
	uint64_t bar_size;
	uint64_t table_offset;
	uint32_t address_high;
	uint32_t address_low;
	uint32_t data;
	uint32_t vector_control;
	uint16_t vector;
	int error;

	if (device == NULL || provider == NULL || message == NULL ||
	    device->borrow_imm_pcie == NULL)
		return EINVAL;
	pcie = device->borrow_imm_pcie;
	bar_object = NULL;
	bar_size = 0;
	attachment_generation = le64toh(message->le_attachment_generation);
	vector = le16toh(message->le_vector);
	lwkt_gettoken(&pcie->token_registry);
	if (device->borrow_mut_provider != provider || !device->mut_registered ||
	    device->own_mut_config == NULL ||
	    le64toh(message->le_device_id) != device->imm_id ||
	    attachment_generation != device->mut_attachment_generation ||
	    vector >= device->mut_msix_vectors) {
		error = ESTALE;
	} else if (!vmm_pcie_config_msix_enabled_locked(device->own_mut_config) ||
	    vmm_pcie_config_msix_function_masked_locked(device->own_mut_config)) {
		error = 0;
	} else {
		error = vmm_pcie_bar_snapshot(
		    &device->own_mut_bars[VMM_PCIE_ABI_MSIX_BAR_INDEX],
		    &bar_object, &bar_size);
	}
	lwkt_reltoken(&pcie->token_registry);
	if (error != 0 || bar_object == NULL)
		return error;
	table_offset = VMM_PCIE_ABI_MSIX_TABLE_OFFSET +
	    (uint64_t)vector * VMM_PCIE_ABI_MSIX_ENTRY_SIZE;
	error = vmm_pcie_bar_object_read32(bar_object, bar_size,
	    table_offset + VMM_PCIE_MSIX_TABLE_ADDRESS_LOW, &address_low);
	if (error == 0) {
		error = vmm_pcie_bar_object_read32(bar_object, bar_size,
		    table_offset + VMM_PCIE_MSIX_TABLE_ADDRESS_HIGH, &address_high);
	}
	if (error == 0) {
		error = vmm_pcie_bar_object_read32(bar_object, bar_size,
		    table_offset + VMM_PCIE_MSIX_TABLE_DATA, &data);
	}
	if (error == 0) {
		error = vmm_pcie_bar_object_read32(bar_object, bar_size,
		    table_offset + VMM_PCIE_MSIX_TABLE_VECTOR_CONTROL,
		    &vector_control);
	}
	vm_object_deallocate(bar_object);
	if (error != 0)
		return error;
	/* This first backend accepts one xAPIC physical-destination MSI format. */
	if (address_high != 0 ||
	    (address_low & ~(VMM_PCIE_MSI_ADDRESS_DEST_MASK |
	    VMM_PCIE_MSI_ADDRESS_CONTROL_MASK)) != VMM_PCIE_MSI_ADDRESS_BASE ||
	    (address_low & (VMM_PCIE_MSI_ADDRESS_DEST_MASK |
	    VMM_PCIE_MSI_ADDRESS_CONTROL_MASK)) != 0 ||
	    (data & ~VMM_PCIE_MSI_DATA_ALLOWED) != 0 ||
	    (data & VMM_PCIE_MSI_DATA_DELIVERY_MASK) != 0 ||
	    (data & VMM_PCIE_MSI_DATA_VECTOR_MASK) < 32 ||
	    vmm_pcie_config_msix_vector_masked(vector_control))
		return 0;

	/* Revalidate after faulting the shared BAR before dereferencing root->machine. */
	lwkt_gettoken(&pcie->token_registry);
	root = device->borrow_mut_root;
	if (device->borrow_mut_provider == provider && device->mut_registered &&
	    device->own_mut_config != NULL &&
	    attachment_generation == device->mut_attachment_generation &&
	    vector < device->mut_msix_vectors &&
	    vmm_pcie_config_msix_enabled_locked(device->own_mut_config) &&
	    !vmm_pcie_config_msix_function_masked_locked(device->own_mut_config) &&
	    root != NULL && root->borrow_imm_machine != NULL) {
		machine = root->borrow_imm_machine;
		vmm_machine_msix(machine,
		    (uint8_t)(data & VMM_PCIE_MSI_DATA_VECTOR_MASK));
	}
	lwkt_reltoken(&pcie->token_registry);
	return 0;
}

void
vmm_pcie_device_provider_force_close(struct vmm_device *device)
{
	struct vmm_machine *machine;
	struct vmm_pcie_user *provider;
	struct vmm_pcie *pcie;
	uint32_t bdf;
	int error;

	if (device == NULL || device->borrow_imm_pcie == NULL)
		return;
	pcie = device->borrow_imm_pcie;
	provider = NULL;
	machine = NULL;
	bdf = 0;
	lwkt_gettoken(&pcie->token_registry);
	if (device->borrow_mut_provider != NULL) {
		provider = device->borrow_mut_provider;
		machine = device->borrow_mut_root->borrow_imm_machine;
		bdf = device->mut_bdf;
		error = vmm_pcie_user_force_close(provider);
	} else {
		error = 0;
	}
	lwkt_reltoken(&pcie->token_registry);

	if (error != 0 && machine != NULL) {
		vmm_machine_logf(machine,
		    "pcie provider force close bdf=%04x error=%d", bdf, error);
	}
	if (provider != NULL)
		vmm_pcie_device_provider_detach(device, provider);
}

int
vmm_pcie_device_consumer_attach(struct vmm_device *device,
    struct vmm_pcie_user *consumer,
    struct vmm_pcie_abi_consumer_ready *response)
{
	struct vmm_pcie *pcie;
	int error;

	if (device == NULL || consumer == NULL || response == NULL ||
	    device->borrow_imm_pcie == NULL)
		return EINVAL;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	if (pcie->mut_closing || device->borrow_mut_offload != NULL) {
		error = EBUSY;
	} else if (device->mut_attachment_generation == (uint64_t)-1) {
		error = EOVERFLOW;
	} else {
		device->borrow_mut_offload = consumer;
		device->mut_attachment_generation++;
		__builtin_memset(response, 0, sizeof(*response));
		response->header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
		response->header.le_version = htole16(VMM_PCIE_ABI_VERSION);
		response->header.le_type = htole16(VMM_PCIE_ABI_MSG_CONSUMER_READY);
		response->header.le_size = htole32(sizeof(*response));
		response->header.le_sequence = htole64(1);
		response->le_device_id = htole64(device->imm_id);
		response->le_consumer_id = htole64(device->borrow_mut_root->imm_id);
		response->le_attachment_generation = htole64(
		    device->mut_attachment_generation);
		error = 0;
	}
	lwkt_reltoken(&pcie->token_registry);
	return error;
}

void
vmm_pcie_device_consumer_detach(struct vmm_device *device,
    struct vmm_pcie_user *consumer)
{
	struct vmm_pcie *pcie;

	if (device == NULL || consumer == NULL ||
	    device->borrow_imm_pcie == NULL)
		return;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	if (device->borrow_mut_offload == consumer) {
		device->borrow_mut_offload = NULL;
		if (device->mut_attachment_generation != (uint64_t)-1)
			device->mut_attachment_generation++;
	}
	lwkt_reltoken(&pcie->token_registry);
}

void
vmm_pcie_device_bar_mappings_take_locked(struct vmm_device *device,
    struct vmm_pcie_bar_mapping *mappings)
{
	unsigned int i;

	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		mappings[i].raw_gpa = device->mut_bar_gpa[i];
		mappings[i].imm_size = device->mut_bar_gpa[i] != 0 ?
		    device->own_mut_bars[i].imm_size : 0;
		device->mut_bar_gpa[i] = 0;
	}
}

void
vmm_pcie_root_bar_mappings_unmap(struct vmm_pcie_root *root,
    const struct vmm_pcie_bar_mapping *mappings)
{
	struct vmm_machine *machine;
	unsigned int i;

	if (root == NULL || mappings == NULL || root->borrow_imm_machine == NULL)
		return;
	machine = root->borrow_imm_machine;
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		if (mappings[i].raw_gpa != 0 && mappings[i].imm_size != 0) {
			vmm_mem_unmap_object(&machine->own_mut_mem,
			    mappings[i].raw_gpa, mappings[i].imm_size);
		}
	}
}

int
vmm_pcie_root_bar_fault(struct vmm_pcie_root *root, uint64_t gpa, int prot)
{
	struct vmm_device *device;
	struct vmm_machine *machine;
	struct vmm_pcie *pcie;
	struct vm_object *object;
	uint64_t bar_gpa;
	uint64_t bar_size;
	uint64_t mapped_gpa;
	uint64_t mapped_size;
	unsigned int bar_index;
	uint32_t bdf;
	int mapped;
	int mapped_now;
	int error;

	if (root == NULL || root->borrow_imm_machine == NULL ||
	    root->borrow_imm_pcie == NULL)
		return ENOENT;
	machine = root->borrow_imm_machine;
	pcie = root->borrow_imm_pcie;
	object = NULL;
	bar_gpa = 0;
	bar_size = 0;
	mapped_gpa = 0;
	mapped_size = 0;
	bar_index = 0;
	bdf = 0;
	mapped = 0;
	mapped_now = 0;
	error = ENOENT;
	lwkt_gettoken(&pcie->token_registry);
	RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
		unsigned int i;

			if (device->borrow_mut_root != root || !device->mut_registered ||
		    device->own_mut_config == NULL)
			continue;
		for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
			if (vmm_pcie_config_bar_decode_locked(device->own_mut_config,
			    i, &bar_gpa, &bar_size) != 0 || gpa < bar_gpa ||
			    gpa - bar_gpa >= bar_size)
				continue;
			if (!vmm_pcie_bar_range_valid(bar_gpa, bar_size) ||
			    vmm_pcie_root_bar_overlap_locked(root, device, i,
			    bar_gpa, bar_size)) {
				error = EINVAL;
				goto out;
			}
			bar_index = i;
			mapped = device->mut_bar_gpa[i] == bar_gpa;
			if (!mapped)
				error = vmm_pcie_bar_snapshot(&device->own_mut_bars[i],
				    &object, &bar_size);
			else
				error = 0;
			goto out;
		}
	}
out:
	lwkt_reltoken(&pcie->token_registry);
	if (error != 0)
		return error;
	if (!mapped) {
		mapped_gpa = bar_gpa;
		mapped_size = bar_size;
		error = vmm_mem_map_object(&machine->own_mut_mem, bar_gpa,
		    bar_size, object);
		vm_object_deallocate(object);
		if (error != 0)
			return error;
		lwkt_gettoken(&pcie->token_registry);
		if (device->borrow_mut_root != root || !device->mut_registered ||
		    device->own_mut_config == NULL ||
		    vmm_pcie_config_bar_decode_locked(device->own_mut_config,
		    bar_index, &bar_gpa, &bar_size) != 0 ||
		    bar_gpa != mapped_gpa || bar_size != mapped_size ||
		    device->mut_bar_gpa[bar_index] != 0) {
			error = ENOENT;
			} else {
				device->mut_bar_gpa[bar_index] = bar_gpa;
				bdf = device->mut_bdf;
				mapped_now = 1;
				error = 0;
			}
		lwkt_reltoken(&pcie->token_registry);
		if (error != 0) {
			vmm_mem_unmap_object(&machine->own_mut_mem, mapped_gpa,
			    mapped_size);
			return error;
		}
	}
	if (mapped_now) {
		vmm_machine_logf(machine,
		    "pcie bar map bdf=%04x bar=%u gpa=0x%jx size=0x%jx", bdf,
		    bar_index, (uintmax_t)mapped_gpa, (uintmax_t)mapped_size);
	}
	return vmm_mem_fault_object_gpa(&machine->own_mut_mem, gpa, prot);
}

void
vmm_pcie_root_reset(struct vmm_pcie_root *root)
{
	struct vmm_device *device;
	struct vmm_pcie *pcie;

	if (root == NULL || root->borrow_imm_pcie == NULL)
		return;
	pcie = root->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
		if (device->borrow_mut_root != root || !device->mut_registered ||
		    device->own_mut_config == NULL)
			continue;
		__builtin_memset(device->mut_bar_gpa, 0,
		    sizeof(device->mut_bar_gpa));
		vmm_pcie_config_reset_locked(device->own_mut_config);
	}
	lwkt_reltoken(&pcie->token_registry);
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

	if (name == NULL || nlen < 0 || nlen > VMM_DEVICE_NAME_MAX)
		return NULL;
	device = RB_ROOT(&pcie->mut_devices);
	while (device != NULL) {
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
	return NULL;
}

static int
vmm_pcie_device_busy_locked(struct vmm_pcie *pcie)
{
	struct vmm_device *device;

	RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
		if (device->borrow_mut_provider != NULL ||
		    device->borrow_mut_offload != NULL || device->mut_registered)
			return 1;
	}
	return 0;
}

static void
vmm_pcie_device_clear_registered_locked(struct vmm_device *device,
    struct vmm_pcie_bar *bars, struct vmm_pcie_config **configp)
{
	unsigned int i;

	if (!device->mut_registered)
		return;
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		bars[i] = device->own_mut_bars[i];
		__builtin_memset(&device->own_mut_bars[i], 0,
		    sizeof(device->own_mut_bars[i]));
	}
	*configp = device->own_mut_config;
	device->own_mut_config = NULL;
	device->mut_vendor_id = 0;
	device->mut_device_id = 0;
	device->mut_subsystem_vendor_id = 0;
	device->mut_subsystem_device_id = 0;
	device->mut_class_code = 0;
	device->mut_msix_vectors = 0;
	device->mut_revision = 0;
	device->mut_registered = 0;
}

static int
vmm_pcie_bar_range_valid(uint64_t gpa, uint64_t size)
{

	return size != 0 && (gpa & (size - 1)) == 0 &&
	    gpa >= VMM_PCIE_MMIO_BASE && gpa < VMM_PCIE_MMIO_END &&
	    size <= VMM_PCIE_MMIO_END - gpa;
}

static int
vmm_pcie_range_overlaps(uint64_t a_gpa, uint64_t a_size, uint64_t b_gpa,
    uint64_t b_size)
{

	if (a_gpa <= b_gpa)
		return b_gpa - a_gpa < a_size;
	return a_gpa - b_gpa < b_size;
}

static int
vmm_pcie_root_bar_overlap_locked(struct vmm_pcie_root *root,
    const struct vmm_device *skip_device, unsigned int skip_index,
    uint64_t gpa, uint64_t size)
{
	struct vmm_device *device;

	RB_FOREACH(device, vmm_pcie_device_tree,
	    &root->borrow_imm_pcie->mut_devices) {
		unsigned int i;

		if (device->borrow_mut_root != root || !device->mut_registered ||
		    device->own_mut_config == NULL)
			continue;
		for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
			uint64_t other_gpa;
			uint64_t other_size;

			if (device == skip_device && i == skip_index)
				continue;
			if (vmm_pcie_config_bar_decode_locked(device->own_mut_config,
			    i, &other_gpa, &other_size) == 0 &&
			    vmm_pcie_bar_range_valid(other_gpa, other_size) &&
			    vmm_pcie_range_overlaps(gpa, size, other_gpa, other_size))
				return 1;
		}
	}
	return 0;
}
