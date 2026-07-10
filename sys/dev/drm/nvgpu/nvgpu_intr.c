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
#include "nvgsp_state.h"

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>
#include <machine/atomic.h>
#include <sys/bus.h>
#include <sys/globaldata.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/rman.h>
#include <sys/serialize.h>

#define NVGPU_PCI_MSI_REARM		0x68
#define NVGPU_CPU_INTR_TOP		0x00b81600u
#define NVGPU_CPU_INTR_TOP_EN_CLEAR	0x00b81610u
#define NVGPU_CPU_INTR_TOP_EN_SET	0x00b81608u
#define NVGPU_CPU_INTR_LEAF(i)		(0x00b81000u + (i) * 4u)
#define NVGPU_GSP_MSGQ_INTR		0x00000040u
#define NVGPU_INTR_EVENT_EXEC		0x00000001u
#define NVGPU_INTR_EVENT_GSP		0x00000002u
#define NVGPU_INTR_EVENT_DISPLAY	0x00000004u
#define NVGPU_INTR_EVENT_FAULT		0x00000008u
#define NVGPU_INTR_CHID_COUNT		2048u

MALLOC_DEFINE(M_NVGPU_INTR, "nvgpu_intr", "nvgpu interrupt state");

struct nvgpu_intr_state {
	struct nvgpu_device *gpu;
	int irq_rid;
	bool irq_msi;
	struct resource *irq_res;
	void *irq_cookie;
	struct lwkt_serialize irq_serialize;
	struct lwkt_token worker_token;
	struct thread *worker;
	volatile u_int events;
	uint64_t fault_chids[NVGPU_INTR_CHID_COUNT / 64];
	bool stopping;
	uint64_t isr_count;
	uint64_t empty_count;
	uint64_t msgq_count;
	uint64_t unexpected_count;
	uint32_t last_stat;
	uint32_t last_top;
};

static void nvgpu_intr_run(void *arg);

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
	u_int events = 0;

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
		goto rearm;
	}
	for (uint32_t leaf = 0; leaf < 8; leaf++) {
		struct nvgsp_intr_masks masks;
		uint32_t leaf_stat, nonstall, stall, known, unhandled;

		if ((top & (1u << (leaf / 2u))) == 0)
			continue;
		leaf_stat = nvgpu_device_rd32(gpu, NVGPU_CPU_INTR_LEAF(leaf));
		nvgsp_state_get_intr_masks(gpu, leaf, &masks);
		nonstall = leaf_stat & masks.nonstall;
		stall = leaf_stat & masks.stall;
		known = masks.nonstall | masks.stall;
		unhandled = leaf_stat & ~known;
		if (unhandled != 0) {
			intr->unexpected_count++;
			nvgpu_log(NVGPU_LOG_DEBUG,
			    "unexpected CPU intr leaf=%u mask=0x%08x\n",
			    leaf, unhandled);
		}
		if (nonstall != 0) {
			nvgpu_device_wr32(gpu, NVGPU_CPU_INTR_LEAF(leaf), nonstall);
			events |= NVGPU_INTR_EVENT_EXEC;
		}
		if (stall != 0) {
			if ((stall & masks.display) != 0)
				events |= NVGPU_INTR_EVENT_DISPLAY;
			if ((stall & masks.engine) != 0)
				events |= NVGPU_INTR_EVENT_GSP;
			nvgpu_device_wr32(gpu, NVGPU_CPU_INTR_LEAF(leaf), stall);
		}
	}
	if (stat & NVGPU_GSP_MSGQ_INTR) {
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x004, NVGPU_GSP_MSGQ_INTR);
		intr->msgq_count++;
		events |= NVGPU_INTR_EVENT_GSP;
		stat &= ~NVGPU_GSP_MSGQ_INTR;
	}
	if (stat != 0) {
		intr->unexpected_count++;
		nvgpu_log(NVGPU_LOG_DEBUG, "unexpected gsp intr stat=0x%08x top=0x%08x\n",
		    stat, top);
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x014, stat);
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x004, stat);
	}

rearm:
	nvgpu_device_wr32(gpu, NVGPU_CPU_INTR_TOP_EN_SET, 0x0000000fu);
	nvgpu_device_wr32(gpu, chip->gsp_base + 0x3e8, 0x1);
	if (events != 0) {
		lwkt_gettoken(&intr->worker_token);
		intr->events |= events;
		wakeup(&intr->events);
		lwkt_reltoken(&intr->worker_token);
	}
}

static void
nvgpu_intr_run(void *arg)
{
	struct nvgpu_intr_state *intr = arg;
	struct nvgpu_device *gpu = intr->gpu;
	uint64_t fault_chids[NVGPU_INTR_CHID_COUNT / 64];
	u_int events;

	for (;;) {
		lwkt_gettoken(&intr->worker_token);
		while (intr->events == 0 && !intr->stopping)
			tsleep(&intr->events, 0, "nvgpui", MAX(hz / 10, 1));
		if (intr->events == 0 && intr->stopping)
			break;
		events = (u_int)atomic_swap_int((volatile int *)&intr->events, 0);
		if ((events & NVGPU_INTR_EVENT_FAULT) != 0) {
			memcpy(fault_chids, intr->fault_chids,
			    sizeof(fault_chids));
			memset(intr->fault_chids, 0, sizeof(intr->fault_chids));
		}
		lwkt_reltoken(&intr->worker_token);

		if ((events & NVGPU_INTR_EVENT_GSP) != 0)
			nvgsp_event_dispatch(gpu);
		if ((events & NVGPU_INTR_EVENT_EXEC) != 0)
			nvgpu_exec_harvest_completed(gpu);
		if ((events & NVGPU_INTR_EVENT_DISPLAY) != 0)
			nvgpu_display_handle_vblank(gpu);
		if ((events & NVGPU_INTR_EVENT_FAULT) != 0) {
			for (uint32_t word = 0; word < NVGPU_INTR_CHID_COUNT / 64;
			    word++) {
				while (fault_chids[word] != 0) {
					uint32_t bit = __builtin_ctzll(fault_chids[word]);

					fault_chids[word] &= ~(1ULL << bit);
					nvgpu_exec_fail_channel(gpu, word * 64 + bit, EIO);
				}
			}
		}
	}
	intr->worker = NULL;
	wakeup(&intr->worker);
	lwkt_reltoken(&intr->worker_token);
	lwkt_exit();
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
	intr->gpu = gpu;
	lwkt_token_init(&intr->worker_token, "nvgpui");
	if (lwkt_create(nvgpu_intr_run, intr, &intr->worker, NULL,
	    TDF_NOSTART, mycpu->gd_cpuid, "nvgpu_intr") != 0) {
		lwkt_token_uninit(&intr->worker_token);
		kfree(intr, M_NVGPU_INTR);
		return (ENOMEM);
	}
	lwkt_setpri_initial(intr->worker, TDPRI_KERN_DAEMON);
	lwkt_schedule(intr->worker);
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
		lwkt_gettoken(&intr->worker_token);
		intr->stopping = true;
		wakeup(&intr->events);
		while (intr->worker != NULL)
			tsleep(&intr->worker, 0, "nvgpuix", 0);
		lwkt_reltoken(&intr->worker_token);
		lwkt_token_uninit(&intr->worker_token);
		kfree(intr, M_NVGPU_INTR);
		return (ENXIO);
	}

	lwkt_serialize_init(&intr->irq_serialize);
	if (bus_setup_intr(dev, intr->irq_res, INTR_MPSAFE, nvgpu_intr_handle_isr,
	    gpu, &intr->irq_cookie, &intr->irq_serialize) != 0) {
		bus_release_resource(dev, SYS_RES_IRQ, intr->irq_rid, intr->irq_res);
		if (intr->irq_msi)
			pci_release_msi(dev);
		lwkt_gettoken(&intr->worker_token);
		intr->stopping = true;
		wakeup(&intr->events);
		while (intr->worker != NULL)
			tsleep(&intr->worker, 0, "nvgpuix", 0);
		lwkt_reltoken(&intr->worker_token);
		lwkt_token_uninit(&intr->worker_token);
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
	if (nvgsp_state_enable_intr(gpu) != 0)
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
	lwkt_gettoken(&intr->worker_token);
	intr->stopping = true;
	wakeup(&intr->events);
	while (intr->worker != NULL)
		tsleep(&intr->worker, 0, "nvgpuix", 0);
	lwkt_reltoken(&intr->worker_token);
	lwkt_token_uninit(&intr->worker_token);
	nvgpu_device_set_intr(gpu, NULL);
	kfree(intr, M_NVGPU_INTR);
}

void
nvgpu_intr_handle(struct nvgpu_device *gpu)
{
	nvgpu_intr_decode(gpu);
}

void
nvgpu_intr_report_channel_fault(struct nvgpu_device *gpu, uint32_t chid)
{
	struct nvgpu_intr_state *intr = nvgpu_device_get_intr(gpu);

	if (intr == NULL || chid >= NVGPU_INTR_CHID_COUNT)
		return;
	lwkt_gettoken(&intr->worker_token);
	intr->fault_chids[chid / 64] |= 1ULL << (chid % 64);
	intr->events |= NVGPU_INTR_EVENT_FAULT;
	wakeup(&intr->events);
	lwkt_reltoken(&intr->worker_token);
}

void
nvgpu_intr_request_exec_harvest(struct nvgpu_device *gpu)
{
	struct nvgpu_intr_state *intr = nvgpu_device_get_intr(gpu);

	if (intr == NULL)
		return;
	atomic_set_int(&intr->events, NVGPU_INTR_EVENT_EXEC);
	wakeup(&intr->events);
}
