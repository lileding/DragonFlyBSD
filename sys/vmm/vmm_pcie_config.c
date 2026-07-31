/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * PCIe configuration header -- see vmm_pcie_config.h.
 */
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/types.h>

#include "vmm_pcie_config.h"

#define VMM_PCI_VENDOR_ID		0x00U
#define VMM_PCI_DEVICE_ID		0x02U
#define VMM_PCI_COMMAND		0x04U
#define VMM_PCI_STATUS			0x06U
#define VMM_PCI_CLASS_REVISION		0x08U
#define VMM_PCI_HEADER_TYPE		0x0eU
#define VMM_PCI_BAR0			0x10U
#define VMM_PCI_SUBSYSTEM_VENDOR	0x2cU
#define VMM_PCI_SUBSYSTEM_DEVICE	0x2eU
#define VMM_PCI_CAP_PTR		0x34U
#define VMM_PCI_PCIE_CAP		0x50U
#define VMM_PCI_MSIX_CAP		0x70U
#define VMM_PCI_MSIX_CONTROL		(VMM_PCI_MSIX_CAP + 2U)
#define VMM_PCI_MSIX_TABLE		(VMM_PCI_MSIX_CAP + 4U)
#define VMM_PCI_MSIX_PBA		(VMM_PCI_MSIX_CAP + 8U)
#define VMM_PCI_VENDOR_CAP_BASE		0x80U

#define VMM_PCI_STATUS_CAP_LIST	0x0010U
#define VMM_PCI_COMMAND_VALID		0x0007U
#define VMM_PCI_COMMAND_MEMORY		0x0002U
#define VMM_PCI_BAR_MEMORY_64		0x00000004U
#define VMM_PCI_BAR_PREFETCHABLE	0x00000008U
#define VMM_PCI_BAR_ATTRIBUTE_MASK	0x0000000fU
#define VMM_PCI_CAP_ID_EXP		0x10U
#define VMM_PCI_CAP_ID_MSIX		0x11U
#define VMM_PCI_CAP_ID_VENDOR		0x09U
#define VMM_PCI_MSIX_CONTROL_FUNCTION_MASK	0x40000000U
#define VMM_PCI_MSIX_CONTROL_ENABLE		0x80000000U
#define VMM_PCI_MSIX_CONTROL_WRITABLE		(VMM_PCI_MSIX_CONTROL_FUNCTION_MASK | \
	 VMM_PCI_MSIX_CONTROL_ENABLE)
#define VMM_PCI_MSIX_VECTOR_MASK		0x00000001U

static uint32_t vmm_pcie_config_read32(const uint8_t *bytes,
	    unsigned int offset);
static void vmm_pcie_config_write16(uint8_t *bytes, unsigned int offset,
	    uint16_t value);
static void vmm_pcie_config_write32(uint8_t *bytes, unsigned int offset,
	    uint32_t value);
static int vmm_pcie_config_bar_index(const struct vmm_pcie_config *config,
	    unsigned int offset, unsigned int *indexp, int *highp);
static uint32_t vmm_pcie_config_bar_read32(const struct vmm_pcie_config *config,
	    unsigned int index, int high);
static void vmm_pcie_config_write_bar(struct vmm_pcie_config *config,
	    unsigned int offset, int size, uint64_t value);
static void vmm_pcie_config_write_command(struct vmm_pcie_config *config,
	    unsigned int offset, int size, uint64_t value);
static void vmm_pcie_config_write_msix(struct vmm_pcie_config *config,
    unsigned int offset, int size, uint64_t value);
static void vmm_pcie_config_normalize_bar(struct vmm_pcie_config *config,
	    unsigned int index);

int
vmm_pcie_config_create(struct vmm_pcie_config **configp,
    const struct vmm_pcie_abi_register *request)
{
	struct vmm_pcie_config *config;
	uint32_t class_revision;
	unsigned int i;
	unsigned int vendor_cap_offset;
	unsigned int vendor_cap_count;

	if (configp == NULL || request == NULL)
		return EINVAL;
	vendor_cap_count = request->vendor_cap_count;
	if (le16toh(request->le_msix_vectors) == 0 ||
	    le16toh(request->le_msix_vectors) > VMM_PCIE_ABI_MAX_MSIX_VECTORS ||
	    le64toh(request->bar[VMM_PCIE_ABI_MSIX_BAR_INDEX].le_size) <
	    VMM_PCIE_ABI_MSIX_MIN_BAR_SIZE(
	    le16toh(request->le_msix_vectors)) ||
	    vendor_cap_count > VMM_PCIE_ABI_MAX_VENDOR_CAPS)
		return EINVAL;
	for (i = 0; i < vendor_cap_count; i++) {
		if (request->vendor_cap[i].length <
		    VMM_PCIE_ABI_VENDOR_CAP_MIN_SIZE ||
		    request->vendor_cap[i].length >
		    VMM_PCIE_ABI_VENDOR_CAP_MAX_SIZE)
			return EINVAL;
	}
	vendor_cap_offset = VMM_PCI_VENDOR_CAP_BASE;
	for (i = 0; i < vendor_cap_count; i++) {
		if (vendor_cap_offset > VMM_PCIE_CONFIG_HEADER_SIZE -
		    request->vendor_cap[i].length)
			return EINVAL;
		vendor_cap_offset += request->vendor_cap[i].length;
		vendor_cap_offset = (vendor_cap_offset + 3U) & ~3U;
	}
	config = kmalloc(sizeof(*config), M_TEMP, M_WAITOK | M_ZERO);
	vmm_pcie_config_write16(config->own_mut_bytes, VMM_PCI_VENDOR_ID,
	    le16toh(request->le_vendor_id));
	vmm_pcie_config_write16(config->own_mut_bytes, VMM_PCI_DEVICE_ID,
	    le16toh(request->le_device_id));
	vmm_pcie_config_write16(config->own_mut_bytes, VMM_PCI_STATUS,
	    VMM_PCI_STATUS_CAP_LIST);
	class_revision = (le32toh(request->le_class_code) << 8) |
	    request->revision;
	vmm_pcie_config_write32(config->own_mut_bytes, VMM_PCI_CLASS_REVISION,
	    class_revision);
	config->own_mut_bytes[VMM_PCI_HEADER_TYPE] = 0;
	vmm_pcie_config_write16(config->own_mut_bytes, VMM_PCI_SUBSYSTEM_VENDOR,
	    le16toh(request->le_subsystem_vendor_id));
	vmm_pcie_config_write16(config->own_mut_bytes, VMM_PCI_SUBSYSTEM_DEVICE,
	    le16toh(request->le_subsystem_device_id));
	config->own_mut_bytes[VMM_PCI_CAP_PTR] = VMM_PCI_PCIE_CAP;
	config->own_mut_bytes[VMM_PCI_PCIE_CAP] = VMM_PCI_CAP_ID_EXP;
	config->own_mut_bytes[VMM_PCI_PCIE_CAP + 1] = VMM_PCI_MSIX_CAP;
	vmm_pcie_config_write16(config->own_mut_bytes, VMM_PCI_PCIE_CAP + 2,
	    0x0002U);
	config->own_mut_bytes[VMM_PCI_MSIX_CAP] = VMM_PCI_CAP_ID_MSIX;
	vmm_pcie_config_write16(config->own_mut_bytes, VMM_PCI_MSIX_CAP + 2,
	    le16toh(request->le_msix_vectors) - 1U);
	vmm_pcie_config_write32(config->own_mut_bytes, VMM_PCI_MSIX_TABLE,
	    VMM_PCIE_ABI_MSIX_TABLE_OFFSET |
	    VMM_PCIE_ABI_MSIX_BAR_INDEX);
	vmm_pcie_config_write32(config->own_mut_bytes, VMM_PCI_MSIX_PBA,
	    VMM_PCIE_ABI_MSIX_PBA_OFFSET(le16toh(request->le_msix_vectors)) |
	    VMM_PCIE_ABI_MSIX_BAR_INDEX);
	config->own_mut_bytes[VMM_PCI_MSIX_CAP + 1] =
	    vendor_cap_count == 0 ? 0 : VMM_PCI_VENDOR_CAP_BASE;
	vendor_cap_offset = VMM_PCI_VENDOR_CAP_BASE;
	for (i = 0; i < vendor_cap_count; i++) {
		unsigned int offset;
		unsigned int next_offset;

		offset = vendor_cap_offset;
		next_offset = offset + request->vendor_cap[i].length;
		next_offset = (next_offset + 3U) & ~3U;
		config->own_mut_bytes[offset] = VMM_PCI_CAP_ID_VENDOR;
		config->own_mut_bytes[offset + 1] = i + 1 == vendor_cap_count ?
		    0 : next_offset;
		config->own_mut_bytes[offset + 2] = request->vendor_cap[i].length;
		__builtin_memcpy(&config->own_mut_bytes[offset +
		    VMM_PCIE_ABI_VENDOR_CAP_MIN_SIZE], request->vendor_cap[i].bytes,
		    request->vendor_cap[i].length -
		    VMM_PCIE_ABI_VENDOR_CAP_MIN_SIZE);
		vendor_cap_offset = next_offset;
	}
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		uint32_t attributes;

		config->imm_bar_size[i] = le64toh(request->bar[i].le_size);
		config->imm_bar_flags[i] = le32toh(request->bar[i].le_flags);
		if (config->imm_bar_size[i] == 0)
			continue;
		attributes = 0;
		if ((config->imm_bar_flags[i] & VMM_PCIE_ABI_BAR_F_64BIT) != 0)
			attributes |= VMM_PCI_BAR_MEMORY_64;
		if ((config->imm_bar_flags[i] &
		    VMM_PCIE_ABI_BAR_F_PREFETCHABLE) != 0)
			attributes |= VMM_PCI_BAR_PREFETCHABLE;
		vmm_pcie_config_write32(config->own_mut_bytes,
		    VMM_PCI_BAR0 + i * sizeof(uint32_t), attributes);
	}
	__builtin_memcpy(config->own_imm_reset_bytes, config->own_mut_bytes,
	    sizeof(config->own_imm_reset_bytes));
	*configp = config;
	return 0;
}

void
vmm_pcie_config_reset_locked(struct vmm_pcie_config *config)
{

	if (config == NULL)
		return;
	__builtin_memcpy(config->own_mut_bytes, config->own_imm_reset_bytes,
	    sizeof(config->own_mut_bytes));
	__builtin_memset(config->mut_bar_probe, 0, sizeof(config->mut_bar_probe));
}

void
vmm_pcie_config_destroy(struct vmm_pcie_config *config)
{

	if (config != NULL)
		kfree(config, M_TEMP);
}

int
vmm_pcie_config_bar_decode_locked(const struct vmm_pcie_config *config,
    unsigned int index, uint64_t *gpap, uint64_t *sizep)
{
	uint64_t gpa;
	uint64_t size;
	uint32_t low;

	if (gpap != NULL)
		*gpap = 0;
	if (sizep != NULL)
		*sizep = 0;
	if (config == NULL || gpap == NULL || sizep == NULL ||
	    index >= VMM_PCIE_ABI_MAX_BARS ||
	    config->imm_bar_size[index] == 0 || config->mut_bar_probe[index] ||
	    (vmm_pcie_config_read32(config->own_mut_bytes, VMM_PCI_COMMAND) &
	    VMM_PCI_COMMAND_MEMORY) == 0)
		return ENOENT;
	low = vmm_pcie_config_read32(config->own_mut_bytes,
	    VMM_PCI_BAR0 + index * sizeof(uint32_t));
	gpa = low & ~VMM_PCI_BAR_ATTRIBUTE_MASK;
	if ((config->imm_bar_flags[index] & VMM_PCIE_ABI_BAR_F_64BIT) != 0) {
		gpa |= (uint64_t)vmm_pcie_config_read32(config->own_mut_bytes,
		    VMM_PCI_BAR0 + (index + 1) * sizeof(uint32_t)) << 32;
	}
	size = config->imm_bar_size[index];
	if (gpa == 0 || (gpa & (size - 1)) != 0)
		return ENOENT;
	*gpap = gpa;
	*sizep = size;
	return 0;
}

int
vmm_pcie_config_access_locked(struct vmm_pcie_config *config,
    unsigned int offset, int write, int size, uint64_t *valuep)
{
	unsigned int base;
	unsigned int index;
	uint32_t value;
	int high;

	if (config == NULL || valuep == NULL ||
	    (size != 1 && size != 2 && size != 4) ||
	    (offset & (unsigned int)(size - 1)) != 0 ||
	    offset > VMM_PCIE_CONFIG_SPACE_SIZE - (unsigned int)size)
		return EINVAL;
	if (write) {
		if (vmm_pcie_config_bar_index(config, offset, &index, &high)) {
			vmm_pcie_config_write_bar(config, offset, size, *valuep);
			return 0;
		}
		if (offset < VMM_PCI_COMMAND + sizeof(uint32_t) &&
		    offset + (unsigned int)size > VMM_PCI_COMMAND) {
			vmm_pcie_config_write_command(config, offset, size, *valuep);
			return 0;
		}
		if (offset < VMM_PCI_MSIX_CONTROL + sizeof(uint16_t) &&
		    offset + (unsigned int)size > VMM_PCI_MSIX_CONTROL) {
			vmm_pcie_config_write_msix(config, offset, size, *valuep);
		}
		return 0;
	}
	if (offset >= VMM_PCIE_CONFIG_HEADER_SIZE) {
		*valuep = 0;
		return 0;
	}
	base = offset & ~(unsigned int)(sizeof(uint32_t) - 1);
	if (vmm_pcie_config_bar_index(config, base, &index, &high)) {
		value = vmm_pcie_config_bar_read32(config, index, high);
	} else {
		value = vmm_pcie_config_read32(config->own_mut_bytes, base);
	}
	value >>= (offset - base) * 8U;
	if (size == 1)
		value &= 0xffU;
	else if (size == 2)
		value &= 0xffffU;
	*valuep = value;
	return 0;
}

int
vmm_pcie_config_msix_enabled_locked(const struct vmm_pcie_config *config)
{

	return config != NULL && (vmm_pcie_config_read32(config->own_mut_bytes,
	    VMM_PCI_MSIX_CAP) & VMM_PCI_MSIX_CONTROL_ENABLE) != 0;
}

int
vmm_pcie_config_msix_function_masked_locked(
    const struct vmm_pcie_config *config)
{

	return config == NULL || (vmm_pcie_config_read32(config->own_mut_bytes,
	    VMM_PCI_MSIX_CAP) & VMM_PCI_MSIX_CONTROL_FUNCTION_MASK) != 0;
}

int
vmm_pcie_config_msix_vector_masked(uint32_t vector_control)
{

	return (vector_control & VMM_PCI_MSIX_VECTOR_MASK) != 0;
}

static uint32_t
vmm_pcie_config_read32(const uint8_t *bytes, unsigned int offset)
{

	return (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1] << 8) |
	    ((uint32_t)bytes[offset + 2] << 16) |
	    ((uint32_t)bytes[offset + 3] << 24);
}

static void
vmm_pcie_config_write16(uint8_t *bytes, unsigned int offset, uint16_t value)
{

	bytes[offset] = (uint8_t)value;
	bytes[offset + 1] = (uint8_t)(value >> 8);
}

static void
vmm_pcie_config_write32(uint8_t *bytes, unsigned int offset, uint32_t value)
{

	bytes[offset] = (uint8_t)value;
	bytes[offset + 1] = (uint8_t)(value >> 8);
	bytes[offset + 2] = (uint8_t)(value >> 16);
	bytes[offset + 3] = (uint8_t)(value >> 24);
}

static int
vmm_pcie_config_bar_index(const struct vmm_pcie_config *config,
    unsigned int offset, unsigned int *indexp, int *highp)
{
	unsigned int index;

	if (offset < VMM_PCI_BAR0 || offset >= VMM_PCI_BAR0 +
	    VMM_PCIE_ABI_MAX_BARS * sizeof(uint32_t))
		return 0;
	index = (offset - VMM_PCI_BAR0) / sizeof(uint32_t);
	if (config->imm_bar_size[index] != 0) {
		*indexp = index;
		*highp = 0;
		return 1;
	}
	if (index == 0 || config->imm_bar_size[index - 1] == 0 ||
	    (config->imm_bar_flags[index - 1] &
	    VMM_PCIE_ABI_BAR_F_64BIT) == 0)
		return 0;
	*indexp = index - 1;
	*highp = 1;
	return 1;
}

static uint32_t
vmm_pcie_config_bar_read32(const struct vmm_pcie_config *config,
    unsigned int index, int high)
{
	uint32_t value;

	value = vmm_pcie_config_read32(config->own_mut_bytes,
	    VMM_PCI_BAR0 + index * sizeof(uint32_t));
	if (!config->mut_bar_probe[index]) {
		if (high)
			return vmm_pcie_config_read32(config->own_mut_bytes,
			    VMM_PCI_BAR0 + (index + 1) * sizeof(uint32_t));
		return value;
	}
	if (high)
		return (uint32_t)(~(config->imm_bar_size[index] - 1) >> 32);
	return ((uint32_t)~(config->imm_bar_size[index] - 1) &
	    ~VMM_PCI_BAR_ATTRIBUTE_MASK) |
	    (value & VMM_PCI_BAR_ATTRIBUTE_MASK);
}

static void
vmm_pcie_config_write_bar(struct vmm_pcie_config *config,
    unsigned int offset, int size, uint64_t value)
{
	unsigned int base;
	unsigned int index;
	uint32_t mask;
	uint32_t old;
	uint32_t next;
	int high;

	base = offset & ~(unsigned int)(sizeof(uint32_t) - 1);
	if (!vmm_pcie_config_bar_index(config, base, &index, &high))
		return;
	if (size == 4 && offset == base && (uint32_t)value == 0xffffffffU) {
		config->mut_bar_probe[index] = 1;
		return;
	}
	config->mut_bar_probe[index] = 0;
	if (high) {
		old = vmm_pcie_config_read32(config->own_mut_bytes, base);
	} else {
		old = vmm_pcie_config_read32(config->own_mut_bytes,
		    VMM_PCI_BAR0 + index * sizeof(uint32_t));
	}
	mask = size == 4 ? 0xffffffffU :
	    ((1U << (size * 8U)) - 1U) << ((offset - base) * 8U);
	next = (old & ~mask) | (((uint32_t)value <<
	    ((offset - base) * 8U)) & mask);
	if (!high)
		next = (next & ~VMM_PCI_BAR_ATTRIBUTE_MASK) |
		    (old & VMM_PCI_BAR_ATTRIBUTE_MASK);
	vmm_pcie_config_write32(config->own_mut_bytes,
	    high ? base : VMM_PCI_BAR0 + index * sizeof(uint32_t), next);
	vmm_pcie_config_normalize_bar(config, index);
}

static void
vmm_pcie_config_write_command(struct vmm_pcie_config *config,
    unsigned int offset, int size, uint64_t value)
{
	uint32_t mask;
	uint32_t old;
	uint32_t next;

	old = vmm_pcie_config_read32(config->own_mut_bytes, VMM_PCI_COMMAND);
	mask = size == 4 ? 0xffffffffU :
	    ((1U << (size * 8U)) - 1U) << ((offset - VMM_PCI_COMMAND) * 8U);
	next = (old & ~mask) | (((uint32_t)value <<
	    ((offset - VMM_PCI_COMMAND) * 8U)) & mask);
	next = (old & ~(uint32_t)VMM_PCI_COMMAND_VALID) |
	    (next & VMM_PCI_COMMAND_VALID);
	next |= (uint32_t)VMM_PCI_STATUS_CAP_LIST << 16;
	vmm_pcie_config_write32(config->own_mut_bytes, VMM_PCI_COMMAND, next);
}

static void
vmm_pcie_config_write_msix(struct vmm_pcie_config *config,
    unsigned int offset, int size, uint64_t value)
{
	unsigned int base;
	uint32_t mask;
	uint32_t old;
	uint32_t next;

	base = VMM_PCI_MSIX_CAP;
	old = vmm_pcie_config_read32(config->own_mut_bytes, base);
	mask = size == 4 ? 0xffffffffU :
	    ((1U << (size * 8U)) - 1U) << ((offset - base) * 8U);
	next = (old & ~mask) | (((uint32_t)value <<
	    ((offset - base) * 8U)) & mask);
	next = (next & VMM_PCI_MSIX_CONTROL_WRITABLE) |
	    (old & ~VMM_PCI_MSIX_CONTROL_WRITABLE);
	vmm_pcie_config_write32(config->own_mut_bytes, base, next);
}

static void
vmm_pcie_config_normalize_bar(struct vmm_pcie_config *config,
    unsigned int index)
{
	uint64_t gpa;
	uint64_t size;
	uint32_t attributes;

	if (config->imm_bar_size[index] == 0)
		return;
	size = config->imm_bar_size[index];
	attributes = vmm_pcie_config_read32(config->own_mut_bytes,
	    VMM_PCI_BAR0 + index * sizeof(uint32_t)) &
	    VMM_PCI_BAR_ATTRIBUTE_MASK;
	gpa = vmm_pcie_config_read32(config->own_mut_bytes,
	    VMM_PCI_BAR0 + index * sizeof(uint32_t)) &
	    ~VMM_PCI_BAR_ATTRIBUTE_MASK;
	if ((config->imm_bar_flags[index] & VMM_PCIE_ABI_BAR_F_64BIT) != 0) {
		gpa |= (uint64_t)vmm_pcie_config_read32(config->own_mut_bytes,
		    VMM_PCI_BAR0 + (index + 1) * sizeof(uint32_t)) << 32;
	}
	gpa &= ~(size - 1);
	vmm_pcie_config_write32(config->own_mut_bytes,
	    VMM_PCI_BAR0 + index * sizeof(uint32_t),
	    ((uint32_t)gpa & ~VMM_PCI_BAR_ATTRIBUTE_MASK) | attributes);
	if ((config->imm_bar_flags[index] & VMM_PCIE_ABI_BAR_F_64BIT) != 0) {
		vmm_pcie_config_write32(config->own_mut_bytes,
		    VMM_PCI_BAR0 + (index + 1) * sizeof(uint32_t),
		    (uint32_t)(gpa >> 32));
	}
}
