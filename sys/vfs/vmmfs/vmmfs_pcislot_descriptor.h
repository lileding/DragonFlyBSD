/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI slot descriptor transaction node.
 */
#ifndef VMMFS_PCISLOT_DESCRIPTOR_H
#define VMMFS_PCISLOT_DESCRIPTOR_H

#include <sys/types.h>

#include "vmmfs_node.h"
#include <sys/vmmfs.h>

#define VMMFS_PCISLOT_DESCRIPTOR_MAX 16384
#define VMMFS_PCISLOT_MAX_BARS 6
#define VMMFS_PCISLOT_MAX_DOORBELLS 16
#define VMMFS_PCISLOT_MAX_CONFIGS 64
#define VMMFS_PCISLOT_MAX_MSIX_VECTORS 2048
#define VMMFS_PCISLOT_MAX_CAPS 32
#define VMMFS_PCISLOT_MAX_ECAPS 32
#define VMMFS_PCISLOT_CAP_DATA_MAX 4096

struct vmmfs_pcislot;
struct vmmfs_pcislot_resources;
struct vmmfs_pcislot_auth;
struct vnode;
struct vop_ops;

enum vmmfs_pcislot_bar_type {
	VMMFS_PCISLOT_BAR_NONE,
	VMMFS_PCISLOT_BAR_IO,
	VMMFS_PCISLOT_BAR_MEM32,
	VMMFS_PCISLOT_BAR_MEM64,
};

enum vmmfs_pcislot_intx_pin {
	VMMFS_PCISLOT_INTX_NONE,
	VMMFS_PCISLOT_INTX_A,
	VMMFS_PCISLOT_INTX_B,
	VMMFS_PCISLOT_INTX_C,
	VMMFS_PCISLOT_INTX_D,
};

enum vmmfs_pcislot_doorbell_space {
	VMMFS_PCISLOT_DOORBELL_MMIO,
	VMMFS_PCISLOT_DOORBELL_PIO,
};

enum vmmfs_pcislot_config_space {
	VMMFS_PCISLOT_CONFIG_MMIO = VMMFS_PCI_CONFIG_MMIO,
	VMMFS_PCISLOT_CONFIG_PIO = VMMFS_PCI_CONFIG_PIO,
};

enum vmmfs_pcislot_cap_kind {
	VMMFS_PCISLOT_CAP_PCIE,
	VMMFS_PCISLOT_CAP_MSI,
	VMMFS_PCISLOT_CAP_MSIX,
	VMMFS_PCISLOT_CAP_BLOB,
};

enum vmmfs_pcislot_cap_access {
	VMMFS_PCISLOT_CAP_STATIC,
};

struct vmmfs_pcislot_bar {
	bool present;
	enum vmmfs_pcislot_bar_type type;
	bool prefetchable;
	uint64_t size;
};

struct vmmfs_pcislot_doorbell {
	bool present;
	uint8_t bar;
	uint64_t offset;
	uint64_t size;
	uint8_t width;
	enum vmmfs_pcislot_doorbell_space space;
};

struct vmmfs_pcislot_config_register {
	bool present;
	uint8_t bar;
	uint64_t offset;
	uint8_t width;
	enum vmmfs_pcislot_config_space space;
};

struct vmmfs_pcislot_cap {
	bool present;
	enum vmmfs_pcislot_cap_kind kind;
	enum vmmfs_pcislot_cap_access access;
	uint16_t vectors;
	uint8_t address_width;
	bool maskable;
	uint8_t table_bar;
	uint64_t table_offset;
	uint8_t pba_bar;
	uint64_t pba_offset;
	uint8_t id;
	uint16_t data_offset;
	uint16_t data_length;
};

struct vmmfs_pcislot_ecap {
	bool present;
	uint16_t id;
	uint8_t version;
	enum vmmfs_pcislot_cap_access access;
	uint16_t data_offset;
	uint16_t data_length;
};

struct vmmfs_pcislot_descriptor_value {
	char text[VMMFS_PCISLOT_DESCRIPTOR_MAX];
	size_t length;
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t subsystem_vendor_id;
	uint16_t subsystem_device_id;
	uint32_t class;
	uint8_t revision;
	enum vmmfs_pcislot_intx_pin intx_pin;
	struct vmmfs_pcislot_bar bars[VMMFS_PCISLOT_MAX_BARS];
	bool rom_present;
	uint64_t rom_size;
	struct vmmfs_pcislot_doorbell doorbells[VMMFS_PCISLOT_MAX_DOORBELLS];
	struct vmmfs_pcislot_config_register configs[VMMFS_PCISLOT_MAX_CONFIGS];
	struct vmmfs_pcislot_cap caps[VMMFS_PCISLOT_MAX_CAPS];
	struct vmmfs_pcislot_ecap ecaps[VMMFS_PCISLOT_MAX_ECAPS];
	uint8_t cap_data[VMMFS_PCISLOT_CAP_DATA_MAX];
	uint16_t cap_data_length;
};

struct vmmfs_pcislot_descriptor {
	struct vmmfs_node node;
	struct vmmfs_pcislot *slot;
	ino_t inode;
	bool updating;
	bool committed;
	uint64_t generation;
	struct vmmfs_pcislot_descriptor_value value;
	struct vmmfs_pcislot_auth *auth;
	struct vmmfs_pcislot_resources *resources;
};

extern struct vop_ops vmmfs_pcislot_descriptor_vops;

int vmmfs_pcislot_descriptor_init(struct vmmfs_pcislot *,
	struct vmmfs_pcislot_descriptor *);
int vmmfs_pcislot_descriptor_publish(
	struct vmmfs_pcislot_descriptor *);
void vmmfs_pcislot_descriptor_fini(struct vmmfs_pcislot_descriptor *);
bool vmmfs_pcislot_descriptor_busy(struct vmmfs_pcislot_descriptor *);

#endif /* VMMFS_PCISLOT_DESCRIPTOR_H */
