/*
 * Copyright 2023 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "nvkm_priv.h"

#include <engine/disp/priv.h>
#include <engine/disp/chan.h>
#include <engine/disp/conn.h>
#include <engine/disp/dp.h>
#include <engine/disp/head.h>
#include <engine/disp/ior.h>
#include <engine/disp/outp.h>

#include <core/ramht.h>
#include <subdev/bios.h>
#include <subdev/bios/conn.h>
#include <subdev/gsp.h>
#include <subdev/mmu.h>
#include <subdev/vfn.h>

#include "nvkm_gsp_gpu.h"

#include <nvif/class.h>
#include <nvhw/drf.h>

#include "nvkm_gsp_disp.h"

#ifndef NVKM_DFLY_GSP_DISPLAY_ONLY
static u64
r535_chan_user(struct nvkm_disp_chan *chan, u64 *psize)
{
	switch (chan->object.oclass & 0xff) {
	case 0x7d: *psize = 0x10000; return 0x680000;
	case 0x7e: *psize = 0x01000; return 0x690000 + (chan->head * *psize);
	case 0x7b: *psize = 0x01000; return 0x6b0000 + (chan->head * *psize);
	case 0x7a: *psize = 0x01000; return 0x6d8000 + (chan->head * *psize);
	default:
		BUG_ON(1);
		break;
	}

	return 0ULL;
}

static void
r535_chan_intr(struct nvkm_disp_chan *chan, bool en)
{
}

static void
r535_chan_fini(struct nvkm_disp_chan *chan)
{
	nvkm_gsp_rm_free(&chan->rm.object);
}
#endif

static int
r535_disp_chan_set_pushbuf(struct nvkm_disp *disp, s32 oclass, int inst, struct nvkm_memory *memory)
{
	struct nvkm_gsp *gsp = disp->rm.objcom.client->gsp;
	NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice,
				    NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER,
				    sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	if (memory) {
		switch (nvkm_memory_target(memory)) {
		case NVKM_MEM_TARGET_NCOH:
			ctrl->addressSpace = ADDR_SYSMEM;
			ctrl->cacheSnoop = 0;
			ctrl->pbTargetAperture = PHYS_PCI;
			break;
		case NVKM_MEM_TARGET_HOST:
			ctrl->addressSpace = ADDR_SYSMEM;
			ctrl->cacheSnoop = 1;
			ctrl->pbTargetAperture = PHYS_PCI_COHERENT;
			break;
		case NVKM_MEM_TARGET_VRAM:
			ctrl->addressSpace = ADDR_FBMEM;
			ctrl->pbTargetAperture = PHYS_NVM;
			break;
		default:
			WARN_ON(1);
			return -EINVAL;
		}

		ctrl->physicalAddr = nvkm_memory_addr(memory);
		ctrl->limit = nvkm_memory_size(memory) - 1;
	}

	ctrl->hclass = oclass;
	ctrl->channelInstance = inst;
	ctrl->valid = ((oclass & 0xff) != 0x7a) ? 1 : 0;
#ifndef NVKM_DFLY_GSP_DISPLAY_ONLY
	ctrl->channelPBSize = PB_SIZE_4KB;
#else
	/*
	 * Linux v7.0 r570 display RM set_pushbuf does not set channelPBSize.
	 * Keep the first-light GSP path on the 570 ABI shape.
	 */
#endif
	ctrl->subDeviceId = BIT(0);

	return nvkm_gsp_rm_ctrl_wr(&gsp->internal.device.subdevice, ctrl);
}

#ifndef NVKM_DFLY_GSP_DISPLAY_ONLY
static int
r535_curs_init(struct nvkm_disp_chan *chan)
{
	const struct nvkm_rm_api *rmapi = chan->disp->rm.objcom.client->gsp->rm->api;
	NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS *args;
	int ret;

	ret = rmapi->disp->chan.set_pushbuf(chan->disp, chan->object.oclass, chan->head, NULL);
	if (ret)
		return ret;

	args = nvkm_gsp_rm_alloc_get(&chan->disp->rm.object,
				     (chan->object.oclass << 16) | chan->head,
				     chan->object.oclass, sizeof(*args), &chan->rm.object);
	if (IS_ERR(args))
		return PTR_ERR(args);

	args->channelInstance = chan->head;

	return nvkm_gsp_rm_alloc_wr(&chan->rm.object, args);
}

static const struct nvkm_disp_chan_func
r535_curs_func = {
	.init = r535_curs_init,
	.fini = r535_chan_fini,
	.intr = r535_chan_intr,
	.user = r535_chan_user,
};

static const struct nvkm_disp_chan_user
r535_curs = {
	.func = &r535_curs_func,
	.user = 73,
};
#endif

static int
r535_dmac_bind(struct nvkm_disp_chan *chan, struct nvkm_object *object, u32 handle)
{
	return nvkm_ramht_insert(chan->disp->ramht, object, chan->chid.user, -9, handle,
				 chan->chid.user << 25 |
				 (chan->disp->rm.client.object.handle & 0x3fff));
}

#ifndef NVKM_DFLY_GSP_DISPLAY_ONLY
static void
r535_dmac_fini(struct nvkm_disp_chan *chan)
{
	struct nvkm_device *device = chan->disp->engine.subdev.device;
	const u32 uoff = (chan->chid.user - 1) * 0x1000;

	chan->suspend_put = nvkm_rd32(device, 0x690000 + uoff);
	r535_chan_fini(chan);
}
#endif

static int
r535_dmac_alloc(struct nvkm_disp *disp, u32 oclass, int inst, u32 put_offset,
		struct nvkm_gsp_object *dmac)
{
	NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS *args;

	args = nvkm_gsp_rm_alloc_get(&disp->rm.object, (oclass << 16) | inst, oclass,
				     sizeof(*args), dmac);
	if (IS_ERR(args))
		return PTR_ERR(args);

	args->channelInstance = inst;
	args->offset = put_offset;
#ifndef NVKM_DFLY_GSP_DISPLAY_ONLY
	args->channelPBSize = PB_SIZE_4KB;
#else
	/*
	 * Linux v7.0 r570 display RM allocation omits channelPBSize.
	 * Keep the DragonFly first-light path aligned with the running
	 * 570.144 firmware ABI instead of the older r535 allocation shape.
	 */
#endif
	args->subDeviceId = BIT(0);

	return nvkm_gsp_rm_alloc_wr(dmac, args);
}

#ifndef NVKM_DFLY_GSP_DISPLAY_ONLY
static int
r535_dmac_init(struct nvkm_disp_chan *chan)
{
	const struct nvkm_rm_api *rmapi = chan->disp->rm.objcom.client->gsp->rm->api;
	int ret;

	ret = rmapi->disp->chan.set_pushbuf(chan->disp, chan->object.oclass, chan->head, chan->memory);
	if (ret)
		return ret;

	return rmapi->disp->chan.dmac_alloc(chan->disp, chan->object.oclass, chan->head,
					    chan->suspend_put, &chan->rm.object);
}

static int
r535_dmac_push(struct nvkm_disp_chan *chan, u64 memory)
{
	chan->memory = nvkm_umem_search(chan->object.client, memory);
	if (IS_ERR(chan->memory))
		return PTR_ERR(chan->memory);

	return 0;
}

static const struct nvkm_disp_chan_func
r535_dmac_func = {
	.push = r535_dmac_push,
	.init = r535_dmac_init,
	.fini = r535_dmac_fini,
	.intr = r535_chan_intr,
	.user = r535_chan_user,
	.bind = r535_dmac_bind,
};

static const struct nvkm_disp_chan_func
r535_wimm_func = {
	.push = r535_dmac_push,
	.init = r535_dmac_init,
	.fini = r535_dmac_fini,
	.intr = r535_chan_intr,
	.user = r535_chan_user,
};

static const struct nvkm_disp_chan_user
r535_wimm = {
	.func = &r535_wimm_func,
	.user = 33,
};

static const struct nvkm_disp_chan_user
r535_wndw = {
	.func = &r535_dmac_func,
	.user = 1,
};

static void
r535_core_fini(struct nvkm_disp_chan *chan)
{
	struct nvkm_device *device = chan->disp->engine.subdev.device;

	chan->suspend_put = nvkm_rd32(device, 0x680000);
	r535_chan_fini(chan);
}

static const struct nvkm_disp_chan_func
r535_core_func = {
	.push = r535_dmac_push,
	.init = r535_dmac_init,
	.fini = r535_core_fini,
	.intr = r535_chan_intr,
	.user = r535_chan_user,
	.bind = r535_dmac_bind,
};

static const struct nvkm_disp_chan_user
r535_core = {
	.func = &r535_core_func,
	.user = 0,
};
#endif

static int
r535_bl_ctrl(struct nvkm_disp *disp, unsigned display_id, bool set, int *pval)
{
	u32 cmd = set ? NV0073_CTRL_CMD_SPECIFIC_SET_BACKLIGHT_BRIGHTNESS :
			NV0073_CTRL_CMD_SPECIFIC_GET_BACKLIGHT_BRIGHTNESS;
	NV0073_CTRL_SPECIFIC_BACKLIGHT_BRIGHTNESS_PARAMS *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom, cmd, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->displayId = BIT(display_id);
	ctrl->brightness = *pval;

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret)
		return ret;

	*pval = ctrl->brightness;

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

static int
r535_sor_bl_set(struct nvkm_ior *sor, int lvl)
{
	struct nvkm_disp *disp = sor->disp;
	const struct nvkm_rm_api *rmapi = disp->engine.subdev.device->gsp->rm->api;

	return rmapi->disp->bl_ctrl(disp, sor->asy.outp->index, true, &lvl);
}

static int
r535_sor_bl_get(struct nvkm_ior *sor)
{
	struct nvkm_disp *disp = sor->disp;
	const struct nvkm_rm_api *rmapi = disp->engine.subdev.device->gsp->rm->api;
	int lvl, ret = rmapi->disp->bl_ctrl(disp, sor->asy.outp->index, false, &lvl);

	return (ret == 0) ? lvl : ret;
}

static const struct nvkm_ior_func_bl
r535_sor_bl = {
	.get = r535_sor_bl_get,
	.set = r535_sor_bl_set,
};

static void
r535_sor_hda_eld(struct nvkm_ior *sor, int head, u8 *data, u8 size)
{
	struct nvkm_disp *disp = sor->disp;
	NV0073_CTRL_DFP_SET_ELD_AUDIO_CAP_PARAMS *ctrl;

	if (WARN_ON(size > sizeof(ctrl->bufferELD)))
		return;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DFP_SET_ELD_AUDIO_CAPS, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->displayId = BIT(sor->asy.outp->index);
	ctrl->numELDSize = size;
	memcpy(ctrl->bufferELD, data, size);
	ctrl->maxFreqSupported = 0; //XXX
	ctrl->ctrl  = NVDEF(NV0073, CTRL_DFP_ELD_AUDIO_CAPS_CTRL, PD, TRUE);
	ctrl->ctrl |= NVDEF(NV0073, CTRL_DFP_ELD_AUDIO_CAPS_CTRL, ELDV, TRUE);
	ctrl->deviceEntry = head;

	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));
}

static void
r535_sor_hda_hpd(struct nvkm_ior *sor, int head, bool present)
{
	struct nvkm_disp *disp = sor->disp;
	NV0073_CTRL_DFP_SET_ELD_AUDIO_CAP_PARAMS *ctrl;

	if (present)
		return;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DFP_SET_ELD_AUDIO_CAPS, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->displayId = BIT(sor->asy.outp->index);
	ctrl->deviceEntry = head;

	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));
}

static const struct nvkm_ior_func_hda
r535_sor_hda = {
	.hpd = r535_sor_hda_hpd,
	.eld = r535_sor_hda_eld,
};

static void
r535_sor_dp_audio_mute(struct nvkm_ior *sor, bool mute)
{
	struct nvkm_disp *disp = sor->disp;
	NV0073_CTRL_DP_SET_AUDIO_MUTESTREAM_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_SET_AUDIO_MUTESTREAM, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->displayId = BIT(sor->asy.outp->index);
	ctrl->mute = mute;
	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));
}

static void
r535_sor_dp_audio(struct nvkm_ior *sor, int head, bool enable)
{
	struct nvkm_disp *disp = sor->disp;
	NV0073_CTRL_DFP_SET_AUDIO_ENABLE_PARAMS *ctrl;

	if (!enable)
		r535_sor_dp_audio_mute(sor, true);

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DFP_SET_AUDIO_ENABLE, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->displayId = BIT(sor->asy.outp->index);
	ctrl->enable = enable;
	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));

	if (enable)
		r535_sor_dp_audio_mute(sor, false);
}

static void
r535_sor_dp_vcpi(struct nvkm_ior *sor, int head, u8 slot, u8 slot_nr, u16 pbn, u16 aligned_pbn)
{
	struct nvkm_disp *disp = sor->disp;
	struct NV0073_CTRL_CMD_DP_CONFIG_STREAM_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_CONFIG_STREAM, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->subDeviceInstance = 0;
	ctrl->head = head;
	ctrl->sorIndex = sor->id;
	ctrl->dpLink = sor->asy.link == 2;
	ctrl->bEnableOverride = 1;
	ctrl->bMST = 1;
	ctrl->hBlankSym = 0;
	ctrl->vBlankSym = 0;
	ctrl->colorFormat = 0;
	ctrl->bEnableTwoHeadOneOr = 0;
	ctrl->singleHeadMultistreamMode = 0;
	ctrl->MST.slotStart = slot;
	ctrl->MST.slotEnd = slot + slot_nr - 1;
	ctrl->MST.PBN = pbn;
	ctrl->MST.Timeslice = aligned_pbn;
	ctrl->MST.sendACT = 0;
	ctrl->MST.singleHeadMSTPipeline = 0;
	ctrl->MST.bEnableAudioOverRightPanel = 0;
	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));
}

static int
r535_sor_dp_sst(struct nvkm_ior *sor, int head, bool ef,
		u32 watermark, u32 hblanksym, u32 vblanksym)
{
	struct nvkm_disp *disp = sor->disp;
	struct NV0073_CTRL_CMD_DP_CONFIG_STREAM_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_CONFIG_STREAM, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->head = head;
	ctrl->sorIndex = sor->id;
	ctrl->dpLink = sor->asy.link == 2;
	ctrl->bEnableOverride = 1;
	ctrl->bMST = 0;
	ctrl->hBlankSym = hblanksym;
	ctrl->vBlankSym = vblanksym;
	ctrl->colorFormat = 0;
	ctrl->bEnableTwoHeadOneOr = 0;
	ctrl->SST.bEnhancedFraming = ef;
	ctrl->SST.tuSize = 64;
	ctrl->SST.waterMark = watermark;
	ctrl->SST.bEnableAudioOverRightPanel = 0;
	return nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl);
}

static const struct nvkm_ior_func_dp
r535_sor_dp = {
	.sst = r535_sor_dp_sst,
	.vcpi = r535_sor_dp_vcpi,
	.audio = r535_sor_dp_audio,
};

static void
r535_sor_hdmi_scdc(struct nvkm_ior *sor, u32 khz, bool support, bool scrambling,
		   bool scrambling_low_rates)
{
	struct nvkm_outp *outp = sor->asy.outp;
	struct nvkm_disp *disp = outp->disp;
	NV0073_CTRL_SPECIFIC_SET_HDMI_SINK_CAPS_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_SINK_CAPS, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->displayId = BIT(outp->index);
	ctrl->caps = 0;
	if (support)
		ctrl->caps |= NVDEF(NV0073_CTRL_CMD_SPECIFIC, SET_HDMI_SINK_CAPS, SCDC_SUPPORTED, TRUE);
	if (scrambling)
		ctrl->caps |= NVDEF(NV0073_CTRL_CMD_SPECIFIC, SET_HDMI_SINK_CAPS, GT_340MHZ_CLOCK_SUPPORTED, TRUE);
	if (scrambling_low_rates)
		ctrl->caps |= NVDEF(NV0073_CTRL_CMD_SPECIFIC, SET_HDMI_SINK_CAPS, LTE_340MHZ_SCRAMBLING_SUPPORTED, TRUE);

	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));
}

static void
r535_sor_hdmi_ctrl_audio_mute(struct nvkm_outp *outp, bool mute)
{
	struct nvkm_disp *disp = outp->disp;
	NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_AUDIO_MUTESTREAM_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_AUDIO_MUTESTREAM, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->displayId = BIT(outp->index);
	ctrl->mute = mute;
	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));
}

static void
r535_sor_hdmi_ctrl_audio(struct nvkm_outp *outp, bool enable)
{
	struct nvkm_disp *disp = outp->disp;
	NV0073_CTRL_SPECIFIC_SET_OD_PACKET_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SPECIFIC_SET_OD_PACKET, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->displayId = BIT(outp->index);
	ctrl->transmitControl =
		NVDEF(NV0073_CTRL_SPECIFIC, SET_OD_PACKET_TRANSMIT_CONTROL, ENABLE, YES) |
		NVDEF(NV0073_CTRL_SPECIFIC, SET_OD_PACKET_TRANSMIT_CONTROL, OTHER_FRAME, DISABLE) |
		NVDEF(NV0073_CTRL_SPECIFIC, SET_OD_PACKET_TRANSMIT_CONTROL, SINGLE_FRAME, DISABLE) |
		NVDEF(NV0073_CTRL_SPECIFIC, SET_OD_PACKET_TRANSMIT_CONTROL, ON_HBLANK, DISABLE) |
		NVDEF(NV0073_CTRL_SPECIFIC, SET_OD_PACKET_TRANSMIT_CONTROL, VIDEO_FMT, SW_CONTROLLED) |
		NVDEF(NV0073_CTRL_SPECIFIC, SET_OD_PACKET_TRANSMIT_CONTROL, RESERVED_LEGACY_MODE, NO);
	ctrl->packetSize = 10;
	ctrl->aPacket[0] = 0x03;
	ctrl->aPacket[1] = 0x00;
	ctrl->aPacket[2] = 0x00;
	ctrl->aPacket[3] = enable ? 0x10 : 0x01;
	ctrl->aPacket[4] = 0x00;
	ctrl->aPacket[5] = 0x00;
	ctrl->aPacket[6] = 0x00;
	ctrl->aPacket[7] = 0x00;
	ctrl->aPacket[8] = 0x00;
	ctrl->aPacket[9] = 0x00;
	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));
}

static void
r535_sor_hdmi_audio(struct nvkm_ior *sor, int head, bool enable)
{
	struct nvkm_device *device = sor->disp->engine.subdev.device;
	const u32 hdmi = head * 0x400;

	r535_sor_hdmi_ctrl_audio(sor->asy.outp, enable);
	r535_sor_hdmi_ctrl_audio_mute(sor->asy.outp, !enable);

	/* General Control (GCP). */
	nvkm_mask(device, 0x6f00c0 + hdmi, 0x00000001, 0x00000000);
	nvkm_wr32(device, 0x6f00cc + hdmi, !enable ? 0x00000001 : 0x00000010);
	nvkm_mask(device, 0x6f00c0 + hdmi, 0x00000001, 0x00000001);
}

static void
r535_sor_hdmi_ctrl(struct nvkm_ior *sor, int head, bool enable, u8 max_ac_packet, u8 rekey)
{
	struct nvkm_disp *disp = sor->disp;
	NV0073_CTRL_SPECIFIC_SET_HDMI_ENABLE_PARAMS *ctrl;

	if (!enable)
		return;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_ENABLE, sizeof(*ctrl));
	if (WARN_ON(IS_ERR(ctrl)))
		return;

	ctrl->displayId = BIT(sor->asy.outp->index);
	ctrl->enable = enable;

	WARN_ON(nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl));
}

static const struct nvkm_ior_func_hdmi
r535_sor_hdmi = {
	.ctrl = r535_sor_hdmi_ctrl,
	.scdc = r535_sor_hdmi_scdc,
	/* DragonFly KMS does not route HDMI infoframes through NVIF yet. */
	.infoframe_avi = NULL,
	.infoframe_vsi = NULL,
	.audio = r535_sor_hdmi_audio,
};

static const struct nvkm_ior_func
r535_sor = {
	.hdmi = &r535_sor_hdmi,
	.dp = &r535_sor_dp,
	.hda = &r535_sor_hda,
	.bl = &r535_sor_bl,
};

static int
r535_sor_new(struct nvkm_disp *disp, int id)
{
	return nvkm_ior_new_(&r535_sor, disp, SOR, id, true/*XXX: hda cap*/);
}

static int
r535_sor_cnt(struct nvkm_disp *disp, unsigned long *pmask)
{
	*pmask = 0xf;
	return 4;
}

static void
r535_head_vblank_put(struct nvkm_head *head)
{
	struct nvkm_device *device = head->disp->engine.subdev.device;

	nvkm_mask(device, 0x611d80 + (head->id * 4), 0x00000002, 0x00000000);
}

static void
r535_head_vblank_get(struct nvkm_head *head)
{
	struct nvkm_device *device = head->disp->engine.subdev.device;

	nvkm_wr32(device, 0x611800 + (head->id * 4), 0x00000002);
	nvkm_mask(device, 0x611d80 + (head->id * 4), 0x00000002, 0x00000002);
}

static void
r535_head_state(struct nvkm_head *head, struct nvkm_head_state *state)
{
}

static const struct nvkm_head_func
r535_head = {
	.state = r535_head_state,
	.vblank_get = r535_head_vblank_get,
	.vblank_put = r535_head_vblank_put,
};

static struct nvkm_conn *
r535_conn_new(struct nvkm_disp *disp, u32 id)
{
	NV0073_CTRL_SPECIFIC_GET_CONNECTOR_DATA_PARAMS *ctrl;
	struct nvbios_connE dcbE = {};
	struct nvkm_conn *conn;
	int ret, index;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return ERR_CAST(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayId = BIT(id);

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ERR_PTR(ret);
	}

	list_for_each_entry(conn, &disp->conns, head) {
		if (conn->index == ctrl->data[0].index) {
			nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
			return conn;
		}
	}

	dcbE.type = ctrl->data[0].type;
	index = ctrl->data[0].index;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);

	ret = nvkm_conn_new(disp, index, &dcbE, &conn);
	if (ret)
		return ERR_PTR(ret);

	list_add_tail(&conn->head, &disp->conns);
	return conn;
}

static void
r535_outp_release(struct nvkm_outp *outp)
{
	outp->disp->rm.assigned_sors &= ~BIT(outp->ior->id);
	outp->ior->asy.outp = NULL;
	outp->ior = NULL;
}

static int
r535_outp_acquire(struct nvkm_outp *outp, bool hda)
{
	struct nvkm_disp *disp = outp->disp;
	struct nvkm_ior *ior;
	NV0073_CTRL_DFP_ASSIGN_SOR_PARAMS *ctrl;
	int ret, or;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DFP_ASSIGN_SOR, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayId = BIT(outp->index);
	ctrl->sorExcludeMask = disp->rm.assigned_sors;
	if (hda)
		ctrl->flags |= NVDEF(NV0073_CTRL, DFP_ASSIGN_SOR_FLAGS, AUDIO, OPTIMAL);

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	for (or = 0; or < ARRAY_SIZE(ctrl->sorAssignListWithTag); or++) {
		if (ctrl->sorAssignListWithTag[or].displayMask & BIT(outp->index)) {
			disp->rm.assigned_sors |= BIT(or);
			break;
		}
	}

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);

	if (WARN_ON(or == ARRAY_SIZE(ctrl->sorAssignListWithTag)))
		return -EINVAL;

	ior = nvkm_ior_find(disp, SOR, or);
	if (WARN_ON(!ior))
		return -EINVAL;

	nvkm_outp_acquire_ior(outp, NVKM_OUTP_USER, ior);
	return 0;
}

static int
r535_disp_get_active(struct nvkm_disp *disp, unsigned head, u32 *displayid)
{
	NV0073_CTRL_SYSTEM_GET_ACTIVE_PARAMS *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->head = head;

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	*displayid = ctrl->displayId;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

static struct nvkm_ior *
r535_outp_inherit(struct nvkm_outp *outp)
{
	struct nvkm_disp *disp = outp->disp;
	struct nvkm_head *head;
	u32 displayid;
	int ret;

	list_for_each_entry(head, &disp->heads, head) {
		const struct nvkm_rm_api *rmapi = disp->rm.objcom.client->gsp->rm->api;

		ret = rmapi->disp->get_active(disp, head->id, &displayid);
		if (WARN_ON(ret))
			return NULL;

		if (displayid == BIT(outp->index)) {
			NV0073_CTRL_SPECIFIC_OR_GET_INFO_PARAMS *ctrl;
			u32 id, proto;
			struct nvkm_ior *ior;

			ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
						    NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO,
						    sizeof(*ctrl));
			if (IS_ERR(ctrl))
				return NULL;

			ctrl->subDeviceInstance = 0;
			ctrl->displayId = displayid;

			ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
			if (ret) {
				nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
				return NULL;
			}

			id = ctrl->index;
			proto = ctrl->protocol;
			nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);

			ior = nvkm_ior_find(disp, SOR, id);
			if (WARN_ON(!ior))
				return NULL;

			switch (proto) {
			case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_SINGLE_TMDS_A:
				ior->arm.proto = TMDS;
				ior->arm.link = 1;
				break;
			case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_SINGLE_TMDS_B:
				ior->arm.proto = TMDS;
				ior->arm.link = 2;
				break;
			case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DUAL_TMDS:
				ior->arm.proto = TMDS;
				ior->arm.link = 3;
				break;
			case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_A:
				ior->arm.proto = DP;
				ior->arm.link = 1;
				break;
			case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_B:
				ior->arm.proto = DP;
				ior->arm.link = 2;
				break;
			default:
				WARN_ON(1);
				return NULL;
			}

			ior->arm.proto_evo = proto;
			ior->arm.head = BIT(head->id);
			disp->rm.assigned_sors |= BIT(ior->id);
			return ior;
		}
	}

	return NULL;
}

static int
r535_outp_dfp_get_info(struct nvkm_outp *outp)
{
	NV0073_CTRL_DFP_GET_INFO_PARAMS *ctrl;
	struct nvkm_disp *disp = outp->disp;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom, NV0073_CTRL_CMD_DFP_GET_INFO, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->displayId = BIT(outp->index);

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	nvkm_debug(&disp->engine.subdev, "DFP %08x: flags:%08x flags2:%08x\n",
		   ctrl->displayId, ctrl->flags, ctrl->flags2);

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

static int
r535_disp_get_connect_state(struct nvkm_disp *disp, unsigned display_id)
{
	NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_PARAMS *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayMask = BIT(display_id);

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret == 0 && (ctrl->displayMask & BIT(display_id)))
		ret = 1;

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return ret;
}

static int
r535_outp_detect(struct nvkm_outp *outp)
{
	const struct nvkm_rm_api *rmapi = outp->disp->rm.objcom.client->gsp->rm->api;
	int ret;

	ret = rmapi->disp->get_connect_state(outp->disp, outp->index);
	if (ret == 1) {
		ret = r535_outp_dfp_get_info(outp);
		if (ret == 0)
			ret = 1;
	}

	return ret;
}

static int
r535_dp_mst_id_put(struct nvkm_outp *outp, u32 id)
{
	NV0073_CTRL_CMD_DP_TOPOLOGY_FREE_DISPLAYID_PARAMS *ctrl;
	struct nvkm_disp *disp = outp->disp;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_TOPOLOGY_FREE_DISPLAYID, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayId = id;
	return nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl);
}

static int
r535_dp_mst_id_get(struct nvkm_outp *outp, u32 *pid)
{
	NV0073_CTRL_CMD_DP_TOPOLOGY_ALLOCATE_DISPLAYID_PARAMS *ctrl;
	struct nvkm_disp *disp = outp->disp;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_TOPOLOGY_ALLOCATE_DISPLAYID,
				    sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayId = BIT(outp->index);
	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	*pid = ctrl->displayIdAssigned;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

static int
r535_dp_drive(struct nvkm_outp *outp, u8 lanes, u8 pe[4], u8 vs[4])
{
	NV0073_CTRL_DP_LANE_DATA_PARAMS *ctrl;
	struct nvkm_disp *disp = outp->disp;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_SET_LANE_DATA, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->displayId = BIT(outp->index);
	ctrl->numLanes = lanes;
	for (int i = 0; i < lanes; i++)
		ctrl->data[i] = NVVAL(NV0073_CTRL, DP_LANE_DATA,  PREEMPHASIS, pe[i]) |
				NVVAL(NV0073_CTRL, DP_LANE_DATA, DRIVECURRENT, vs[i]);

	return nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl);
}

static int
r535_dp_train_target(struct nvkm_outp *outp, u8 target, bool mst, u8 link_nr, u8 link_bw)
{
	struct nvkm_disp *disp = outp->disp;
	NV0073_CTRL_DP_CTRL_PARAMS *ctrl;
	int ret, retries;
	u32 cmd, data;

	cmd = NVDEF(NV0073_CTRL, DP_CMD, SET_LANE_COUNT, TRUE) |
	      NVDEF(NV0073_CTRL, DP_CMD, SET_LINK_BW, TRUE) |
	      NVDEF(NV0073_CTRL, DP_CMD, TRAIN_PHY_REPEATER, YES);
	data = NVVAL(NV0073_CTRL, DP_DATA, SET_LANE_COUNT, link_nr) |
	       NVVAL(NV0073_CTRL, DP_DATA, SET_LINK_BW, link_bw) |
	       NVVAL(NV0073_CTRL, DP_DATA, TARGET, target);

	if (mst)
		cmd |= NVDEF(NV0073_CTRL, DP_CMD, SET_FORMAT_MODE, MULTI_STREAM);

	if (outp->dp.dpcd[DPCD_RC02] & DPCD_RC02_ENHANCED_FRAME_CAP)
		cmd |= NVDEF(NV0073_CTRL, DP_CMD, SET_ENHANCED_FRAMING, TRUE);

	if (target == 0 &&
	     (outp->dp.dpcd[DPCD_RC02] & 0x20) &&
	    !(outp->dp.dpcd[DPCD_RC03] & DPCD_RC03_TPS4_SUPPORTED))
		cmd |= NVDEF(NV0073_CTRL, DP_CMD, POST_LT_ADJ_REQ_GRANTED, YES);

	/* We should retry up to 3 times, but only if GSP asks politely */
	for (retries = 0; retries < 3; ++retries) {
		ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom, NV0073_CTRL_CMD_DP_CTRL,
					    sizeof(*ctrl));
		if (IS_ERR(ctrl))
			return PTR_ERR(ctrl);

		ctrl->subDeviceInstance = 0;
		ctrl->displayId = BIT(outp->index);
		ctrl->retryTimeMs = 0;
		ctrl->cmd = cmd;
		ctrl->data = data;

		ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
		if ((ret == -EAGAIN || ret == -EBUSY) && ctrl->retryTimeMs) {
			/*
			 * Device (likely an eDP panel) isn't ready yet, wait for the time specified
			 * by GSP before retrying again
			 */
			nvkm_debug(&disp->engine.subdev,
				   "Waiting %dms for GSP LT panel delay before retrying\n",
				   ctrl->retryTimeMs);
			nvkm_msleep(ctrl->retryTimeMs);
			nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		} else {
			/* GSP didn't say to retry, or we were successful */
			if (ctrl->err)
				ret = -EIO;
			nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
			break;
		}
	}

	return ret;
}

static int
r535_dp_train(struct nvkm_outp *outp, bool retrain)
{
	for (int target = outp->dp.lttprs; target >= 0; target--) {
		int ret = r535_dp_train_target(outp, target, outp->dp.lt.mst,
							     outp->dp.lt.nr,
							     outp->dp.lt.bw);
		if (ret)
			return ret;
	}

	return 0;
}

static int
r535_dp_set_indexed_link_rates(struct nvkm_outp *outp)
{
	NV0073_CTRL_CMD_DP_CONFIG_INDEXED_LINK_RATES_PARAMS *ctrl;
	struct nvkm_disp *disp = outp->disp;

	if (WARN_ON(outp->dp.rates > ARRAY_SIZE(ctrl->linkRateTbl)))
		return -EINVAL;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_CONFIG_INDEXED_LINK_RATES, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->displayId = BIT(outp->index);
	for (int i = 0; i < outp->dp.rates; i++)
		ctrl->linkRateTbl[outp->dp.rate[i].dpcd] = outp->dp.rate[i].rate * 10 / 200;

	return nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl);
}

static int
r535_dp_rates(struct nvkm_outp *outp)
{
	struct nvkm_rm *rm = outp->disp->rm.objcom.client->gsp->rm;

	if (outp->conn->info.type != DCB_CONNECTOR_eDP ||
	    !outp->dp.rates || outp->dp.rate[0].dpcd < 0)
		return 0;

	return rm->api->disp->dp.set_indexed_link_rates(outp);
}

static int
r535_dp_aux_xfer(struct nvkm_outp *outp, u8 type, u32 addr, u8 *data, u8 *psize)
{
	struct nvkm_disp *disp = outp->disp;
	NV0073_CTRL_DP_AUXCH_CTRL_PARAMS *ctrl;
	u8 size = *psize;
	int ret;
	int retries;

	for (retries = 0; retries < 3; ++retries) {
		ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom, NV0073_CTRL_CMD_DP_AUXCH_CTRL, sizeof(*ctrl));
		if (IS_ERR(ctrl))
			return PTR_ERR(ctrl);

		ctrl->subDeviceInstance = 0;
		ctrl->displayId = BIT(outp->index);
		ctrl->bAddrOnly = !size;
		ctrl->cmd = type;
		if (ctrl->bAddrOnly) {
			ctrl->cmd = NVDEF_SET(ctrl->cmd, NV0073_CTRL, DP_AUXCH_CMD, REQ_TYPE, WRITE);
			ctrl->cmd = NVDEF_SET(ctrl->cmd, NV0073_CTRL, DP_AUXCH_CMD,  I2C_MOT, FALSE);
		}
		ctrl->addr = addr;
		ctrl->size = !ctrl->bAddrOnly ? (size - 1) : 0;
		memcpy(ctrl->data, data, size);

		ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
		if ((ret == -EAGAIN || ret == -EBUSY) && ctrl->retryTimeMs) {
			/*
			 * Device (likely an eDP panel) isn't ready yet, wait for the time specified
			 * by GSP before retrying again
			 */
			nvkm_debug(&disp->engine.subdev,
				   "Waiting %dms for GSP LT panel delay before retrying in AUX\n",
				   ctrl->retryTimeMs);
			nvkm_msleep(ctrl->retryTimeMs);
			nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		} else {
			memcpy(data, ctrl->data, size);
			*psize = ctrl->size;
			ret = ctrl->replyType;
			nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
			break;
		}
	}
	return ret;
}

static int
r535_dp_aux_pwr(struct nvkm_outp *outp, bool pu)
{
	return 0;
}

static void
r535_dp_release(struct nvkm_outp *outp)
{
	if (!outp->dp.lt.bw) {
		if (!WARN_ON(!outp->dp.rates))
			outp->dp.lt.bw = outp->dp.rate[0].rate / 27000;
		else
			outp->dp.lt.bw = 0x06;
	}

	outp->dp.lt.nr = 0;

	r535_dp_train_target(outp, 0, outp->dp.lt.mst, outp->dp.lt.nr, outp->dp.lt.bw);
	r535_outp_release(outp);
}

static int
r535_dp_acquire(struct nvkm_outp *outp, bool hda)
{
	int ret;

	ret = r535_outp_acquire(outp, hda);
	if (ret)
		return ret;

	return 0;
}

static const struct nvkm_outp_func
r535_dp = {
	.detect = r535_outp_detect,
	.inherit = r535_outp_inherit,
	.acquire = r535_dp_acquire,
	.release = r535_dp_release,
	.dp.aux_pwr = r535_dp_aux_pwr,
	.dp.aux_xfer = r535_dp_aux_xfer,
	.dp.mst_id_get = r535_dp_mst_id_get,
	.dp.mst_id_put = r535_dp_mst_id_put,
	.dp.rates = r535_dp_rates,
	.dp.train = r535_dp_train,
	.dp.drive = r535_dp_drive,
};

static int
r535_dp_get_caps(struct nvkm_disp *disp, int *plink_bw, bool *pmst, bool *pwm)
{
	NV0073_CTRL_CMD_DP_GET_CAPS_PARAMS *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_GET_CAPS, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->sorIndex = ~0;

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	switch (NVVAL_GET(ctrl->maxLinkRate, NV0073_CTRL_CMD, DP_GET_CAPS, MAX_LINK_RATE)) {
	case NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_1_62:
		*plink_bw = 0x06;
		break;
	case NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_2_70:
		*plink_bw = 0x0a;
		break;
	case NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_5_40:
		*plink_bw = 0x14;
		break;
	case NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_8_10:
		*plink_bw = 0x1e;
		break;
	default:
		*plink_bw = 0x00;
		break;
	}

	*pmst = ctrl->bIsMultistreamSupported;
	*pwm = ctrl->bHasIncreasedWatermarkLimits;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

static int
r535_tmds_edid_get(struct nvkm_outp *outp, u8 *data, u16 *psize)
{
	NV0073_CTRL_SPECIFIC_GET_EDID_V2_PARAMS *ctrl;
	struct nvkm_disp *disp = outp->disp;
	int ret = -E2BIG;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayId = BIT(outp->index);

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	ret = -E2BIG;
	if (ctrl->bufferSize <= *psize) {
		memcpy(data, ctrl->edidBuffer, ctrl->bufferSize);
		*psize = ctrl->bufferSize;
		ret = 0;
	}

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return ret;
}

static const struct nvkm_outp_func
r535_tmds = {
	.detect = r535_outp_detect,
	.inherit = r535_outp_inherit,
	.acquire = r535_outp_acquire,
	.release = r535_outp_release,
	.edid_get = r535_tmds_edid_get,
};

static int
r535_outp_new(struct nvkm_disp *disp, u32 id)
{
	const struct nvkm_rm_api *rmapi = disp->rm.objcom.client->gsp->rm->api;
	NV0073_CTRL_SPECIFIC_OR_GET_INFO_PARAMS *ctrl;
	enum nvkm_ior_proto proto;
	struct dcb_output dcbE = {};
	struct nvkm_conn *conn;
	struct nvkm_outp *outp;
	u8 locn, link = 0;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayId = BIT(id);

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	switch (ctrl->type) {
	case NV0073_CTRL_SPECIFIC_OR_TYPE_NONE:
		return 0;
	case NV0073_CTRL_SPECIFIC_OR_TYPE_SOR:
		switch (ctrl->protocol) {
		case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_SINGLE_TMDS_A:
			proto = TMDS;
			link = 1;
			break;
		case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_SINGLE_TMDS_B:
			proto = TMDS;
			link = 2;
			break;
		case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DUAL_TMDS:
			proto = TMDS;
			link = 3;
			break;
		case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_A:
			proto = DP;
			link = 1;
			break;
		case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_B:
			proto = DP;
			link = 2;
			break;
		default:
			WARN_ON(1);
			return -EINVAL;
		}

		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	locn = ctrl->location;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);

	conn = r535_conn_new(disp, id);
	if (IS_ERR(conn))
		return PTR_ERR(conn);

	switch (proto) {
	case TMDS: dcbE.type = DCB_OUTPUT_TMDS; break;
	case   DP: dcbE.type = DCB_OUTPUT_DP; break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	dcbE.location = locn;
	dcbE.connector = conn->index;
	dcbE.heads = disp->head.mask;
	dcbE.i2c_index = 0xff;
	dcbE.link = dcbE.sorconf.link = link;

	if (proto == TMDS) {
		ret = nvkm_outp_new_(&r535_tmds, disp, id, &dcbE, &outp);
		if (ret)
			return ret;
	} else {
		bool mst, wm;

		ret = rmapi->disp->dp.get_caps(disp, &dcbE.dpconf.link_bw, &mst, &wm);
		if (ret)
			return ret;

		if (WARN_ON(!dcbE.dpconf.link_bw))
			return -EINVAL;

		dcbE.dpconf.link_nr = 4;

		ret = nvkm_outp_new_(&r535_dp, disp, id, &dcbE, &outp);
		if (ret)
			return ret;

		outp->dp.mst = mst;
		outp->dp.increased_wm = wm;
	}


	outp->conn = conn;
	list_add_tail(&outp->head, &disp->outps);
	return 0;
}

static void
r535_disp_irq(struct nvkm_gsp_event *event, void *repv, u32 repc)
{
	struct nvkm_disp *disp = container_of(event, typeof(*disp), rm.irq);
	Nv2080DpIrqNotification *irq = repv;

	if (WARN_ON(repc < sizeof(*irq)))
		return;

	nvkm_debug(&disp->engine.subdev, "event: dp irq displayId %08x\n", irq->displayId);

	if (irq->displayId)
		nvkm_event_ntfy(&disp->rm.event, fls(irq->displayId) - 1, NVKM_DPYID_IRQ);
}

static void
r535_disp_hpd(struct nvkm_gsp_event *event, void *repv, u32 repc)
{
	struct nvkm_disp *disp = container_of(event, typeof(*disp), rm.hpd);
	Nv2080HotplugNotification *hpd = repv;

	if (WARN_ON(repc < sizeof(*hpd)))
		return;

	nvkm_debug(&disp->engine.subdev, "event: hpd plug %08x unplug %08x\n",
		   hpd->plugDisplayMask, hpd->unplugDisplayMask);

	for (int i = 0; i < 31; i++) {
		u32 mask = 0;

		if (hpd->plugDisplayMask & BIT(i))
			mask |= NVKM_DPYID_PLUG;
		if (hpd->unplugDisplayMask & BIT(i))
			mask |= NVKM_DPYID_UNPLUG;

		if (mask)
			nvkm_event_ntfy(&disp->rm.event, i, mask);
	}

#ifdef __DragonFly__
	if (hpd->plugDisplayMask) {
		struct nvkm_gsp *gsp = disp->engine.subdev.device->gsp;

		if (gsp && gsp->sc)
			(void)nvkm_drm_kms_schedule(gsp->sc, "hotplug");
	}
#endif
}

static const struct nvkm_event_func
r535_disp_event = {
};

static void
r535_disp_intr_head_timing(struct nvkm_disp *disp, int head)
{
	struct nvkm_subdev *subdev = &disp->engine.subdev;
	struct nvkm_device *device = subdev->device;
	u32 stat = nvkm_rd32(device, 0x611c00 + (head * 0x04));

	if (stat & 0x00000002) {
		nvkm_disp_vblank(disp, head);

		nvkm_wr32(device, 0x611800 + (head * 0x04), 0x00000002);
	}
}

static irqreturn_t
r535_disp_intr(struct nvkm_inth *inth)
{
	struct nvkm_disp *disp = container_of(inth, typeof(*disp), engine.subdev.inth);
	struct nvkm_subdev *subdev = &disp->engine.subdev;
	struct nvkm_device *device = subdev->device;
	unsigned long mask = nvkm_rd32(device, 0x611ec0) & 0x000000ff;
	int head;

	for_each_set_bit(head, &mask, 8)
		r535_disp_intr_head_timing(disp, head);

	return IRQ_HANDLED;
}

static void
r535_disp_fini(struct nvkm_disp *disp, bool suspend)
{
	if (!disp->engine.subdev.use.enabled)
		return;

	nvkm_gsp_rm_free(&disp->rm.object);

	if (!suspend) {
		nvkm_gsp_event_dtor(&disp->rm.irq);
		nvkm_gsp_event_dtor(&disp->rm.hpd);
		nvkm_event_fini(&disp->rm.event);

		nvkm_gsp_rm_free(&disp->rm.objcom);
		nvkm_gsp_device_dtor(&disp->rm.device);
		nvkm_gsp_client_dtor(&disp->rm.client);
	}
}

static int
r535_disp_init(struct nvkm_disp *disp)
{
	int ret;

	ret = nvkm_gsp_rm_alloc(&disp->rm.device.object, disp->func->root.oclass << 16,
				disp->func->root.oclass, 0, &disp->rm.object);
	if (ret)
		return ret;

	return 0;
}

static int
r535_disp_get_supported(struct nvkm_disp *disp, unsigned long *pmask)
{
	NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_rd(&disp->rm.objcom,
				   NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	*pmask = ctrl->displayMask;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

static int
r535_disp_get_static_info(struct nvkm_disp *disp)
{
	NV2080_CTRL_INTERNAL_DISPLAY_GET_STATIC_INFO_PARAMS *ctrl;
	struct nvkm_gsp *gsp = disp->rm.objcom.client->gsp;

	ctrl = nvkm_gsp_rm_ctrl_rd(&gsp->internal.device.subdevice,
				   NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO,
				   sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	disp->wndw.mask = ctrl->windowPresentMask;
	disp->wndw.nr = fls(disp->wndw.mask);
#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
	/*
	 * r570.144 static-info carries head count.  The r535-only GET_NUM_HEADS
	 * and GET_ALL_HEAD_MASK controls fail FINN validation on this firmware.
	 */
	disp->head.nr = ctrl->numHeads;
#endif

	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, ctrl);
	return 0;
}

static int
r535_disp_oneinit(struct nvkm_disp *disp)
{
	struct nvkm_device *device = disp->engine.subdev.device;
	struct nvkm_gsp *gsp = device->gsp;
	const struct nvkm_rm_api *rmapi = gsp->rm->api;
	NV2080_CTRL_INTERNAL_DISPLAY_WRITE_INST_MEM_PARAMS *ctrl;
	unsigned long mask;
	int ret, i;

	/* RAMIN. */
	ret = nvkm_gpuobj_new(device, 0x10000, 0x10000, false, NULL, &disp->inst);
	if (ret)
		return ret;

	if (WARN_ON(nvkm_memory_target(disp->inst->memory) != NVKM_MEM_TARGET_VRAM))
		return -EINVAL;

	ctrl = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice,
				    NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM,
				    sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->instMemPhysAddr = nvkm_memory_addr(disp->inst->memory);
	ctrl->instMemSize = nvkm_memory_size(disp->inst->memory);
	ctrl->instMemAddrSpace = ADDR_FBMEM;
	ctrl->instMemCpuCacheAttr = NV_MEMORY_WRITECOMBINED;

	ret = nvkm_gsp_rm_ctrl_wr(&gsp->internal.device.subdevice, ctrl);
	if (ret)
		return ret;

	/* OBJs. */
	ret = nvkm_gsp_client_device_ctor(gsp, &disp->rm.client, &disp->rm.device);
	if (ret)
		return ret;

	ret = nvkm_gsp_rm_alloc(&disp->rm.device.object, NVKM_RM_DISP, NV04_DISPLAY_COMMON, 0,
				&disp->rm.objcom);
	if (ret)
		return ret;

	ret = rmapi->disp->get_static_info(disp);
	if (ret)
		return ret;

	/* */
	{
#if defined(CONFIG_ACPI) && defined(CONFIG_X86)
		NV2080_CTRL_INTERNAL_INIT_BRIGHTC_STATE_LOAD_PARAMS *ctrl;
		struct nvkm_gsp_object *subdevice = &disp->rm.client.gsp->internal.device.subdevice;

		ctrl = nvkm_gsp_rm_ctrl_get(subdevice,
					    NV2080_CTRL_CMD_INTERNAL_INIT_BRIGHTC_STATE_LOAD,
					    sizeof(*ctrl));
		if (IS_ERR(ctrl))
			return PTR_ERR(ctrl);

		ctrl->status = 0x56; /* NV_ERR_NOT_SUPPORTED */

		{
			const guid_t NBCI_DSM_GUID =
				GUID_INIT(0xD4A50B75, 0x65C7, 0x46F7,
					  0xBF, 0xB7, 0x41, 0x51, 0x4C, 0xEA, 0x02, 0x44);
			u64 NBCI_DSM_REV = 0x00000102;
			const guid_t NVHG_DSM_GUID =
				GUID_INIT(0x9D95A0A0, 0x0060, 0x4D48,
					  0xB3, 0x4D, 0x7E, 0x5F, 0xEA, 0x12, 0x9F, 0xD4);
			u64 NVHG_DSM_REV = 0x00000102;
			acpi_handle handle = ACPI_HANDLE(device->dev);

			if (handle && acpi_has_method(handle, "_DSM")) {
				bool nbci = acpi_check_dsm(handle, &NBCI_DSM_GUID, NBCI_DSM_REV,
						           1ULL << 0x00000014);
				bool nvhg = acpi_check_dsm(handle, &NVHG_DSM_GUID, NVHG_DSM_REV,
						           1ULL << 0x00000014);

				if (nbci || nvhg) {
					union acpi_object argv4 = {
						.buffer.type    = ACPI_TYPE_BUFFER,
						.buffer.length  = sizeof(ctrl->backLightData),
						.buffer.pointer = nvkm_kmalloc(argv4.buffer.length, M_WAITOK),
					}, *obj;

					obj = acpi_evaluate_dsm(handle, nbci ? &NBCI_DSM_GUID : &NVHG_DSM_GUID,
								0x00000102, 0x14, &argv4);
					if (!obj) {
						acpi_handle_info(handle, "failed to evaluate _DSM\n");
					} else {
						for (int i = 0; i < obj->package.count; i++) {
							union acpi_object *elt = &obj->package.elements[i];
							u32 size;

							if (elt->integer.value & ~0xffffffffULL)
								size = 8;
							else
								size = 4;

							memcpy(&ctrl->backLightData[ctrl->backLightDataSize], &elt->integer.value, size);
							ctrl->backLightDataSize += size;
						}

						ctrl->status = 0;
						ACPI_FREE(obj);
					}

					nvkm_kfree(argv4.buffer.pointer);
				}
			}
		}

		ret = nvkm_gsp_rm_ctrl_wr(subdevice, ctrl);
		if (ret)
			return ret;
#endif
	}

#ifndef NVKM_DFLY_GSP_DISPLAY_ONLY
	/* r535-only legacy display control; r570.144 rejects it. */
	{
		NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT_PARAMS *ctrl;

		ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
					    NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT,
					    sizeof(*ctrl));
		if (IS_ERR(ctrl))
			return PTR_ERR(ctrl);

		ret = nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl);
		if (ret)
			return ret;
	}

	/* r535-only legacy display control; r570.144 reports numHeads above. */
	{
		NV0073_CTRL_SYSTEM_GET_NUM_HEADS_PARAMS *ctrl;

		ctrl = nvkm_gsp_rm_ctrl_rd(&disp->rm.objcom,
					   NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS, sizeof(*ctrl));
		if (IS_ERR(ctrl))
			return PTR_ERR(ctrl);

		disp->head.nr = ctrl->numHeads;
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	}

	/* r535-only legacy display control; r570.144 uses a contiguous head set. */
	{
		NV0073_CTRL_SPECIFIC_GET_ALL_HEAD_MASK_PARAMS *ctrl;

		ctrl = nvkm_gsp_rm_ctrl_rd(&disp->rm.objcom,
					   NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK,
					   sizeof(*ctrl));
		if (IS_ERR(ctrl))
			return PTR_ERR(ctrl);

		disp->head.mask = ctrl->headMask;
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);

		for_each_set_bit(i, &disp->head.mask, disp->head.nr) {
			ret = nvkm_head_new_(&r535_head, disp, i);
			if (ret)
				return ret;
		}
	}
#else
	/*
	 * r570.144 removed the r535 GET_ALL_HEAD_MASK control from its display
	 * RM interface.  Use the head count from GET_STATIC_INFO and stage a
	 * contiguous mask until the full upstream r570 display API is imported.
	 */
	if (disp->head.nr <= 0 || disp->head.nr >= BITS_PER_LONG)
		return -EINVAL;

	disp->head.mask = BIT(disp->head.nr) - 1;
	for_each_set_bit(i, &disp->head.mask, disp->head.nr) {
		ret = nvkm_head_new_(&r535_head, disp, i);
		if (ret)
			return ret;
	}
#endif

	disp->sor.nr = disp->func->sor.cnt(disp, &disp->sor.mask);
	nvkm_debug(&disp->engine.subdev, "   SOR(s): %d (%02lx)\n", disp->sor.nr, disp->sor.mask);
	for_each_set_bit(i, &disp->sor.mask, disp->sor.nr) {
		ret = disp->func->sor.new(disp, i);
		if (ret)
			return ret;
	}

	ret = rmapi->disp->get_supported(disp, &mask);
	if (ret)
		return ret;

	for_each_set_bit(i, &mask, 32) {
		ret = r535_outp_new(disp, i);
		if (ret)
			return ret;
	}

	ret = nvkm_event_init(&r535_disp_event, &gsp->subdev, 3, 32, &disp->rm.event);
	if (WARN_ON(ret))
		return ret;

	ret = nvkm_gsp_device_event_ctor(&disp->rm.device, 0x007e0000, NV2080_NOTIFIERS_HOTPLUG,
					 r535_disp_hpd, &disp->rm.hpd);
	if (ret)
		return ret;

	ret = nvkm_gsp_device_event_ctor(&disp->rm.device, 0x007e0001, NV2080_NOTIFIERS_DP_IRQ,
					 r535_disp_irq, &disp->rm.irq);
	if (ret)
		return ret;

	/* RAMHT. */
	ret = nvkm_ramht_new(device, disp->func->ramht_size ? disp->func->ramht_size :
			     0x1000, 0, disp->inst, &disp->ramht);
	if (ret)
		return ret;

#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
	/*
	 * DragonFly still routes GSP/PCI interrupts through nvkm_pci.c.  Keep
	 * the r535 display object creation path intact, but do not dereference
	 * nouveau's vfn/top interrupt graph until that topology is imported.
	 */
	(void)r535_disp_intr;
#else
	ret = nvkm_gsp_intr_stall(gsp, disp->engine.subdev.type, disp->engine.subdev.inst);
	if (ret < 0)
		return ret;

	ret = nvkm_inth_add(&device->vfn->intr, ret, NVKM_INTR_PRIO_NORMAL, &disp->engine.subdev,
			    r535_disp_intr, &disp->engine.subdev.inth);
	if (ret)
		return ret;

	nvkm_inth_allow(&disp->engine.subdev.inth);
#endif
	return 0;
}

static void
r535_disp_dtor(struct nvkm_disp *disp)
{
	nvkm_kfree(disp->func);
}

int
r535_disp_new(const struct nvkm_disp_func *hw, struct nvkm_device *device,
	      enum nvkm_subdev_type type, int inst, struct nvkm_disp **pdisp)
{
	const struct nvkm_rm_gpu *gpu = device->gsp->rm->gpu;
	struct nvkm_disp_func *rm;
#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
	const size_t user_count = 1; /* zeroed sentinel only */
#else
	const size_t user_count = 6;
#endif
	int ret;

	if (!(rm = nvkm_kzalloc(sizeof(*rm) + user_count * sizeof(rm->user[0]), M_WAITOK)))
		return -ENOMEM;

	rm->dtor = r535_disp_dtor;
	rm->oneinit = r535_disp_oneinit;
	rm->init = r535_disp_init;
	rm->fini = r535_disp_fini;
	rm->uevent = hw->uevent;
	rm->sor.cnt = r535_sor_cnt;
	rm->sor.new = r535_sor_new;
	rm->ramht_size = hw->ramht_size;

	rm->root.oclass = gpu->disp.class.root;

#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
	/*
	 * The NVIF display user objects below back nouveau's userspace display
	 * channel ABI.  DragonFly first-light currently enters display through
	 * native KMS glue and RM/GSP controls, so keep the upstream r535 layout
	 * but do not compile these constructors until that ABI layer is ported.
	 */
#else
	rm->user[0].base.oclass = gpu->disp.class.caps;
	rm->user[0].ctor = gv100_disp_caps_new;

	rm->user[1].base.oclass = gpu->disp.class.core;
	rm->user[1].ctor = nvkm_disp_core_new;
	rm->user[1].chan = &r535_core;

	rm->user[2].base.oclass = gpu->disp.class.wndw;
	rm->user[2].ctor = nvkm_disp_wndw_new;
	rm->user[2].chan = &r535_wndw;

	rm->user[3].base.oclass = gpu->disp.class.wimm;
	rm->user[3].ctor = nvkm_disp_wndw_new;
	rm->user[3].chan = &r535_wimm;

	rm->user[4].base.oclass = gpu->disp.class.curs;
	rm->user[4].ctor = nvkm_disp_chan_new;
	rm->user[4].chan = &r535_curs;
#endif

	ret = nvkm_disp_new_(rm, device, type, inst, pdisp);
	if (ret) {
		nvkm_kfree(rm);
		return ret;
	}

	lockinit(&(*pdisp)->super.mutex, "nvdispsuper", 0, LK_CANRECURSE);
	return ret;
}

const struct nvkm_rm_api_disp
r535_disp = {
	.get_static_info = r535_disp_get_static_info,
	.get_supported = r535_disp_get_supported,
	.get_connect_state = r535_disp_get_connect_state,
	.get_active = r535_disp_get_active,
	.bl_ctrl = r535_bl_ctrl,
	.dp = {
		.get_caps = r535_dp_get_caps,
		.set_indexed_link_rates = r535_dp_set_indexed_link_rates,
	},
	.chan = {
		.set_pushbuf = r535_disp_chan_set_pushbuf,
		.dmac_alloc = r535_dmac_alloc,
	}
};

#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
const struct nvkm_rm_gpu
tu1xx_gpu = {
	.disp.class.root = TU102_DISP,
	.disp.class.caps = GV100_DISP_CAPS,
	.disp.class.core = TU102_DISP_CORE_CHANNEL_DMA,
	.disp.class.wndw = TU102_DISP_WINDOW_CHANNEL_DMA,
	.disp.class.wimm = TU102_DISP_WINDOW_IMM_CHANNEL_DMA,
	.disp.class.curs = TU102_DISP_CURSOR,
};

static const struct nvkm_rm_api
nvkm_dfly_r535_api = {
	.disp = &r535_disp,
};

static int
nvkm_dfly_core_display_alloc(struct nvkm_softc *sc)
{
	struct nvkm_device *device;
	struct nvkm_gsp *gsp;
	struct nvkm_rm *rm;
	struct nvkm_gsp_client *internal;
	struct device *ldev;
	int prio;

	if (sc->core_device != NULL && sc->core_gsp != NULL &&
	    sc->core_rm != NULL && sc->core_internal_client != NULL)
		return 0;

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	gsp = kzalloc(sizeof(*gsp), GFP_KERNEL);
	rm = kzalloc(sizeof(*rm), GFP_KERNEL);
	internal = kzalloc(sizeof(*internal), GFP_KERNEL);
	ldev = kzalloc(sizeof(*ldev), GFP_KERNEL);
	if (device == NULL || gsp == NULL || rm == NULL || internal == NULL ||
	    ldev == NULL) {
		kfree(device);
		kfree(gsp);
		kfree(rm);
		kfree(internal);
		kfree(ldev);
		return -ENOMEM;
	}

	ldev->bsddev = sc->dev;
	device->dev = ldev;
	device->type = NVKM_DEVICE_PCIE;
	device->name = "tu102";
	device->card_type = TU100;
	device->chipset = 0x162;
	device->pri = (void __iomem *)rman_get_virtual(sc->bar_res[0]);
	INIT_LIST_HEAD(&device->head);
	INIT_LIST_HEAD(&device->subdev);
	INIT_LIST_HEAD(&device->intr.intr);
	for (prio = 0; prio < NVKM_INTR_PRIO_NR; prio++)
		INIT_LIST_HEAD(&device->intr.prio[prio]);
	lockinit(&device->mutex, "nvdev", 0, LK_CANRECURSE);
	lockinit(&device->intr.lock, "nvintr", 0, LK_CANRECURSE);

	rm->device = device;
	rm->gpu = &tu1xx_gpu;
	rm->api = &nvkm_dfly_r535_api;

	internal->sc = sc;
	internal->gsp = gsp;
	internal->object.client = internal;
	internal->object.parent = NULL;
	internal->object.handle = sc->gsp_internal_client;

	gsp->sc = sc;
	gsp->rm = rm;
	gsp->subdev.device = device;
	gsp->subdev.name[0] = 'g';
	gsp->subdev.name[1] = 's';
	gsp->subdev.name[2] = 'p';
	gsp->subdev.name[3] = '\0';
	gsp->internal.device.object.client = internal;
	gsp->internal.device.object.parent = &internal->object;
	gsp->internal.device.object.handle = sc->gsp_internal_device;
	gsp->internal.device.subdevice.client = internal;
	gsp->internal.device.subdevice.parent = &gsp->internal.device.object;
	gsp->internal.device.subdevice.handle = sc->gsp_internal_subdevice;

	device->gsp = gsp;

	sc->core_device = device;
	sc->core_gsp = gsp;
	sc->core_rm = rm;
	sc->core_internal_client = internal;
	return 0;
}

/*
 * Temporary KMS compatibility entry point.  This builds the imported nouveau
 * core/r535 display engine and stores it in sc->disp.  Existing KMS code can
 * keep calling this name until its external ABI is renamed around struct
 * nvkm_disp.
 */
int
nvkm_gsp_disp_init(struct nvkm_softc *sc)
{
	int ret;

	if (sc == NULL || !sc->gsp_running || sc->bar_res[0] == NULL)
		return -ENODEV;
	if (sc->disp != NULL)
		return 0;

	ret = nvkm_dfly_core_display_alloc(sc);
	if (ret)
		return ret;

	ret = tu102_disp_new(sc->core_device, NVKM_ENGINE_DISP, 0, &sc->disp);
	if (ret)
		return ret;
	sc->core_device->disp = sc->disp;

	ret = nvkm_subdev_ref(&sc->disp->engine.subdev);
	if (ret) {
		nvkm_infof(sc->dev, "gsp: core display init failed %d\n", ret);
		return ret;
	}

	nvkm_infof(sc->dev, "gsp: core display init heads=%d mask=0x%lx outps=%s\n",
	    sc->disp->head.nr, sc->disp->head.mask,
	    list_empty(&sc->disp->outps) ? "none" : "present");
	return 0;
}

uint32_t
nvkm_gsp_disp_supported_mask(struct nvkm_softc *sc)
{
	unsigned long mask = 0;

	if (sc == NULL || sc->disp == NULL)
		return 0;
	if (sc->core_rm->api->disp->get_supported(sc->disp, &mask) != 0)
		return 0;
	return (uint32_t)mask;
}

uint32_t
nvkm_gsp_disp_head_count(struct nvkm_softc *sc)
{
	if (sc == NULL || sc->disp == NULL || sc->disp->head.nr == 0)
		return 1;
	return (uint32_t)sc->disp->head.nr;
}

int
nvkm_gsp_disp_connected(struct nvkm_softc *sc, uint32_t display_id)
{
	int id;

	if (sc == NULL || sc->disp == NULL || display_id == 0)
		return -ENODEV;
	id = ffs(display_id) - 1;
	if (id < 0 || id >= 32)
		return -EINVAL;
	return sc->core_rm->api->disp->get_connect_state(sc->disp, id);
}

int
nvkm_gsp_disp_read_edid(struct nvkm_softc *sc, uint32_t display_id,
    uint8_t *out, uint32_t *outlen)
{
	NV0073_CTRL_SPECIFIC_GET_EDID_V2_PARAMS *ctrl;
	struct nvkm_disp *disp;
	uint16_t size;
	int id, ret;

	if (sc == NULL || sc->disp == NULL || out == NULL || outlen == NULL ||
	    *outlen == 0 || display_id == 0)
		return -EINVAL;

	id = ffs(display_id) - 1;
	if (id < 0 || id >= 32)
		return -EINVAL;
	disp = sc->disp;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
	    NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayId = BIT(id);

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	size = (uint16_t)MIN(*outlen, (uint32_t)sizeof(ctrl->edidBuffer));
	if (ctrl->bufferSize > size) {
		ret = -E2BIG;
	} else {
		memcpy(out, ctrl->edidBuffer, ctrl->bufferSize);
		*outlen = ctrl->bufferSize;
		ret = 0;
	}

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return ret;
}

int
nvkm_gsp_disp_channel_pushbuf(struct nvkm_softc *sc, int32_t oclass,
    int inst, struct nvkm_memory *memory)
{
	if (sc == NULL || sc->disp == NULL || memory == NULL)
		return -ENODEV;
	return r535_disp_chan_set_pushbuf(sc->disp, oclass, inst, memory);
}

int
nvkm_gsp_disp_dmac_alloc(struct nvkm_softc *sc, uint32_t oclass,
    int inst, uint32_t put_offset, struct nvkm_gsp_object *object)
{
	if (sc == NULL || sc->disp == NULL || object == NULL)
		return -ENODEV;
	return r535_dmac_alloc(sc->disp, oclass, inst, put_offset, object);
}

int
nvkm_gsp_disp_dmac_bind(struct nvkm_softc *sc, uint32_t oclass,
    int inst, struct nvkm_object *object, uint32_t handle)
{
	struct nvkm_disp_chan chan = { 0 };
	int ret;

	if (sc == NULL || sc->disp == NULL || object == NULL || inst < 0)
		return -ENODEV;

	chan.disp = sc->disp;
	switch (oclass & 0xff) {
	case 0x7d:
		chan.chid.user = 0;
		break;
	case 0x7e:
		chan.chid.user = 1 + inst;
		break;
	case 0x7b:
		chan.chid.user = 33 + inst;
		break;
	case 0x7a:
		chan.chid.user = 73 + inst;
		break;
	default:
		return -EINVAL;
	}

	ret = r535_dmac_bind(&chan, object, handle);
	if (ret > 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 dmac bind class=0x%x inst=%d chid=%d "
		    "handle=0x%x client=0x%x cookie=%d\n",
		    oclass, inst, chan.chid.user, handle,
		    chan.disp->rm.client.object.handle, ret);
		nvkm_gsp_bar1_flush(sc);
	}
	return ret;
}

void
nvkm_gsp_disp_dmac_unbind(struct nvkm_softc *sc, int cookie)
{
	if (sc == NULL || sc->disp == NULL || sc->disp->ramht == NULL ||
	    cookie <= 0)
		return;
	nvkm_ramht_remove(sc->disp->ramht, cookie);
}
#endif
