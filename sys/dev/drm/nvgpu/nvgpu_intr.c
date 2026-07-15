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
#include "nvgpu_future.h"
#include "nvgpu_sched.h"
#include "nvgsp_channel.h"
#include "nvgsp_event.h"
#include "nvgsp_state.h"

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>
#include <sys/bus.h>
#include <sys/globaldata.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/rman.h>
#include <sys/serialize.h>
#include <sys/spinlock.h>
#include <sys/spinlock2.h>

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
#define NVGPU_DISPLAY_INTR_HEAD_MASK	0x00611ec0u
#define NVGPU_DISPLAY_INTR_HEAD_STATUS(head) (0x00611c00u + (head) * 4u)
#define NVGPU_DISPLAY_INTR_HEAD_ACK(head)	(0x00611800u + (head) * 4u)
#define NVGPU_DISPLAY_INTR_VBLANK	0x00000002u

#ifndef KTR_NVGPU
#define KTR_NVGPU KTR_ALL
#endif

KTR_INFO_MASTER_EXTERN(nvgpu);
KTR_INFO(KTR_NVGPU, nvgpu, intr_decode, 16,
    "intr decode stat=0x%x top=0x%x isr=%ju", uint32_t stat,
    uint32_t top, uintmax_t isr_count);
KTR_INFO(KTR_NVGPU, nvgpu, intr_empty, 17,
    "intr empty isr=%ju", uintmax_t isr_count);
KTR_INFO(KTR_NVGPU, nvgpu, intr_wake_worker, 18,
    "intr wake worker events=0x%x", u_int events);
KTR_INFO(KTR_NVGPU, nvgpu, intr_worker_events, 19,
    "intr worker events=0x%x", u_int events);
KTR_INFO(KTR_NVGPU, nvgpu, intr_exec_complete, 20,
    "intr exec complete sema=%p future=%p chid=%u value=%u target=%u",
    void *sema, void *future, uint32_t chid, uint32_t value,
    uint32_t target);
KTR_INFO(KTR_NVGPU, nvgpu, intr_display_vblank, 21,
    "intr display vblank head=%u status=0x%x", uint32_t head,
    uint32_t status);
KTR_INFO(KTR_NVGPU, nvgpu, intr_park, 22,
    "intr park sema=%p future=%p chid=%u target=%u", void *sema,
    void *future, uint32_t chid, uint32_t target);

MALLOC_DEFINE(M_NVGPU_INTR, "nvgpu_intr", "nvgpu interrupt state");

TAILQ_HEAD(nvgpu_sema_list, nvgpu_sema);

struct nvgpu_intr_state {
	struct nvgpu_device *gpu;
	int irq_rid;
	bool irq_msi;
	struct resource *irq_res;
	void *irq_cookie;
	struct lwkt_serialize irq_serialize;
	struct lwkt_token worker_token;
	struct spinlock parked_spin;
	struct nvgpu_sema_list parked;
	struct thread *worker;
	volatile u_int events;
	uint64_t fault_chids[NVGPU_INTR_CHID_COUNT / 64];
	bool display_dispatch_enabled;
	bool display_dispatch_running;
	bool stopping;
	uint64_t isr_count;
	uint64_t empty_count;
	uint64_t msgq_count;
	uint64_t unexpected_count;
	uint32_t unexpected_seen[8];
	uint32_t last_stat;
	uint32_t last_top;
};

static void nvgpu_intr_run(void *arg);
static int nvgpu_intr_init(struct nvgpu_device *gpu);
static int nvgpu_intr_enable(struct nvgpu_device *gpu);
static void nvgpu_intr_disable(struct nvgpu_device *gpu);
static void nvgpu_intr_fini(struct nvgpu_device *gpu);

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
	uint32_t unhandled_leaf[8] = {};
	bool handled = false;
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
	KTR_LOG(nvgpu_intr_decode, stat, top, (uintmax_t)intr->isr_count);

	if (stat == 0 && top == 0) {
		intr->empty_count++;
		KTR_LOG(nvgpu_intr_empty, (uintmax_t)intr->isr_count);
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
			unhandled_leaf[leaf] = unhandled;
		}
		if (nonstall != 0) {
			handled = true;
			nvgpu_device_wr32(gpu, NVGPU_CPU_INTR_LEAF(leaf), nonstall);
			events |= NVGPU_INTR_EVENT_EXEC;
		}
		if (stall != 0) {
			handled = true;
			if ((stall & masks.display) != 0)
				events |= NVGPU_INTR_EVENT_DISPLAY;
			if ((stall & masks.engine) != 0)
				events |= NVGPU_INTR_EVENT_GSP;
			nvgpu_device_wr32(gpu, NVGPU_CPU_INTR_LEAF(leaf), stall);
		}
	}
	if (stat & NVGPU_GSP_MSGQ_INTR) {
		handled = true;
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x004, NVGPU_GSP_MSGQ_INTR);
		intr->msgq_count++;
		events |= NVGPU_INTR_EVENT_GSP;
		stat &= ~NVGPU_GSP_MSGQ_INTR;
	}
	if (!handled) {
		for (uint32_t leaf = 0; leaf < 8; leaf++) {
			uint32_t unhandled = unhandled_leaf[leaf];

			if (unhandled == 0 ||
			    (unhandled & ~intr->unexpected_seen[leaf]) == 0)
				continue;
			intr->unexpected_seen[leaf] |= unhandled;
			nvgpu_log(NVGPU_LOG_DEBUG,
			    "unexpected CPU intr leaf=%u unhandled=0x%08x "
			    "top=0x%08x\n", leaf, unhandled, top);
		}
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
		KTR_LOG(nvgpu_intr_wake_worker, events);
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

		KTR_LOG(nvgpu_intr_worker_events, events);
		if ((events & NVGPU_INTR_EVENT_GSP) != 0)
			nvgsp_event_dispatch(gpu);
		if ((events & NVGPU_INTR_EVENT_EXEC) != 0) {
			struct nvgpu_sema_list completed;
			struct nvgpu_sema *sema, *next;

			TAILQ_INIT(&completed);
			spin_lock(&intr->parked_spin);
			for (sema = TAILQ_FIRST(&intr->parked); sema != NULL;
			    sema = next) {
				next = TAILQ_NEXT(sema, parked_link);
				cpu_lfence();
				if ((int32_t)(*sema->address - sema->target) < 0)
					continue;
				TAILQ_REMOVE(&intr->parked, sema, parked_link);
				sema->parked = false;
				TAILQ_INSERT_TAIL(&completed, sema, parked_link);
			}
			spin_unlock(&intr->parked_spin);
			while ((sema = TAILQ_FIRST(&completed)) != NULL) {
				struct nvgpu_future *future = sema->future;
				int error;

				TAILQ_REMOVE(&completed, sema, parked_link);
				sema->future = NULL;
				KTR_LOG(nvgpu_intr_exec_complete, sema, future,
				    sema->chid, sema->address != NULL ?
				    *sema->address : 0u, sema->target);
				error = nvgpu_sched_put(future);
				KASSERT(error == 0,
				    ("completion after scheduler stop: %d", error));
			}
		}
		if ((events & NVGPU_INTR_EVENT_DISPLAY) != 0) {
			const struct nvgpu_chip_config *chip = nvgpu_device_get_chip(gpu);
			uint32_t head_mask = nvgpu_device_rd32(gpu,
			    NVGPU_DISPLAY_INTR_HEAD_MASK) & 0xffu;
			bool dispatch;

			lwkt_gettoken(&intr->worker_token);
			dispatch = intr->display_dispatch_enabled;
			if (dispatch)
				intr->display_dispatch_running = true;
			lwkt_reltoken(&intr->worker_token);

			for (uint32_t head = 0; dispatch &&
			    head < chip->display_heads; head++) {
				uint32_t status;

				if ((head_mask & (1u << head)) == 0)
					continue;
				status = nvgpu_device_rd32(gpu,
				    NVGPU_DISPLAY_INTR_HEAD_STATUS(head));
				if ((status & NVGPU_DISPLAY_INTR_VBLANK) == 0)
					continue;
				KTR_LOG(nvgpu_intr_display_vblank, head, status);
				nvgpu_display_handle_vblank(gpu, head);
				nvgpu_device_wr32(gpu,
				    NVGPU_DISPLAY_INTR_HEAD_ACK(head),
				    NVGPU_DISPLAY_INTR_VBLANK);
			}
			if (dispatch) {
				lwkt_gettoken(&intr->worker_token);
				intr->display_dispatch_running = false;
				wakeup(&intr->display_dispatch_running);
				lwkt_reltoken(&intr->worker_token);
			}
		}
		if ((events & NVGPU_INTR_EVENT_FAULT) != 0) {
			for (uint32_t word = 0; word < NVGPU_INTR_CHID_COUNT / 64;
			    word++) {
				while (fault_chids[word] != 0) {
					struct nvgpu_sema_list failed;
					struct nvgpu_sema *sema, *next;
					uint32_t bit = __builtin_ctzll(fault_chids[word]);
					uint32_t chid = word * 64 + bit;

					fault_chids[word] &= ~(1ULL << bit);
					nvgsp_channel_mark_fault(gpu, chid, EIO);
					TAILQ_INIT(&failed);
					spin_lock(&intr->parked_spin);
					for (sema = TAILQ_FIRST(&intr->parked);
					    sema != NULL; sema = next) {
						next = TAILQ_NEXT(sema, parked_link);
						if (sema->chid != chid)
							continue;
						TAILQ_REMOVE(&intr->parked, sema,
						    parked_link);
						sema->parked = false;
						sema->error = EIO;
						TAILQ_INSERT_TAIL(&failed, sema,
						    parked_link);
					}
					spin_unlock(&intr->parked_spin);
					while ((sema = TAILQ_FIRST(&failed)) != NULL) {
						struct nvgpu_future *future = sema->future;
						int error;

						TAILQ_REMOVE(&failed, sema, parked_link);
						sema->future = NULL;
						error = nvgpu_sched_put(future);
						KASSERT(error == 0,
						    ("fault after scheduler stop: %d", error));
					}
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

	nvgpu_intr_decode(gpu);
}

static int
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
	spin_init(&intr->parked_spin, "nvgpu parked futures");
	TAILQ_INIT(&intr->parked);
	if (lwkt_create(nvgpu_intr_run, intr, &intr->worker, NULL,
	    TDF_NOSTART, mycpu->gd_cpuid, "nvgpu_intr") != 0) {
		lwkt_token_uninit(&intr->worker_token);
		spin_uninit(&intr->parked_spin);
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
		spin_uninit(&intr->parked_spin);
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
		spin_uninit(&intr->parked_spin);
		kfree(intr, M_NVGPU_INTR);
		return (ENXIO);
	}
	nvgpu_device_set_intr(gpu, intr);
	nvgpu_log(NVGPU_LOG_DEBUG, "irq wired rid=%d msi=%d\n", intr->irq_rid,
	    intr->irq_msi ? 1 : 0);
	return (0);
}

static int
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
nvgpu_intr_enable_display_dispatch(struct nvgpu_device *gpu)
{
	struct nvgpu_intr_state *intr = nvgpu_device_get_intr(gpu);

	if (intr == NULL)
		return;
	lwkt_gettoken(&intr->worker_token);
	intr->display_dispatch_enabled = true;
	lwkt_reltoken(&intr->worker_token);
}

void
nvgpu_intr_disable_display_dispatch(struct nvgpu_device *gpu)
{
	struct nvgpu_intr_state *intr = nvgpu_device_get_intr(gpu);

	if (intr == NULL)
		return;
	lwkt_gettoken(&intr->worker_token);
	intr->display_dispatch_enabled = false;
	intr->events &= ~NVGPU_INTR_EVENT_DISPLAY;
	while (intr->display_dispatch_running)
		tsleep(&intr->display_dispatch_running, 0, "nvgpudq", 0);
	lwkt_reltoken(&intr->worker_token);
}

static void
nvgpu_intr_disable(struct nvgpu_device *gpu)
{
	const struct nvgpu_chip_config *chip = nvgpu_device_get_chip(gpu);

	if (chip != NULL)
		nvgpu_device_wr32(gpu, chip->gsp_base + 0x004, 0);
}

static void
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
	spin_lock(&intr->parked_spin);
	KASSERT(TAILQ_EMPTY(&intr->parked),
	    ("stopping interrupt state with parked futures"));
	spin_unlock(&intr->parked_spin);
	spin_uninit(&intr->parked_spin);
	nvgpu_device_set_intr(gpu, NULL);
	kfree(intr, M_NVGPU_INTR);
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

int
nvgpu_intr_start(struct nvgpu_device *gpu)
{
	int error;

	error = nvgpu_intr_init(gpu);
	if (error != 0)
		return (error);
	error = nvgpu_intr_enable(gpu);
	if (error != 0) {
		nvgpu_intr_fini(gpu);
		return (error);
	}
	return (0);
}

void
nvgpu_intr_stop(struct nvgpu_device *gpu)
{
	nvgpu_intr_disable(gpu);
	nvgpu_intr_fini(gpu);
}

int
nvgpu_intr_park(struct nvgpu_sema *sema, struct nvgpu_future *future)
{
	struct nvgpu_intr_state *intr;

	if (sema == NULL || future == NULL || sema->device == NULL ||
	    sema->address == NULL || sema->target == 0)
		return (EINVAL);
	intr = nvgpu_device_get_intr(sema->device);
	if (intr == NULL)
		return (ENODEV);
	spin_lock(&intr->parked_spin);
	if (intr->stopping) {
		spin_unlock(&intr->parked_spin);
		return (ENODEV);
	}
	KASSERT(!sema->parked && sema->future == NULL,
	    ("parking one GPU semaphore twice"));
	sema->future = future;
	sema->error = 0;
	sema->parked = true;
	TAILQ_INSERT_TAIL(&intr->parked, sema, parked_link);
	KTR_LOG(nvgpu_intr_park, sema, future, sema->chid, sema->target);
	spin_unlock(&intr->parked_spin);
	return (0);
}
