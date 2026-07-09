/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Interrupt ingress boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_intr.h"
#include "nvgpu_chip.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgpu_display.h"
#include "nvgpu_exec.h"
#include "nvgpu_sched.h"
#include "nvgsp_event.h"

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/serialize.h>

#define NVGPU_PCI_MSI_REARM		0x68
#define NVGPU_CPU_INTR_TOP		0x00000100u
#define NVGPU_CPU_INTR_TOP_EN_CLEAR	0x00000164u
#define NVGPU_GSP_MSGQ_INTR		0x00000040u

MALLOC_DEFINE(M_NVGPU_INTR, "nvgpu_intr", "nvgpu interrupt state");

struct nvgpu_intr_state {
	int irq_rid;
	bool irq_msi;
	struct resource *irq_res;
	void *irq_cookie;
	struct lwkt_serialize irq_serialize;
	uint64_t isr_count;
	uint64_t empty_count;
	uint64_t msgq_count;
	uint64_t unexpected_count;
	uint32_t last_stat;
	uint32_t last_top;
};

static void
nvgpu_intr_rearm_msi(struct nvgpu_device *gpu, struct nvgpu_intr_state *intr)
{
	if (!intr->irq_msi)
		return;
	pci_write_config(nvgpu_device_get_newbus_dev(gpu), NVGPU_PCI_MSI_REARM, 0xff, 1);
}

static void
nvgpu_intr_decode(struct nvgpu_device *gpu)
{
	struct nvgpu_intr_state *intr = nvgpu_device_get_intr(gpu);
	const struct nvgpu_chip_config *chip = nvgpu_device_get_chip(gpu);
	uint32_t intr_reg, mask, stat, top;

	if (intr == NULL)
		return;
	intr->isr_count++;
	nvgpu_device_wr32(gpu, NVGPU_CPU_INTR_TOP_EN_CLEAR, 0x0000000fu);
	nvgpu_intr_rearm_msi(gpu, intr);

	intr_reg = nvgpu_device_rd32(gpu, chip->gsp_base + 0x008);
	mask = nvgpu_device_rd32(gpu, chip->gsp_riscv + 0x2b4);
	stat = intr_reg & mask;
	top = nvgpu_device_rd32(gpu, NVGPU_CPU_INTR_TOP);
	intr->last_stat = stat;
	intr->last_top = top;

	if (stat == 0 && top == 0) {
		intr->empty_count++;
		return;
	}
	if (stat & NVGPU_GSP_MSGQ_INTR) {
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x004, NVGPU_GSP_MSGQ_INTR);
		intr->msgq_count++;
		nvgsp_event_dispatch(gpu);
		stat &= ~NVGPU_GSP_MSGQ_INTR;
	}
	if (stat != 0) {
		intr->unexpected_count++;
		nvgpu_log(NVGPU_LOG_DEBUG, "unexpected gsp intr stat=0x%08x top=0x%08x\n",
		    stat, top);
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x014, stat);
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x004, stat);
	}
}

static void
nvgpu_intr_handle_isr(void *arg)
{
	struct nvgpu_device *gpu = arg;

	nvgpu_intr_handle(gpu);
}

int
nvgpu_intr_init(struct nvgpu_device *gpu)
{
	struct nvgpu_intr_state *intr;
	device_t dev = nvgpu_device_get_newbus_dev(gpu);
	int msi_count;
	int want = 1;

	if (nvgpu_device_get_intr(gpu) != NULL)
		return (0);
	intr = kmalloc(sizeof(*intr), M_NVGPU_INTR, M_WAITOK | M_ZERO);
	msi_count = pci_msi_count(dev);
	if (msi_count >= 1 && pci_alloc_msi(dev, &want, 1, -1) == 0) {
		uint16_t cmd;

		intr->irq_rid = 1;
		intr->irq_msi = true;
		cmd = pci_read_config(dev, PCIR_COMMAND, 2);
		pci_write_config(dev, PCIR_COMMAND, cmd | 0x0400, 2);
	} else {
		intr->irq_rid = 0;
		intr->irq_msi = false;
	}

	intr->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &intr->irq_rid,
	    intr->irq_msi ? RF_ACTIVE : (RF_ACTIVE | RF_SHAREABLE));
	if (intr->irq_res == NULL) {
		if (intr->irq_msi)
			pci_release_msi(dev);
		kfree(intr, M_NVGPU_INTR);
		return (ENXIO);
	}

	lwkt_serialize_init(&intr->irq_serialize);
	if (bus_setup_intr(dev, intr->irq_res, INTR_MPSAFE, nvgpu_intr_handle_isr,
	    gpu, &intr->irq_cookie, &intr->irq_serialize) != 0) {
		bus_release_resource(dev, SYS_RES_IRQ, intr->irq_rid, intr->irq_res);
		if (intr->irq_msi)
			pci_release_msi(dev);
		kfree(intr, M_NVGPU_INTR);
		return (ENXIO);
	}
	nvgpu_device_set_intr(gpu, intr);
	nvgpu_log(NVGPU_LOG_DEBUG, "irq wired rid=%d msi=%d\n", intr->irq_rid,
	    intr->irq_msi ? 1 : 0);
	return (0);
}

int
nvgpu_intr_enable(struct nvgpu_device *gpu)
{
	struct nvgpu_intr_state *intr = nvgpu_device_get_intr(gpu);
	const struct nvgpu_chip_config *chip = nvgpu_device_get_chip(gpu);

	if (intr == NULL)
		return (ENXIO);
	nvgpu_device_wr32(gpu, chip->gsp_base + 0x004, NVGPU_GSP_MSGQ_INTR);
	nvgpu_intr_rearm_msi(gpu, intr);
	return (0);
}

void
nvgpu_intr_disable(struct nvgpu_device *gpu)
{
	const struct nvgpu_chip_config *chip = nvgpu_device_get_chip(gpu);

	if (chip != NULL)
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x004, 0);
}

void
nvgpu_intr_fini(struct nvgpu_device *gpu)
{
	struct nvgpu_intr_state *intr = nvgpu_device_get_intr(gpu);
	device_t dev = nvgpu_device_get_newbus_dev(gpu);

	if (intr == NULL)
		return;
	if (intr->irq_cookie != NULL) {
		bus_teardown_intr(dev, intr->irq_res, intr->irq_cookie);
		intr->irq_cookie = NULL;
	}
	if (intr->irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, intr->irq_rid, intr->irq_res);
		intr->irq_res = NULL;
	}
	if (intr->irq_msi)
		pci_release_msi(dev);
	nvgpu_device_set_intr(gpu, NULL);
	kfree(intr, M_NVGPU_INTR);
}

void
nvgpu_intr_handle(struct nvgpu_device *gpu)
{
	nvgpu_intr_decode(gpu);
	nvgpu_exec_complete_from_intr(gpu);
	nvgpu_display_handle_vblank(gpu);
	nvgsp_event_wake_msgq(gpu);
}
