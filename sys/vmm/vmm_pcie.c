/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Core vPCIe fabric -- see vmm_pcie.h.
 */
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/types.h>
#include <vm/vm_object.h>

#include "vmm_machine.h"
#include "vmm_dma.h"
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

struct vmm_pcie_dma_ref {
	struct vmm_pcie_user	*own_mut_user;
	struct vmm_dma_cap	*own_mut_cap;
	uint64_t		imm_device_id;
	uint64_t		imm_generation;
};

struct vmm_pcie_device_runtime {
	struct vmm_pcie_root	*borrow_mut_root;
	struct vmm_dma_cap	*own_mut_dma_cap;
	struct vmm_pcie_config	*own_mut_config;
	struct vmm_pcie_bar	own_mut_bars[VMM_PCIE_ABI_MAX_BARS];
	struct vmm_pcie_bar_mapping
				own_mut_mappings[VMM_PCIE_ABI_MAX_BARS];
};

static int	vmm_pcie_device_cmp(struct vmm_device *left,
		    struct vmm_device *right);
static struct vmm_device *vmm_pcie_device_find_name_locked(
		    struct vmm_pcie *pcie, const char *name, int nlen);
static struct vmm_device *vmm_pcie_device_find_id_locked(
		    struct vmm_pcie *pcie, uint64_t id);
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
static void	vmm_pcie_device_runtime_take_locked(struct vmm_device *device,
		    struct vmm_pcie_device_runtime *runtime);
static void	vmm_pcie_device_runtime_release(
		    struct vmm_pcie_device_runtime *runtime);
static int	vmm_pcie_root_stop_pending_locked(
		    struct vmm_pcie_root *root);
static int	vmm_pcie_device_bar_range_flags_locked(
		    const struct vmm_device *device, unsigned int bar_index,
		    uint64_t offset, uint64_t size, uint32_t *flagsp);

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
	    device->borrow_mut_offload != NULL ||
	    device->own_mut_dma_cap != NULL ||
	    device->mut_registered) {
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
	struct vmm_pcie_device_runtime runtime;
	struct vmm_machine *machine;
	uint32_t bdf;
	int start;
	int error;

	if (device == NULL || device->borrow_imm_pcie != pcie || root == NULL ||
	    root->borrow_imm_pcie != pcie)
		return EINVAL;
	bzero(&runtime, sizeof(runtime));
	machine = NULL;
	start = 0;
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
	if (device->borrow_mut_offload != NULL) {
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
	if (device->mut_run_generation != 0 || device->mut_registered ||
	    device->own_mut_dma_cap != NULL)
		vmm_pcie_device_runtime_take_locked(device, &runtime);
	vmm_pcie_root_bdf_release_locked(device->borrow_mut_root,
	    device->mut_bdf);
	device->borrow_mut_root = root;
	device->mut_bdf = bdf;
	if (device->mut_attachment_generation != (uint64_t)-1)
		device->mut_attachment_generation++;
	if (device->borrow_mut_provider != NULL && root->mut_running &&
	    root->borrow_imm_machine != NULL) {
		machine = root->borrow_imm_machine;
		start = 1;
	}
	error = 0;
out:
	lwkt_reltoken(&pcie->token_registry);
	vmm_pcie_device_runtime_release(&runtime);
	if (error == 0 && start)
		vmm_pcie_root_start(root, &machine->own_mut_dma);
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
	struct vmm_pcie_root *root;
	struct vmm_machine *machine;
	struct vmm_pcie *pcie;
	int start;
	int error;

	if (device == NULL || provider == NULL ||
	    device->borrow_imm_pcie == NULL)
		return EINVAL;
	pcie = device->borrow_imm_pcie;
	root = NULL;
	machine = NULL;
	start = 0;
	lwkt_gettoken(&pcie->token_registry);
	if (pcie->mut_closing || device->borrow_mut_provider != NULL ||
	    device->mut_registered) {
		error = EBUSY;
	} else if (device->mut_attachment_generation == (uint64_t)-1) {
		error = EOVERFLOW;
	} else {
		device->borrow_mut_provider = provider;
		device->mut_attachment_generation++;
		root = device->borrow_mut_root;
		if (root->mut_running && root->borrow_imm_machine != NULL) {
			machine = root->borrow_imm_machine;
			start = 1;
		}
		error = 0;
	}
	lwkt_reltoken(&pcie->token_registry);
	if (error == 0 && start)
		vmm_pcie_root_start(root, &machine->own_mut_dma);
	return error;
}

int
vmm_pcie_device_provider_register(struct vmm_device *device,
    struct vmm_pcie_user *provider, const struct vmm_pcie_abi_register *request,
    struct vmm_pcie_abi_registered *response, struct file **fps,
    unsigned int *file_countp)
{
	struct vmm_pcie_bar bars[VMM_PCIE_ABI_MAX_BARS];
	struct vmm_pcie_config *config;
	struct vmm_dma_cap *cap;
	struct file *dma_fp;
	struct vmm_pcie *pcie;
	uint32_t bar_fd_mask;
	unsigned int file_count;
	unsigned int i;
	int error;

	if (device == NULL || provider == NULL || request == NULL ||
	    response == NULL || fps == NULL || file_countp == NULL ||
	    device->borrow_imm_pcie == NULL ||
	    vmm_pcie_abi_validate(request, sizeof(*request)) != 0)
		return EINVAL;
	__builtin_memset(bars, 0, sizeof(bars));
	config = NULL;
	dma_fp = NULL;
	error = vmm_pcie_config_create(&config, request);
	if (error != 0)
		return error;
	file_count = 0;
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
		bar_fd_mask |= 1U << i;
	}

	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	if (pcie->mut_closing || device->borrow_mut_provider != provider ||
	    device->mut_registered || device->mut_run_generation == 0 ||
	    device->mut_stop_requested || le64toh(request->header.le_sequence) !=
	    device->mut_run_generation || device->own_mut_dma_cap == NULL) {
		error = EBUSY;
		lwkt_reltoken(&pcie->token_registry);
		goto fail;
	}
	cap = device->own_mut_dma_cap;
	dma_fp = vmm_dma_cap_file_hold(cap);
	if (dma_fp == NULL) {
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
	__builtin_memcpy(device->own_mut_bar_range, request->bar_range,
	    sizeof(device->own_mut_bar_range));
	device->mut_bar_range_count = request->bar_range_count;
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
	response->header.le_flags = htole32(
	    VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY);
	response->header.le_sequence = request->header.le_sequence;
	response->le_device_id = htole64(device->imm_id);
	response->le_consumer_id = htole64(device->borrow_mut_root->imm_id);
	response->le_attachment_generation = htole64(
	    device->mut_attachment_generation);
	response->le_bdf = htole32(device->mut_bdf);
	response->le_bar_fd_mask = htole32(bar_fd_mask);
	response->le_msix_vectors = htole16(device->mut_msix_vectors);
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		if (device->own_mut_bars[i].own_mut_fp == NULL)
			continue;
		fhold(device->own_mut_bars[i].own_mut_fp);
		fps[file_count++] = device->own_mut_bars[i].own_mut_fp;
	}
	/* BAR fds are ordered by BAR index; the DMA fd is always final. */
	fps[file_count++] = dma_fp;
	lwkt_reltoken(&pcie->token_registry);
	*file_countp = file_count;
	return 0;

fail:
	if (dma_fp != NULL)
		fdrop(dma_fp);
	vmm_pcie_config_destroy(config);
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++)
		vmm_pcie_bar_destroy(&bars[i]);
	return error;
}

int
vmm_pcie_device_provider_stopped(struct vmm_device *device,
    struct vmm_pcie_user *provider,
    const struct vmm_pcie_abi_stopped *message)
{
	struct vmm_pcie_root *root;
	struct vmm_pcie *pcie;
	uint64_t generation;
	int error;

	if (device == NULL || provider == NULL || message == NULL ||
	    device->borrow_imm_pcie == NULL ||
	    vmm_pcie_abi_validate(message, sizeof(*message)) != 0)
		return EINVAL;
	pcie = device->borrow_imm_pcie;
	generation = le64toh(message->le_memory_generation);
	root = NULL;
	lwkt_gettoken(&pcie->token_registry);
	if (device->borrow_mut_provider != provider ||
	    le64toh(message->le_device_id) != device->imm_id ||
	    device->mut_run_generation != generation ||
	    !device->mut_stop_requested) {
		error = ESTALE;
	} else {
		device->mut_stop_requested = 0;
		root = device->borrow_mut_root;
		error = 0;
	}
	lwkt_reltoken(&pcie->token_registry);
	if (root != NULL)
		wakeup(root);
	return error;
}

void
vmm_pcie_device_provider_detach(struct vmm_device *device,
    struct vmm_pcie_user *provider)
{
	struct vmm_pcie_device_runtime runtime;
	struct vmm_pcie *pcie;
	struct vmm_pcie_root *root;

	if (device == NULL || provider == NULL ||
	    device->borrow_imm_pcie == NULL)
		return;
	bzero(&runtime, sizeof(runtime));
	root = NULL;
	pcie = device->borrow_imm_pcie;
	lwkt_gettoken(&pcie->token_registry);
	if (device->borrow_mut_provider == provider) {
		root = device->borrow_mut_root;
		vmm_pcie_device_runtime_take_locked(device, &runtime);
		device->borrow_mut_provider = NULL;
		if (device->mut_attachment_generation != (uint64_t)-1)
			device->mut_attachment_generation++;
		wakeup(root);
	}
	lwkt_reltoken(&pcie->token_registry);
	/* Provider loss is surprise removal: no STOP handshake or wait. */
	vmm_pcie_device_runtime_release(&runtime);
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
	uint64_t object_offset;
	uint64_t object_size;
	unsigned int bar_index;
	uint32_t bdf;
	uint32_t range_flags;
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
	object_offset = 0;
	object_size = 0;
	bar_index = 0;
	bdf = 0;
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
			error = vmm_pcie_device_bar_range_flags_locked(device, i,
			    gpa - bar_gpa, 1, &range_flags);
			if (error != 0)
				goto out;
			if (range_flags != VMM_PCIE_ABI_BAR_RANGE_F_DIRECT) {
				error = EAGAIN;
				goto out;
			}
			bar_index = i;
			mapped_gpa = trunc_page(gpa);
			mapped_size = PAGE_SIZE;
			object_offset = mapped_gpa - bar_gpa;
			error = vmm_pcie_bar_snapshot(&device->own_mut_bars[i],
			    &object, &object_size);
			if (error == 0 && object_size != bar_size)
				error = EINVAL;
			goto out;
		}
	}
out:
	lwkt_reltoken(&pcie->token_registry);
	if (error != 0)
		return error;
	error = vmm_mem_map_object(&machine->own_mut_mem, mapped_gpa,
	    mapped_size, object, object_offset);
	vm_object_deallocate(object);
	if (error != 0 && error != EBUSY)
		return error;
	if (error == EBUSY)
		error = 0;
	lwkt_gettoken(&pcie->token_registry);
	if (device->borrow_mut_root != root || !device->mut_registered ||
	    device->own_mut_config == NULL ||
	    vmm_pcie_config_bar_decode_locked(device->own_mut_config,
	    bar_index, &bar_gpa, &bar_size) != 0 ||
	    object_offset >= bar_size || mapped_size > bar_size - object_offset) {
		error = ENOENT;
	} else if (device->mut_bar_gpa[bar_index] == 0) {
		device->mut_bar_gpa[bar_index] = bar_gpa;
		bdf = device->mut_bdf;
		mapped_now = 1;
	}
	lwkt_reltoken(&pcie->token_registry);
	if (error != 0) {
		vmm_mem_unmap_object(&machine->own_mut_mem, mapped_gpa,
		    mapped_size);
		return error;
	}
	if (mapped_now) {
		vmm_machine_logf(machine,
		    "pcie bar map bdf=%04x bar=%u gpa=0x%jx size=0x%jx", bdf,
		    bar_index, (uintmax_t)mapped_gpa, (uintmax_t)mapped_size);
	}
	return vmm_mem_fault_object_gpa(&machine->own_mut_mem, gpa, prot);
}

int
vmm_pcie_root_bar_access(struct vmm_pcie_root *root, uint64_t gpa,
    int write, int size, uint64_t *valuep)
{
	struct vmm_pcie_user *user;
	struct vmm_device *device;
	struct vmm_pcie *pcie;
	uint64_t attachment_generation;
	uint64_t bar_gpa;
	uint64_t bar_size;
	uint64_t offset;
	unsigned int bar_index;
	uint32_t range_flags;
	int error;

	if (root == NULL || root->borrow_imm_pcie == NULL || valuep == NULL ||
	    (size != 1 && size != 2 && size != 4))
		return EINVAL;
	pcie = root->borrow_imm_pcie;
	user = NULL;
	attachment_generation = 0;
	bar_index = 0;
	offset = 0;
	error = ENOENT;
	lwkt_gettoken(&pcie->token_registry);
	RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
		unsigned int i;

		if (device->borrow_mut_root != root || !device->mut_registered ||
		    device->own_mut_config == NULL ||
		    device->borrow_mut_provider == NULL)
			continue;
		for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
			if (vmm_pcie_config_bar_decode_locked(device->own_mut_config, i,
			    &bar_gpa, &bar_size) != 0 || gpa < bar_gpa ||
			    gpa - bar_gpa > bar_size || size > bar_size - (gpa - bar_gpa))
				continue;
			offset = gpa - bar_gpa;
			error = vmm_pcie_device_bar_range_flags_locked(device, i,
			    offset, size, &range_flags);
			if (error != 0 || range_flags !=
			    VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED)
				goto out;
			user = device->borrow_mut_provider;
			vmm_pcie_user_hold(user);
			attachment_generation = device->mut_attachment_generation;
			bar_index = i;
			error = 0;
			goto out;
		}
	}
out:
	lwkt_reltoken(&pcie->token_registry);
	if (error != 0)
		return error;
	error = vmm_pcie_user_mmio_access(user, device->imm_id,
	    attachment_generation, bar_index, offset, write, size, valuep);
	vmm_pcie_user_release(user);
	return error;
}

void
vmm_pcie_root_start(struct vmm_pcie_root *root, struct vmm_dma *dma)
{
	struct vmm_pcie_dma_ref refs[VMM_PCIE_ROOT_BDF_COUNT];
	struct vmm_pcie_abi_start message;
	struct vmm_pcie_device_runtime runtime;
	struct vmm_pcie_user *user;
	struct vmm_pcie *pcie;
	struct vmm_device *device;
	struct vmm_dma_cap *cap;
	struct vmm_mem_dma_range ranges[VMM_MEM_DMA_MAX_RANGES];
	uint64_t device_id;
	unsigned int count;
	unsigned int i;
	unsigned int range_count;
	unsigned int j;
	int attached;
	int error;

	if (root == NULL || dma == NULL || root->borrow_imm_pcie == NULL ||
	    root->borrow_imm_machine == NULL)
		return;
	pcie = root->borrow_imm_pcie;
	count = 0;
	lwkt_gettoken(&pcie->token_registry);
	root->mut_running = 1;
	RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
		if (device->borrow_mut_root != root ||
		    device->borrow_mut_provider == NULL ||
		    device->mut_run_generation != 0)
			continue;
		if (count == nitems(refs))
			break;
		refs[count].own_mut_user = device->borrow_mut_provider;
		refs[count].own_mut_cap = NULL;
		refs[count].imm_device_id = device->imm_id;
		vmm_pcie_user_hold(refs[count].own_mut_user);
		count++;
	}
	lwkt_reltoken(&pcie->token_registry);

	for (i = 0; i < count; i++) {
		user = refs[i].own_mut_user;
		device_id = refs[i].imm_device_id;
		cap = NULL;
		error = vmm_dma_cap_create(dma, &cap);
		if (error != 0) {
			vmm_machine_logf(root->borrow_imm_machine,
			    "pcie start capability failed device=%ju error=%d",
			    (uintmax_t)device_id, error);
			vmm_pcie_user_release(user);
			continue;
		}
		attached = 0;
		lwkt_gettoken(&pcie->token_registry);
		device = vmm_pcie_device_find_id_locked(pcie, device_id);
		if (root->mut_running && device != NULL &&
		    device->borrow_mut_root == root &&
		    device->borrow_mut_provider == user &&
		    device->mut_run_generation == 0 &&
		    device->own_mut_dma_cap == NULL) {
			device->own_mut_dma_cap = cap;
			device->mut_run_generation = vmm_dma_cap_generation(cap);
			device->mut_stop_requested = 0;
			attached = 1;
		}
		lwkt_reltoken(&pcie->token_registry);
		if (!attached) {
			vmm_dma_cap_revoke(cap);
			vmm_pcie_user_release(user);
			continue;
		}
		bzero(&message, sizeof(message));
		message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
		message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
		message.header.le_type = htole16(VMM_PCIE_ABI_MSG_START);
		message.header.le_size = htole32(sizeof(message));
		message.header.le_sequence = htole64(vmm_dma_cap_generation(cap));
		message.le_device_id = htole64(device_id);
		message.le_memory_generation = htole64(vmm_dma_cap_generation(cap));
		range_count = vmm_dma_cap_ranges(cap, ranges, nitems(ranges));
		KKASSERT(range_count != 0);
		message.le_dma_segment_count = htole32(range_count);
		for (j = 0; j < range_count; j++) {
			message.dma_segment[j].le_gpa = htole64(ranges[j].raw_gpa);
			message.dma_segment[j].le_length = htole64(ranges[j].imm_size);
			message.dma_segment[j].le_permissions = htole32(
			    VMM_PCIE_ABI_DMA_PERM_READ |
			    VMM_PCIE_ABI_DMA_PERM_WRITE);
		}
		error = vmm_pcie_user_send_start(user, &message);
		if (error != 0) {
			bzero(&runtime, sizeof(runtime));
			lwkt_gettoken(&pcie->token_registry);
			device = vmm_pcie_device_find_id_locked(pcie, device_id);
			if (device != NULL && device->own_mut_dma_cap == cap)
				vmm_pcie_device_runtime_take_locked(device, &runtime);
			lwkt_reltoken(&pcie->token_registry);
			vmm_pcie_device_runtime_release(&runtime);
			vmm_machine_logf(root->borrow_imm_machine,
			    "pcie start send failed device=%ju error=%d",
			    (uintmax_t)device_id, error);
		} else {
			vmm_machine_logf(root->borrow_imm_machine,
			    "pcie start device=%ju generation=%ju",
			    (uintmax_t)device_id,
			    (uintmax_t)vmm_dma_cap_generation(cap));
		}
		vmm_pcie_user_release(user);
	}
}

void
vmm_pcie_root_stop(struct vmm_pcie_root *root)
{
	struct vmm_pcie_dma_ref refs[VMM_PCIE_ROOT_BDF_COUNT];
	struct vmm_pcie_abi_stop message;
	struct vmm_pcie_device_runtime runtime;
	struct vmm_pcie *pcie;
	struct vmm_device *device;
	unsigned int count;
	unsigned int i;
	int error;

	if (root == NULL || root->borrow_imm_pcie == NULL)
		return;
	pcie = root->borrow_imm_pcie;
	count = 0;
	lwkt_gettoken(&pcie->token_registry);
	root->mut_running = 0;
	RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
		if (device->borrow_mut_root != root ||
		    device->mut_run_generation == 0)
			continue;
		if (count == nitems(refs))
			break;
		refs[count].own_mut_user = device->borrow_mut_provider;
		if (refs[count].own_mut_user != NULL)
			vmm_pcie_user_hold(refs[count].own_mut_user);
		refs[count].own_mut_cap = device->own_mut_dma_cap;
		refs[count].imm_device_id = device->imm_id;
		refs[count].imm_generation = device->mut_run_generation;
		device->mut_stop_requested = 1;
		count++;
	}
	lwkt_reltoken(&pcie->token_registry);

	for (i = 0; i < count; i++) {
		bzero(&message, sizeof(message));
		message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
		message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
		message.header.le_type = htole16(VMM_PCIE_ABI_MSG_STOP);
		message.header.le_size = htole32(sizeof(message));
		message.header.le_sequence = htole64(refs[i].imm_generation);
		message.le_device_id = htole64(refs[i].imm_device_id);
		message.le_memory_generation = htole64(refs[i].imm_generation);
		if (refs[i].own_mut_user != NULL) {
			error = vmm_pcie_user_send_stop(refs[i].own_mut_user, &message);
			if (error != 0 && root->borrow_imm_machine != NULL) {
				vmm_machine_logf(root->borrow_imm_machine,
				    "pcie stop send failed device=%ju error=%d",
				    (uintmax_t)refs[i].imm_device_id, error);
			}
		}
		vmm_pcie_user_release(refs[i].own_mut_user);
	}
	for (i = 0; i < 10; i++) {
		lwkt_gettoken(&pcie->token_registry);
		if (!vmm_pcie_root_stop_pending_locked(root)) {
			lwkt_reltoken(&pcie->token_registry);
			break;
		}
		(void)tsleep(root, 0, "vmmpcistop", hz);
		lwkt_reltoken(&pcie->token_registry);
	}
	for (;;) {
		bzero(&runtime, sizeof(runtime));
		lwkt_gettoken(&pcie->token_registry);
		RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
			if (device->borrow_mut_root == root &&
			    device->mut_run_generation != 0) {
				vmm_pcie_device_runtime_take_locked(device, &runtime);
				break;
			}
		}
		lwkt_reltoken(&pcie->token_registry);
		if (runtime.borrow_mut_root == NULL)
			break;
		vmm_pcie_device_runtime_release(&runtime);
	}
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

static struct vmm_device *
vmm_pcie_device_find_id_locked(struct vmm_pcie *pcie, uint64_t id)
{
	struct vmm_device *device;

	RB_FOREACH(device, vmm_pcie_device_tree, &pcie->mut_devices) {
		if (device->imm_id == id)
			return device;
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
	__builtin_memset(device->own_mut_bar_range, 0,
	    sizeof(device->own_mut_bar_range));
	device->mut_bar_range_count = 0;
	device->mut_vendor_id = 0;
	device->mut_device_id = 0;
	device->mut_subsystem_vendor_id = 0;
	device->mut_subsystem_device_id = 0;
	device->mut_class_code = 0;
	device->mut_msix_vectors = 0;
	device->mut_revision = 0;
	device->mut_registered = 0;
}

static void
vmm_pcie_device_runtime_take_locked(struct vmm_device *device,
    struct vmm_pcie_device_runtime *runtime)
{

	KKASSERT(device != NULL);
	KKASSERT(runtime != NULL);
	bzero(runtime, sizeof(*runtime));
	runtime->borrow_mut_root = device->borrow_mut_root;
	runtime->own_mut_dma_cap = device->own_mut_dma_cap;
	device->own_mut_dma_cap = NULL;
	vmm_pcie_device_bar_mappings_take_locked(device,
	    runtime->own_mut_mappings);
	vmm_pcie_device_clear_registered_locked(device, runtime->own_mut_bars,
	    &runtime->own_mut_config);
	device->mut_run_generation = 0;
	device->mut_stop_requested = 0;
}

static void
vmm_pcie_device_runtime_release(struct vmm_pcie_device_runtime *runtime)
{
	unsigned int i;

	if (runtime == NULL || runtime->borrow_mut_root == NULL)
		return;
	vmm_pcie_root_bar_mappings_unmap(runtime->borrow_mut_root,
	    runtime->own_mut_mappings);
	vmm_dma_cap_revoke(runtime->own_mut_dma_cap);
	vmm_pcie_config_destroy(runtime->own_mut_config);
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		vmm_pcie_bar_revoke(&runtime->own_mut_bars[i]);
		vmm_pcie_bar_destroy(&runtime->own_mut_bars[i]);
	}
}

static int
vmm_pcie_root_stop_pending_locked(struct vmm_pcie_root *root)
{
	struct vmm_device *device;

	RB_FOREACH(device, vmm_pcie_device_tree,
	    &root->borrow_imm_pcie->mut_devices) {
		if (device->borrow_mut_root == root &&
		    device->mut_run_generation != 0 &&
		    device->mut_stop_requested)
			return 1;
	}
	return 0;
}

static int
vmm_pcie_device_bar_range_flags_locked(const struct vmm_device *device,
    unsigned int bar_index, uint64_t offset, uint64_t size,
    uint32_t *flagsp)
{
	const struct vmm_pcie_abi_bar_range *range;
	unsigned int i;

	if (device == NULL || flagsp == NULL || size == 0 ||
	    bar_index >= VMM_PCIE_ABI_MAX_BARS)
		return EINVAL;
	for (i = 0; i < device->mut_bar_range_count; i++) {
		uint64_t range_offset;
		uint64_t range_size;
		uint64_t delta;

		range = &device->own_mut_bar_range[i];
		range_offset = le64toh(range->le_offset);
		range_size = le64toh(range->le_size);
		if (le32toh(range->le_bar_index) != bar_index ||
		    offset < range_offset)
			continue;
		delta = offset - range_offset;
		if (delta > range_size || size > range_size - delta)
			continue;
		*flagsp = le32toh(range->le_flags);
		return 0;
	}
	return ENOENT;
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
