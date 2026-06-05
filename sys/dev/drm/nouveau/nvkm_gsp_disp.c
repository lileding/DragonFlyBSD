/*
 * nvkm_gsp_disp.c -- GSP display bring-up + connector/EDID enumeration.
 *
 * Phase 2, milestone M1a: bring up the GSP display subsystem and read the EDID
 * of connected outputs. This proves the display control chain (RAMIN +
 * WRITE_INST_MEM + NV04_DISPLAY_COMMON + the NV0073 connector/EDID controls)
 * works against r570 GSP-RM, and is the foundation for modeset (M2+).
 *
 * GSP proxy model: GSP-RM owns the display hardware; we only orchestrate via RM
 * controls on the NV04_DISPLAY_COMMON object. Runs on the device-attach kernel
 * thread (blockable; synchronous GSP RPCs under gsp_tok), NOT the GSP ithread.
 *
 * The struct layouts and control numbers below are copied (SPDX MIT) from
 * NVIDIA open-gpu-kernel-modules 570.144, via nouveau linux-v7.0
 * nvkm/subdev/gsp/rm/{r570,r535}/nvrm/disp.h. r570 firmware uses the r570
 * control numbers (GET_SUPPORTED=0x730107, GET_CONNECT_STATE=0x730108), which
 * differ from r535 -- verified against r570_disp_get_supported() etc.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/taskqueue.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/pmap.h>		/* vtophys */
#include <linux/slab.h>

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"

static MALLOC_DEFINE(M_NVKM_DISP, "nvkm_disp", "nvkm display pushbuffers");

/* ===== MIT definitions (open-gpu 570.144; NvU32->u32, NvU64->u64, NvBool/NvU8->u8) ===== */

#define NV04_DISPLAY_COMMON				0x00000073u
#define NV_MEMORY_WRITECOMBINED				2u
#define ADDR_FBMEM					2u

#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM	0x20800a49u
struct disp_write_inst_mem_params {
	uint64_t instMemPhysAddr;
	uint64_t instMemSize;
	uint32_t instMemAddrSpace;
	uint32_t instMemCpuCacheAttr;
};

#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO 0x20800a01u
struct disp_get_static_info_params {
	uint32_t feHwSysCap;
	uint32_t windowPresentMask;
	uint8_t  bFbRemapperEnabled;
	uint32_t numHeads;
	uint32_t i2cPort;
	uint32_t internalDispActiveMask;
	uint32_t embeddedDisplayPortMask;
	uint8_t  bExternalMuxSupported;
	uint8_t  bInternalMuxSupported;
	uint32_t numDispChannels;
};

/* r570 numbers */
#define NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED		0x730107u
struct disp_get_supported_params {
	uint32_t subDeviceInstance;
	uint32_t displayMask;
	uint32_t displayMaskDDC;
};

#define NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE	0x730108u
struct disp_get_connect_state_params {
	uint32_t subDeviceInstance;
	uint32_t flags;
	uint32_t displayMask;
	uint32_t retryTimeMs;
};

/* shared r535/r570 numbers */
#define NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS		0x730102u
struct disp_get_num_heads_params {
	uint32_t subDeviceInstance;
	uint32_t flags;
	uint32_t numHeads;
};

#define NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK	0x730287u
struct disp_get_all_head_mask_params {
	uint32_t subDeviceInstance;
	uint32_t headMask;
};

#define NV0073_CTRL_SPECIFIC_GET_EDID_MAX_EDID_BYTES	2048u
#define NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2		0x730245u
struct disp_get_edid_v2_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t bufferSize;
	uint32_t flags;
	uint8_t  edidBuffer[NV0073_CTRL_SPECIFIC_GET_EDID_MAX_EDID_BYTES];
};

/* NV04_DISPLAY_COMMON child handle base (our convention; mirrors NVKM_RM_*).
 * Must NOT share the disp client's prefix, or child_handle() (base | client&0xfff)
 * would alias the client handle itself. 0x0073xxxx mirrors the class number. */
#define NVKM_RM_DISP					0x00730000u

/* Hotplug event (M1b). NV01_EVENT without NONSTALL_INTR -> delivered as a GSP
 * POST_EVENT message (software event), not a HW interrupt. */
#define NV01_EVENT_KERNEL_CALLBACK_EX			0x0000007eu
#define NV2080_NOTIFIERS_HOTPLUG			1u
#define NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION		0x20800301u
#define NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT 2u
/* Event-object child handle base; distinct from client/objcom/device handles. */
#define NVKM_RM_DISP_HPD				0x007e0000u

struct disp_nv0005_alloc_params {
	uint32_t hParentClient;
	uint32_t hSrcResource;
	uint32_t hClass;
	uint32_t notifyIndex;
	uint64_t data;
};

struct disp_event_set_notification_params {
	uint32_t event;
	uint32_t action;
	uint32_t bNotifyState;
	uint32_t info32;
	uint16_t info16;
	uint8_t  _pad[2];
};

/* Core display channel (M2a). TU102 NVDisplay classes. */
#define TU102_DISP				0x0000c570u	/* display root */
#define TU102_DISP_CORE_CHANNEL_DMA		0x0000c57du	/* core (NVC57D) */
#define NVKM_RM_DISP_CORE			0xc57d0000u

#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER 0x20800a58u
#define DISP_ADDR_SYSMEM			1u
#define DISP_PHYS_PCI_COHERENT			3u	/* PBTARGETAPERTURE */

struct disp_channel_pushbuffer_params {
	uint32_t addressSpace;
	uint64_t physicalAddr;	/* NV_DECLARE_ALIGNED 8 */
	uint64_t limit;
	uint32_t cacheSnoop;
	uint32_t hclass;
	uint32_t channelInstance;
	uint8_t  valid;
	uint32_t pbTargetAperture;
	uint32_t channelPBSize;
	uint32_t subDeviceId;
};

struct disp_channeldma_alloc_params {
	uint32_t channelInstance;
	uint32_t hObjectBuffer;
	uint32_t hObjectNotify;
	uint32_t offset;
	uint64_t pControl;	/* NV_ALIGN_BYTES(8) */
	uint32_t flags;
	uint32_t channelPBSize;
	uint32_t subDeviceId;
};

/* ===== M4a: NVDisplay core channel EVO method push =====
 *
 * NVDisplay (Volta+) DMA pushbuffer method encoding (MIT, open-gpu 570.144 via
 * nouveau linux-v7.0 nvhw/class/clc37b.h). A method group is one header dword
 * plus `count` data dwords:
 *
 *   header = (OPCODE_METHOD << 29) | (count << 18) | ((mthd >> 2) << 2)
 *
 * with OPCODE_METHOD = 0, METHOD_COUNT in bits [27:18], METHOD_OFFSET in bits
 * [13:2]. PUT/GET are dword offsets into the pushbuffer; the host writes PUT
 * directly and the channel's DMA fetcher advances GET to PUT as it consumes
 * methods (nouveau dispnv50/disp.c nv50_dmac_kick writes PUT = cur_in_dwords).
 */
#define NVC57D_WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(i)	\
						(0x00001004u + (i) * 0x00000080u)
/* RGB_PACKED 1/2/4/8 BPP (bits 0..3) -- the harmless arm value nouveau's
 * corec57d_init writes; meaningless without a following UPDATE. */
#define EVO_FORMAT_USAGE_RGB_PACKED_ALL		0x0000000fu

static inline uint32_t
evo_method_hdr(uint32_t mthd, uint32_t count)
{
	/* OPCODE_METHOD(0) | METHOD_COUNT[27:18] | METHOD_OFFSET[13:2]. */
	return ((count & 0x3ffu) << 18) | (mthd & 0x3ffcu);
}

/*
 * M4a smoke: push a small batch of harmless arm methods into the NVC57D core
 * channel pushbuffer, kick PUT, and confirm the channel's GET catches up to
 * PUT. This proves three things end to end: EVO method encoding, the direct
 * PUT doorbell write, and that the GSP-initialised core channel actually
 * fetches and consumes host-written methods. It is the foundation for real
 * modeset (M4b/c). No NOTIFIER/ctxdma is involved yet -- those arrive with the
 * first real UPDATE round-trip. The methods pushed (WINDOW_SET_WINDOW_FORMAT_
 * USAGE_BOUNDS) are arm-state config that does nothing without an UPDATE, so
 * the channel state is left untouched.
 */
static int
nvkm_gsp_disp_core_push_smoke(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	volatile uint32_t *pb = disp->core_push_kva;
	uint32_t cap = disp->core_push_size / 4;	/* pushbuffer in dwords */
	uint32_t base, put_dw, get, i, n;
	int us;

	/* Start where the channel will next fetch (fresh channel: 0). Keep the
	 * smoke entirely within the buffer; do not handle wrap here. */
	base = nvkm_rd32(sc, disp->core_put_reg);
	if (base + 32u >= cap) {
		nvkm_infof(sc->dev,
		    "gsp_disp: M4a core push skipped -- PUT=%u near wrap (cap=%u)\n",
		    base, cap);
		return (0);
	}

	n = base;
	for (i = 0; i < 8; i++) {
		pb[n++] = evo_method_hdr(
		    NVC57D_WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(i), 1);
		pb[n++] = EVO_FORMAT_USAGE_RGB_PACKED_ALL;
	}

	cpu_sfence();	/* coherent sysmem stores visible before the PUT write */

	put_dw = n;	/* PUT/GET are dword offsets into the pushbuffer */
	nvkm_wr32(sc, disp->core_put_reg, put_dw);

	/* Poll GET until it reaches PUT (methods fetched) or ~100ms timeout. */
	get = base;
	for (us = 0; us < 100000; us += 10) {
		get = nvkm_rd32(sc, disp->core_put_reg + 4);
		if (get == put_dw)
			break;
		DELAY(10);
	}

	nvkm_infof(sc->dev,
	    "gsp_disp: M4a core push: %u methods [%u..%u) PUT=%u GET=%u -> %s\n",
	    (n - base) / 2u, base, put_dw, put_dw, get,
	    get == put_dw ? "consumed (GET caught PUT)" : "STUCK (GET != PUT)");

	return (get == put_dw ? 0 : ETIMEDOUT);
}

/* ===== EDID decode (just enough to prove we read the real monitor) ===== */

static void
nvkm_gsp_disp_edid_dump(struct nvkm_softc *sc, uint32_t display_id,
    const uint8_t *e, uint32_t len)
{
	static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	char mfg[4];
	uint16_t prod;
	uint32_t serial;

	if (len < 128 || memcmp(e, hdr, 8) != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: displayId=0x%x EDID len=%u (no valid header)\n",
		    display_id, len);
		return;
	}

	/* Manufacturer PNP ID: bytes 8-9, big-endian, 3 x 5-bit letters. */
	mfg[0] = (char)(((e[8] >> 2) & 0x1f) + 'A' - 1);
	mfg[1] = (char)((((e[8] & 0x3) << 3) | (e[9] >> 5)) + 'A' - 1);
	mfg[2] = (char)((e[9] & 0x1f) + 'A' - 1);
	mfg[3] = '\0';
	prod = (uint16_t)(e[10] | (e[11] << 8));
	serial = (uint32_t)(e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24));

	nvkm_infof(sc->dev,
	    "gsp_disp: displayId=0x%x EDID %u bytes: mfg=%s product=0x%04x "
	    "serial=0x%08x week=%u year=%u ver=%u.%u\n",
	    display_id, len, mfg, prod, serial, e[16], 1990u + e[17],
	    e[18], e[19]);

	/* First detailed timing descriptor (bytes 54..): pixel clock + active. */
	if (e[54] || e[55]) {
		uint32_t pclk10k = (uint32_t)(e[54] | (e[55] << 8)); /* in 10 kHz */
		uint32_t hact = e[56] | ((uint32_t)(e[58] & 0xf0) << 4);
		uint32_t vact = e[59] | ((uint32_t)(e[61] & 0xf0) << 4);
		nvkm_infof(sc->dev,
		    "gsp_disp:   preferred timing %ux%u pixclk=%u kHz\n",
		    hact, vact, pclk10k * 10u);
	}
}

/* ===== internal subdevice helper (privileged NV2080_CTRL_INTERNAL_* ctrls) ===== */

static void
nvkm_gsp_disp_internal_subdev(struct nvkm_softc *sc,
    struct nvkm_gsp_client *tmp_client, struct nvkm_gsp_object *tmp_subdev)
{
	memset(tmp_client, 0, sizeof(*tmp_client));
	tmp_client->sc = sc;
	tmp_client->object.client = tmp_client;
	tmp_client->object.handle = sc->gsp_internal_client;
	tmp_subdev->client = tmp_client;
	tmp_subdev->parent = NULL;
	tmp_subdev->handle = sc->gsp_internal_subdevice;
}

/* Is the given GSP displayId currently connected? Blockable context only. */
int
nvkm_gsp_disp_connected(struct nvkm_softc *sc, uint32_t display_id)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_get_connect_state_params *cs;
	void *p;
	int err, connected;

	if (disp == NULL)
		return (-1);
	cs = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, sizeof(*cs));
	if (cs == NULL)
		return (-1);
	memset(cs, 0, sizeof(*cs));
	cs->displayMask = display_id;
	p = cs;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*cs));
	if (err != 0 || p == NULL)
		return (-1);
	connected = (((struct disp_get_connect_state_params *)p)->displayMask &
	    display_id) ? 1 : 0;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
	return (connected);
}

/* Read EDID for a displayId into out (capacity *outlen); sets *outlen to the
 * actual length. Blockable context only. Returns 0 on success. */
int
nvkm_gsp_disp_read_edid(struct nvkm_softc *sc, uint32_t display_id,
    uint8_t *out, uint32_t *outlen)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_get_edid_v2_params *ed;
	void *p;
	int err;

	if (disp == NULL)
		return (ENXIO);
	ed = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2, sizeof(*ed));
	if (ed == NULL)
		return (ENOMEM);
	memset(ed, 0, sizeof(*ed));
	ed->displayId = display_id;
	p = ed;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*ed));
	if (err != 0 || p == NULL)
		return (err != 0 ? err : EIO);
	ed = p;
	if (ed->bufferSize == 0 || ed->bufferSize > *outlen) {
		nvkm_gsp_rm_ctrl_done(&disp->objcom, ed);
		return (EINVAL);
	}
	memcpy(out, ed->edidBuffer, ed->bufferSize);
	*outlen = ed->bufferSize;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, ed);
	return (0);
}

/* ===== connected-output probe: read + print EDID of every connected output =====
 * Shared by the attach-time initial probe (M1a) and the hotplug worker (M1b).
 * Caller context must be blockable (issues synchronous GSP RPCs). */
void
nvkm_gsp_disp_probe_connected(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_get_supported_params *sup;
	uint32_t supported_mask;
	void *p;
	int err, id;

	if (disp == NULL)
		return;

	/* Which displayIds exist. */
	sup = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, sizeof(*sup));
	if (sup == NULL)
		return;
	memset(sup, 0, sizeof(*sup));
	p = sup;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*sup));
	if (err != 0 || p == NULL) {
		nvkm_infof(sc->dev, "gsp_disp: GET_SUPPORTED err=%d\n", err);
		return;
	}
	supported_mask = ((struct disp_get_supported_params *)p)->displayMask;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
	disp->supported_mask = supported_mask;
	nvkm_infof(sc->dev, "gsp_disp: supported displayId mask=0x%08x\n",
	    supported_mask);

	/* For each supported displayId: connected? then read EDID. */
	for (id = 0; id < 32; id++) {
		struct disp_get_connect_state_params *cs;
		struct disp_get_edid_v2_params *ed;
		uint32_t connected;

		if (!(supported_mask & (1u << id)))
			continue;

		cs = nvkm_gsp_rm_ctrl_get(&disp->objcom,
		    NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, sizeof(*cs));
		if (cs == NULL)
			return;
		memset(cs, 0, sizeof(*cs));
		cs->displayMask = (1u << id);
		p = cs;
		err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*cs));
		if (err != 0 || p == NULL) {
			nvkm_infof(sc->dev,
			    "gsp_disp: displayId=0x%x CONNECT_STATE err=%d\n",
			    1u << id, err);
			continue;
		}
		connected = ((struct disp_get_connect_state_params *)p)->displayMask
		    & (1u << id);
		nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
		if (!connected)
			continue;

		ed = nvkm_gsp_rm_ctrl_get(&disp->objcom,
		    NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2, sizeof(*ed));
		if (ed == NULL)
			return;
		memset(ed, 0, sizeof(*ed));
		ed->displayId = (1u << id);
		p = ed;
		err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*ed));
		if (err != 0 || p == NULL) {
			nvkm_infof(sc->dev,
			    "gsp_disp: displayId=0x%x GET_EDID_V2 err=%d\n",
			    1u << id, err);
			continue;
		}
		ed = p;
		nvkm_gsp_disp_edid_dump(sc, 1u << id, ed->edidBuffer,
		    ed->bufferSize);
		nvkm_gsp_rm_ctrl_done(&disp->objcom, ed);
	}
}

/* ===== hotplug event (M1b) ===== */

/* Background-lwkt worker: re-probe connected outputs + log EDID. Enqueued by
 * the GSP ithread on each hotplug POST_EVENT (which must not block). */
static void
nvkm_gsp_disp_hotplug_task(void *ctx, int pending)
{
	struct nvkm_softc *sc = ctx;

	(void)pending;
	nvkm_infof(sc->dev, "gsp_disp: hotplug event -> re-probing outputs\n");
	nvkm_gsp_disp_probe_connected(sc);
}

/* Register for GSP hotplug notifications. The event arrives via POST_EVENT;
 * nvkm_gsp_evt_post_event matches hpd_event_handle and enqueues hpd_task. */
static int
nvkm_gsp_disp_register_hotplug(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_nv0005_alloc_params *args;
	struct disp_event_set_notification_params *ctrl;
	uint32_t handle;
	int err;

	TASK_INIT(&disp->hpd_task, 0, nvkm_gsp_disp_hotplug_task, sc);

	handle = nvkm_gsp_client_child_handle(&disp->client, NVKM_RM_DISP_HPD);
	memset(&disp->hpd_event, 0, sizeof(disp->hpd_event));
	args = nvkm_gsp_rm_alloc_get(&disp->device.subdevice, handle,
	    NV01_EVENT_KERNEL_CALLBACK_EX, sizeof(*args), &disp->hpd_event);
	if (args == NULL)
		return (ENOMEM);
	args->hParentClient = disp->client.object.handle;
	args->hSrcResource = 0;
	args->hClass = NV01_EVENT_KERNEL_CALLBACK_EX;
	args->notifyIndex = NV2080_NOTIFIERS_HOTPLUG;
	args->data = handle;
	err = nvkm_gsp_rm_alloc_wr(&disp->hpd_event, args);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: hotplug event alloc err=%d\n", err);
		return (err);
	}

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->device.subdevice,
	    NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION, sizeof(*ctrl));
	if (ctrl == NULL)
		return (ENOMEM);
	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->event = NV2080_NOTIFIERS_HOTPLUG;
	ctrl->action = NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT;
	err = nvkm_gsp_rm_ctrl_wr(&disp->device.subdevice, ctrl);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: hotplug SET_NOTIFICATION err=%d\n", err);
		return (err);
	}

	disp->hpd_event_handle = handle;
	nvkm_infof(sc->dev,
	    "gsp_disp: hotplug event registered (handle=0x%x)\n", handle);
	return (0);
}

/* ===== core display channel (M2a) =====
 * Allocate the TU102_DISP display root + the NVC57D core channel with a
 * coherent-sysmem pushbuffer. This is the channel modeset (M4) pushes EVO
 * methods to. M2a stops at allocation (no method push / PUT yet -> M2b). */
static int
nvkm_gsp_disp_core_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct disp_channel_pushbuffer_params *pb;
	struct disp_channeldma_alloc_params *ca;
	void *args;
	uint32_t handle;
	int err;

	/* (1) TU102_DISP display root (parent of all disp channels). */
	handle = nvkm_gsp_client_child_handle(&disp->client, TU102_DISP << 16);
	args = nvkm_gsp_rm_alloc_get(&disp->device.object, handle, TU102_DISP, 0,
	    &disp->dispclass);
	if (args == NULL)
		return (ENOMEM);
	err = nvkm_gsp_rm_alloc_wr(&disp->dispclass, args);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: TU102_DISP root alloc (handle=0x%x) err=%d\n",
		    disp->dispclass.handle, err);
		return (err);
	}

	/* (2) Core pushbuffer: 4KB coherent sysmem (Turing PB default). */
	disp->core_push_size = 0x1000;
	disp->core_push_kva = contigmalloc(disp->core_push_size, M_NVKM_DISP,
	    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
	if (disp->core_push_kva == NULL)
		return (ENOMEM);
	disp->core_push_paddr = vtophys(disp->core_push_kva);

	/* (3) Tell GSP the core channel's pushbuffer (on internal subdevice). */
	nvkm_gsp_disp_internal_subdev(sc, &tmp_client, &tmp_subdev);
	pb = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER, sizeof(*pb));
	if (pb == NULL)
		return (ENOMEM);
	memset(pb, 0, sizeof(*pb));
	pb->addressSpace = DISP_ADDR_SYSMEM;
	pb->cacheSnoop = 1;
	pb->pbTargetAperture = DISP_PHYS_PCI_COHERENT;
	pb->physicalAddr = disp->core_push_paddr;
	pb->limit = disp->core_push_size - 1;
	pb->hclass = TU102_DISP_CORE_CHANNEL_DMA;
	pb->channelInstance = 0;
	pb->valid = 1;
	pb->subDeviceId = 1u;		/* BIT(0) */
	err = nvkm_gsp_rm_ctrl_wr(&tmp_subdev, pb);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: core set_pushbuf err=%d\n", err);
		return (err);
	}

	/* (4) Allocate the NVC57D core channel under the display root. */
	handle = nvkm_gsp_client_child_handle(&disp->client, NVKM_RM_DISP_CORE);
	ca = nvkm_gsp_rm_alloc_get(&disp->dispclass, handle,
	    TU102_DISP_CORE_CHANNEL_DMA, sizeof(*ca), &disp->core);
	if (ca == NULL)
		return (ENOMEM);
	memset(ca, 0, sizeof(*ca));
	ca->channelInstance = 0;
	ca->offset = 0;
	ca->subDeviceId = 1u;		/* BIT(0) */
	err = nvkm_gsp_rm_alloc_wr(&disp->core, ca);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: NVC57D core alloc (handle=0x%x) err=%d\n",
		    disp->core.handle, err);
		return (err);
	}

	/* (M2b) The NVC57D core channel's control registers live in BAR0 MMIO
	 * at 0x680000 (PUT, NV507C_PUT=0x0) and 0x680004 (GET, NV507C_GET=0x4).
	 * GSP-RM did the HW init during dmac_alloc; read PUT/GET to confirm the
	 * channel is a real, accessible HW channel at a sane initial state. */
	disp->core_put_reg = 0x680000;
	{
		uint32_t put = nvkm_rd32(sc, disp->core_put_reg);
		uint32_t get = nvkm_rd32(sc, disp->core_put_reg + 4);
		nvkm_infof(sc->dev,
		    "gsp_disp: core channel up -- root=0x%x core=0x%x pb@0x%llx "
		    "PUT=0x%x GET=0x%x\n",
		    disp->dispclass.handle, disp->core.handle,
		    (unsigned long long)disp->core_push_paddr, put, get);
	}

	/* (M4a) Prove the EVO method-push path: encode methods, kick PUT, and
	 * confirm the core channel consumes them (GET catches PUT). */
	(void)nvkm_gsp_disp_core_push_smoke(sc);

	return (0);
}

/* ===== display subsystem bring-up (attach thread) ===== */

int
nvkm_gsp_disp_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp;
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct disp_write_inst_mem_params *wim;
	struct disp_get_static_info_params *si;
	struct disp_get_num_heads_params *nh;
	struct disp_get_all_head_mask_params *hm;
	void *p, *args;
	uint32_t handle;
	int err;

	if (!sc->gsp_running || sc->gsp_internal_subdevice == 0)
		return (ENXIO);

	disp = kzalloc(sizeof(*disp), GFP_KERNEL);
	if (disp == NULL)
		return (ENOMEM);
	disp->sc = sc;

	/* (1) RAMIN: 0x10000 VRAM, handed to GSP as the display instance mem. */
	disp->inst_paddr = nvkm_gsp_vram_alloc(sc, 0x10000, 0x10000);
	if (disp->inst_paddr == 0) {
		nvkm_infof(sc->dev, "gsp_disp: RAMIN VRAM alloc failed\n");
		err = ENOMEM;
		goto fail_free;
	}

	/* (2) WRITE_INST_MEM on the internal subdevice. */
	nvkm_gsp_disp_internal_subdev(sc, &tmp_client, &tmp_subdev);
	wim = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM, sizeof(*wim));
	if (wim == NULL) { err = ENOMEM; goto fail_free; }
	wim->instMemPhysAddr = disp->inst_paddr;
	wim->instMemSize = 0x10000;
	wim->instMemAddrSpace = ADDR_FBMEM;
	wim->instMemCpuCacheAttr = NV_MEMORY_WRITECOMBINED;
	err = nvkm_gsp_rm_ctrl_wr(&tmp_subdev, wim);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: WRITE_INST_MEM err=%d\n", err);
		goto fail_free;
	}

	/* (3) Display client+device, then NV04_DISPLAY_COMMON (objcom). */
	err = nvkm_gsp_client_ctor(sc, 0xd1590042u, &disp->client);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: client ctor err=%d\n", err);
		goto fail_free;
	}
	err = nvkm_gsp_device_ctor(&disp->client, &disp->device);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: device ctor err=%d\n", err);
		goto fail_client;
	}
	handle = nvkm_gsp_client_child_handle(&disp->client, NVKM_RM_DISP);
	args = nvkm_gsp_rm_alloc_get(&disp->device.object, handle,
	    NV04_DISPLAY_COMMON, 0, &disp->objcom);
	if (args == NULL) {
		nvkm_infof(sc->dev, "gsp_disp: NV04_DISPLAY_COMMON get failed\n");
		err = ENOMEM;
		goto fail_device;
	}
	err = nvkm_gsp_rm_alloc_wr(&disp->objcom, args);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: NV04_DISPLAY_COMMON alloc (handle=0x%x) err=%d\n",
		    disp->objcom.handle, err);
		goto fail_device;
	}

	/* (4) Topology (informational; also validates objcom works). */
	si = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO, sizeof(*si));
	if (si != NULL) {
		memset(si, 0, sizeof(*si));
		p = si;
		if (nvkm_gsp_rm_ctrl_rd(&tmp_subdev, &p, sizeof(*si)) == 0 &&
		    p != NULL) {
			disp->window_mask =
			    ((struct disp_get_static_info_params *)p)->windowPresentMask;
			nvkm_gsp_rm_ctrl_done(&tmp_subdev, p);
		}
	}
	nh = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS, sizeof(*nh));
	if (nh != NULL) {
		memset(nh, 0, sizeof(*nh));
		p = nh;
		if (nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*nh)) == 0 &&
		    p != NULL) {
			disp->num_heads =
			    ((struct disp_get_num_heads_params *)p)->numHeads;
			nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
		}
	}
	hm = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK, sizeof(*hm));
	if (hm != NULL) {
		memset(hm, 0, sizeof(*hm));
		p = hm;
		if (nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*hm)) == 0 &&
		    p != NULL) {
			disp->head_mask =
			    ((struct disp_get_all_head_mask_params *)p)->headMask;
			nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
		}
	}

	sc->gsp_disp = disp;
	nvkm_infof(sc->dev,
	    "gsp_disp: ready -- inst@0x%llx heads=%u headMask=0x%x windowMask=0x%x\n",
	    (unsigned long long)disp->inst_paddr, disp->num_heads,
	    disp->head_mask, disp->window_mask);

	/* (5) Register for runtime hotplug events (deferred to a worker lwkt). */
	(void)nvkm_gsp_disp_register_hotplug(sc);

	/* (6) Initial probe: read+print EDID of already-connected outputs.
	 * Inline on the attach thread (single-threaded bring-up); runtime
	 * re-probes use the same probe_connected() from the hotplug worker. */
	nvkm_gsp_disp_probe_connected(sc);

	/* (7) Core display channel (M2a). Best-effort; EDID still useful if it
	 * fails. The channel is what modeset (M4) will push EVO methods to. */
	(void)nvkm_gsp_disp_core_init(sc);
	return (0);

fail_device:
	nvkm_gsp_device_dtor(&disp->device);
fail_client:
	nvkm_gsp_client_dtor(&disp->client);
fail_free:
	kfree(disp);
	return (err);
}
