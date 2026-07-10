/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Chip family and PCI ID table for the RDNA+ native AMD GPU driver (Phase A0).
 */

#ifndef _AMDGPU_CHIP_H_
#define _AMDGPU_CHIP_H_

#include <sys/types.h>
#include <sys/stdint.h>
#include <stdbool.h>

#define AMDGPU_PCI_VENDOR_AMD	0x1002

enum amdgpu_family {
	AMDGPU_FAMILY_UNKNOWN = 0,
	AMDGPU_FAMILY_RDNA1,
	AMDGPU_FAMILY_RDNA2,
	AMDGPU_FAMILY_IP_DISCOVERY
};

enum amdgpu_chip_id {
	AMDGPU_CHIP_UNKNOWN = 0,
	AMDGPU_CHIP_NAVI10,
	AMDGPU_CHIP_NAVI12,
	AMDGPU_CHIP_NAVI14,
	AMDGPU_CHIP_CYAN_SKILLFISH,
	AMDGPU_CHIP_SIENNA_CICHLID,
	AMDGPU_CHIP_NAVY_FLOUNDER,
	AMDGPU_CHIP_VANGOGH,
	AMDGPU_CHIP_DIMGREY_CAVEFISH,
	AMDGPU_CHIP_BEIGE_GOBY,
	AMDGPU_CHIP_YELLOW_CARP,
	AMDGPU_CHIP_IP_DISCOVERY
};

struct amdgpu_chip_info {
	enum amdgpu_chip_id	chip_id;
	enum amdgpu_family	family;
	const char		*codename;
	const char		*family_name;
	bool			is_apu;
};

struct amdgpu_pci_id {
	uint16_t			device;
	const char			*name;
	const struct amdgpu_chip_info	*chip;
};

const struct amdgpu_pci_id *amdgpu_chip_lookup_device(uint16_t device);
const struct amdgpu_chip_info *amdgpu_chip_resolve(uint16_t vendor,
    uint16_t device, uint8_t classb, uint8_t subclass);
const char *amdgpu_chip_id_name(enum amdgpu_chip_id id);

#endif /* _AMDGPU_CHIP_H_ */
