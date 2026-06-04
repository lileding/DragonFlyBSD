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
#include <linux/slab.h>

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"

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

	/* (5) Initial probe: read+print EDID of already-connected outputs. */
	nvkm_gsp_disp_probe_connected(sc);
	return (0);

fail_device:
	nvkm_gsp_device_dtor(&disp->device);
fail_client:
	nvkm_gsp_client_dtor(&disp->client);
fail_free:
	kfree(disp);
	return (err);
}
