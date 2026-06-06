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
#include <machine/pmap.h>
#include <linux/slab.h>
#include <drm/drm_fourcc.h>

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"

static MALLOC_DEFINE(M_NVKM_DISP, "nvkm_disp", "nvkm display pushbuffers");

/* ===== MIT definitions (open-gpu 570.144; NvU32->u32, NvU64->u64, NvBool/NvU8->u8) ===== */

#define NV04_DISPLAY_COMMON				0x00000073u
#define NV_MEMORY_WRITECOMBINED				2u
#define ADDR_SYSMEM					1u
#define ADDR_FBMEM					2u
#define PBTARGET_PHYS_PCI_COHERENT			3u
#define DISP_SUBDEVICE_ID0				1u

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

#define NV2080_CTRL_CMD_INTERNAL_INIT_BRIGHTC_STATE_LOAD 0x20800ac6u
#define NV2080_CTRL_ACPI_DSM_READ_SIZE			0x1000u
#define NV_ERR_NOT_SUPPORTED				0x00000056u
struct disp_init_brightc_state_load_params {
	uint32_t status;
	uint16_t backLightDataSize;
	uint8_t  backLightData[NV2080_CTRL_ACPI_DSM_READ_SIZE];
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
#define NV2080_NOTIFIERS_DP_IRQ			7u
#define NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION		0x20800301u
#define NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT 2u
/* Event-object child handle base; distinct from client/objcom/device handles. */
#define NVKM_RM_DISP_HPD				0x007e0000u
#define NVKM_RM_DISP_DP_IRQ				0x007e0001u

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

/* r535_outp_new()/r535_conn_new(): output oneinit RPCs. */
#define NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA	0x730250u
#define NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO		0x73028bu
#define NV0073_CTRL_CMD_DP_GET_CAPS			0x731369u
#define NV0073_CTRL_CMD_DFP_GET_INFO			0x731140u
#define NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE		0x73010cu

#define NV0073_CTRL_MAX_CONNECTORS			4u
#define NV0073_CTRL_SPECIFIC_OR_TYPE_NONE		0x00000000u
#define NV0073_CTRL_SPECIFIC_OR_TYPE_SOR		0x00000002u
#define NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_SINGLE_TMDS_A 0x00000001u
#define NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_SINGLE_TMDS_B 0x00000002u
#define NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DUAL_TMDS	0x00000005u
#define NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_A	0x00000008u
#define NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_B	0x00000009u

struct disp_connector_data_entry {
	uint32_t index;
	uint32_t type;
	uint32_t location;
};

struct disp_get_connector_data_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t flags;
	uint32_t DDCPartners;
	uint32_t count;
	struct disp_connector_data_entry data[NV0073_CTRL_MAX_CONNECTORS];
	uint32_t platform;
};

struct disp_or_get_info_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t index;
	uint32_t type;
	uint32_t protocol;
	uint32_t ditherType;
	uint32_t ditherAlgo;
	uint32_t location;
	uint32_t rootPortId;
	uint32_t dcbIndex;
	uint64_t vbiosAddress;
	uint8_t  bIsLitByVbios;
	uint8_t  bIsDispDynamic;
};

struct disp_dp_dsc_cap_params {
	uint8_t  bDscSupported;
	uint32_t encoderColorFormatMask;
	uint32_t lineBufferSizeKB;
	uint32_t rateBufferSizeKB;
	uint32_t bitsPerPixelPrecision;
	uint32_t maxNumHztSlices;
	uint32_t lineBufferBitDepth;
};

struct disp_dp_get_caps_params {
	uint32_t subDeviceInstance;
	uint32_t sorIndex;
	uint32_t maxLinkRate;
	uint32_t dpVersionsSupported;
	uint32_t UHBRSupportedByGpu;
	uint32_t minPClkForCompressed;
	uint8_t  bIsMultistreamSupported;
	uint8_t  bIsSCEnabled;
	uint8_t  bHasIncreasedWatermarkLimits;
	uint8_t  bIsPC2Disabled;
	uint8_t  isSingleHeadMSTSupported;
	uint8_t  bFECSupported;
	uint8_t  bIsTrainPhyRepeater;
	uint8_t  bOverrideLinkBw;
	uint8_t  bUseRgFlushSequence;
	uint8_t  bSupportDPDownSpread;
	uint8_t  _pad[2];
	struct disp_dp_dsc_cap_params DSC;
};

struct disp_dfp_get_info_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t flags;
	uint32_t flags2;
};

struct disp_get_active_params {
	uint32_t subDeviceInstance;
	uint32_t head;
	uint32_t flags;
	uint32_t displayId;
};

/* Core display channel (M2a). TU102 NVDisplay classes. */
#define TU102_DISP				0x0000c570u	/* display root */
#define GV100_DISP_CAPS			0x0000c373u
#define NVKM_RM_DISP_CAPS			0x00000000u	/* nouveau uses handle 0 */
#define TU102_DISP_CURSOR			0x0000c57au	/* cursor PIO (NVC57A) */
#define NVKM_RM_DISP_CURSOR			0xc57a0000u
#define TU102_DISP_WINDOW_IMM_CHANNEL_DMA	0x0000c57bu	/* WIMM (NVC57B) */
#define NVKM_RM_DISP_WIMM			0xc57b0000u
#define TU102_DISP_CORE_CHANNEL_DMA		0x0000c57du	/* core (NVC57D) */
#define NVKM_RM_DISP_CORE			0xc57d0000u
#define TU102_DISP_WINDOW_CHANNEL_DMA		0x0000c57eu	/* window (NVC57E) */
#define NVKM_RM_DISP_WINDOW			0xc57e0000u
/* Window image methods (clc57e.h, MIT). */
#define NVC57E_SET_SIZE				0x00000224u
#define NVC57E_SET_SIZE_OUT			0x000002a4u
/* Core notifier methods (clc57d.h/clc37d.h, MIT). NVC57D reuses the C37D
 * method offsets for these. */
#define NVC57D_SET_CONTEXT_DMA_NOTIFIER		0x00000208u
#define NVC57D_SET_NOTIFIER_CONTROL		0x0000020cu	/* MODE[0]/OFFSET[11:4]/NOTIFY[12] */
#define NVC57D_NOTIFIER_CONTROL_NOTIFY_ENABLE	(1u << 12)
#define NVC57D_NOTIFIER_CONTROL_OFFSET(o)	(((o) & 0xffu) << 4)
#define NVC57D_UPDATE				0x00000200u
#define NVC57D_SET_INTERLOCK_FLAGS		0x00000218u
#define NVC57D_SET_WINDOW_INTERLOCK_FLAGS	0x0000021cu
#define NV50_DISP_HANDLE_SYNCBUF		0xf0000000u	/* notifier ctxdma handle */
#define NV50_DISP_HANDLE_VRAM			0xf0000001u	/* core whole-VRAM ctxdma */
#define NV50_DISP_SYNC(c, o)			((c) * 0x40u + (o))
#define NV50_DISP_WNDW_SEM0(c)			NV50_DISP_SYNC(1u + (c), 0x00u)
#define NV50_DISP_WNDW_NTFY(c)			NV50_DISP_SYNC(1u + (c), 0x20u)
#define NVC57D_CORE_NOTIFIER_OFFSET		0u
/* NV_DISP_NOTIFIER dword0 STATUS field (bits 31:30). */
#define DISP_NOTIFIER_STATUS_BEGUN		0x1u
#define DISP_NOTIFIER_STATUS_FINISHED		0x2u

/* Head/SOR/window modeset method offsets (clc57d.h/clc37d.h/clc57e.h, MIT). */
#define NVC57D_HEAD_SET_PROCAMP(h)		(0x00002000u + (h) * 0x400u)
#define NVC57D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(h) (0x00002004u + (h) * 0x400u)
#define NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(h) (0x0000200cu + (h) * 0x400u)
#define NVC57D_HEAD_SET_DISPLAY_ID(h)		(0x00002020u + (h) * 0x400u)
#define NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(h) (0x00002028u + (h) * 0x400u)
#define NVC57D_HEAD_SET_HEAD_USAGE_BOUNDS(h)	(0x00002030u + (h) * 0x400u)
#define NVC57D_HEAD_SET_VIEWPORT_POINT_IN(h)	(0x00002048u + (h) * 0x400u)
#define NVC57D_HEAD_SET_VIEWPORT_SIZE_IN(h)	(0x0000204cu + (h) * 0x400u)
#define NVC57D_HEAD_SET_VIEWPORT_SIZE_OUT(h)	(0x00002058u + (h) * 0x400u)
#define NVC57D_HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(h) (0x0000205cu + (h) * 0x400u)
#define NVC57D_HEAD_SET_RASTER_SIZE(h)		(0x00002064u + (h) * 0x400u)
#define NVC57D_HEAD_SET_RASTER_SYNC_END(h)	(0x00002068u + (h) * 0x400u)
#define NVC57D_HEAD_SET_RASTER_BLANK_END(h)	(0x0000206cu + (h) * 0x400u)
#define NVC57D_HEAD_SET_RASTER_BLANK_START(h)	(0x00002070u + (h) * 0x400u)
#define NVC57D_HEAD_SET_RASTER_VERT_BLANK2(h)	(0x00002074u + (h) * 0x400u)
#define NVC57D_HEAD_SET_DITHER_CONTROL(h)	(0x00002018u + (h) * 0x400u)
#define NVC57D_HEAD_SET_CONTEXT_DMA_CURSOR(h, b) (0x00002088u + (h) * 0x400u + (b) * 4u)
#define NVC57D_HEAD_SET_OFFSET_CURSOR(h, b)	(0x00002090u + (h) * 0x400u + (b) * 4u)
#define NVC57D_HEAD_SET_CONTROL_CURSOR(h)	(0x0000209cu + (h) * 0x400u)
#define NVC57D_HEAD_SET_CONTROL_CURSOR_COMPOSITION(h) (0x000020a0u + (h) * 0x400u)
#define NVC57D_HEAD_SET_CONTROL(h)		(0x00002008u + (h) * 0x400u)
#define NVC57D_HEAD_SET_OLUT_CONTROL(h)		(0x00002280u + (h) * 0x400u)
#define NVC57D_HEAD_SET_OLUT_FP_NORM_SCALE(h)	(0x00002284u + (h) * 0x400u)
#define NVC57D_HEAD_SET_CONTEXT_DMA_OLUT(h)	(0x00002288u + (h) * 0x400u)
#define NVC57D_HEAD_SET_OFFSET_OLUT(h)		(0x0000228cu + (h) * 0x400u)
#define NVC57D_HEAD_SET_TILE_POSITION(h)	(0x000022d0u + (h) * 0x400u)
#define NVC57D_WINDOW_SET_CONTROL(w)		(0x00001000u + (w) * 0x80u)
#define NVC57D_WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(w) \
						(0x00001008u + (w) * 0x80u)
#define NVC57D_WINDOW_SET_WINDOW_USAGE_BOUNDS(w)	(0x00001010u + (w) * 0x80u)
#define NVC57D_SOR_SET_CONTROL(s)		(0x00000300u + (s) * 0x20u)
/* Window image methods (clc57e.h). */
#define NVC57E_SET_PRESENT_CONTROL		0x00000308u
#define NVC57E_SET_SEMAPHORE_CONTROL		0x0000020cu
#define NVC57E_SET_SEMAPHORE_ACQUIRE		0x00000210u
#define NVC57E_SET_SEMAPHORE_RELEASE		0x00000214u
#define NVC57E_SET_CONTEXT_DMA_SEMAPHORE	0x00000218u
#define NVC57E_SEMAPHORE_CONTROL_OFFSET(o)	((o) & 0xffu)
#define NVC57E_SET_CONTEXT_DMA_NOTIFIER		0x0000021cu
#define NVC57E_SET_NOTIFIER_CONTROL		0x00000220u
#define NVC57E_NOTIFIER_CONTROL_OFFSET(o)	(((o) & 0xffu) << 4)
#define NVC57E_SET_STORAGE			0x00000228u
#define NVC57E_SET_PARAMS			0x0000022cu
#define NVC57E_SET_PLANAR_STORAGE(p)		(0x00000230u + (p) * 0x4u)
#define NVC57E_SET_CONTEXT_DMA_ISO(p)		(0x00000240u + (p) * 0x4u)
#define NVC57E_SET_OFFSET(p)			(0x00000260u + (p) * 0x4u)
#define NVC57E_SET_POINT_IN(p)			(0x00000290u + (p) * 0x4u)
#define NVC57E_SET_SIZE_IN			0x00000298u
#define NVC57E_SET_COMPOSITION_CONTROL		0x000002ecu
#define NVC57E_SET_COMPOSITION_CONSTANT_ALPHA	0x000002f0u
#define NVC57E_SET_COMPOSITION_FACTOR_SELECT	0x000002f4u
#define NVC57E_SET_KEY_ALPHA			0x000002f8u
#define NVC57E_SET_KEY_RED_CR			0x000002fcu
#define NVC57E_SET_KEY_GREEN_Y			0x00000300u
#define NVC57E_SET_KEY_BLUE_CB			0x00000304u
#define NVC57E_SET_FMT_COEFFICIENT(i)		(0x00000400u + (i) * 0x4u)
#define NVC57E_SET_ILUT_CONTROL		0x00000440u
#define NVC57E_SET_CONTEXT_DMA_ILUT		0x00000444u
#define NVC57E_SET_OFFSET_ILUT			0x00000448u
#define NVC57E_UPDATE				0x00000200u
#define NVC57E_SET_INTERLOCK_FLAGS		0x00000370u
#define NVC57E_SET_WINDOW_INTERLOCK_FLAGS	0x00000374u
#define NVC57E_SET_PARAMS_FORMAT_I8		0x0000001eu
#define NVC57E_SET_PARAMS_FORMAT_Y8_U8__Y8_V8_N422 0x00000028u
#define NVC57E_SET_PARAMS_FORMAT_U8_Y8__V8_Y8_N422 0x00000029u
#define NVC57E_SET_PARAMS_FORMAT_R5G6B5		0x000000e8u
#define NVC57E_SET_PARAMS_FORMAT_A1R5G5B5	0x000000e9u
#define NVC57E_SET_PARAMS_FORMAT_A8R8G8B8	0x000000cfu
#define NVC57E_SET_PARAMS_FORMAT_A8B8G8R8	0x000000d5u
#define NVC57E_SET_PARAMS_FORMAT_A2R10G10B10	0x000000dfu
#define NVC57E_SET_PARAMS_FORMAT_A2B10G10R10	0x000000d1u
#define NVC57E_SET_PARAMS_FORMAT_RF16_GF16_BF16_AF16 0x000000cau
#define NVC57E_SET_STORAGE_MEMORY_LAYOUT_PITCH	(1u << 4)
#define NVC57E_SET_PRESENT_CONTROL_INTERVAL_1	0x00000001u
#define NVC57E_SET_PRESENT_CONTROL_BEGIN_IMMEDIATE 0x00000001u
#define NVC57E_ILUT_CONTROL_IDENTITY_DIRECT10	((2u << 2) | ((4u + 1024u + 1u) << 8))
#define NVC57E_COMPOSITION_DEPTH_PRIMARY	(0xffu << 4)
#define NVC57E_COMPOSITION_ALPHA_OPAQUE		0x000000ffu
#define NVC57E_COMPOSITION_FACTOR_PIXEL_NONE	0x00004422u
#define NVC57E_KEY_RANGE_FULL			0xffff0000u
#define NVC57E_CSC_IDENTITY_SCALE		0x00010000u
#define NVC57D_SOR_PROTOCOL_SINGLE_TMDS_A	(1u << 8)
#define NVC57D_SOR_PROTOCOL_SINGLE_TMDS_B	(2u << 8)
#define NVC57D_OUTPUT_RESOURCE_HSYNC_NEGATIVE	(1u << 2)
#define NVC57D_OUTPUT_RESOURCE_VSYNC_NEGATIVE	(1u << 3)
#define NVC57D_OUTPUT_RESOURCE_PIXEL_DEPTH_BPP_24_444 (4u << 4)
#define NVC57D_OUTPUT_RESOURCE_EXT_PACKET_WIN_NONE (0x3fu << 26)
#define NVC57D_HEAD_USAGE_BOUNDS_DEFAULT \
	(0x4u | (1u << 4) | (1u << 8) | (1u << 12))
#define NVC57D_OLUT_CONTROL_IDENTITY_DIRECT10	((1u << 0) | (2u << 2) | ((4u + 1024u + 1u) << 8))
#define DISP_OLUT_VRAM_SIZE			0x3000u
#define DISP_ILUT_VRAM_SIZE			0x3000u
#define DISP_CORE_PUSH_BYTES			0x1000u
#define DISP_WINDOW_PUSH_BYTES			0x1000u
#define DISP_WIMM_PUSH_BYTES			0x1000u
#define NV50_DISP_HANDLE_WNDW_ISO		0xfb000000u	/* window ISO ctxdma handle */
#define NV50_DISP_HANDLE_WNDW_CTX(kind)		(0xfb000000u | ((kind) & 0xffu))

#define NVC57B_UPDATE				0x00000200u
#define NVC57B_UPDATE_INTERLOCK_WITH_WINDOW	(1u << 1)
#define NVC57B_SET_POINT_OUT(b)			(0x00000208u + (b) * 0x4u)
#define NVC57A_UPDATE				0x00000200u
#define NVC57A_SET_CURSOR_HOT_SPOT_POINT_OUT(b) (0x00000208u + (b) * 0x4u)

/* NV0073_CTRL_CMD_DFP_ASSIGN_SOR (MIT, open-gpu 570.144 via nvrm/disp.h). The
 * NvU8/NvU32 mix relies on natural alignment matching the firmware struct. */
#define NV0073_CTRL_CMD_DFP_ASSIGN_SOR		0x00731152u
#define DFP_ASSIGN_SOR_MAX_SORS			4u
#define NV0073_CTRL_DFP_ASSIGN_SOR_FLAGS_AUDIO_OPTIMAL	1u
struct disp_dfp_assign_sor_info {
	uint32_t displayMask;
	uint32_t sorType;
};
struct disp_dfp_assign_sor_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint8_t  sorExcludeMask;
	uint32_t slaveDisplayId;
	uint32_t forceSublinkConfig;
	uint8_t  bIs2Head1Or;
	uint32_t sorAssignList[DFP_ASSIGN_SOR_MAX_SORS];
	struct disp_dfp_assign_sor_info sorAssignListWithTag[DFP_ASSIGN_SOR_MAX_SORS];
	uint8_t  reservedSorMask;
	uint32_t flags;
};

/* NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_ENABLE (MIT, r535/nvrm/disp.h). Activates
 * the HDMI/TMDS encoder for a displayId. nouveau's r535_sor_hdmi_ctrl issues
 * this from nv50_sor_atomic_enable before the head/core UPDATE; without it the
 * head timing latches but no TMDS output is driven. Natural-aligned struct
 * matches the finn layout {u8 @0, u32 @4, u8 @8} = 12 bytes. */
#define NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_ENABLE	0x00730273u
struct disp_set_hdmi_enable_params {
	uint8_t  subDeviceInstance;
	uint32_t displayId;
	uint8_t  enable;
};

/* NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE (MIT, r570/open-rm ctrl0073).
 * NVIDIA's modeset path brackets each modeset with START/END so RM/GSP can
 * synchronize display ownership before the EVO UPDATE that promotes arm->live. */
#define NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE	0x007302a4u
#define NV0073_CTRL_SPECIFIC_DISPLAY_CHANGE_END	0x00000000u
#define NV0073_CTRL_SPECIFIC_DISPLAY_CHANGE_START 0x00000001u
struct disp_display_change_params {
	uint32_t subDeviceInstance;
	uint32_t newDevices;
	uint32_t properties;
	uint32_t enable;
};

/* r535 output/SOR RM controls from open-rm 570.144 ctrl0073*.h. */
#define NV0073_CTRL_CMD_SPECIFIC_GET_BACKLIGHT_BRIGHTNESS 0x00730291u
#define NV0073_CTRL_CMD_SPECIFIC_SET_BACKLIGHT_BRIGHTNESS 0x00730292u
#define NV0073_CTRL_SPECIFIC_BACKLIGHT_BRIGHTNESS_TYPE_PERCENT100 1u
struct disp_backlight_brightness_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t brightness;
	uint8_t  bUncalibrated;
	uint8_t  brightnessType;
	uint8_t  _pad[2];
};

#define NV0073_CTRL_CMD_DFP_SET_ELD_AUDIO_CAPS		0x00731144u
#define NV0073_CTRL_DFP_ELD_AUDIO_CAPS_ELD_BUFFER	96u
#define NV0073_CTRL_DFP_ELD_AUDIO_CAPS_CTRL_PD_TRUE	(1u << 0)
#define NV0073_CTRL_DFP_ELD_AUDIO_CAPS_CTRL_ELDV_TRUE	(1u << 1)
struct disp_dfp_set_eld_audio_caps_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t numELDSize;
	uint8_t  bufferELD[NV0073_CTRL_DFP_ELD_AUDIO_CAPS_ELD_BUFFER];
	uint32_t maxFreqSupported;
	uint32_t ctrl;
	uint32_t deviceEntry;
};

#define NV0073_CTRL_CMD_DFP_SET_AUDIO_ENABLE		0x00731150u
struct disp_dfp_set_audio_enable_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint8_t  enable;
	uint8_t  _pad[3];
};

#define NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_AUDIO_MUTESTREAM 0x00730275u
struct disp_set_hdmi_audio_mutestream_params {
	uint8_t  subDeviceInstance;
	uint8_t  _pad0[3];
	uint32_t displayId;
	uint8_t  mute;
	uint8_t  _pad1[3];
};

#define NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_SINK_CAPS	0x00730293u
#define NV0073_CTRL_HDMI_SINK_CAP_GT_340MHZ		(1u << 0)
#define NV0073_CTRL_HDMI_SINK_CAP_LTE_340MHZ_SCRAMBLE	(1u << 1)
#define NV0073_CTRL_HDMI_SINK_CAP_SCDC			(1u << 2)
struct disp_set_hdmi_sink_caps_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t caps;
};

#define NV0073_CTRL_CMD_SPECIFIC_SET_OD_PACKET		0x00730288u
#define NV0073_CTRL_SET_OD_MAX_PACKET_SIZE		36u
#define NV0073_CTRL_OD_PACKET_TRANSMIT_ENABLE		(1u << 0)
struct disp_set_od_packet_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t transmitControl;
	uint32_t packetSize;
	uint32_t targetHead;
	uint8_t  bUsePsrHeadforSdp;
	uint8_t  aPacket[NV0073_CTRL_SET_OD_MAX_PACKET_SIZE];
	uint8_t  _pad[3];
};

#define NV0073_CTRL_CMD_DP_SET_AUDIO_MUTESTREAM		0x00731359u
struct disp_dp_set_audio_mutestream_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t mute;
};

#define NV0073_CTRL_CMD_DP_CONFIG_STREAM		0x00731362u
struct disp_dp_config_stream_params {
	uint32_t subDeviceInstance;
	uint32_t head;
	uint32_t sorIndex;
	uint32_t dpLink;
	uint8_t  bEnableOverride;
	uint8_t  bMST;
	uint8_t  _pad0[2];
	uint32_t singleHeadMultistreamMode;
	uint32_t hBlankSym;
	uint32_t vBlankSym;
	uint32_t colorFormat;
	uint8_t  bEnableTwoHeadOneOr;
	uint8_t  _pad1[3];
	struct {
		uint32_t slotStart;
		uint32_t slotEnd;
		uint32_t PBN;
		uint32_t Timeslice;
		uint8_t  sendACT;
		uint8_t  _pad[3];
		uint32_t singleHeadMSTPipeline;
		uint8_t  bEnableAudioOverRightPanel;
		uint8_t  _pad2[3];
	} MST;
	struct {
		uint8_t  bEnhancedFraming;
		uint8_t  _pad[3];
		uint32_t tuSize;
		uint32_t waterMark;
		uint8_t  bEnableAudioOverRightPanel;
		uint8_t  _pad2[3];
	} SST;
};

#define NV0073_CTRL_CMD_DP_CTRL				0x00731343u
#define NV0073_CTRL_DP_CMD_SET_LANE_COUNT_TRUE		(1u << 0)
#define NV0073_CTRL_DP_CMD_SET_LINK_BW_TRUE		(1u << 1)
#define NV0073_CTRL_DP_CMD_SET_FORMAT_MODE_MST		(1u << 4)
#define NV0073_CTRL_DP_CMD_SET_ENHANCED_FRAMING_TRUE	(1u << 7)
#define NV0073_CTRL_DP_CMD_POST_LT_ADJ_REQ_GRANTED_YES	(1u << 10)
#define NV0073_CTRL_DP_CMD_TRAIN_PHY_REPEATER_YES	(1u << 13)
#define NV0073_CTRL_DP_DATA_LANE_COUNT(v)		(((v) & 0x1fu) << 0)
#define NV0073_CTRL_DP_DATA_LINK_BW(v)			(((v) & 0xffu) << 8)
#define NV0073_CTRL_DP_DATA_TARGET(v)			(((v) & 0x0fu) << 19)
struct disp_dp_ctrl_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t cmd;
	uint32_t data;
	uint32_t err;
	uint32_t retryTimeMs;
	uint32_t eightLaneDpcdBaseAddr;
};

#define NV0073_CTRL_CMD_DP_AUXCH_CTRL			0x00731341u
#define NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE		16u
#define NV0073_CTRL_DP_AUXCH_CMD_TYPE_AUX		(1u << 3)
#define NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_WRITE		0u
#define NV0073_CTRL_DP_AUXCH_REPLYTYPE_INVALID_ARGUMENT 0xffffffffu
struct disp_dp_auxch_ctrl_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint8_t  bAddrOnly;
	uint8_t  _pad[3];
	uint32_t cmd;
	uint32_t addr;
	uint8_t  data[NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE];
	uint32_t size;
	uint32_t replyType;
	uint32_t retryTimeMs;
};

#define NV0073_CTRL_CMD_DP_SET_LANE_DATA		0x00731346u
#define NV0073_CTRL_MAX_LANES				8u
#define NV0073_CTRL_DP_LANE_DATA(pe, vs)		(((pe) & 0x3u) | (((vs) & 0x3u) << 2))
struct disp_dp_lane_data_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t numLanes;
	uint32_t data[NV0073_CTRL_MAX_LANES];
};

#define NV0073_CTRL_CMD_DP_CONFIG_INDEXED_LINK_RATES	0x00731377u
#define NV0073_CTRL_DP_MAX_INDEXED_LINK_RATES		8u
struct disp_dp_config_indexed_link_rates_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint16_t linkRateTbl[NV0073_CTRL_DP_MAX_INDEXED_LINK_RATES];
	uint16_t linkBwTbl[NV0073_CTRL_DP_MAX_INDEXED_LINK_RATES];
	uint8_t  linkBwCount;
	uint8_t  _pad[3];
};

#define NV0073_CTRL_CMD_DP_TOPOLOGY_ALLOCATE_DISPLAYID	0x0073135bu
#define NV0073_CTRL_CMD_DP_TOPOLOGY_FREE_DISPLAYID	0x0073135cu
#define NV0073_CTRL_CMD_DP_INVALID_PREFERRED_DISPLAY_ID 0xffffffffu
struct disp_dp_topology_allocate_displayid_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
	uint32_t preferredDisplayId;
	uint8_t  force;
	uint8_t  useBFM;
	uint8_t  _pad[2];
	uint32_t displayIdAssigned;
	uint32_t allDisplayMask;
};
struct disp_dp_topology_free_displayid_params {
	uint32_t subDeviceInstance;
	uint32_t displayId;
};

#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER 0x20800a58u
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

struct disp_channelpio_alloc_params {
	uint32_t channelInstance;
	uint32_t hObjectNotify;
	uint64_t pControl;	/* NV_ALIGN_BYTES(8) */
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
#define EVO_WINDOW_USAGE_BOUNDS			0x00117fffu

static inline uint32_t
evo_method_hdr(uint32_t mthd, uint32_t count)
{
	/* OPCODE_METHOD(0) | METHOD_COUNT[27:18] | METHOD_OFFSET[13:2]. */
	return ((count & 0x3ffu) << 18) | (mthd & 0x3ffcu);
}

static inline uint32_t
evo_jump_hdr(uint32_t offset_dw)
{
	/* OPCODE_JUMP(1) | JUMP_OFFSET[11:2]. offset is a dword offset. */
	return ((1u << 29) | ((offset_dw << 2) & 0xffcu));
}

static void
disp_log_push_range(struct nvkm_softc *sc, const char *tag,
    volatile uint32_t *pb, uint32_t start, uint32_t end)
{
	uint32_t i, max;

	if (pb == NULL || start >= end)
		return;

	i = start;
	max = start + 64u;
	if (max > end)
		max = end;

	while (i < max) {
		uint32_t hdr = pb[i];
		uint32_t count = (hdr >> 18) & 0x3ffu;
		uint32_t method = hdr & 0x3ffcu;
		uint32_t left = end - i - 1u;

		if (count == 0 || count > left) {
			nvkm_infof(sc->dev,
			    "gsp_disp: PB %s[%u] raw=0x%x count=%u "
			    "left=%u -- stop\n",
			    tag, i, hdr, count, left);
			break;
		}

		if (count == 1) {
			nvkm_infof(sc->dev,
			    "gsp_disp: PB %s[%u] m=0x%x c=1 d0=0x%x\n",
			    tag, i, method, pb[i + 1u]);
		} else if (count == 2) {
			nvkm_infof(sc->dev,
			    "gsp_disp: PB %s[%u] m=0x%x c=2 d0=0x%x "
			    "d1=0x%x\n",
			    tag, i, method, pb[i + 1u], pb[i + 2u]);
		} else if (count == 3) {
			nvkm_infof(sc->dev,
			    "gsp_disp: PB %s[%u] m=0x%x c=3 d0=0x%x "
			    "d1=0x%x d2=0x%x\n",
			    tag, i, method, pb[i + 1u], pb[i + 2u],
			    pb[i + 3u]);
		} else {
			nvkm_infof(sc->dev,
			    "gsp_disp: PB %s[%u] m=0x%x c=%u d0=0x%x "
			    "d1=0x%x d2=0x%x dlast=0x%x\n",
			    tag, i, method, count, pb[i + 1u], pb[i + 2u],
			    pb[i + 3u], pb[i + count]);
		}
		i += count + 1u;
	}
}

/* ===== M4b: display instmem (RAMHT + ctxdma DMA-objects) =====
 *
 * The NVDisplay fixed-function front-end resolves EVO context-DMA handles
 * (SET_CONTEXT_DMA_ISO / SET_CONTEXT_DMA_NOTIFIER) by reading a hash table +
 * DMA-object descriptors out of the display RAMIN (inst_paddr) physically. The
 * host allocates the RAMIN, writes these structures, and hands the phys addr to
 * GSP via WRITE_INST_MEM; GSP just points the display HW at it. The on-chip
 * layout is the Turing HW ABI. M4b kept the open-rm v03 hash as a format
 * self-test; M4e adds nouveau's nvkm_ramht_insert hash/context semantics on the
 * same HW byte layout. RAMIN layout:
 *   [0x0000, 0x2000)  hash table: 1024 entries x 8 bytes {handle, context}
 *   [0x2000, 0x10000) ctxdma DMA-object descriptors, 32-byte aligned
 */
#define DISP_RAMHT_BASE		0x0000u		/* NV_UDISP_HASH_BASE */
#define DISP_RAMHT_SIZE		0x2000u		/* NV_UDISP_HASH_LIMIT+1: 1024 entries */
#define DISP_RAMHT_SLOTS	(DISP_RAMHT_SIZE / 8u)
#define DISP_RAMHT_BITS		10u
#define DISP_RAMHT_MASK		((1u << DISP_RAMHT_BITS) - 1u)
#define DISP_DESCR_BASE		0x2000u		/* NV_UDISP_OBJ_MEM_BASE */
#define DISP_DESCR_STRIDE	0x20u		/* 32-byte-aligned ctxdma objects */
#define DISP_INSTMEM_PAGES	4u		/* BAR1-mapped RAMIN pages (hash 2 + descr 2) */
#define DISP_INSTMEM_BYTES	(DISP_INSTMEM_PAGES * 0x1000u)
/* gv100 DMA-object dword0: TARGET_NODE_PHYSICAL_NVM(1) | ACCESS_RW(1<<2) |
 * KIND_PITCH(0), plus optional PAGE_SP(1<<6). nouveau's core sync/vram DMA
 * objects use the small-page fallback; the window pitch object passes LP. */
#define DISP_DMAOBJ_FLAGS0_VRAM_RW_LP	0x00000005u
#define DISP_DMAOBJ_FLAGS0_VRAM_RW_SP	0x00000045u
#define DISP_DMAOBJ_FLAGS0_VRAM_RW	DISP_DMAOBJ_FLAGS0_VRAM_RW_LP

/* RAMIN byte-offset access across the BAR1-mapped pages. */
static void
disp_instmem_wr32(struct nvkm_softc *sc, uint32_t off, uint32_t val)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	nvkm_gsp_bar1_wr32(sc, disp->instmem_gva[off >> 12] + (off & 0xfffu), val);
}

static uint32_t
disp_instmem_rd32(struct nvkm_softc *sc, uint32_t off)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	return (nvkm_gsp_bar1_rd32(sc,
	    disp->instmem_gva[off >> 12] + (off & 0xfffu)));
}

/* NVDisplay v03 hash (instmemHashFunc_v03_00): folds the ctxdma handle with the
 * client handle and display channel number into a 10-bit slot index. */
static uint32_t
disp_ramht_hash(uint32_t handle, uint32_t client, uint32_t chn)
{
	return ((((handle >> 0)  & 0x3ffu) ^
		 ((handle >> 10) & 0x3ffu) ^
		 ((handle >> 20) & 0x3ffu) ^
		 (((client & 0xffu) << 2) | (handle >> 30)) ^
		 (((chn & 0xfu) << 6) | ((client >> 8) & 0x3fu)) ^
		 ((chn >> 4) & 0x7u)) & 0x3ffu);
}

/* nouveau nvkm_ramht_hash() for a 1024-entry display RAMHT. */
static uint32_t
disp_ramht_hash_nouveau(uint32_t handle, uint32_t chn)
{
	uint32_t hash = 0;

	while (handle != 0) {
		hash ^= handle & DISP_RAMHT_MASK;
		handle >>= DISP_RAMHT_BITS;
	}
	hash ^= (chn & 0x7fu) << (DISP_RAMHT_BITS - 4);
	return (hash & DISP_RAMHT_MASK);
}

/* Write a 32-byte v03/gv100 ctxdma descriptor at RAMIN byte offset off.
 * start/limit are VRAM physical addresses; the descriptor stores them >>8. */
static void
disp_dmaobj_write_flags(struct nvkm_softc *sc, uint32_t off, uint32_t flags0,
    uint64_t start, uint64_t limit)
{
	uint64_t s = start >> 8;
	uint64_t l = limit >> 8;

	disp_instmem_wr32(sc, off + 0x00, flags0);
	disp_instmem_wr32(sc, off + 0x04, (uint32_t)s);
	disp_instmem_wr32(sc, off + 0x08, (uint32_t)(s >> 32) & 0x7fu);
	disp_instmem_wr32(sc, off + 0x0c, (uint32_t)l);
	disp_instmem_wr32(sc, off + 0x10, (uint32_t)(l >> 32) & 0x7fu);
	disp_instmem_wr32(sc, off + 0x14, 0);
	disp_instmem_wr32(sc, off + 0x18, 0);
	disp_instmem_wr32(sc, off + 0x1c, 0);
}

static void
disp_dmaobj_write(struct nvkm_softc *sc, uint32_t off, uint64_t start,
    uint64_t limit)
{
	disp_dmaobj_write_flags(sc, off, DISP_DMAOBJ_FLAGS0_VRAM_RW, start, limit);
}

/* Insert {handle -> descriptor at byte offset descr_off} for display channel
 * chn, using the v03 hash + context layout (context = CLIENT_ID[13:0] |
 * INSTANCE[24:14]=descr_off>>5 (32-byte chunk) | CHN[31:25]). Linear probe on
 * collision. Returns the slot index, or -1 if full. */
static int
disp_ramht_insert(struct nvkm_softc *sc, uint32_t handle, uint32_t chn,
    uint32_t descr_off)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t client = 0x00000040u;	/* nouveau gv100: fixed 0x40, NOT GSP client handle */
	uint32_t chunk = descr_off >> 5;
	uint32_t context = (client & 0x3fffu) | ((chunk & 0x7ffu) << 14) |
	    ((chn & 0x7fu) << 25);
	uint32_t co, ho;

	co = ho = disp_ramht_hash(handle, client, chn);
	do {
		uint32_t ent = DISP_RAMHT_BASE + co * 8;
		if (disp_instmem_rd32(sc, ent + 0) == 0) {
			disp_instmem_wr32(sc, ent + 0, handle);
			disp_instmem_wr32(sc, ent + 4, context);
			return ((int)co);
		}
		if (++co >= DISP_RAMHT_SLOTS)
			co = 0;
	} while (co != ho);

	return (-1);
}

/* Insert a nouveau r535/r570 display channel bind:
 *   nvkm_ramht_insert(ramht, object, chid.user, -9, handle,
 *       chid.user << 25 | (client_handle & 0x3fff))
 * with gv100_dmaobj_bind() returning a descriptor allocated inside display
 * RAMIN. descr_off is 32-byte aligned, so (descr_off << 9) does not collide
 * with the low client-handle bits. */
static int
disp_ramht_insert_nouveau(struct nvkm_softc *sc, uint32_t handle, uint32_t chn,
    uint32_t descr_off, uint32_t *out_context)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t client = 0x00000040u;	/* nouveau gv100: fixed 0x40, NOT GSP client handle */
	uint32_t context = client | (descr_off << 9) | ((chn & 0x7fu) << 25);
	uint32_t co, ho;

	co = ho = disp_ramht_hash_nouveau(handle, chn);
	do {
		uint32_t ent = DISP_RAMHT_BASE + co * 8;
		if (disp_instmem_rd32(sc, ent + 0) == 0) {
			disp_instmem_wr32(sc, ent + 0, handle);
			disp_instmem_wr32(sc, ent + 4, context);
			if (out_context != NULL)
				*out_context = context;
			return ((int)co);
		}
		if (++co >= DISP_RAMHT_SLOTS)
			co = 0;
	} while (co != ho);

	return (-1);
}

static int
disp_descr_alloc(struct nvkm_softc *sc, uint32_t *out_off)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t off;

	off = (disp->descr_next + (DISP_DESCR_STRIDE - 1u)) &
	    ~(DISP_DESCR_STRIDE - 1u);
	if (off < DISP_DESCR_BASE)
		off = DISP_DESCR_BASE;
	if (off + DISP_DESCR_STRIDE > DISP_INSTMEM_BYTES) {
		nvkm_infof(sc->dev,
		    "gsp_disp: M4e descriptor RAMIN full next=0x%x limit=0x%x\n",
		    off, DISP_INSTMEM_BYTES);
		return (ENOSPC);
	}

	disp->descr_next = off + DISP_DESCR_STRIDE;
	*out_off = off;
	return (0);
}

static int
disp_ramht_bind_nouveau(struct nvkm_softc *sc, uint32_t handle, uint32_t chn,
    uint32_t flags0, uint64_t start, uint64_t limit, const char *label)
{
	uint32_t context = 0, off;
	int err, slot;

	if (limit < start)
		return (EINVAL);

	err = disp_descr_alloc(sc, &off);
	if (err != 0)
		return (err);
	disp_dmaobj_write_flags(sc, off, flags0, start, limit);
	slot = disp_ramht_insert_nouveau(sc, handle, chn, off, &context);
	if (slot < 0)
		return (ENOSPC);

	nvkm_infof(sc->dev,
	    "gsp_disp: M4e ramht %s h=0x%x chn=%u slot=%d off=0x%x ctx=0x%x "
	    "flags0=0x%x start=0x%llx limit=0x%llx\n",
	    label, handle, chn, slot, off, context, flags0,
	    (unsigned long long)start, (unsigned long long)limit);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4e ramht-readback %s slot=%d ent={0x%x,0x%x} "
	    "desc={0x%x,0x%x,0x%x,0x%x,0x%x}\n",
	    label, slot,
	    disp_instmem_rd32(sc, DISP_RAMHT_BASE + slot * 8u + 0u),
	    disp_instmem_rd32(sc, DISP_RAMHT_BASE + slot * 8u + 4u),
	    disp_instmem_rd32(sc, off + 0x00u),
	    disp_instmem_rd32(sc, off + 0x04u),
	    disp_instmem_rd32(sc, off + 0x08u),
	    disp_instmem_rd32(sc, off + 0x0cu),
	    disp_instmem_rd32(sc, off + 0x10u));
	return (0);
}

/* Display test surfaces are RM-owned local VRAM allocations. The EVO ctxdma
 * handles are bound by host-written RAMHT entries, matching nouveau's display
 * path rather than NV0002_CTRL_CMD_BIND_CONTEXTDMA. */
#define NV01_MEMORY_LOCAL_USER		0x00000040u
/* NVOS32 attr: LOCATION_VIDMEM(0) | FORMAT_PITCH(0) |
 * PHYSICALITY_CONTIGUOUS(2<<27); type IMAGE(0). */
#define NVOS32_ATTR_CONTIG_VIDMEM_PITCH	(0x2u << 27)

struct disp_vidmem_params {		/* NV_MEMORY_ALLOCATION_PARAMS (NVOS32) */
	uint32_t owner, type, flags;
	uint32_t width, height;
	int32_t  pitch;
	uint32_t attr, attr2;
	uint32_t format, comprCovg, zcullCovg;
	uint32_t _pad0;
	uint64_t rangeLo, rangeHi;
	uint64_t size, alignment, offset, limit, address;
	uint32_t ctagOffset, hVASpace, internalflags, tag;
	int32_t  numaNode;
};

static void disp_clear_exception_chid(struct nvkm_softc *sc, uint32_t chid);
static void disp_dump_exception_chid_tag(struct nvkm_softc *sc,
    const char *tag, uint32_t chid, int clear);

/* Have RM allocate a contiguous VRAM block (RM owns the heap -- we cannot pin a
 * drm_mm address, see NV_ERR_NO_MEMORY) and read back the VRAM address it chose
 * (NV_MEMORY_ALLOCATION_PARAMS.offset is the [OUT] address). The memory object
 * is kept in *mem. */
static int
disp_vidmem_alloc(struct nvkm_softc *sc, struct nvkm_gsp_disp *disp,
    struct nvkm_gsp_object *mem, uint32_t mem_handle, uint64_t size,
    uint64_t *out_paddr)
{
	struct disp_vidmem_params *vm;
	void *p;
	int err;

	vm = nvkm_gsp_rm_alloc_get(&disp->device.object, mem_handle,
	    NV01_MEMORY_LOCAL_USER, sizeof(*vm), mem);
	if (vm == NULL)
		return (ENOMEM);
	memset(vm, 0, sizeof(*vm));
	vm->owner = 0x6d766b6eu;			/* 'nkvm' */
	vm->type = 0;					/* NVOS32_TYPE_IMAGE */
	vm->attr = NVOS32_ATTR_CONTIG_VIDMEM_PITCH;
	vm->size = size;
	p = vm;
	err = nvkm_gsp_rm_alloc_rd(mem, &p, sizeof(*vm));
	if (err != 0 || p == NULL) {
		nvkm_infof(sc->dev,
		    "gsp_disp: vidmem alloc h=0x%x err=%d\n", mem_handle, err);
		if (p != NULL)
			nvkm_gsp_rm_alloc_done(mem, p);
		return (EIO);
	}
	*out_paddr = ((struct disp_vidmem_params *)p)->offset;
	nvkm_infof(sc->dev,
	    "gsp_disp: vidmem h=0x%x size=0x%llx -> paddr=0x%llx\n",
	    mem_handle, (unsigned long long)size,
	    (unsigned long long)*out_paddr);
	nvkm_gsp_rm_alloc_done(mem, p);
	return (0);
}

static void
disp_push_flush(volatile uint32_t *pb, uint32_t bufdw)
{
	if (pb == NULL || bufdw == 0)
		return;

	pmap_invalidate_cache_range((vm_offset_t)pb,
	    (vm_offset_t)pb + (vm_offset_t)bufdw * sizeof(uint32_t));
	cpu_sfence();
}

static int
disp_chan_emit(struct nvkm_softc *sc, volatile uint32_t *pb, uint32_t put_reg,
    uint32_t *put_cur, uint32_t bufdw, const uint32_t *data, uint32_t ndw,
    uint32_t *out_put)
{
	uint32_t base = *put_cur;
	uint32_t get, i, put_dw;
	int us;

	if (ndw >= bufdw)
		return (ENOSPC);
	if (base >= bufdw)
		base = 0;
	if (base + ndw >= bufdw) {
		get = nvkm_rd32(sc, put_reg + 4);
		if (get == 0) {
			for (us = 0; us < 2000000; us += 10) {
				get = nvkm_rd32(sc, put_reg + 4);
				if (get != 0)
					break;
				DELAY(10);
			}
			if (get == 0)
				return (ETIMEDOUT);
		}

		pb[base] = evo_jump_hdr(0);
		disp_push_flush(pb, bufdw);
		nvkm_wr32(sc, put_reg, 0);
		for (us = 0; us < 2000000; us += 10) {
			get = nvkm_rd32(sc, put_reg + 4);
			if (get == 0)
				break;
			DELAY(10);
		}
		if (get != 0)
			return (ETIMEDOUT);
		nvkm_infof(sc->dev,
		    "gsp_disp: channel wind put_reg=0x%x base=%u GET=%u\n",
		    put_reg, base, get);
		base = 0;
		*put_cur = 0;
	}
	for (i = 0; i < ndw; i++)
		pb[base + i] = data[i];
	disp_push_flush(pb, bufdw);
	put_dw = base + ndw;
	nvkm_wr32(sc, put_reg, put_dw);
	*put_cur = put_dw;
	if (out_put != NULL)
		*out_put = put_dw;
	return (0);
}

static uint32_t
disp_chan_wait_get(struct nvkm_softc *sc, uint32_t put_reg, uint32_t put_dw,
    uint32_t timeout_us)
{
	uint32_t get;
	int us;

	get = nvkm_rd32(sc, put_reg + 4);
	for (us = 0; us < (int)timeout_us; us += 10) {
		get = nvkm_rd32(sc, put_reg + 4);
		if (get == put_dw)
			break;
		DELAY(10);
	}
	return (get);
}

/* Append a method batch and kick PUT. Linux nouveau's PUSH_KICK does not
 * synchronously wait for GET; callers may sample GET separately for logs. */
static int
disp_chan_push(struct nvkm_softc *sc, volatile uint32_t *pb, uint32_t put_reg,
    uint32_t *put_cur, uint32_t bufdw, const uint32_t *data, uint32_t ndw)
{
	return (disp_chan_emit(sc, pb, put_reg, put_cur, bufdw, data, ndw,
	    NULL));
}

/*
 * M4b: bring up the display instmem object model in the v03 (Turing) HW format.
 * Map the first DISP_INSTMEM_PAGES of the display RAMIN into BAR1 for host
 * writes, zero the hash table, and self-test the v03 hash/context/descriptor
 * encoders against a read-back.
 */
static int
nvkm_gsp_disp_instmem_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t i;
	int err;

	for (i = 0; i < DISP_INSTMEM_PAGES; i++) {
		err = nvkm_gsp_bar1_map_existing(sc,
		    disp->inst_paddr + (uint64_t)i * 0x1000u,
		    &disp->instmem_gva[i]);
		if (err != 0) {
			nvkm_infof(sc->dev,
			    "gsp_disp: instmem page %u map err=%d\n", i, err);
			while (i-- > 0) {
				nvkm_gsp_bar1_unmap_existing(sc, disp->instmem_gva[i]);
				disp->instmem_gva[i] = 0;
			}
			return (err);
		}
	}
	disp->descr_next = DISP_DESCR_BASE;

	/* Zero the hash table so all slots read empty (handle==0). */
	for (i = DISP_RAMHT_BASE; i < DISP_RAMHT_BASE + DISP_RAMHT_SIZE; i += 4)
		disp_instmem_wr32(sc, i, 0);
	nvkm_gsp_bar1_flush(sc);

	/* Self-test: write one whole-VRAM descriptor + hash entry (v03 format),
	 * read back, bit-verify, then revert to a clean empty table. */
	{
		const uint32_t h = 0xfeed0000u, chn = 0u, off = DISP_DESCR_BASE;
		uint32_t client = 0x00000040u;	/* nouveau gv100: fixed 0x40, NOT GSP client handle */
		uint32_t chunk = off >> 5;
		uint32_t want_ctx = (client & 0x3fffu) |
		    ((chunk & 0x7ffu) << 14) | ((chn & 0x7fu) << 25);
		uint64_t lim = sc->fb_usable_size - 1;
		uint64_t l = lim >> 8;
		int slot;
		uint32_t e_h, e_c, d0, d1, d3;
		int ok;

		disp_dmaobj_write(sc, off, 0, lim);
		slot = disp_ramht_insert(sc, h, chn, off);
		nvkm_gsp_bar1_flush(sc);

		e_h = e_c = 0;
		if (slot >= 0) {
			e_h = disp_instmem_rd32(sc,
			    DISP_RAMHT_BASE + (uint32_t)slot * 8 + 0);
			e_c = disp_instmem_rd32(sc,
			    DISP_RAMHT_BASE + (uint32_t)slot * 8 + 4);
		}
		d0 = disp_instmem_rd32(sc, off + 0x00);
		d1 = disp_instmem_rd32(sc, off + 0x04);
		d3 = disp_instmem_rd32(sc, off + 0x0c);
		ok = (slot >= 0) && (e_h == h) && (e_c == want_ctx) &&
		    (d0 == DISP_DMAOBJ_FLAGS0_VRAM_RW) && (d1 == 0) &&
		    (d3 == (uint32_t)l);
		nvkm_infof(sc->dev,
		    "gsp_disp: M4b instmem self-test (v03): slot=%d ent={0x%x,0x%x} "
		    "want_ctx=0x%x d0=0x%x d3=0x%x -> %s\n",
		    slot, e_h, e_c, want_ctx, d0, d3, ok ? "OK" : "MISMATCH");

		/* COHERENCE TEST: read the same bytes via PRAMIN (BAR0 direct VRAM,
		 * the GPU's working copy) instead of BAR1. If these differ from the
		 * BAR1 values above, our BAR1 writes are not landing in the VRAM the
		 * display HW reads (coherence bug). If they match, the HW reads a
		 * different inst mem than inst_paddr. */
		{
			uint32_t pbase = nvkm_rd32(sc, NV_PBUS_PRAMIN);
			uint32_t p_ctx, p_d0;
			uint32_t coff = (slot >= 0) ?
			    (DISP_RAMHT_BASE + (uint32_t)slot * 8 + 4) : 0;
			nvkm_wr32(sc, NV_PBUS_PRAMIN,
			    (uint32_t)(disp->inst_paddr >> 16));
			p_ctx = nvkm_rd32(sc, NV_PRAMIN + (coff & 0xffffu));
			p_d0 = nvkm_rd32(sc, NV_PRAMIN + (off & 0xffffu));
			nvkm_wr32(sc, NV_PBUS_PRAMIN, pbase);
			nvkm_infof(sc->dev,
			    "gsp_disp: M4b PRAMIN readback: ctx=0x%x d0=0x%x "
			    "(BAR1 ctx=0x%x d0=0x%x) -> %s\n",
			    p_ctx, p_d0, e_c, d0,
			    (p_ctx == e_c && p_d0 == d0) ?
			    "COHERENT (GPU sees our writes)" :
			    "INCOHERENT (GPU does NOT see our writes)");
		}

		if (slot >= 0) {
			disp_instmem_wr32(sc,
			    DISP_RAMHT_BASE + (uint32_t)slot * 8 + 0, 0);
			disp_instmem_wr32(sc,
			    DISP_RAMHT_BASE + (uint32_t)slot * 8 + 4, 0);
		}
		for (i = 0; i < DISP_DESCR_STRIDE; i += 4)
			disp_instmem_wr32(sc, off + i, 0);
		nvkm_gsp_bar1_flush(sc);
		if (!ok)
			return (EIO);
	}
	return (0);
}

static uint32_t
disp_gv100_dmaobj_flags(uint32_t page, uint32_t kind)
{
	uint32_t flags0 = 0x00000004u | 0x00000001u; /* ACCESS_RDWR | TARGET_VRAM */

	if (page != 0)
		flags0 |= 0x00000040u;
	if (kind != 0)
		flags0 |= 0x00100000u;
	return (flags0);
}

static int
disp_nouveau_bind_channel_ctxdma(struct nvkm_softc *sc, uint32_t chid,
    int bind_fb_ctx, uint32_t kind, const char *label)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint64_t vram_limit;
	uint32_t fb_handle, fb_flags;
	int err;

	if (disp->notifier_paddr == 0 || sc->fb_usable_size == 0)
		return (ENXIO);

	vram_limit = sc->fb_usable_size - 1;

	/* nv50_dmac_create(): nvif_object_ctor("kmsSyncCtxDma",
	 * NV50_DISP_HANDLE_SYNCBUF, NV_DMA_IN_MEMORY, nv_dma_v0 VRAM/RW,
	 * start=syncbuf, limit=syncbuf+0xfff).  The host implementation of that
	 * child object is gv100_dmaobj_bind() + r535_dmac_bind(), i.e. a RAMHT
	 * entry whose descriptor lives inside display RAMIN.
	 *
	 * gv100_dmaobj_new() receives only nv_dma_v0, so it falls back to the
	 * small-page VRAM/RW descriptor used by nouveau for sync/vram ctxdma.
	 */
	err = disp_ramht_bind_nouveau(sc, NV50_DISP_HANDLE_SYNCBUF, chid,
	    disp_gv100_dmaobj_flags(1, 0), disp->notifier_paddr,
	    disp->notifier_paddr + 0xfff, label);
	if (err != 0)
		return (err);

	/* nv50_dmac_create(): nvif_object_ctor("kmsVramCtxDma",
	 * NV50_DISP_HANDLE_VRAM, NV_DMA_IN_MEMORY, start=0,
	 * limit=ram_user-1). */
	err = disp_ramht_bind_nouveau(sc, NV50_DISP_HANDLE_VRAM, chid,
	    disp_gv100_dmaobj_flags(1, 0), 0, vram_limit, label);
	if (err != 0)
		return (err);

	if (!bind_fb_ctx)
		return (0);

	/* nv50_wndw_ctxdma_new(): window framebuffer CTXDMA, normally
	 * NV50_DISP_HANDLE_WNDW_CTX(kind).  First-light uses pitch kind 0. */
	fb_handle = NV50_DISP_HANDLE_WNDW_CTX(kind);
	fb_flags = disp_gv100_dmaobj_flags(0, kind);
	return (disp_ramht_bind_nouveau(sc, fb_handle, chid, fb_flags, 0,
	    vram_limit, label));
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

static int nvkm_gsp_disp_dfp_get_info(struct nvkm_softc *sc,
    uint32_t display_id);

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
	if (connected && nvkm_gsp_disp_dfp_get_info(sc, display_id) != 0)
		return (-1);
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

static int
nvkm_gsp_disp_dfp_get_info(struct nvkm_softc *sc, uint32_t display_id)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dfp_get_info_params *dfp;
	void *p;
	int err;

	if (disp == NULL)
		return (ENXIO);
	dfp = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DFP_GET_INFO, sizeof(*dfp));
	if (dfp == NULL)
		return (ENOMEM);
	memset(dfp, 0, sizeof(*dfp));
	dfp->displayId = display_id;
	p = dfp;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*dfp));
	if (err != 0 || p == NULL) {
		if (p != NULL)
			nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
		return (err != 0 ? err : EIO);
	}
	dfp = p;
	nvkm_infof(sc->dev,
	    "gsp_disp: DFP_GET_INFO display=0x%x flags=0x%x flags2=0x%x\n",
	    dfp->displayId, dfp->flags, dfp->flags2);
	nvkm_gsp_rm_ctrl_done(&disp->objcom, dfp);
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

		err = nvkm_gsp_disp_dfp_get_info(sc, 1u << id);
		if (err != 0) {
			nvkm_infof(sc->dev,
			    "gsp_disp: displayId=0x%x DFP_GET_INFO err=%d\n",
			    1u << id, err);
			continue;
		}

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

static int
disp_nouveau_get_supported(struct nvkm_softc *sc, uint32_t *out_mask)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_get_supported_params *sup;
	void *p;
	int err;

	if (disp == NULL || out_mask == NULL)
		return (EINVAL);

	sup = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, sizeof(*sup));
	if (sup == NULL)
		return (ENOMEM);
	memset(sup, 0, sizeof(*sup));
	p = sup;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*sup));
	if (err != 0 || p == NULL) {
		nvkm_infof(sc->dev, "gsp_disp: outp GET_SUPPORTED err=%d\n", err);
		return (err != 0 ? err : EIO);
	}
	*out_mask = ((struct disp_get_supported_params *)p)->displayMask;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
	disp->supported_mask = *out_mask;
	return (0);
}

static int
disp_nouveau_get_active(struct nvkm_softc *sc, uint32_t head,
    uint32_t *display_id)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_get_active_params *act;
	void *p;
	int err;

	if (disp == NULL || display_id == NULL)
		return (EINVAL);
	act = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE, sizeof(*act));
	if (act == NULL)
		return (ENOMEM);
	memset(act, 0, sizeof(*act));
	act->subDeviceInstance = 0;
	act->head = head;
	p = act;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*act));
	if (err != 0 || p == NULL) {
		if (p != NULL)
			nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
		return (err != 0 ? err : EIO);
	}
	act = p;
	*display_id = act->displayId;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, act);
	return (0);
}

static uint32_t
disp_nouveau_proto_link(uint32_t proto)
{
	switch (proto) {
	case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_SINGLE_TMDS_A:
	case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_A:
		return (1);
	case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_SINGLE_TMDS_B:
	case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_B:
		return (2);
	case NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DUAL_TMDS:
		return (3);
	default:
		return (0);
	}
}

static int
disp_nouveau_outp_inherit_probe(struct nvkm_softc *sc, uint32_t display_id)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t head;

	if (disp == NULL)
		return (ENXIO);
	for (head = 0; head < disp->num_heads && head < 32u; head++) {
		struct disp_or_get_info_params *or;
		uint32_t active, link;
		void *p;
		int err;

		err = disp_nouveau_get_active(sc, head, &active);
		if (err != 0) {
			nvkm_infof(sc->dev,
			    "gsp_disp: outp inherit GET_ACTIVE head=%u err=%d\n",
			    head, err);
			return (err);
		}
		if (active != display_id) {
			if (active != 0)
				nvkm_infof(sc->dev,
				    "gsp_disp: outp inherit head=%u active=0x%x "
				    "skip display=0x%x\n", head, active, display_id);
			continue;
		}

		or = nvkm_gsp_rm_ctrl_get(&disp->objcom,
		    NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO, sizeof(*or));
		if (or == NULL)
			return (ENOMEM);
		memset(or, 0, sizeof(*or));
		or->subDeviceInstance = 0;
		or->displayId = display_id;
		p = or;
		err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*or));
		if (err != 0 || p == NULL) {
			if (p != NULL)
				nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
			nvkm_infof(sc->dev,
			    "gsp_disp: outp inherit OR_GET_INFO display=0x%x err=%d\n",
			    display_id, err);
			return (err != 0 ? err : EIO);
		}
		or = p;
		link = disp_nouveau_proto_link(or->protocol);
		nvkm_infof(sc->dev,
		    "gsp_disp: outp inherit display=0x%x head=%u sor=%u "
		    "proto=%u link=%u\n",
		    display_id, head, or->index, or->protocol, link);
		nvkm_gsp_rm_ctrl_done(&disp->objcom, or);
		return (0);
	}

	return (0);
}

static int
disp_nouveau_conn_new_probe(struct nvkm_softc *sc, uint32_t display_id)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_get_connector_data_params *conn;
	void *p;
	int err;

	conn = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA, sizeof(*conn));
	if (conn == NULL)
		return (ENOMEM);
	memset(conn, 0, sizeof(*conn));
	conn->subDeviceInstance = 0;
	conn->displayId = display_id;
	p = conn;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*conn));
	if (err != 0 || p == NULL) {
		nvkm_infof(sc->dev,
		    "gsp_disp: outp GET_CONNECTOR_DATA display=0x%x err=%d\n",
		    display_id, err);
		return (err != 0 ? err : EIO);
	}
	conn = p;
	nvkm_infof(sc->dev,
	    "gsp_disp: outp connector display=0x%x count=%u index=%u type=%u "
	    "loc=%u ddc=0x%x platform=%u\n",
	    display_id, conn->count, conn->data[0].index, conn->data[0].type,
	    conn->data[0].location, conn->DDCPartners, conn->platform);
	nvkm_gsp_rm_ctrl_done(&disp->objcom, conn);
	return (0);
}

static int
disp_nouveau_dp_get_caps_probe(struct nvkm_softc *sc, uint32_t display_id)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_get_caps_params *caps;
	void *p;
	int err;

	caps = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DP_GET_CAPS, sizeof(*caps));
	if (caps == NULL)
		return (ENOMEM);
	memset(caps, 0, sizeof(*caps));
	caps->subDeviceInstance = 0;
	caps->sorIndex = ~0u;
	p = caps;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*caps));
	if (err != 0 || p == NULL) {
		nvkm_infof(sc->dev,
		    "gsp_disp: outp DP_GET_CAPS display=0x%x err=%d\n",
		    display_id, err);
		return (err != 0 ? err : EIO);
	}
	caps = p;
	nvkm_infof(sc->dev,
	    "gsp_disp: outp dp_caps display=0x%x maxLinkRate=%u mst=%u "
	    "wm=%u dpVer=0x%x uhbr=0x%x\n",
	    display_id, caps->maxLinkRate, caps->bIsMultistreamSupported,
	    caps->bHasIncreasedWatermarkLimits, caps->dpVersionsSupported,
	    caps->UHBRSupportedByGpu);
	nvkm_gsp_rm_ctrl_done(&disp->objcom, caps);
	return (0);
}

static void
disp_bar0_mask(struct nvkm_softc *sc, uint32_t addr, uint32_t mask,
    uint32_t val)
{
	uint32_t tmp = nvkm_rd32(sc, addr);

	tmp = (tmp & ~mask) | (val & mask);
	nvkm_wr32(sc, addr, tmp);
}

static int
disp_r535_bl_ctrl(struct nvkm_softc *sc, uint32_t display_id, int set,
    int *pval)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_backlight_brightness_params *bl;
	uint32_t cmd;
	void *p;
	int err;

	if (disp == NULL || pval == NULL)
		return (EINVAL);

	cmd = set ? NV0073_CTRL_CMD_SPECIFIC_SET_BACKLIGHT_BRIGHTNESS :
	    NV0073_CTRL_CMD_SPECIFIC_GET_BACKLIGHT_BRIGHTNESS;
	bl = nvkm_gsp_rm_ctrl_get(&disp->objcom, cmd, sizeof(*bl));
	if (bl == NULL)
		return (ENOMEM);
	memset(bl, 0, sizeof(*bl));
	bl->subDeviceInstance = 0;
	bl->displayId = display_id;
	bl->brightness = (uint32_t)*pval;
	bl->brightnessType =
	    NV0073_CTRL_SPECIFIC_BACKLIGHT_BRIGHTNESS_TYPE_PERCENT100;
	p = bl;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*bl));
	if (err != 0 || p == NULL) {
		if (p != NULL)
			nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
		return (err != 0 ? err : EIO);
	}
	bl = p;
	*pval = (int)bl->brightness;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, bl);
	return (0);
}

static int
disp_r535_sor_hda_eld(struct nvkm_softc *sc, uint32_t display_id,
    uint32_t head, const uint8_t *data, uint32_t size)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dfp_set_eld_audio_caps_params *eld;
	int err;

	if (disp == NULL)
		return (EINVAL);
	if (size > NV0073_CTRL_DFP_ELD_AUDIO_CAPS_ELD_BUFFER)
		return (E2BIG);
	if (size != 0 && data == NULL)
		return (EINVAL);

	eld = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DFP_SET_ELD_AUDIO_CAPS, sizeof(*eld));
	if (eld == NULL)
		return (ENOMEM);
	memset(eld, 0, sizeof(*eld));
	eld->subDeviceInstance = 0;
	eld->displayId = display_id;
	eld->numELDSize = size;
	if (size != 0)
		memcpy(eld->bufferELD, data, size);
	eld->maxFreqSupported = 0;
	eld->ctrl = NV0073_CTRL_DFP_ELD_AUDIO_CAPS_CTRL_PD_TRUE |
	    NV0073_CTRL_DFP_ELD_AUDIO_CAPS_CTRL_ELDV_TRUE;
	eld->deviceEntry = head;
	err = nvkm_gsp_rm_ctrl_wr(&disp->objcom, eld);
	if (err != 0)
		nvkm_infof(sc->dev,
		    "gsp_disp: r535 hda_eld display=0x%x head=%u err=%d\n",
		    display_id, head, err);
	return (err);
}

static int
disp_r535_sor_hda_hpd(struct nvkm_softc *sc, uint32_t display_id,
    uint32_t head, int present)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dfp_set_eld_audio_caps_params *eld;
	int err;

	if (present)
		return (0);
	if (disp == NULL)
		return (EINVAL);

	eld = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DFP_SET_ELD_AUDIO_CAPS, sizeof(*eld));
	if (eld == NULL)
		return (ENOMEM);
	memset(eld, 0, sizeof(*eld));
	eld->subDeviceInstance = 0;
	eld->displayId = display_id;
	eld->deviceEntry = head;
	err = nvkm_gsp_rm_ctrl_wr(&disp->objcom, eld);
	if (err != 0)
		nvkm_infof(sc->dev,
		    "gsp_disp: r535 hda_hpd display=0x%x head=%u err=%d\n",
		    display_id, head, err);
	return (err);
}

static int
disp_r535_sor_dp_audio_mute(struct nvkm_softc *sc, uint32_t display_id,
    int mute)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_set_audio_mutestream_params *am;

	if (disp == NULL)
		return (EINVAL);
	am = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DP_SET_AUDIO_MUTESTREAM, sizeof(*am));
	if (am == NULL)
		return (ENOMEM);
	memset(am, 0, sizeof(*am));
	am->subDeviceInstance = 0;
	am->displayId = display_id;
	am->mute = mute ? 1u : 0u;
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, am));
}

static int
disp_r535_sor_dp_audio(struct nvkm_softc *sc, uint32_t display_id, int enable)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dfp_set_audio_enable_params *ae;
	int err;

	if (!enable) {
		err = disp_r535_sor_dp_audio_mute(sc, display_id, 1);
		if (err != 0)
			return (err);
	}
	if (disp == NULL)
		return (EINVAL);
	ae = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DFP_SET_AUDIO_ENABLE, sizeof(*ae));
	if (ae == NULL)
		return (ENOMEM);
	memset(ae, 0, sizeof(*ae));
	ae->subDeviceInstance = 0;
	ae->displayId = display_id;
	ae->enable = enable ? 1u : 0u;
	err = nvkm_gsp_rm_ctrl_wr(&disp->objcom, ae);
	if (err != 0)
		return (err);
	if (enable)
		err = disp_r535_sor_dp_audio_mute(sc, display_id, 0);
	return (err);
}

static int
disp_r535_sor_dp_vcpi(struct nvkm_softc *sc, uint32_t sor, uint32_t link,
    uint32_t head, uint8_t slot, uint8_t slot_nr, uint16_t pbn,
    uint16_t aligned_pbn)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_config_stream_params *cs;

	if (disp == NULL)
		return (EINVAL);
	if (slot_nr == 0)
		return (EINVAL);
	cs = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DP_CONFIG_STREAM, sizeof(*cs));
	if (cs == NULL)
		return (ENOMEM);
	memset(cs, 0, sizeof(*cs));
	cs->subDeviceInstance = 0;
	cs->head = head;
	cs->sorIndex = sor;
	cs->dpLink = (link == 2u) ? 1u : 0u;
	cs->bEnableOverride = 1;
	cs->bMST = 1;
	cs->MST.slotStart = slot;
	cs->MST.slotEnd = slot + slot_nr - 1u;
	cs->MST.PBN = pbn;
	cs->MST.Timeslice = aligned_pbn;
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, cs));
}

static int
disp_r535_sor_dp_sst(struct nvkm_softc *sc, uint32_t sor, uint32_t link,
    uint32_t head, int enhanced_framing, uint32_t watermark,
    uint32_t hblanksym, uint32_t vblanksym)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_config_stream_params *cs;

	if (disp == NULL)
		return (EINVAL);
	cs = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DP_CONFIG_STREAM, sizeof(*cs));
	if (cs == NULL)
		return (ENOMEM);
	memset(cs, 0, sizeof(*cs));
	cs->subDeviceInstance = 0;
	cs->head = head;
	cs->sorIndex = sor;
	cs->dpLink = (link == 2u) ? 1u : 0u;
	cs->bEnableOverride = 1;
	cs->bMST = 0;
	cs->hBlankSym = hblanksym;
	cs->vBlankSym = vblanksym;
	cs->SST.bEnhancedFraming = enhanced_framing ? 1u : 0u;
	cs->SST.tuSize = 64;
	cs->SST.waterMark = watermark;
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, cs));
}

static int
disp_r535_sor_hdmi_scdc(struct nvkm_softc *sc, uint32_t display_id,
    int support, int scrambling, int scrambling_low_rates)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_set_hdmi_sink_caps_params *caps;

	if (disp == NULL)
		return (EINVAL);
	caps = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_SINK_CAPS, sizeof(*caps));
	if (caps == NULL)
		return (ENOMEM);
	memset(caps, 0, sizeof(*caps));
	caps->subDeviceInstance = 0;
	caps->displayId = display_id;
	if (support)
		caps->caps |= NV0073_CTRL_HDMI_SINK_CAP_SCDC;
	if (scrambling)
		caps->caps |= NV0073_CTRL_HDMI_SINK_CAP_GT_340MHZ;
	if (scrambling_low_rates)
		caps->caps |= NV0073_CTRL_HDMI_SINK_CAP_LTE_340MHZ_SCRAMBLE;
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, caps));
}

static int
disp_r535_sor_hdmi_ctrl_audio_mute(struct nvkm_softc *sc,
    uint32_t display_id, int mute)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_set_hdmi_audio_mutestream_params *am;

	if (disp == NULL)
		return (EINVAL);
	am = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_AUDIO_MUTESTREAM, sizeof(*am));
	if (am == NULL)
		return (ENOMEM);
	memset(am, 0, sizeof(*am));
	am->subDeviceInstance = 0;
	am->displayId = display_id;
	am->mute = mute ? 1u : 0u;
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, am));
}

static int
disp_r535_sor_hdmi_ctrl_audio(struct nvkm_softc *sc, uint32_t display_id,
    int enable)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_set_od_packet_params *op;

	if (disp == NULL)
		return (EINVAL);
	op = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SPECIFIC_SET_OD_PACKET, sizeof(*op));
	if (op == NULL)
		return (ENOMEM);
	memset(op, 0, sizeof(*op));
	op->subDeviceInstance = 0;
	op->displayId = display_id;
	op->transmitControl = NV0073_CTRL_OD_PACKET_TRANSMIT_ENABLE;
	op->packetSize = 10;
	op->aPacket[0] = 0x03;
	op->aPacket[3] = enable ? 0x10 : 0x01;
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, op));
}

static int
disp_r535_sor_hdmi_audio(struct nvkm_softc *sc, uint32_t display_id,
    uint32_t head, int enable)
{
	uint32_t hdmi = head * 0x400u;
	int err;

	err = disp_r535_sor_hdmi_ctrl_audio(sc, display_id, enable);
	if (err != 0)
		return (err);
	err = disp_r535_sor_hdmi_ctrl_audio_mute(sc, display_id, !enable);
	if (err != 0)
		return (err);

	disp_bar0_mask(sc, 0x6f00c0u + hdmi, 0x00000001u, 0x00000000u);
	nvkm_wr32(sc, 0x6f00ccu + hdmi, enable ? 0x00000010u : 0x00000001u);
	disp_bar0_mask(sc, 0x6f00c0u + hdmi, 0x00000001u, 0x00000001u);
	return (0);
}

static int
disp_r535_dp_mst_id_get(struct nvkm_softc *sc, uint32_t display_id,
    uint32_t *pid)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_topology_allocate_displayid_params *dp;
	void *p;
	int err;

	if (disp == NULL || pid == NULL)
		return (EINVAL);
	dp = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DP_TOPOLOGY_ALLOCATE_DISPLAYID, sizeof(*dp));
	if (dp == NULL)
		return (ENOMEM);
	memset(dp, 0, sizeof(*dp));
	dp->subDeviceInstance = 0;
	dp->displayId = display_id;
	dp->preferredDisplayId =
	    NV0073_CTRL_CMD_DP_INVALID_PREFERRED_DISPLAY_ID;
	p = dp;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*dp));
	if (err != 0 || p == NULL) {
		if (p != NULL)
			nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
		return (err != 0 ? err : EIO);
	}
	dp = p;
	*pid = dp->displayIdAssigned;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, dp);
	return (0);
}

static int
disp_r535_dp_mst_id_put(struct nvkm_softc *sc, uint32_t display_id)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_topology_free_displayid_params *dp;

	if (disp == NULL)
		return (EINVAL);
	dp = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DP_TOPOLOGY_FREE_DISPLAYID, sizeof(*dp));
	if (dp == NULL)
		return (ENOMEM);
	memset(dp, 0, sizeof(*dp));
	dp->subDeviceInstance = 0;
	dp->displayId = display_id;
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, dp));
}

static int
disp_r535_dp_drive(struct nvkm_softc *sc, uint32_t display_id, uint8_t lanes,
    const uint8_t pe[4], const uint8_t vs[4])
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_lane_data_params *ld;
	uint32_t lane;

	if (disp == NULL || pe == NULL || vs == NULL)
		return (EINVAL);
	if (lanes > 4)
		return (EINVAL);
	ld = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DP_SET_LANE_DATA, sizeof(*ld));
	if (ld == NULL)
		return (ENOMEM);
	memset(ld, 0, sizeof(*ld));
	ld->subDeviceInstance = 0;
	ld->displayId = display_id;
	ld->numLanes = lanes;
	for (lane = 0; lane < lanes; lane++)
		ld->data[lane] = NV0073_CTRL_DP_LANE_DATA(pe[lane], vs[lane]);
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, ld));
}

static int
disp_r535_dp_train_target(struct nvkm_softc *sc, uint32_t display_id,
    uint8_t target, int mst, uint8_t link_nr, uint8_t link_bw)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_ctrl_params *ctrl;
	void *p;
	int attempt, err;

	if (disp == NULL)
		return (EINVAL);

	for (attempt = 0; attempt < 3; attempt++) {
		ctrl = nvkm_gsp_rm_ctrl_get(&disp->objcom,
		    NV0073_CTRL_CMD_DP_CTRL, sizeof(*ctrl));
		if (ctrl == NULL)
			return (ENOMEM);
		memset(ctrl, 0, sizeof(*ctrl));
		ctrl->subDeviceInstance = 0;
		ctrl->displayId = display_id;
		ctrl->cmd = NV0073_CTRL_DP_CMD_SET_LANE_COUNT_TRUE |
		    NV0073_CTRL_DP_CMD_SET_LINK_BW_TRUE |
		    NV0073_CTRL_DP_CMD_TRAIN_PHY_REPEATER_YES;
		if (mst)
			ctrl->cmd |= NV0073_CTRL_DP_CMD_SET_FORMAT_MODE_MST;
		ctrl->data = NV0073_CTRL_DP_DATA_LANE_COUNT(link_nr) |
		    NV0073_CTRL_DP_DATA_LINK_BW(link_bw) |
		    NV0073_CTRL_DP_DATA_TARGET(target);
		if (target == 0)
			ctrl->cmd |=
			    NV0073_CTRL_DP_CMD_POST_LT_ADJ_REQ_GRANTED_YES;
		p = ctrl;
		err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*ctrl));
		if (err == EAGAIN || err == EBUSY) {
			uint32_t delay_ms = 0;
			if (p != NULL) {
				delay_ms = ((struct disp_dp_ctrl_params *)p)->
				    retryTimeMs;
				nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
			}
			if (delay_ms != 0) {
				DELAY(delay_ms * 1000);
				continue;
			}
		}
		if (err != 0 || p == NULL) {
			if (p != NULL)
				nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
			return (err != 0 ? err : EIO);
		}
		ctrl = p;
		err = ctrl->err != 0 ? EIO : 0;
		nvkm_gsp_rm_ctrl_done(&disp->objcom, ctrl);
		return (err);
	}
	return (EAGAIN);
}

static int
disp_r535_dp_aux_xfer(struct nvkm_softc *sc, uint32_t display_id, uint8_t type,
    uint32_t addr, uint8_t *data, uint8_t *psize)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_auxch_ctrl_params *aux;
	uint8_t size;
	void *p;
	int attempt, err;

	if (disp == NULL || psize == NULL)
		return (EINVAL);
	size = *psize;
	if (size > NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE)
		return (E2BIG);
	if (size != 0 && data == NULL)
		return (EINVAL);

	for (attempt = 0; attempt < 3; attempt++) {
		aux = nvkm_gsp_rm_ctrl_get(&disp->objcom,
		    NV0073_CTRL_CMD_DP_AUXCH_CTRL, sizeof(*aux));
		if (aux == NULL)
			return (ENOMEM);
		memset(aux, 0, sizeof(*aux));
		aux->subDeviceInstance = 0;
		aux->displayId = display_id;
		aux->bAddrOnly = size == 0 ? 1u : 0u;
		aux->cmd = type;
		if (aux->bAddrOnly)
			aux->cmd = NV0073_CTRL_DP_AUXCH_CMD_TYPE_AUX |
			    NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_WRITE;
		aux->addr = addr;
		aux->size = size == 0 ? 0u : (uint32_t)size - 1u;
		if (size != 0)
			memcpy(aux->data, data, size);
		p = aux;
		err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*aux));
		if (err == EAGAIN || err == EBUSY) {
			uint32_t delay_ms = 0;
			if (p != NULL) {
				delay_ms = ((struct disp_dp_auxch_ctrl_params *)p)->
				    retryTimeMs;
				nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
			}
			if (delay_ms != 0) {
				DELAY(delay_ms * 1000);
				continue;
			}
		}
		if (err != 0 || p == NULL) {
			if (p != NULL)
				nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
			return (err != 0 ? err : EIO);
		}
		aux = p;
		if (data != NULL && aux->size <= NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE)
			memcpy(data, aux->data, aux->size);
		*psize = (uint8_t)aux->size;
		err = (int)aux->replyType;
		nvkm_gsp_rm_ctrl_done(&disp->objcom, aux);
		return (err);
	}
	return (EAGAIN);
}

static int
disp_r535_dp_set_indexed_link_rates(struct nvkm_softc *sc,
    uint32_t display_id, const uint16_t rate_tbl[8])
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dp_config_indexed_link_rates_params *lr;
	uint32_t i;

	if (disp == NULL || rate_tbl == NULL)
		return (EINVAL);
	lr = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_DP_CONFIG_INDEXED_LINK_RATES, sizeof(*lr));
	if (lr == NULL)
		return (ENOMEM);
	memset(lr, 0, sizeof(*lr));
	lr->subDeviceInstance = 0;
	lr->displayId = display_id;
	for (i = 0; i < NV0073_CTRL_DP_MAX_INDEXED_LINK_RATES; i++)
		lr->linkRateTbl[i] = rate_tbl[i];
	return (nvkm_gsp_rm_ctrl_wr(&disp->objcom, lr));
}

static int
disp_r535_dp_train(struct nvkm_softc *sc, uint32_t display_id, int mst,
    uint8_t lttprs, uint8_t link_nr, uint8_t link_bw)
{
	int err;

	do {
		err = disp_r535_dp_train_target(sc, display_id, lttprs, mst,
		    link_nr, link_bw);
		if (err != 0)
			return (err);
	} while (lttprs-- != 0);

	return (0);
}

static int
disp_r535_dp_rates(struct nvkm_softc *sc, uint32_t display_id,
    const uint16_t rate_tbl[8], uint8_t rate_nr)
{
	if (rate_nr == 0)
		return (0);
	return (disp_r535_dp_set_indexed_link_rates(sc, display_id, rate_tbl));
}

static int
disp_r535_outp_acquire(struct nvkm_softc *sc, uint32_t display_id, int hda,
    uint32_t sor_exclude_mask, uint32_t *out_orid)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dfp_assign_sor_params *sor;
	uint32_t i, orid = 0xffu;
	void *p;
	int err;

	if (disp == NULL)
		return (EINVAL);
	sor = nvkm_gsp_rm_ctrl_get(&disp->objcom, NV0073_CTRL_CMD_DFP_ASSIGN_SOR,
	    sizeof(*sor));
	if (sor == NULL)
		return (ENOMEM);
	memset(sor, 0, sizeof(*sor));
	sor->subDeviceInstance = 0;
	sor->displayId = display_id;
	sor->sorExcludeMask = (uint8_t)sor_exclude_mask;
	if (hda)
		sor->flags |= NV0073_CTRL_DFP_ASSIGN_SOR_FLAGS_AUDIO_OPTIMAL;
	p = sor;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*sor));
	if (err != 0 || p == NULL) {
		if (p != NULL)
			nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
		return (err != 0 ? err : EIO);
	}
	sor = p;
	for (i = 0; i < DFP_ASSIGN_SOR_MAX_SORS; i++) {
		if ((sor->sorAssignListWithTag[i].displayMask & display_id) != 0) {
			orid = i;
			break;
		}
	}
	if (out_orid != NULL)
		*out_orid = orid;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, sor);
	return (orid == 0xffu ? ENODEV : 0);
}

static int
disp_r535_dp_acquire(struct nvkm_softc *sc, uint32_t display_id, int hda,
    uint32_t sor_exclude_mask, uint32_t *out_orid)
{
	return (disp_r535_outp_acquire(sc, display_id, hda, sor_exclude_mask,
	    out_orid));
}

static void
disp_r535_outp_release(struct nvkm_softc *sc, uint32_t display_id,
    uint32_t orid)
{
	nvkm_infof(sc->dev,
	    "gsp_disp: r535 outp_release display=0x%x or=%u (software unlink only)\n",
	    display_id, orid);
}

static int
disp_r535_dp_release(struct nvkm_softc *sc, uint32_t display_id,
    uint32_t orid, uint8_t fallback_bw)
{
	int err;

	err = disp_r535_dp_train_target(sc, display_id, 0, 0, 0, fallback_bw);
	if (err != 0)
		return (err);
	disp_r535_outp_release(sc, display_id, orid);
	return (0);
}

static void
disp_r535_head_vblank_put(struct nvkm_softc *sc, uint32_t head)
{
	uint32_t addr = 0x611d80u + head * 4u;

	nvkm_wr32(sc, addr, nvkm_rd32(sc, addr) & ~0x00000002u);
	nvkm_infof(sc->dev, "gsp_disp: r535 head%u vblank_put\n", head);
}

static void
disp_r535_head_vblank_get(struct nvkm_softc *sc, uint32_t head)
{
	uint32_t addr = 0x611d80u + head * 4u;

	nvkm_wr32(sc, 0x611800u + head * 4u, 0x00000002u);
	nvkm_wr32(sc, addr, nvkm_rd32(sc, addr) | 0x00000002u);
	nvkm_infof(sc->dev, "gsp_disp: r535 head%u vblank_get\n", head);
}

static void
disp_r535_channel_fini(struct nvkm_softc *sc, struct nvkm_gsp_object *obj,
    uint32_t put_reg, uint32_t *suspend_put, const char *label)
{
	uint32_t put = 0;

	if (put_reg != 0)
		put = nvkm_rd32(sc, put_reg);
	if (suspend_put != NULL)
		*suspend_put = put;
	if (obj != NULL && obj->handle != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: r535 %s fini handle=0x%x put=0x%x\n",
		    label, obj->handle, put);
		(void)nvkm_gsp_rm_free(obj);
		memset(obj, 0, sizeof(*obj));
	}
}

static int
nvkm_gsp_disp_outp_oneinit(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t mask, id;
	int err, ret;

	ret = disp_nouveau_get_supported(sc, &mask);
	if (ret != 0)
		return (ret);
	nvkm_infof(sc->dev,
	    "gsp_disp: outp oneinit supported displayId mask=0x%08x "
	    "sizeof(or=%u conn=%u dp=%u)\n",
	    mask, (unsigned)sizeof(struct disp_or_get_info_params),
	    (unsigned)sizeof(struct disp_get_connector_data_params),
	    (unsigned)sizeof(struct disp_dp_get_caps_params));

	for (id = 0; id < 32; id++) {
		struct disp_or_get_info_params *or;
		uint32_t display_id, type, proto, index, locn;
		void *p;

		display_id = 1u << id;
		if ((mask & display_id) == 0)
			continue;

		or = nvkm_gsp_rm_ctrl_get(&disp->objcom,
		    NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO, sizeof(*or));
		if (or == NULL)
			return (ENOMEM);
		memset(or, 0, sizeof(*or));
		or->subDeviceInstance = 0;
		or->displayId = display_id;
		p = or;
		err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*or));
		if (err != 0 || p == NULL) {
			nvkm_infof(sc->dev,
			    "gsp_disp: outp OR_GET_INFO display=0x%x err=%d\n",
			    display_id, err);
			if (ret == 0)
				ret = (err != 0 ? err : EIO);
			continue;
		}
		or = p;
		type = or->type;
		proto = or->protocol;
		index = or->index;
		locn = or->location;
		nvkm_infof(sc->dev,
		    "gsp_disp: outp OR display=0x%x index=%u type=%u proto=%u "
		    "loc=%u dcb=%u lit=%u dyn=%u\n",
		    display_id, index, type, proto, locn, or->dcbIndex,
		    or->bIsLitByVbios, or->bIsDispDynamic);
		nvkm_gsp_rm_ctrl_done(&disp->objcom, or);

		if (type == NV0073_CTRL_SPECIFIC_OR_TYPE_NONE)
			continue;
		if (type != NV0073_CTRL_SPECIFIC_OR_TYPE_SOR) {
			nvkm_infof(sc->dev,
			    "gsp_disp: outp display=0x%x unsupported OR type=%u\n",
			    display_id, type);
			continue;
		}

		err = disp_nouveau_outp_inherit_probe(sc, display_id);
		if (err != 0)
			nvkm_infof(sc->dev,
			    "gsp_disp: outp inherit display=0x%x ignored err=%d\n",
			    display_id, err);

		err = disp_nouveau_conn_new_probe(sc, display_id);
		if (err != 0 && ret == 0)
			ret = err;

		if (proto == NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_A ||
		    proto == NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_DP_B) {
			err = disp_nouveau_dp_get_caps_probe(sc, display_id);
			if (err != 0 && ret == 0)
				ret = err;
		}
	}

	return (ret);
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

static int
disp_register_device_event(struct nvkm_softc *sc, struct nvkm_gsp_object *event,
    uint32_t *out_handle, uint32_t handle_base, uint32_t notify_index,
    const char *label)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_nv0005_alloc_params *args;
	struct disp_event_set_notification_params *ctrl;
	uint32_t handle;
	int err;

	handle = nvkm_gsp_client_child_handle(&disp->client, handle_base);
	memset(event, 0, sizeof(*event));
	args = nvkm_gsp_rm_alloc_get(&disp->device.subdevice, handle,
	    NV01_EVENT_KERNEL_CALLBACK_EX, sizeof(*args), event);
	if (args == NULL)
		return (ENOMEM);
	args->hParentClient = disp->client.object.handle;
	args->hSrcResource = 0;
	args->hClass = NV01_EVENT_KERNEL_CALLBACK_EX;
	args->notifyIndex = notify_index;
	args->data = handle;
	err = nvkm_gsp_rm_alloc_wr(event, args);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: %s event alloc notify=%u err=%d\n",
		    label, notify_index, err);
		return (err);
	}

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->device.subdevice,
	    NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION, sizeof(*ctrl));
	if (ctrl == NULL)
		return (ENOMEM);
	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->event = notify_index;
	ctrl->action = NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT;
	err = nvkm_gsp_rm_ctrl_wr(&disp->device.subdevice, ctrl);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: %s SET_NOTIFICATION notify=%u err=%d\n",
		    label, notify_index, err);
		return (err);
	}

	*out_handle = handle;
	nvkm_infof(sc->dev,
	    "gsp_disp: %s event registered notify=%u handle=0x%x\n",
	    label, notify_index, handle);
	return (0);
}

/* Register for GSP hotplug/DP-IRQ notifications. The hotplug event arrives via
 * POST_EVENT; nvkm_gsp_evt_post_event matches hpd_event_handle and enqueues
 * hpd_task. DP IRQ is present to mirror nouveau's oneinit object graph. */
static int
nvkm_gsp_disp_register_hotplug(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	int err;

	TASK_INIT(&disp->hpd_task, 0, nvkm_gsp_disp_hotplug_task, sc);

	err = disp_register_device_event(sc, &disp->hpd_event,
	    &disp->hpd_event_handle, NVKM_RM_DISP_HPD, NV2080_NOTIFIERS_HOTPLUG,
	    "hotplug");
	if (err != 0)
		return (err);

	err = disp_register_device_event(sc, &disp->dp_irq_event,
	    &disp->dp_irq_event_handle, NVKM_RM_DISP_DP_IRQ,
	    NV2080_NOTIFIERS_DP_IRQ, "dp_irq");
	if (err != 0)
		return (err);
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
	handle = TU102_DISP << 16;	/* r535: root handle = oclass<<16 (low=0) */
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

	/* (2) Core pushbuffer. Nouveau uses an EVO DMAC PB and wraps with
	 * PUSH_JUMP when needed. */
	disp->core_push_size = DISP_CORE_PUSH_BYTES;
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
	pb->addressSpace = ADDR_SYSMEM;
	pb->cacheSnoop = 1;
	pb->physicalAddr = disp->core_push_paddr;
	pb->limit = disp->core_push_size - 1;
	pb->hclass = TU102_DISP_CORE_CHANNEL_DMA;
	pb->channelInstance = 0;
	pb->valid = 1;
	pb->pbTargetAperture = 0u;		/* nouveau leaves 0 */
	pb->subDeviceId = 0u;		/* nouveau leaves 0 */
	err = nvkm_gsp_rm_ctrl_wr(&tmp_subdev, pb);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: core set_pushbuf err=%d\n", err);
		return (err);
	}

	/* (4) Allocate the NVC57D core channel under the display root. */
	handle = NVKM_RM_DISP_CORE;	/* r535: (oclass<<16)|inst, low=inst=0 */
	ca = nvkm_gsp_rm_alloc_get(&disp->dispclass, handle,
	    TU102_DISP_CORE_CHANNEL_DMA, sizeof(*ca), &disp->core);
	if (ca == NULL)
		return (ENOMEM);
	memset(ca, 0, sizeof(*ca));
	ca->channelInstance = 0;
	ca->offset = 0;
	ca->subDeviceId = 0u;		/* nouveau leaves 0 */
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

			disp->core_put_cur = put;
			nvkm_infof(sc->dev,
		    "gsp_disp: core channel up -- root=0x%x core=0x%x pb@0x%llx "
		    "PUT=0x%x GET=0x%x\n",
			    disp->dispclass.handle, disp->core.handle,
			    (unsigned long long)disp->core_push_paddr, put, get);
		}

		if (!disp->ramht_ready) {
			err = disp_nouveau_bind_channel_ctxdma(sc, 0, 0, 0,
			    "core-dmac");
			if (err != 0) {
				nvkm_infof(sc->dev,
				    "gsp_disp: core dmac ctxdma bind err=%d\n", err);
				return (err);
			}
			disp->ramht_ready = 1;
		}

		return (0);
	}

static int
nvkm_gsp_disp_corec57d_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	volatile uint32_t *cpb = disp->core_push_kva;
	uint32_t cmds[64], i, n = 0, base, put = 0, get;
	int emit;

	if (cpb == NULL || disp->core_put_reg == 0)
		return (ENXIO);

	/* corec57d_init(): declare the notifier and every window's allowed
	 * format/usage bounds. This is a direct translation of the nouveau
	 * NVC57D core init state, not a light-up shortcut.
	 *
	 * nouveau sets assign_windows before the kick and does not synchronously
	 * wait for FE exception state from this init batch. */
#define M1(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
#define M2(off, v0, v1) do { cmds[n++] = evo_method_hdr((off), 2); cmds[n++] = (v0); cmds[n++] = (v1); } while (0)
	disp_clear_exception_chid(sc, 0);
	if (disp->ramht_ready)
		M1(NVC57D_SET_CONTEXT_DMA_NOTIFIER, NV50_DISP_HANDLE_SYNCBUF);
	for (i = 0; i < NVKM_GSP_DISP_WINDOW_NR; i++) {
		M2(NVC57D_WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(i),
		    EVO_FORMAT_USAGE_RGB_PACKED_ALL, 0x00000000u);
		M1(NVC57D_WINDOW_SET_WINDOW_USAGE_BOUNDS(i),
		    EVO_WINDOW_USAGE_BOUNDS);
	}
	disp->core_assign_windows = 1;
	base = disp->core_put_cur;
	emit = disp_chan_emit(sc, cpb, disp->core_put_reg,
	    &disp->core_put_cur, disp->core_push_size / 4, cmds, n, &put);
	disp_log_push_range(sc, "corec57d_init", cpb, base, put);
#undef M2
#undef M1

	get = disp_chan_wait_get(sc, disp->core_put_reg, put, 100000);
	nvkm_infof(sc->dev,
	    "gsp_disp: corec57d_init all-window bounds emit=%d PUT=%u GET=%u "
	    "ndw=%u assign_windows=%d\n",
	    emit, put, get, n, disp->core_assign_windows);
	disp_dump_exception_chid_tag(sc, "core_init-after", 0, 1);
	return (emit);
}

static int
nvkm_gsp_disp_window_channel_init(struct nvkm_softc *sc, uint32_t win)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct disp_channel_pushbuffer_params *pb;
	struct disp_channeldma_alloc_params *ca;
	uint32_t handle;
	int err;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	wndw->id = win;
	wndw->heads = 1u << (win >> 1);
	wndw->interlock_data = 1u << win;
	wndw->interlock_wimm = 0;
	wndw->ntfy = NV50_DISP_WNDW_NTFY(win);
	wndw->sema = NV50_DISP_WNDW_SEM0(win);
	wndw->data = 0;
	wndw->push_size = DISP_WINDOW_PUSH_BYTES;
	wndw->push_kva = contigmalloc(wndw->push_size, M_NVKM_DISP,
	    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
	if (wndw->push_kva == NULL)
		return (ENOMEM);
	wndw->push_paddr = vtophys(wndw->push_kva);

	/* r535_disp_chan_set_pushbuf(chan->head = win, chan->memory = PB). */
	nvkm_gsp_disp_internal_subdev(sc, &tmp_client, &tmp_subdev);
	pb = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER, sizeof(*pb));
	if (pb == NULL)
		return (ENOMEM);
	memset(pb, 0, sizeof(*pb));
	pb->addressSpace = ADDR_SYSMEM;
	pb->cacheSnoop = 1;
	pb->physicalAddr = wndw->push_paddr;
	pb->limit = wndw->push_size - 1;
	pb->hclass = TU102_DISP_WINDOW_CHANNEL_DMA;
	pb->channelInstance = win;
	pb->valid = 1;
	pb->pbTargetAperture = 0u;		/* nouveau leaves 0 */
	pb->subDeviceId = 0u;		/* nouveau leaves 0 */
	err = nvkm_gsp_rm_ctrl_wr(&tmp_subdev, pb);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: window%u set_pushbuf err=%d\n", win, err);
		return (err);
	}

	/* r535_dmac_alloc(): handle=(oclass<<16)|inst, channelInstance=inst. */
	handle = NVKM_RM_DISP_WINDOW | win;
	ca = nvkm_gsp_rm_alloc_get(&disp->dispclass, handle,
	    TU102_DISP_WINDOW_CHANNEL_DMA, sizeof(*ca), &wndw->object);
	if (ca == NULL)
		return (ENOMEM);
	memset(ca, 0, sizeof(*ca));
	ca->channelInstance = win;
	ca->offset = 0;
	ca->subDeviceId = 0u;		/* nouveau leaves 0 */
	err = nvkm_gsp_rm_alloc_wr(&wndw->object, ca);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: NVC57E window%u alloc handle=0x%x err=%d\n",
		    win, wndw->object.handle, err);
		return (err);
	}

	if (!wndw->ramht_ready) {
		err = disp_nouveau_bind_channel_ctxdma(sc, 1u + win, 1, 0,
		    "window-dmac");
		if (err != 0) {
			nvkm_infof(sc->dev,
			    "gsp_disp: window%u ctxdma bind err=%d\n", win, err);
			return (err);
		}
		wndw->ramht_ready = 1;
	}

	wndw->put_reg = 0x690000 + win * 0x1000;
	wndw->put_cur = nvkm_rd32(sc, wndw->put_reg);
	wndw->ready = 1;
	nvkm_infof(sc->dev,
	    "gsp_disp: window%u channel up -- handle=0x%x pb@0x%llx "
	    "heads=0x%x interlock=0x%x ntfy=0x%x sema=0x%x PUT=0x%x GET=0x%x\n",
	    win, wndw->object.handle, (unsigned long long)wndw->push_paddr,
	    wndw->heads, wndw->interlock_data, wndw->ntfy, wndw->sema,
	    nvkm_rd32(sc, wndw->put_reg), nvkm_rd32(sc, wndw->put_reg + 4));
	return (0);
}

static int
nvkm_gsp_disp_wimm_channel_init(struct nvkm_softc *sc, uint32_t win)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	struct nvkm_gsp_disp_wimm *wimm;
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct disp_channel_pushbuffer_params *pb;
	struct disp_channeldma_alloc_params *ca;
	uint32_t handle;
	int err;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	wimm = &disp->wimm[win];
	if (!wndw->ready)
		return (ENXIO);
	if (wimm->ready)
		return (0);

	wimm->id = win;
	wimm->push_size = DISP_WIMM_PUSH_BYTES;
	wimm->push_kva = contigmalloc(wimm->push_size, M_NVKM_DISP,
	    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
	if (wimm->push_kva == NULL)
		return (ENOMEM);
	wimm->push_paddr = vtophys(wimm->push_kva);

	nvkm_gsp_disp_internal_subdev(sc, &tmp_client, &tmp_subdev);
	pb = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER, sizeof(*pb));
	if (pb == NULL) {
		contigfree(wimm->push_kva, wimm->push_size, M_NVKM_DISP);
		wimm->push_kva = NULL;
		return (ENOMEM);
	}
	memset(pb, 0, sizeof(*pb));
	pb->addressSpace = ADDR_SYSMEM;
	pb->cacheSnoop = 1;
	pb->physicalAddr = wimm->push_paddr;
	pb->limit = wimm->push_size - 1;
	pb->hclass = TU102_DISP_WINDOW_IMM_CHANNEL_DMA;
	pb->channelInstance = win;
	pb->valid = 1;
	pb->pbTargetAperture = 0u;		/* nouveau leaves 0 */
	pb->subDeviceId = 0u;		/* nouveau leaves 0 */
	err = nvkm_gsp_rm_ctrl_wr(&tmp_subdev, pb);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: wimm%u set_pushbuf err=%d\n", win, err);
		contigfree(wimm->push_kva, wimm->push_size, M_NVKM_DISP);
		wimm->push_kva = NULL;
		return (err);
	}

	handle = NVKM_RM_DISP_WIMM | win;
	ca = nvkm_gsp_rm_alloc_get(&disp->dispclass, handle,
	    TU102_DISP_WINDOW_IMM_CHANNEL_DMA, sizeof(*ca), &wimm->object);
	if (ca == NULL) {
		contigfree(wimm->push_kva, wimm->push_size, M_NVKM_DISP);
		wimm->push_kva = NULL;
		return (ENOMEM);
	}
	memset(ca, 0, sizeof(*ca));
	ca->channelInstance = win;
	ca->offset = 0;
	ca->subDeviceId = 0u;		/* nouveau leaves 0 */
	err = nvkm_gsp_rm_alloc_wr(&wimm->object, ca);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: NVC57B wimm%u alloc handle=0x%x err=%d\n",
		    win, wimm->object.handle, err);
		contigfree(wimm->push_kva, wimm->push_size, M_NVKM_DISP);
		wimm->push_kva = NULL;
		return (err);
	}

	wimm->put_reg = 0x6b0000 + win * 0x1000;
	wimm->put_cur = nvkm_rd32(sc, wimm->put_reg);
	wndw->interlock_wimm = wndw->interlock_data;
	wimm->ready = 1;
	nvkm_infof(sc->dev,
	    "gsp_disp: wimm%u channel up -- handle=0x%x pb@0x%llx "
	    "interlock=0x%x PUT=0x%x GET=0x%x\n",
	    win, wimm->object.handle, (unsigned long long)wimm->push_paddr,
	    wndw->interlock_wimm, nvkm_rd32(sc, wimm->put_reg),
	    nvkm_rd32(sc, wimm->put_reg + 4));
	return (0);
}

static int
nvkm_gsp_disp_cursor_channel_init(struct nvkm_softc *sc, uint32_t head)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_curs *curs;
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct disp_channel_pushbuffer_params *pb;
	struct disp_channelpio_alloc_params *pa;
	uint32_t handle;
	int err;

	if (head >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	curs = &disp->curs[head];
	if (curs->ready)
		return (0);

	curs->id = head;
	nvkm_gsp_disp_internal_subdev(sc, &tmp_client, &tmp_subdev);
	pb = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER, sizeof(*pb));
	if (pb == NULL)
		return (ENOMEM);
	memset(pb, 0, sizeof(*pb));
	pb->hclass = TU102_DISP_CURSOR;
	pb->channelInstance = head;
	pb->valid = 0;
	pb->subDeviceId = 0u;		/* nouveau leaves 0 */
	err = nvkm_gsp_rm_ctrl_wr(&tmp_subdev, pb);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: cursor%u set_pushbuf err=%d\n", head, err);
		return (err);
	}

	handle = NVKM_RM_DISP_CURSOR | head;
	pa = nvkm_gsp_rm_alloc_get(&disp->dispclass, handle, TU102_DISP_CURSOR,
	    sizeof(*pa), &curs->object);
	if (pa == NULL)
		return (ENOMEM);
	memset(pa, 0, sizeof(*pa));
	pa->channelInstance = head;
	err = nvkm_gsp_rm_alloc_wr(&curs->object, pa);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: NVC57A cursor%u alloc handle=0x%x err=%d\n",
		    head, curs->object.handle, err);
		return (err);
	}

	curs->put_reg = 0x6d8000 + head * 0x1000;
	curs->ready = 1;
	nvkm_infof(sc->dev,
	    "gsp_disp: cursor%u channel up -- handle=0x%x PUT=0x%x GET=0x%x\n",
	    head, curs->object.handle, nvkm_rd32(sc, curs->put_reg),
	    nvkm_rd32(sc, curs->put_reg + 4));
	return (0);
}

/* ===== window display channels (M4c) =====
 * Direct r535_dmac_init()/wndwc37e_new_() translation: create every NVC57E
 * window DMA channel instance described by the display window mask. */
static int
nvkm_gsp_disp_window_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t win, mask;
	int err;

	if (disp->dispclass.handle == 0)
		return (ENXIO);

	mask = disp->window_mask & ((1u << NVKM_GSP_DISP_WINDOW_NR) - 1u);
	if (mask == 0)
		mask = (1u << NVKM_GSP_DISP_WINDOW_NR) - 1u;

	for (win = 0; win < NVKM_GSP_DISP_WINDOW_NR; win++) {
		if ((mask & (1u << win)) == 0)
			continue;
		err = nvkm_gsp_disp_window_channel_init(sc, win);
		if (err != 0)
			return (err);
		err = nvkm_gsp_disp_wimm_channel_init(sc, win);
		if (err != 0)
			return (err);
	}

	return (0);
}

static int
nvkm_gsp_disp_cursor_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t head, mask, max_heads;
	int err;

	if (disp->dispclass.handle == 0)
		return (ENXIO);

	max_heads = disp->num_heads;
	if (max_heads == 0)
		max_heads = NVKM_GSP_DISP_WINDOW_NR;
	if (max_heads > NVKM_GSP_DISP_WINDOW_NR)
		max_heads = NVKM_GSP_DISP_WINDOW_NR;

	mask = disp->head_mask;
	if (mask == 0)
		mask = (max_heads >= 32) ? 0xffffffffu : ((1u << max_heads) - 1u);

	for (head = 0; head < max_heads; head++) {
		if ((mask & (1u << head)) == 0)
			continue;
		err = nvkm_gsp_disp_cursor_channel_init(sc, head);
		if (err != 0)
			return (err);
	}

	return (0);
}

static int
nvkm_gsp_disp_caps_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t sor0_caps;

	if (disp->dispclass.handle == 0)
		return (ENXIO);

	memset(&disp->caps, 0, sizeof(disp->caps));
	disp->caps.client = &disp->client;
	disp->caps.parent = &disp->dispclass;
	disp->caps.handle = NVKM_RM_DISP_CAPS;

	sor0_caps = nvkm_rd32(sc, 0x640000 + 0x144);
	nvkm_infof(sc->dev,
	    "gsp_disp: corec37d_caps_init GV100_DISP_CAPS BAR0=0x640000 "
	    "size=0x1000 sor0_caps=0x%x\n",
	    sor0_caps);
	return (0);
}

static int
nvkm_gsp_disp_nouveau_ramht_setup(struct nvkm_softc *sc, uint64_t fb_size)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint64_t vram_limit;
	int err;

	if (sc->fb_usable_size == 0)
		return (ENXIO);

	if (disp->notifier_paddr == 0) {
		disp->notifier_paddr = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
		if (disp->notifier_paddr == 0)
			return (ENOMEM);
		nvkm_infof(sc->dev,
		    "gsp_disp: M4y host-vram notifier size=0x1000 -> paddr=0x%llx\n",
		    (unsigned long long)disp->notifier_paddr);
	}
	if (disp->fb_paddr == 0) {
		disp->fb_paddr = nvkm_gsp_vram_alloc(sc, fb_size, 0x1000);
		if (disp->fb_paddr == 0)
			return (ENOMEM);
		nvkm_infof(sc->dev,
		    "gsp_disp: M4y host-vram fb size=0x%llx -> paddr=0x%llx\n",
		    (unsigned long long)fb_size,
		    (unsigned long long)disp->fb_paddr);
	}
	vram_limit = sc->fb_usable_size - 1;
	if (disp->notifier_gva == 0) {
		err = nvkm_gsp_bar1_map_existing(sc, disp->notifier_paddr,
		    &disp->notifier_gva);
		if (err != 0) {
			nvkm_infof(sc->dev,
			    "gsp_disp: M4e notifier BAR1 map paddr=0x%llx err=%d\n",
			    (unsigned long long)disp->notifier_paddr, err);
			return (err);
		}
	}

	/* Clear the notifier to NOT_BEGUN before the UPDATE that should finish it. */
	nvkm_gsp_bar1_wr32(sc, disp->notifier_gva + 0x0, 0);
	nvkm_gsp_bar1_wr32(sc, disp->notifier_gva + 0x4, 0);
	nvkm_gsp_bar1_wr32(sc, disp->notifier_gva + 0x8, 0);
	nvkm_gsp_bar1_wr32(sc, disp->notifier_gva + 0xc, 0);

	nvkm_infof(sc->dev,
	    "gsp_disp: M4e sync/fb backing ready ntfy@0x%llx fb@0x%llx "
	    "ram_user=0x%llx ctxdma_limit=0x%llx core_ramht=%d\n",
	    (unsigned long long)disp->notifier_paddr,
	    (unsigned long long)disp->fb_paddr,
	    (unsigned long long)sc->fb_usable_size,
	    (unsigned long long)vram_limit, disp->ramht_ready);

	nvkm_gsp_bar1_flush(sc);
	return (0);
}

static int
disp_fb_fill_bars(struct nvkm_softc *sc, uint64_t fb_paddr, uint32_t pitch,
    uint32_t width, uint32_t height)
{
	static const uint32_t colors[] = {
		0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu,
		0xffffff00u, 0xffff00ffu, 0xff00ffffu, 0xff202020u,
	};
	uint64_t mapped_page = ~(uint64_t)0;
	uint64_t mapped_gva = 0;
	uint32_t fill_h;
	int err = 0;

	fill_h = height < 256u ? height : 256u;
	for (uint32_t y = 0; y < fill_h; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint64_t addr = fb_paddr + (uint64_t)y * pitch + (uint64_t)x * 4u;
			uint64_t page = addr & ~(uint64_t)0xfffu;
			uint32_t color = colors[((uint64_t)x * nitems(colors)) / width];

			if (page != mapped_page) {
				if (mapped_gva != 0)
					nvkm_gsp_bar1_unmap_existing(sc, mapped_gva);
				err = nvkm_gsp_bar1_map_existing(sc, page, &mapped_gva);
				if (err != 0) {
					nvkm_infof(sc->dev,
					    "gsp_disp: M4y fb BAR1 map page=0x%llx err=%d\n",
					    (unsigned long long)page, err);
					mapped_gva = 0;
					goto out;
				}
				mapped_page = page;
			}
			nvkm_gsp_bar1_wr32(sc, mapped_gva + (addr & 0xfffu), color);
		}
	}

	nvkm_gsp_bar1_flush(sc);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4y fb pattern: filled %u rows at fb@0x%llx pitch=%u\n",
	    fill_h, (unsigned long long)fb_paddr, pitch);

out:
	if (mapped_gva != 0)
		nvkm_gsp_bar1_unmap_existing(sc, mapped_gva);
	return (err);
}

static int
disp_olut_fill_identity(struct nvkm_softc *sc, uint64_t olut_paddr)
{
	uint64_t mapped_page = ~(uint64_t)0;
	uint64_t mapped_gva = 0;
	int err = 0;

#define OLUT_WR32(off, val) do {						\
	uint64_t _addr = olut_paddr + (uint64_t)(off);				\
	uint64_t _page = _addr & ~(uint64_t)0xfffu;				\
	if (_page != mapped_page) {						\
		if (mapped_gva != 0)						\
			nvkm_gsp_bar1_unmap_existing(sc, mapped_gva);		\
		err = nvkm_gsp_bar1_map_existing(sc, _page, &mapped_gva);	\
		if (err != 0) {						\
			nvkm_infof(sc->dev,					\
			    "gsp_disp: M4t olut BAR1 map page=0x%llx err=%d\n", \
			    (unsigned long long)_page, err);			\
			mapped_gva = 0;					\
			goto out;						\
		}								\
		mapped_page = _page;						\
	}									\
	nvkm_gsp_bar1_wr32(sc, mapped_gva + (_addr & 0xfffu), (val));		\
} while (0)

	for (uint32_t off = 0; off < 0x20u; off += 4u)
		OLUT_WR32(off, 0);

	for (uint32_t i = 0; i < 1024u; i++) {
		uint32_t v = (i << 16) >> 10;
		uint32_t off = 0x20u + i * 8u;

		OLUT_WR32(off + 0u, v | (v << 16));
		OLUT_WR32(off + 4u, v);
	}

	/* INTERPOLATE mode reads one extra entry; nouveau replicates the last. */
	{
		uint32_t v = (1023u << 16) >> 10;
		uint32_t off = 0x20u + 1024u * 8u;

		OLUT_WR32(off + 0u, v | (v << 16));
		OLUT_WR32(off + 4u, v);
	}

	nvkm_gsp_bar1_flush(sc);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4t olut identity: wrote direct10 ramp at olut@0x%llx\n",
	    (unsigned long long)olut_paddr);

out:
	if (mapped_gva != 0)
		nvkm_gsp_bar1_unmap_existing(sc, mapped_gva);
#undef OLUT_WR32
	return (err);
}

static uint16_t
disp_fixed_u0_16_to_fp16(uint16_t fixed)
{
	int sign = 0, exp = 0, man = 0;

	if (fixed != 0) {
		while (--exp != 0 && (fixed & 0x8000u) == 0)
			fixed <<= 1;
		man = ((fixed << 1) & 0xffc0u) >> 6;
		exp += 15;
	}
	return ((uint16_t)((sign << 15) | (exp << 10) | man));
}

static int
disp_ilut_fill_identity(struct nvkm_softc *sc, uint64_t ilut_paddr)
{
	uint64_t mapped_page = ~(uint64_t)0;
	uint64_t mapped_gva = 0;
	int err = 0;

#define ILUT_WR32(off, val) do {						\
	uint64_t _addr = ilut_paddr + (uint64_t)(off);				\
	uint64_t _page = _addr & ~(uint64_t)0xfffu;				\
	if (_page != mapped_page) {						\
		if (mapped_gva != 0)						\
			nvkm_gsp_bar1_unmap_existing(sc, mapped_gva);		\
		err = nvkm_gsp_bar1_map_existing(sc, _page, &mapped_gva);	\
		if (err != 0) {						\
			nvkm_infof(sc->dev,					\
			    "gsp_disp: M4t ilut BAR1 map page=0x%llx err=%d\n", \
			    (unsigned long long)_page, err);			\
			mapped_gva = 0;					\
			goto out;						\
		}								\
		mapped_page = _page;						\
	}									\
	nvkm_gsp_bar1_wr32(sc, mapped_gva + (_addr & 0xfffu), (val));		\
} while (0)

	for (uint32_t off = 0; off < 0x20u; off += 4u)
		ILUT_WR32(off, 0);

	for (uint32_t i = 0; i < 1024u; i++) {
		uint16_t v = disp_fixed_u0_16_to_fp16((uint16_t)((i << 16) >> 10));
		uint32_t off = 0x20u + i * 8u;

		ILUT_WR32(off + 0u, (uint32_t)v | ((uint32_t)v << 16));
		ILUT_WR32(off + 4u, (uint32_t)v);
	}

	{
		uint16_t v = disp_fixed_u0_16_to_fp16((uint16_t)((1023u << 16) >> 10));
		uint32_t off = 0x20u + 1024u * 8u;

		ILUT_WR32(off + 0u, (uint32_t)v | ((uint32_t)v << 16));
		ILUT_WR32(off + 4u, (uint32_t)v);
	}

	nvkm_gsp_bar1_flush(sc);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4t ilut identity: wrote direct10 FP16 ramp at ilut@0x%llx\n",
	    (unsigned long long)ilut_paddr);

out:
	if (mapped_gva != 0)
		nvkm_gsp_bar1_unmap_existing(sc, mapped_gva);
#undef ILUT_WR32
	return (err);
}

static const char *
disp_exception_reason(uint32_t type)
{
	switch (type) {
	case 0:
		return ("NONE");
	case 1:
		return ("PUSHBUFFER_ERR");
	case 2:
		return ("TRAP");
	case 3:
		return ("RESERVED_METHOD");
	case 4:
		return ("INVALID_ARG");
	case 5:
		return ("INVALID_STATE");
	case 7:
		return ("UNRESOLVABLE_HANDLE");
	default:
		return ("UNKNOWN");
	}
}

static void
disp_clear_exception_chid(struct nvkm_softc *sc, uint32_t chid)
{
	nvkm_wr32(sc, 0x611020 + chid * 12u, 0x90000000u);
}

static void
disp_dump_exception_chid_tag(struct nvkm_softc *sc, const char *tag,
    uint32_t chid, int clear)
{
	uint32_t stat = nvkm_rd32(sc, 0x611020 + chid * 12u);
	uint32_t data = nvkm_rd32(sc, 0x611024 + chid * 12u);
	uint32_t code = nvkm_rd32(sc, 0x611028 + chid * 12u);
	uint32_t type = (stat & 0x00007000u) >> 12;
	uint32_t method = (stat & 0x00000fffu) << 2;

	nvkm_infof(sc->dev,
	    "gsp_disp: %s EXC chid=%u stat=0x%x reason=%u[%s] "
	    "method=0x%x data=0x%x code=0x%x\n",
	    tag, chid, stat, type, disp_exception_reason(type), method, data,
	    code);
	if (clear)
		disp_clear_exception_chid(sc, chid);
}

static void
disp_dump_gv100_exception_state(struct nvkm_softc *sc)
{
	uint32_t intr = nvkm_rd32(sc, 0x611ec0);
	uint32_t ctrl = nvkm_rd32(sc, 0x611c30);
	uint32_t error = nvkm_rd32(sc, 0x611848);
	uint32_t wndw = nvkm_rd32(sc, 0x61184c);
	uint32_t wimm = nvkm_rd32(sc, 0x611850);
	uint32_t other = nvkm_rd32(sc, 0x611854);
	uint32_t super = nvkm_rd32(sc, 0x6107a8);

	nvkm_infof(sc->dev,
	    "gsp_disp: M4y DIAG intr=0x%x ctrl=0x%x error=0x%x "
	    "wndw=0x%x wimm=0x%x other=0x%x super=0x%x\n",
	    intr, ctrl, error, wndw, wimm, other, super);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4y DIAG headmask[0..3]=0x%x 0x%x 0x%x 0x%x\n",
	    nvkm_rd32(sc, 0x6107ac + 0 * 4), nvkm_rd32(sc, 0x6107ac + 1 * 4),
	    nvkm_rd32(sc, 0x6107ac + 2 * 4), nvkm_rd32(sc, 0x6107ac + 3 * 4));

	/* GV100+ maps core/window CHIDs directly to FE_EXCEPT slots. */
	disp_dump_exception_chid_tag(sc, "M4y", 0, 0);
	disp_dump_exception_chid_tag(sc, "M4y", 1, 0);
}

static void
disp_dump_gv100_sor_route_state(struct nvkm_softc *sc, const char *tag)
{
	nvkm_infof(sc->dev,
	    "gsp_disp: %s SOR asy[0..3]=0x%x 0x%x 0x%x 0x%x "
	    "arm_live[0..3]=0x%x 0x%x 0x%x 0x%x\n",
	    tag,
	    nvkm_rd32(sc, 0x680300 + 0 * 0x20),
	    nvkm_rd32(sc, 0x680300 + 1 * 0x20),
	    nvkm_rd32(sc, 0x680300 + 2 * 0x20),
	    nvkm_rd32(sc, 0x680300 + 3 * 0x20),
	    nvkm_rd32(sc, 0x680300 + 0x8000 + 0 * 0x20),
	    nvkm_rd32(sc, 0x680300 + 0x8000 + 1 * 0x20),
	    nvkm_rd32(sc, 0x680300 + 0x8000 + 2 * 0x20),
	    nvkm_rd32(sc, 0x680300 + 0x8000 + 3 * 0x20));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s route[0..7]=0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
	    tag,
	    nvkm_rd32(sc, 0x612308 + 0 * 0x80),
	    nvkm_rd32(sc, 0x612308 + 1 * 0x80),
	    nvkm_rd32(sc, 0x612308 + 2 * 0x80),
	    nvkm_rd32(sc, 0x612308 + 3 * 0x80),
	    nvkm_rd32(sc, 0x612308 + 4 * 0x80),
	    nvkm_rd32(sc, 0x612308 + 5 * 0x80),
	    nvkm_rd32(sc, 0x612308 + 6 * 0x80),
	    nvkm_rd32(sc, 0x612308 + 7 * 0x80));
}

static void
disp_dump_gv100_head_window_state(struct nvkm_softc *sc, const char *tag)
{
	uint32_t head_asy = 0x682000;
	uint32_t head_arm_live = head_asy + 0x8000;
	uint32_t win_asy = 0x681000;
	uint32_t win_arm_live = win_asy + 0x8000;

	nvkm_infof(sc->dev,
	    "gsp_disp: %s head0 asy ctrl=0x%x out=0x%x clk=0x%x "
	    "display=0x%x clkmax=0x%x usage=0x%x\n",
	    tag,
	    nvkm_rd32(sc, head_asy + 0x008),
	    nvkm_rd32(sc, head_asy + 0x004),
	    nvkm_rd32(sc, head_asy + 0x00c),
	    nvkm_rd32(sc, head_asy + 0x020),
	    nvkm_rd32(sc, head_asy + 0x028),
	    nvkm_rd32(sc, head_asy + 0x030));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s head0 asy viewin=0x%x viewout=0x%x "
	    "raster=0x%x sync=0x%x blanke=0x%x blanks=0x%x blank2=0x%x\n",
	    tag,
	    nvkm_rd32(sc, head_asy + 0x04c),
	    nvkm_rd32(sc, head_asy + 0x058),
	    nvkm_rd32(sc, head_asy + 0x064),
	    nvkm_rd32(sc, head_asy + 0x068),
	    nvkm_rd32(sc, head_asy + 0x06c),
	    nvkm_rd32(sc, head_asy + 0x070),
	    nvkm_rd32(sc, head_asy + 0x074));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s head0 arm_live ctrl=0x%x out=0x%x clk=0x%x "
	    "display=0x%x clkmax=0x%x usage=0x%x\n",
	    tag,
	    nvkm_rd32(sc, head_arm_live + 0x008),
	    nvkm_rd32(sc, head_arm_live + 0x004),
	    nvkm_rd32(sc, head_arm_live + 0x00c),
	    nvkm_rd32(sc, head_arm_live + 0x020),
	    nvkm_rd32(sc, head_arm_live + 0x028),
	    nvkm_rd32(sc, head_arm_live + 0x030));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s head0 arm_live viewin=0x%x viewout=0x%x "
	    "raster=0x%x sync=0x%x blanke=0x%x blanks=0x%x blank2=0x%x\n",
	    tag,
	    nvkm_rd32(sc, head_arm_live + 0x04c),
	    nvkm_rd32(sc, head_arm_live + 0x058),
	    nvkm_rd32(sc, head_arm_live + 0x064),
	    nvkm_rd32(sc, head_arm_live + 0x068),
	    nvkm_rd32(sc, head_arm_live + 0x06c),
	    nvkm_rd32(sc, head_arm_live + 0x070),
	    nvkm_rd32(sc, head_arm_live + 0x074));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s win-core asy ctl[0..3]=0x%x 0x%x 0x%x 0x%x "
	    "usage[0..3]=0x%x 0x%x 0x%x 0x%x\n",
	    tag,
	    nvkm_rd32(sc, win_asy + 0 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_asy + 1 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_asy + 2 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_asy + 3 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_asy + 0 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_asy + 1 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_asy + 2 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_asy + 3 * 0x80 + 0x010));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s win-core arm_live ctl[0..3]=0x%x 0x%x 0x%x 0x%x "
	    "usage[0..3]=0x%x 0x%x 0x%x 0x%x\n",
	    tag,
	    nvkm_rd32(sc, win_arm_live + 0 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_arm_live + 1 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_arm_live + 2 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_arm_live + 3 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_arm_live + 0 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_arm_live + 1 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_arm_live + 2 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_arm_live + 3 * 0x80 + 0x010));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s win0-core asy fmt=0x%x rot=0x%x data0c=0x%x "
	    "arm_live fmt=0x%x rot=0x%x data0c=0x%x\n",
	    tag,
	    nvkm_rd32(sc, win_asy + 0x004),
	    nvkm_rd32(sc, win_asy + 0x008),
	    nvkm_rd32(sc, win_asy + 0x00c),
	    nvkm_rd32(sc, win_arm_live + 0x004),
	    nvkm_rd32(sc, win_arm_live + 0x008),
	    nvkm_rd32(sc, win_arm_live + 0x00c));
}

/* ===== nouveau_disp.txt state machine =====
 * This is the first-light subset of nv50_disp_atomic_commit_tail translated as
 * C steps. The RM/GSP allocation and push-buffer primitives above remain the
 * transport layer; this block owns display state submission order. */
struct nvkm_disp_interlock {
	uint32_t core;
	uint32_t curs;
	uint32_t wndw;
	uint32_t wimm;
};

#define DISP_WNDW_ATOM_SEMA		(1u << 0)
#define DISP_WNDW_ATOM_NTFY		(1u << 1)
#define DISP_WNDW_ATOM_IMAGE		(1u << 2)
#define DISP_WNDW_ATOM_XLUT		(1u << 3)
#define DISP_WNDW_ATOM_CSC		(1u << 4)
#define DISP_WNDW_ATOM_BLEND		(1u << 5)
#define DISP_WNDW_ATOM_POINT		(1u << 6)

struct disp_nv50_head_atom {
	uint32_t head;
	uint32_t display_id;
	uint32_t orid;
	uint32_t proto;
	uint32_t sor_ctrl;
	uint32_t or_payload;
	uint32_t view_in;
	uint32_t view_out;
	uint32_t raster;
	uint32_t sync;
	uint32_t blanke;
	uint32_t blanks;
	uint32_t blank2;
	uint32_t interlace;
	uint32_t nhsync;
	uint32_t nvsync;
	uint32_t clock;
	uint32_t clock_max;
	uint32_t usage;
	uint32_t dither;
	uint32_t procamp;
	uint32_t olut_control;
	uint32_t olut_scale;
	uint32_t olut_handle;
	uint32_t olut_offset;
};

struct disp_nv50_wndw_atom {
	uint32_t win;
	uint32_t head;
	uint32_t visible;
	uint32_t set_mask;
	uint32_t clr_mask;
	uint32_t sema_handle;
	uint32_t sema_offset;
	uint32_t sema_acquire;
	uint32_t sema_release;
	uint32_t ntfy_handle;
	uint32_t ntfy_offset;
	uint32_t ntfy_awaken;
	uint32_t image_handle;
	uint32_t image_offset;
	uint32_t image_w;
	uint32_t image_h;
	uint32_t image_layout;
	uint32_t image_blockh;
	uint32_t image_pitch;
	uint32_t image_format;
	uint32_t image_interval;
	uint32_t image_mode;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t dst_w;
	uint32_t dst_h;
	uint32_t xlut_control;
	uint32_t xlut_handle;
	uint32_t xlut_offset;
	uint32_t csc_matrix[12];
	uint32_t blend_depth;
	uint32_t blend_k1;
	uint32_t blend_factor;
	uint32_t point_x;
	uint32_t point_y;
};

static uint32_t disp_headc57d_or_payload(const struct nvkm_disp_mode *m);

static void
disp_nv50_head_atom_first_light(struct disp_nv50_head_atom *asyh,
    uint32_t head, uint32_t display_id, uint32_t orid, uint32_t proto,
    const struct nvkm_disp_mode *m, uint64_t olut_paddr)
{
	memset(asyh, 0, sizeof(*asyh));
	asyh->head = head;
	asyh->display_id = display_id;
	asyh->orid = orid;
	asyh->proto = proto;
	asyh->sor_ctrl = proto | (1u << head);
	asyh->or_payload = disp_headc57d_or_payload(m);
	asyh->view_in = m->iw | (m->ih << 16);
	asyh->view_out = m->ow | (m->oh << 16);
	asyh->raster = m->raster;
	asyh->sync = m->sync;
	asyh->blanke = m->blanke;
	asyh->blanks = m->blanks;
	asyh->blank2 = m->blank2;
	asyh->interlace = m->interlace;
	asyh->nhsync = m->nhsync;
	asyh->nvsync = m->nvsync;
	asyh->clock = m->clk & 0x7fffffffu;
	asyh->clock_max = asyh->clock;
	asyh->usage = NVC57D_HEAD_USAGE_BOUNDS_DEFAULT;
	asyh->dither = 0;
	asyh->procamp = 0;
	if (olut_paddr != 0) {
		asyh->olut_control = NVC57D_OLUT_CONTROL_IDENTITY_DIRECT10;
		asyh->olut_scale = 0xffffffffu;
		asyh->olut_handle = NV50_DISP_HANDLE_VRAM;
		asyh->olut_offset = (uint32_t)(olut_paddr >> 8);
	}
}

static void
disp_nv50_wndw_atom_identity_csc(struct disp_nv50_wndw_atom *asyw)
{
	static const uint32_t identity[12] = {
		NVC57E_CSC_IDENTITY_SCALE, 0, 0, 0,
		0, NVC57E_CSC_IDENTITY_SCALE, 0, 0,
		0, 0, NVC57E_CSC_IDENTITY_SCALE, 0,
	};

	memcpy(asyw->csc_matrix, identity, sizeof(identity));
}

static int
disp_nv50_wndw_format(uint32_t fourcc, uint32_t *format)
{
	switch (fourcc) {
	case DRM_FORMAT_C8:
		*format = NVC57E_SET_PARAMS_FORMAT_I8;
		return (0);
	case DRM_FORMAT_YUYV:
		*format = NVC57E_SET_PARAMS_FORMAT_Y8_U8__Y8_V8_N422;
		return (0);
	case DRM_FORMAT_UYVY:
		*format = NVC57E_SET_PARAMS_FORMAT_U8_Y8__V8_Y8_N422;
		return (0);
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		*format = NVC57E_SET_PARAMS_FORMAT_A8R8G8B8;
		return (0);
	case DRM_FORMAT_RGB565:
		*format = NVC57E_SET_PARAMS_FORMAT_R5G6B5;
		return (0);
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
		*format = NVC57E_SET_PARAMS_FORMAT_A1R5G5B5;
		return (0);
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		*format = NVC57E_SET_PARAMS_FORMAT_A2B10G10R10;
		return (0);
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		*format = NVC57E_SET_PARAMS_FORMAT_A8B8G8R8;
		return (0);
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		*format = NVC57E_SET_PARAMS_FORMAT_A2R10G10B10;
		return (0);
#ifdef DRM_FORMAT_XBGR16161616F
	case DRM_FORMAT_XBGR16161616F:
		*format = NVC57E_SET_PARAMS_FORMAT_RF16_GF16_BF16_AF16;
		return (0);
#endif
#ifdef DRM_FORMAT_ABGR16161616F
	case DRM_FORMAT_ABGR16161616F:
		*format = NVC57E_SET_PARAMS_FORMAT_RF16_GF16_BF16_AF16;
		return (0);
#endif
	default:
		return (EINVAL);
	}
}

static int
disp_nv50_wndw_atom_first_light(struct nvkm_softc *sc,
    struct disp_nv50_wndw_atom *asyw, uint32_t win, uint32_t head,
    const struct nvkm_disp_scanout *scanout)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	uint32_t image_format;
	int err;

	if (disp == NULL || asyw == NULL || scanout == NULL)
		return (EINVAL);
	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	if (!wndw->ready)
		return (ENXIO);
	if (scanout->paddr == 0 || scanout->width == 0 ||
	    scanout->height == 0 || scanout->pitch == 0)
		return (EINVAL);
	if ((scanout->pitch & 0x3fu) != 0)
		return (EINVAL);
	if (scanout->modifier != DRM_FORMAT_MOD_LINEAR &&
	    scanout->modifier != DRM_FORMAT_MOD_INVALID)
		return (EINVAL);
	err = disp_nv50_wndw_format(scanout->format, &image_format);
	if (err != 0)
		return (err);

	memset(asyw, 0, sizeof(*asyw));
	asyw->win = win;
	asyw->head = head;
	asyw->visible = 1;
	asyw->set_mask = DISP_WNDW_ATOM_IMAGE | DISP_WNDW_ATOM_CSC |
	    DISP_WNDW_ATOM_BLEND | DISP_WNDW_ATOM_POINT;
	if (wndw->ramht_ready) {
		asyw->set_mask |= DISP_WNDW_ATOM_SEMA | DISP_WNDW_ATOM_NTFY;
		asyw->sema_handle = NV50_DISP_HANDLE_SYNCBUF;
		asyw->sema_offset = wndw->sema;
		asyw->sema_acquire = 0;
		asyw->sema_release = 1;
		asyw->ntfy_handle = NV50_DISP_HANDLE_SYNCBUF;
		asyw->ntfy_offset = wndw->ntfy;
		asyw->ntfy_awaken = 0;
		wndw->armed_ntfy = wndw->ntfy;
	}
	if (disp->ilut_paddr != 0) {
		asyw->set_mask |= DISP_WNDW_ATOM_XLUT;
		asyw->xlut_control = NVC57E_ILUT_CONTROL_IDENTITY_DIRECT10;
		asyw->xlut_handle = NV50_DISP_HANDLE_VRAM;
		asyw->xlut_offset = (uint32_t)(disp->ilut_paddr >> 8);
	} else {
		asyw->clr_mask |= DISP_WNDW_ATOM_XLUT;
	}
	asyw->image_handle = NV50_DISP_HANDLE_WNDW_ISO;
	asyw->image_offset = (uint32_t)(scanout->paddr >> 8);
	asyw->image_w = scanout->width;
	asyw->image_h = scanout->height;
	asyw->image_layout = NVC57E_SET_STORAGE_MEMORY_LAYOUT_PITCH;
	asyw->image_blockh = 0;
	asyw->image_pitch = scanout->pitch >> 6;
	asyw->image_format = image_format;
	if (scanout->async_flip) {
		asyw->image_interval = 0;
		asyw->image_mode = NVC57E_SET_PRESENT_CONTROL_BEGIN_IMMEDIATE;
	} else {
		asyw->image_interval = NVC57E_SET_PRESENT_CONTROL_INTERVAL_1;
		asyw->image_mode = 0;
	}
	asyw->src_x = scanout->src_x;
	asyw->src_y = scanout->src_y;
	asyw->src_w = scanout->src_w;
	asyw->src_h = scanout->src_h;
	asyw->dst_w = scanout->crtc_w;
	asyw->dst_h = scanout->crtc_h;
	disp_nv50_wndw_atom_identity_csc(asyw);
	asyw->blend_depth = NVC57E_COMPOSITION_DEPTH_PRIMARY;
	asyw->blend_k1 = NVC57E_COMPOSITION_ALPHA_OPAQUE;
	asyw->blend_factor = NVC57E_COMPOSITION_FACTOR_PIXEL_NONE;
	asyw->point_x = scanout->crtc_x;
	asyw->point_y = scanout->crtc_y;
	return (0);
}

static uint32_t
disp_headc57d_or_payload(const struct nvkm_disp_mode *m)
{
	uint32_t value;

	value = NVC57D_OUTPUT_RESOURCE_PIXEL_DEPTH_BPP_24_444 |
	    NVC57D_OUTPUT_RESOURCE_EXT_PACKET_WIN_NONE;
	if (m->nhsync)
		value |= NVC57D_OUTPUT_RESOURCE_HSYNC_NEGATIVE;
	if (m->nvsync)
		value |= NVC57D_OUTPUT_RESOURCE_VSYNC_NEGATIVE;
	return (value);
}

static int
disp_nouveau_display_change(struct nvkm_softc *sc, uint32_t display_id,
    uint32_t enable, const char *tag)
{
	(void)sc; (void)display_id; (void)enable; (void)tag;
	return (0);	/* nouveau never issues DISPLAY_CHANGE; skip */
}

static int
disp_nouveau_wndw_ntfy_wait_begun(struct nvkm_softc *sc, uint32_t win,
    uint32_t ntfy)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t st0, status;
	int us;

	if (win >= NVKM_GSP_DISP_WINDOW_NR || disp->notifier_gva == 0)
		return (EINVAL);

	st0 = 0;
	for (us = 0; us < 2000000; us += 100) {
		st0 = nvkm_gsp_bar1_rd32(sc, disp->notifier_gva + ntfy + 0x0);
		status = (st0 >> 30) & 0x3u;
		if (status == DISP_NOTIFIER_STATUS_BEGUN ||
		    status == DISP_NOTIFIER_STATUS_FINISHED)
			break;
		DELAY(100);
	}
	status = (st0 >> 30) & 0x3u;
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau wndw_ntfy_wait_begun win=%u ntfy=0x%x "
	    "status=%u raw=0x%x -> %s\n",
	    win, ntfy, status, st0,
	    status == DISP_NOTIFIER_STATUS_BEGUN ? "BEGUN" :
	    status == DISP_NOTIFIER_STATUS_FINISHED ? "FINISHED" :
	    "not-begun");
	return (status == DISP_NOTIFIER_STATUS_BEGUN ||
	    status == DISP_NOTIFIER_STATUS_FINISHED ? 0 : ETIMEDOUT);
}

static int
disp_nouveau_modeset_setup(struct nvkm_softc *sc,
    const struct nvkm_disp_mode *m, struct nvkm_disp_scanout *scanout)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint64_t fb_size;
	int err;

	if (scanout == NULL || scanout->pitch == 0 || scanout->height == 0)
		return (EINVAL);
	fb_size = (uint64_t)scanout->pitch * scanout->height;
	err = nvkm_gsp_disp_nouveau_ramht_setup(sc, fb_size);
	if (err != 0)
		return (err);
	if (scanout->paddr == 0)
		scanout->paddr = disp->fb_paddr;

	if (disp->olut_paddr == 0) {
		disp->olut_paddr = nvkm_gsp_vram_alloc(sc, DISP_OLUT_VRAM_SIZE,
		    0x1000);
		if (disp->olut_paddr != 0)
			(void)disp_olut_fill_identity(sc, disp->olut_paddr);
	}
	if (disp->ilut_paddr == 0) {
		disp->ilut_paddr = nvkm_gsp_vram_alloc(sc, DISP_ILUT_VRAM_SIZE,
		    0x1000);
		if (disp->ilut_paddr != 0)
			(void)disp_ilut_fill_identity(sc, disp->ilut_paddr);
	}
	if (scanout->paddr == disp->fb_paddr &&
	    (scanout->format == DRM_FORMAT_XRGB8888 ||
	     scanout->format == DRM_FORMAT_ARGB8888))
		(void)disp_fb_fill_bars(sc, scanout->paddr, scanout->pitch,
		    scanout->width, scanout->height);

	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau setup notifier@0x%llx fb@0x%llx olut@0x%llx "
	    "ilut@0x%llx mode=%ux%u scanout=%ux%u pitch=%u fmt=0x%x\n",
	    (unsigned long long)disp->notifier_paddr,
	    (unsigned long long)scanout->paddr,
	    (unsigned long long)disp->olut_paddr,
	    (unsigned long long)disp->ilut_paddr, m->iw, m->ih,
	    scanout->width, scanout->height, scanout->pitch, scanout->format);
	return (0);
}

static int
disp_nouveau_assign_sor(struct nvkm_softc *sc, uint32_t display_id,
    uint32_t *out_orid, uint32_t *out_proto)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_set_hdmi_enable_params *he;
	uint32_t orid = 0xffu, proto, sor_arm;
	int err;

	err = disp_r535_outp_acquire(sc, display_id, 0, 0, &orid);
	if (err != 0)
		return (err);

	/* Protocol: prefer the assigned OR's arm TMDS protocol (GV100 reg). */
	proto = NVC57D_SOR_PROTOCOL_SINGLE_TMDS_A;
	sor_arm = nvkm_rd32(sc, 0x680300 + 0x8000 + orid * 0x20) & 0x00000f00u;
	if (sor_arm == NVC57D_SOR_PROTOCOL_SINGLE_TMDS_A ||
	    sor_arm == NVC57D_SOR_PROTOCOL_SINGLE_TMDS_B)
		proto = sor_arm;

	/* Activate the HDMI encoder for this displayId. */
	he = nvkm_gsp_rm_ctrl_get(&disp->objcom,
	    NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_ENABLE, sizeof(*he));
	if (he != NULL) {
		memset(he, 0, sizeof(*he));
		he->subDeviceInstance = 0;
		he->displayId = display_id;
		he->enable = 1;
		err = nvkm_gsp_rm_ctrl_wr(&disp->objcom, he);
		nvkm_infof(sc->dev,
		    "gsp_disp: nouveau HDMI_ENABLE display=0x%x err=%d\n",
		    display_id, err);
	} else {
		nvkm_infof(sc->dev,
		    "gsp_disp: nouveau HDMI_ENABLE display=0x%x alloc failed\n",
		    display_id);
	}

	if (out_orid != NULL)
		*out_orid = orid;
	if (out_proto != NULL)
		*out_proto = proto;
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau assign_sor display=0x%x -> or=%u proto=0x%x\n",
	    display_id, orid, proto);
	return (0);
}

static int
disp_nouveau_corec37d_update(struct nvkm_softc *sc,
    const struct nvkm_disp_interlock *interlock, const char *tag)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	volatile uint32_t *cpb = disp->core_push_kva;
	uint32_t cmds[16], n, st0, status, put_start, put_end;
	int cpush, us;

	if (tag == NULL)
		tag = "corec37d_update";

#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
#define M2(off, v0, v1) do { cmds[n++] = evo_method_hdr((off), 2); cmds[n++] = (v0); cmds[n++] = (v1); } while (0)
	nvkm_gsp_bar1_wr32(sc, disp->notifier_gva + 0x0, 0);
	nvkm_gsp_bar1_wr32(sc, disp->notifier_gva + 0x4, 0);
	nvkm_gsp_bar1_wr32(sc, disp->notifier_gva + 0x8, 0);
	nvkm_gsp_bar1_wr32(sc, disp->notifier_gva + 0xc, 0);
	disp_clear_exception_chid(sc, 0);
	n = 0;
	M(NVC57D_SET_NOTIFIER_CONTROL,
	    NVC57D_NOTIFIER_CONTROL_NOTIFY_ENABLE |
	    NVC57D_NOTIFIER_CONTROL_OFFSET(NVC57D_CORE_NOTIFIER_OFFSET));
	M2(NVC57D_SET_INTERLOCK_FLAGS, interlock->curs, interlock->wndw);
	M(NVC57D_UPDATE, 0x1u);
	M(NVC57D_SET_NOTIFIER_CONTROL, 0x00000000u);
#undef M2
#undef M
	put_start = disp->core_put_cur;
	cpush = disp_chan_push(sc, cpb, disp->core_put_reg,
	    &disp->core_put_cur,
	    disp->core_push_size / 4, cmds, n);
	put_end = disp->core_put_cur;
	disp_log_push_range(sc, tag, cpb, put_start, put_end);
	st0 = 0;
	for (us = 0; us < 2000000; us += 100) {
		st0 = nvkm_gsp_bar1_rd32(sc, disp->notifier_gva + 0x0);
		if (((st0 >> 30) & 0x3u) == DISP_NOTIFIER_STATUS_FINISHED)
			break;
		DELAY(100);
	}
	status = (st0 >> 30) & 0x3u;
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau %s core=%u curs=0x%x wndw=0x%x push=%d "
	    "coreGET=%u ntfy=0x%x -> %s\n",
	    tag, interlock->core, interlock->curs, interlock->wndw, cpush,
	    nvkm_rd32(sc, disp->core_put_reg + 4), st0,
	    status == DISP_NOTIFIER_STATUS_FINISHED ? "FINISHED" : "no notifier");
	disp_dump_exception_chid_tag(sc, tag, 0, 1);
	return (cpush);
}

static int
disp_nouveau_step5_enable_encoder(struct nvkm_softc *sc, uint32_t head,
    uint32_t orid, uint32_t proto, uint32_t display_id)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	volatile uint32_t *cpb = disp->core_push_kva;
	uint32_t cmds[8], n, base, put = 0, get;
	int err;

#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
	disp_clear_exception_chid(sc, 0);
	n = 0;
	/* headc57d_display_id(), then nv50_sor_update(). */
	M(NVC57D_HEAD_SET_DISPLAY_ID(head), display_id);
	M(NVC57D_SOR_SET_CONTROL(orid), proto | (1u << head));
#undef M
	base = disp->core_put_cur;
	err = disp_chan_emit(sc, cpb, disp->core_put_reg,
	    &disp->core_put_cur, disp->core_push_size / 4, cmds, n, &put);
	disp_log_push_range(sc, "step5_encoder", cpb, base, put);
	get = nvkm_rd32(sc, disp->core_put_reg + 4);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau step5 encoder head=%u or=%u proto=0x%x "
	    "display=0x%x emit=%d corePUT=%u coreGET=%u\n",
	    head, orid, proto, display_id, err, put, get);
	disp_dump_exception_chid_tag(sc, "step5_encoder-after", 0, 1);
	return (err);
}

static int
disp_nouveau_step6_head_flush_set(struct nvkm_softc *sc,
    const struct disp_nv50_head_atom *asyh)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	volatile uint32_t *cpb = disp->core_push_kva;
	uint32_t cmds[64], n, base, put = 0, get;
	int err;

	if (asyh == NULL)
		return (EINVAL);
#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
	disp_clear_exception_chid(sc, 0);
	n = 0;
	/* nv50_head_flush_set(): headc37d_view(), headc57d_mode(),
	 * headc57d_procamp(), headc57d_or(). */
	M(NVC57D_HEAD_SET_VIEWPORT_SIZE_IN(asyh->head), asyh->view_in);
	M(NVC57D_HEAD_SET_VIEWPORT_SIZE_OUT(asyh->head), asyh->view_out);
	cmds[n++] = evo_method_hdr(NVC57D_HEAD_SET_RASTER_SIZE(asyh->head), 4);
	cmds[n++] = asyh->raster;
	cmds[n++] = asyh->sync;
	cmds[n++] = asyh->blanke;
	cmds[n++] = asyh->blanks;
	M(NVC57D_HEAD_SET_RASTER_VERT_BLANK2(asyh->head), asyh->blank2);
	M(NVC57D_HEAD_SET_CONTROL(asyh->head), asyh->interlace);
	M(NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(asyh->head), asyh->clock);
	M(NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(asyh->head), asyh->clock_max);
	M(NVC57D_HEAD_SET_HEAD_USAGE_BOUNDS(asyh->head), asyh->usage);
	M(NVC57D_HEAD_SET_TILE_POSITION(asyh->head), 0x00000000u);
	M(NVC57D_HEAD_SET_DITHER_CONTROL(asyh->head), asyh->dither);
	M(NVC57D_HEAD_SET_PROCAMP(asyh->head), asyh->procamp);
	M(NVC57D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(asyh->head), asyh->or_payload);
#undef M
	base = disp->core_put_cur;
	err = disp_chan_emit(sc, cpb, disp->core_put_reg,
	    &disp->core_put_cur, disp->core_push_size / 4, cmds, n, &put);
	disp_log_push_range(sc, "step6_head", cpb, base, put);
	get = nvkm_rd32(sc, disp->core_put_reg + 4);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau step6 head_flush_set head=%u emit=%d "
	    "corePUT=%u coreGET=%u or_payload=0x%x sync=%c%c interlace=%u\n",
	    asyh->head, err, put, get, asyh->or_payload,
	    asyh->nhsync ? '-' : '+', asyh->nvsync ? '-' : '+',
	    asyh->interlace);
	disp_dump_exception_chid_tag(sc, "step6_head-after", 0, 1);
	return (err);
}

static int
disp_nouveau_step7_assign_windows(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_disp_interlock interlock;
	volatile uint32_t *cpb = disp->core_push_kva;
	uint32_t cmds[24], n, base, put = 0, get;
	int err;

	if (!disp->core_assign_windows) {
		nvkm_infof(sc->dev,
		    "gsp_disp: nouveau step7 wndw_owner skipped "
		    "assign_windows=0\n");
		return (0);
	}

#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
	disp_clear_exception_chid(sc, 0);
	n = 0;
	for (uint32_t i = 0; i < NVKM_GSP_DISP_WINDOW_NR; i++)
		M(NVC57D_WINDOW_SET_CONTROL(i), i >> 1);
#undef M
	base = disp->core_put_cur;
	err = disp_chan_emit(sc, cpb, disp->core_put_reg,
	    &disp->core_put_cur, disp->core_push_size / 4, cmds, n, &put);
	disp_log_push_range(sc, "step7_wndw_owner", cpb, base, put);
	get = nvkm_rd32(sc, disp->core_put_reg + 4);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau step7 wndw_owner emit=%d corePUT=%u "
	    "coreGET=%u\n",
	    err, put, get);
	if (err != 0)
		return (err);

	memset(&interlock, 0, sizeof(interlock));
	err = disp_nouveau_corec37d_update(sc, &interlock,
	    "step7_assign_windows");
	if (err == 0)
		disp->core_assign_windows = 0;
	disp_dump_gv100_head_window_state(sc, "step7_assign_windows-state");
	return (err);
}

static int
disp_nouveau_step8_head_flush_set_wndw(struct nvkm_softc *sc,
    const struct disp_nv50_head_atom *asyh)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	volatile uint32_t *cpb = disp->core_push_kva;
	uint32_t cmds[16], n, base, put = 0, get;
	int err;

	if (asyh == NULL)
		return (EINVAL);
#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
	disp_clear_exception_chid(sc, 0);
	n = 0;
	if (asyh->olut_handle != 0) {
		M(NVC57D_HEAD_SET_OLUT_CONTROL(asyh->head), asyh->olut_control);
		M(NVC57D_HEAD_SET_OLUT_FP_NORM_SCALE(asyh->head),
		    asyh->olut_scale);
		M(NVC57D_HEAD_SET_CONTEXT_DMA_OLUT(asyh->head),
		    asyh->olut_handle);
		M(NVC57D_HEAD_SET_OFFSET_OLUT(asyh->head), asyh->olut_offset);
	} else {
		M(NVC57D_HEAD_SET_CONTEXT_DMA_OLUT(asyh->head), 0x00000000u);
	}
#undef M
	base = disp->core_put_cur;
	err = disp_chan_emit(sc, cpb, disp->core_put_reg,
	    &disp->core_put_cur, disp->core_push_size / 4, cmds, n, &put);
	disp_log_push_range(sc, "step8_head_wndw", cpb, base, put);
	get = nvkm_rd32(sc, disp->core_put_reg + 4);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau step8 head_flush_set_wndw head=%u emit=%d "
	    "corePUT=%u coreGET=%u\n",
	    asyh->head, err, put, get);
	disp_dump_exception_chid_tag(sc, "step8_head_wndw-after", 0, 1);
	return (err);
}

static int
disp_nouveau_wndwc57e_flush_set_atom(struct nvkm_softc *sc,
    const struct disp_nv50_wndw_atom *asyw)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	volatile uint32_t *wpb;
	uint32_t cmds[128], n, wbase, wput = 0, wget;
	int err;

	if (asyw == NULL)
		return (EINVAL);
	if (asyw->win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[asyw->win];
	if (!wndw->ready)
		return (ENXIO);
	wpb = wndw->push_kva;

#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
#define M2(off, v0, v1) do { cmds[n++] = evo_method_hdr((off), 2); cmds[n++] = (v0); cmds[n++] = (v1); } while (0)
#define M3(off, v0, v1, v2) do { cmds[n++] = evo_method_hdr((off), 3); cmds[n++] = (v0); cmds[n++] = (v1); cmds[n++] = (v2); } while (0)
#define M4(off, v0, v1, v2, v3) do { cmds[n++] = evo_method_hdr((off), 4); cmds[n++] = (v0); cmds[n++] = (v1); cmds[n++] = (v2); cmds[n++] = (v3); } while (0)
#define M7(off, v0, v1, v2, v3, v4, v5, v6) do { cmds[n++] = evo_method_hdr((off), 7); cmds[n++] = (v0); cmds[n++] = (v1); cmds[n++] = (v2); cmds[n++] = (v3); cmds[n++] = (v4); cmds[n++] = (v5); cmds[n++] = (v6); } while (0)
	if (disp->notifier_gva != 0 && wndw->ramht_ready) {
		if ((asyw->set_mask & DISP_WNDW_ATOM_SEMA) != 0)
			nvkm_gsp_bar1_wr32(sc,
			    disp->notifier_gva + asyw->sema_offset, 0);
		if ((asyw->set_mask & DISP_WNDW_ATOM_NTFY) != 0) {
			nvkm_gsp_bar1_wr32(sc,
			    disp->notifier_gva + asyw->ntfy_offset + 0x0, 0);
			nvkm_gsp_bar1_wr32(sc,
			    disp->notifier_gva + asyw->ntfy_offset + 0x4, 0);
			nvkm_gsp_bar1_wr32(sc,
			    disp->notifier_gva + asyw->ntfy_offset + 0x8, 0);
			nvkm_gsp_bar1_wr32(sc,
			    disp->notifier_gva + asyw->ntfy_offset + 0xc, 0);
		}
		nvkm_gsp_bar1_flush(sc);
	}
	n = 0;
	if ((asyw->set_mask & DISP_WNDW_ATOM_SEMA) != 0) {
		if (asyw->sema_offset <= 0xffu) {
			M4(NVC57E_SET_SEMAPHORE_CONTROL,
			    NVC57E_SEMAPHORE_CONTROL_OFFSET(asyw->sema_offset),
			    asyw->sema_acquire, asyw->sema_release,
			    asyw->sema_handle);
		} else {
			nvkm_infof(sc->dev,
			    "gsp_disp: nouveau step9 wndw_flush_set win=%u "
			    "sema=0x%x outside C57E 8-bit offset field\n",
			    asyw->win, asyw->sema_offset);
		}
	}
	if ((asyw->set_mask & DISP_WNDW_ATOM_NTFY) != 0) {
		M2(NVC57E_SET_CONTEXT_DMA_NOTIFIER, asyw->ntfy_handle,
		    (asyw->ntfy_awaken ? 1u : 0u) |
		    NVC57E_NOTIFIER_CONTROL_OFFSET(asyw->ntfy_offset >> 4));
		wndw->ntfy ^= 0x10;
	}
	if ((asyw->set_mask & DISP_WNDW_ATOM_IMAGE) != 0) {
		M(NVC57E_SET_PRESENT_CONTROL,
		    asyw->image_interval | (asyw->image_mode << 4));
		M4(NVC57E_SET_SIZE, asyw->image_w | (asyw->image_h << 16),
		    asyw->image_layout | asyw->image_blockh,
		    asyw->image_format, asyw->image_pitch);
		M(NVC57E_SET_CONTEXT_DMA_ISO(0), asyw->image_handle);
		M(NVC57E_SET_OFFSET(0), asyw->image_offset);
		M(NVC57E_SET_POINT_IN(0), asyw->src_x | (asyw->src_y << 16));
		M(NVC57E_SET_SIZE_IN, asyw->src_w | (asyw->src_h << 16));
		M(NVC57E_SET_SIZE_OUT, asyw->dst_w | (asyw->dst_h << 16));
	}
	if ((asyw->set_mask & DISP_WNDW_ATOM_XLUT) != 0) {
		M3(NVC57E_SET_ILUT_CONTROL, asyw->xlut_control,
		    asyw->xlut_handle, asyw->xlut_offset);
	} else if ((asyw->clr_mask & DISP_WNDW_ATOM_XLUT) != 0) {
		M(NVC57E_SET_CONTEXT_DMA_ILUT, 0x00000000u);
	}
	if ((asyw->set_mask & DISP_WNDW_ATOM_CSC) != 0) {
		cmds[n++] = evo_method_hdr(NVC57E_SET_FMT_COEFFICIENT(0), 12);
		for (uint32_t i = 0; i < 12; i++)
			cmds[n++] = asyw->csc_matrix[i];
	}
	if ((asyw->set_mask & DISP_WNDW_ATOM_BLEND) != 0) {
		M7(NVC57E_SET_COMPOSITION_CONTROL, asyw->blend_depth,
		    asyw->blend_k1, asyw->blend_factor,
		    NVC57E_KEY_RANGE_FULL, NVC57E_KEY_RANGE_FULL,
		    NVC57E_KEY_RANGE_FULL, NVC57E_KEY_RANGE_FULL);
	}
#undef M7
#undef M4
#undef M3
#undef M2
#undef M
	wbase = wndw->put_cur;
	err = disp_chan_emit(sc, wpb, wndw->put_reg,
	    &wndw->put_cur, wndw->push_size / 4, cmds, n, &wput);
	disp_log_push_range(sc, "step9_window", wpb, wbase, wput);
	wget = nvkm_rd32(sc, wndw->put_reg + 4);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau step9 wndw_flush_set win=%u head=%u emit=%d "
	    "winPUT=%u winGET=%u fb@0x%llx pitch=%u ilut@0x%llx "
	    "sema=0x%x ntfy=0x%x next=0x%x\n",
	    asyw->win, asyw->head, err, wput, wget,
	    (unsigned long long)((uint64_t)asyw->image_offset << 8),
	    asyw->image_pitch << 6, (unsigned long long)disp->ilut_paddr,
	    asyw->sema_offset, asyw->ntfy_offset, wndw->ntfy);
	return (err);
}

static int
disp_nouveau_step9_wndw_flush_set(struct nvkm_softc *sc, uint32_t win,
    uint32_t head, const struct nvkm_disp_scanout *scanout)
{
	struct disp_nv50_wndw_atom asyw;
	int err;

	err = disp_nv50_wndw_atom_first_light(sc, &asyw, win, head, scanout);
	if (err != 0)
		return (err);
	return (disp_nouveau_wndwc57e_flush_set_atom(sc, &asyw));
}

static int
disp_nouveau_cursc37a_point(struct nvkm_softc *sc, uint32_t head,
    uint32_t x, uint32_t y)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_curs *curs;
	uint32_t val;

	if (head >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	curs = &disp->curs[head];
	if (!curs->ready)
		return (0);
	val = (x & 0xffffu) | ((y & 0xffffu) << 16);
	nvkm_wr32(sc, curs->put_reg + NVC57A_SET_CURSOR_HOT_SPOT_POINT_OUT(0),
	    val);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau cursc37a_point head=%u point=%u,%u val=0x%x\n",
	    head, x, y, val);
	return (0);
}

static int
disp_nouveau_cursc37a_update(struct nvkm_softc *sc, uint32_t head)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_curs *curs;

	if (head >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	curs = &disp->curs[head];
	if (!curs->ready)
		return (0);
	nvkm_wr32(sc, curs->put_reg + NVC57A_UPDATE, 0x00000001u);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau cursc37a_update head=%u UPDATE=1\n", head);
	return (0);
}

static int
disp_nouveau_step9_wimm_point_update(struct nvkm_softc *sc, uint32_t win,
    const struct nvkm_disp_interlock *interlock, uint32_t x, uint32_t y)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	struct nvkm_gsp_disp_wimm *wimm;
	volatile uint32_t *wpb;
	uint32_t cmds[4], n, update, wput = 0, wget;
	int err;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	wimm = &disp->wimm[win];
	if (!wimm->ready)
		return (0);
	wpb = wimm->push_kva;

#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
	n = 0;
	M(NVC57B_SET_POINT_OUT(0), (x & 0xffffu) | ((y & 0xffffu) << 16));
	update = 0x00000001u;
	if ((interlock->wndw & wndw->interlock_data) != 0)
		update |= NVC57B_UPDATE_INTERLOCK_WITH_WINDOW;
	M(NVC57B_UPDATE, update);
#undef M
	err = disp_chan_emit(sc, wpb, wimm->put_reg, &wimm->put_cur,
	    wimm->push_size / 4, cmds, n, &wput);
	wget = nvkm_rd32(sc, wimm->put_reg + 4);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau step9 wimm_point_update win=%u emit=%d "
	    "wimmPUT=%u wimmGET=%u point=%u,%u update=0x%x\n",
	    win, err, wput, wget, x, y, update);
	return (err);
}

static int
disp_nouveau_step10_wndw_update(struct nvkm_softc *sc, uint32_t win,
    const struct nvkm_disp_interlock *interlock)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	volatile uint32_t *wpb;
	uint32_t cmds[8], n, wbase, wput = 0, wget;
	int err;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	if (!wndw->ready)
		return (ENXIO);
	wpb = wndw->push_kva;

#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
#define M2(off, v0, v1) do { cmds[n++] = evo_method_hdr((off), 2); cmds[n++] = (v0); cmds[n++] = (v1); } while (0)
	n = 0;
	M2(NVC57E_SET_INTERLOCK_FLAGS,
	    (interlock->curs << 1) | interlock->core, interlock->wndw);
	M(NVC57E_UPDATE,
	    0x1u | ((interlock->wimm & (1u << win)) != 0 ? (1u << 12) : 0));
#undef M2
#undef M
	wbase = wndw->put_cur;
	err = disp_chan_emit(sc, wpb, wndw->put_reg,
	    &wndw->put_cur, wndw->push_size / 4, cmds, n, &wput);
	disp_log_push_range(sc, "step10_window", wpb, wbase, wput);
	wget = nvkm_rd32(sc, wndw->put_reg + 4);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau step10 wndw_update win=%u emit=%d "
	    "winPUT=%u winGET=%u\n",
	    win, err, wput, wget);
	return (err);
}

static int
disp_nouveau_wndwc37e_image_clr(struct nvkm_softc *sc, uint32_t win)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	volatile uint32_t *wpb;
	uint32_t cmds[4], n, wput = 0;
	int err;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	if (!wndw->ready)
		return (0);
	wpb = wndw->push_kva;

#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
	n = 0;
	M(NVC57E_SET_PRESENT_CONTROL, 0x00000000u);
	M(NVC57E_SET_CONTEXT_DMA_ISO(0), 0x00000000u);
#undef M
	err = disp_chan_emit(sc, wpb, wndw->put_reg, &wndw->put_cur,
	    wndw->push_size / 4, cmds, n, &wput);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau wndwc37e_image_clr win=%u emit=%d PUT=%u GET=%u\n",
	    win, err, wput, nvkm_rd32(sc, wndw->put_reg + 4));
	return (err);
}

static int
disp_nouveau_wndwc37e_ntfy_clr(struct nvkm_softc *sc, uint32_t win)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	uint32_t cmds[2], wput = 0;
	int err;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	if (!wndw->ready)
		return (0);
	cmds[0] = evo_method_hdr(NVC57E_SET_CONTEXT_DMA_NOTIFIER, 1);
	cmds[1] = 0x00000000u;
	err = disp_chan_emit(sc, wndw->push_kva, wndw->put_reg,
	    &wndw->put_cur, wndw->push_size / 4, cmds, 2, &wput);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau wndwc37e_ntfy_clr win=%u emit=%d PUT=%u GET=%u\n",
	    win, err, wput, nvkm_rd32(sc, wndw->put_reg + 4));
	return (err);
}

static int
disp_nouveau_wndwc37e_sema_clr(struct nvkm_softc *sc, uint32_t win)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	uint32_t cmds[2], wput = 0;
	int err;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	if (!wndw->ready)
		return (0);
	cmds[0] = evo_method_hdr(NVC57E_SET_CONTEXT_DMA_SEMAPHORE, 1);
	cmds[1] = 0x00000000u;
	err = disp_chan_emit(sc, wndw->push_kva, wndw->put_reg,
	    &wndw->put_cur, wndw->push_size / 4, cmds, 2, &wput);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau wndwc37e_sema_clr win=%u emit=%d PUT=%u GET=%u\n",
	    win, err, wput, nvkm_rd32(sc, wndw->put_reg + 4));
	return (err);
}

static int
disp_nouveau_wndwc57e_ilut_clr(struct nvkm_softc *sc, uint32_t win)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	uint32_t cmds[2], wput = 0;
	int err;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	if (!wndw->ready)
		return (0);
	cmds[0] = evo_method_hdr(NVC57E_SET_CONTEXT_DMA_ILUT, 1);
	cmds[1] = 0x00000000u;
	err = disp_chan_emit(sc, wndw->push_kva, wndw->put_reg,
	    &wndw->put_cur, wndw->push_size / 4, cmds, 2, &wput);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau wndwc57e_ilut_clr win=%u emit=%d PUT=%u GET=%u\n",
	    win, err, wput, nvkm_rd32(sc, wndw->put_reg + 4));
	return (err);
}

static int
disp_nouveau_wndw_release(struct nvkm_softc *sc, uint32_t win,
    const struct nvkm_disp_interlock *interlock)
{
	int err;

	err = disp_nouveau_wndwc37e_ntfy_clr(sc, win);
	if (err != 0)
		return (err);
	err = disp_nouveau_wndwc37e_sema_clr(sc, win);
	if (err != 0)
		return (err);
	err = disp_nouveau_wndwc57e_ilut_clr(sc, win);
	if (err != 0)
		return (err);
	err = disp_nouveau_wndwc37e_image_clr(sc, win);
	if (err != 0)
		return (err);
	return (disp_nouveau_step10_wndw_update(sc, win, interlock));
}

static int
disp_nouveau_wndw_wait_armed(struct nvkm_softc *sc, uint32_t win,
    uint32_t timeout_us)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_disp_window *wndw;
	uint32_t put, get;

	if (win >= NVKM_GSP_DISP_WINDOW_NR)
		return (EINVAL);
	wndw = &disp->window[win];
	if (!wndw->ready)
		return (0);
	put = nvkm_rd32(sc, wndw->put_reg);
	get = disp_chan_wait_get(sc, wndw->put_reg, put, timeout_us);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau wndw_wait_armed win=%u PUT=%u GET=%u\n",
	    win, put, get);
	return (get == put ? 0 : ETIMEDOUT);
}

int
nvkm_gsp_disp_nouveau_commit_tail(struct nvkm_softc *sc, uint32_t head,
    uint32_t win, uint32_t display_id, const struct nvkm_disp_mode *m,
    const struct nvkm_disp_scanout *scanout)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_disp_interlock interlock;
	struct disp_nv50_head_atom asyh;
	struct nvkm_disp_scanout asyw_scanout;
	uint32_t orid, proto;
	int err, change_started = 0;

	if (disp == NULL || m == NULL || scanout == NULL)
		return (ENODEV);

	asyw_scanout = *scanout;
	memset(&interlock, 0, sizeof(interlock));
	err = disp_nouveau_modeset_setup(sc, m, &asyw_scanout);
	if (err != 0)
		return (err);

	err = disp_nouveau_assign_sor(sc, display_id, &orid, &proto);
	if (err != 0)
		return (err);
	disp_nv50_head_atom_first_light(&asyh, head, display_id, orid, proto, m,
	    disp->olut_paddr);

	err = disp_nouveau_display_change(sc, display_id,
	    NV0073_CTRL_SPECIFIC_DISPLAY_CHANGE_START, "START");
	if (err == 0) {
		change_started = 1;
	} else {
		nvkm_infof(sc->dev,
		    "gsp_disp: nouveau display_change START failed; "
		    "continuing EVO sequence err=%d\n", err);
		err = 0;
	}

	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau commit_tail begin head=%u win=%u display=0x%x "
	    "or=%u proto=0x%x mode=%ux%u sync=%c%c interlace=%u\n",
	    asyh.head, win, asyh.display_id, asyh.orid, asyh.proto, m->iw, m->ih,
	    m->nhsync ? '-' : '+', m->nvsync ? '-' : '+', m->interlace);

	/* Steps 1-4 are inactive for the first-light enable-only atomic state. */
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau commit_tail assign_windows=%u\n",
	    disp->core_assign_windows);

	err = disp_nouveau_step5_enable_encoder(sc, asyh.head, asyh.orid,
	    asyh.proto, asyh.display_id);
	if (err != 0)
		goto out_end;
	interlock.core = 1;

	err = disp_nouveau_step6_head_flush_set(sc, &asyh);
	if (err != 0)
		goto out_end;
	interlock.core = 1;

	err = disp_nouveau_step7_assign_windows(sc);
	if (err != 0)
		goto out_end;
	interlock.core = 0;

	err = disp_nouveau_step8_head_flush_set_wndw(sc, &asyh);
	if (err != 0)
		goto out_end;
	interlock.core = 1;

	err = disp_nouveau_step9_wndw_flush_set(sc, win, head, &asyw_scanout);
	if (err != 0)
		goto out_end;
	interlock.wndw |= 1u << win;

	err = disp_nouveau_step9_wimm_point_update(sc, win, &interlock, 0, 0);
	if (err != 0)
		goto out_end;
	if (disp->wimm[win].ready)
		interlock.wimm |= disp->window[win].interlock_wimm;

	err = disp_nouveau_step10_wndw_update(sc, win, &interlock);
	if (err != 0)
		goto out_end;

	if (interlock.core)
		err = disp_nouveau_corec37d_update(sc, &interlock,
		    "step10_final_core");
	if (err == 0 && win < NVKM_GSP_DISP_WINDOW_NR &&
	    disp->window[win].ramht_ready)
		err = disp_nouveau_wndw_ntfy_wait_begun(sc, win,
		    disp->window[win].armed_ntfy);

out_end:
	if (change_started)
		(void)disp_nouveau_display_change(sc, display_id,
		    NV0073_CTRL_SPECIFIC_DISPLAY_CHANGE_END, "END");
	if (err == 0)
		(void)disp_nouveau_wndw_wait_armed(sc, win, 100000);
	nvkm_infof(sc->dev,
	    "gsp_disp: nouveau commit_tail end err=%d head=%u win=%u\n",
	    err, head, win);
	return (err);
}

/* Dump host-side disp state after a modeset, to tell apart "GSP latched but
 * notifier didn't flip" from "disp engine PRI is dead (BROKEN_FB)". */
void
nvkm_gsp_disp_dump_state(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	uint32_t n0, n1, n2, n3;
	uint32_t h0_state, h0_set, h612608, e10, e14, e78, sor1_asy;
	uint32_t sor1_arm_live;
	uint32_t intr_c30, intr_top, super_a8, intren, core_put, core_get;
	uint32_t window_put, window_get, window_ready = 0;
	uint32_t exc0_stat, exc0_data, exc0_code, exc0_type, exc0_method;
	uint32_t exc1_stat, exc1_data, exc1_code, exc1_type, exc1_method;
	uint64_t logrm_put;

	n0 = nvkm_gsp_bar1_rd32(sc, disp->notifier_gva + 0x0);
	n1 = nvkm_gsp_bar1_rd32(sc, disp->notifier_gva + 0x4);
	n2 = nvkm_gsp_bar1_rd32(sc, disp->notifier_gva + 0x8);
	n3 = nvkm_gsp_bar1_rd32(sc, disp->notifier_gva + 0xc);

	h0_state  = nvkm_rd32(sc, 0x612078);	/* FE_CORE_HEAD_STATE head0 */
	h0_set    = nvkm_rd32(sc, 0x616300);
	h612608   = nvkm_rd32(sc, 0x612608);
	e10       = nvkm_rd32(sc, 0x610010);
	e14       = nvkm_rd32(sc, 0x610014);
	e78       = nvkm_rd32(sc, 0x610078);
	sor1_asy  = nvkm_rd32(sc, 0x680300 + 1 * 0x20);
	sor1_arm_live = nvkm_rd32(sc, 0x680300 + 0x8000 + 1 * 0x20);
	intr_c30  = nvkm_rd32(sc, 0x611c30);	/* disp supervisor intr */
	intr_top  = nvkm_rd32(sc, 0x611800);	/* disp intr top */
	super_a8  = nvkm_rd32(sc, 0x6107a8);	/* FE pending-changes */
	intren    = nvkm_rd32(sc, 0x611494);	/* disp intr enable */
	core_put  = nvkm_rd32(sc, disp->core_put_reg);
	core_get  = nvkm_rd32(sc, disp->core_put_reg + 4);
	for (uint32_t win = 0; win < NVKM_GSP_DISP_WINDOW_NR; win++) {
		if (disp->window[win].ready)
			window_ready |= 1u << win;
	}
	if (disp->window[0].ready) {
		window_put = nvkm_rd32(sc, disp->window[0].put_reg);
		window_get = nvkm_rd32(sc, disp->window[0].put_reg + 4);
	} else {
		window_put = 0;
		window_get = 0;
	}
	exc0_stat = nvkm_rd32(sc, 0x611020 + 0 * 12u);
	exc0_data = nvkm_rd32(sc, 0x611024 + 0 * 12u);
	exc0_code = nvkm_rd32(sc, 0x611028 + 0 * 12u);
	exc0_type = (exc0_stat & 0x00007000u) >> 12;
	exc0_method = (exc0_stat & 0x00000fffu) << 2;
	exc1_stat = nvkm_rd32(sc, 0x611020 + 1 * 12u);
	exc1_data = nvkm_rd32(sc, 0x611024 + 1 * 12u);
	exc1_code = nvkm_rd32(sc, 0x611028 + 1 * 12u);
	exc1_type = (exc1_stat & 0x00007000u) >> 12;
	exc1_method = (exc1_stat & 0x00000fffu) << 2;
	logrm_put = (sc->gsp_logrm.kva != NULL) ?
	    *(volatile uint64_t *)sc->gsp_logrm.kva : 0;

	nvkm_infof(sc->dev,
	    "gsp_disp: STATE ntfy=[%08x %08x %08x %08x] head0=0x%x(op=%u) "
	    "set=0x%x 612608=0x%x e10/14/78=%x/%x/%x "
	    "sor1 asy=0x%x arm_live=0x%x\n",
	    n0, n1, n2, n3, h0_state, (h0_state >> 8) & 0x3u, h0_set, h612608,
	    e10, e14, e78, sor1_asy, sor1_arm_live);
	nvkm_infof(sc->dev,
	    "gsp_disp: STATE2 intr_c30=0x%x intr_top=0x%x super6107a8=0x%x "
	    "intren611494=0x%x core PUT=%u GET=%u window_ready=0x%x "
	    "window0 PUT=%u GET=%u "
	    "disp_intr=%llu vblank=0x%x logrm_put=0x%llx\n",
	    intr_c30, intr_top, super_a8, intren, core_put, core_get,
	    window_ready, window_put, window_get,
	    (unsigned long long)sc->gsp_disp_intr_count, sc->gsp_disp_vblank_mask,
	    (unsigned long long)logrm_put);
	nvkm_infof(sc->dev,
	    "gsp_disp: STATE3 exc0=%x/%x/%x reason=%u[%s] method=0x%x "
	    "exc1=%x/%x/%x reason=%u[%s] method=0x%x\n",
	    exc0_stat, exc0_data, exc0_code, exc0_type,
	    disp_exception_reason(exc0_type), exc0_method,
	    exc1_stat, exc1_data, exc1_code, exc1_type,
	    disp_exception_reason(exc1_type), exc1_method);
	disp_dump_gv100_sor_route_state(sc, "STATE4");
	disp_dump_gv100_head_window_state(sc, "STATE4");
}

static int
nvkm_gsp_disp_brightc_state_load(struct nvkm_softc *sc,
    struct nvkm_gsp_object *subdev)
{
	struct disp_init_brightc_state_load_params *bl;
	int err;

	bl = nvkm_gsp_rm_ctrl_get(subdev,
	    NV2080_CTRL_CMD_INTERNAL_INIT_BRIGHTC_STATE_LOAD, sizeof(*bl));
	if (bl == NULL)
		return (ENOMEM);
	memset(bl, 0, sizeof(*bl));
	bl->status = NV_ERR_NOT_SUPPORTED;
	err = nvkm_gsp_rm_ctrl_wr(subdev, bl);
	if (err == 0)
		nvkm_infof(sc->dev,
		    "gsp_disp: INIT_BRIGHTC_STATE_LOAD status=0x%x size=%u\n",
		    NV_ERR_NOT_SUPPORTED, 0u);
	return (err);
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

	/* (1) Display client + device (needed to RM-allocate the inst mem).
	 * Linux r535 allocates display clients from NVKM_RM_CLIENT(id), and
	 * r535_dmac_bind stores the low 14 handle bits in RAMHT context. Keep
	 * this in the same handle family; 0xc1d00001 is already used by VMM. */
	err = nvkm_gsp_client_ctor(sc, 0xc1d00002u, &disp->client);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: client ctor err=%d\n", err);
		goto fail_free;
	}
	err = nvkm_gsp_device_ctor(&disp->client, &disp->device);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: device ctor err=%d\n", err);
		goto fail_client;
	}

	/* (2) RAMIN: RM-allocate the display inst mem (GSP-RM owns VRAM) and read
	 * back its address. A host-managed drm_mm address is not honoured by the
	 * HW hash-table view; RM must bless the inst-mem allocation. */
	err = disp_vidmem_alloc(sc, disp, &disp->inst_mem, 0xde1d0050u, 0x10000,
	    &disp->inst_paddr);
	if (err != 0 || disp->inst_paddr == 0) {
		nvkm_infof(sc->dev, "gsp_disp: RAMIN RM-alloc err=%d\n", err);
		goto fail_device;
	}

	/* (3) WRITE_INST_MEM (internal subdevice) with the RM inst-mem address. */
	nvkm_gsp_disp_internal_subdev(sc, &tmp_client, &tmp_subdev);
	wim = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM, sizeof(*wim));
	if (wim == NULL) { err = ENOMEM; goto fail_device; }
	wim->instMemPhysAddr = disp->inst_paddr;
	wim->instMemSize = 0x10000;
	wim->instMemAddrSpace = ADDR_FBMEM;
	wim->instMemCpuCacheAttr = NV_MEMORY_WRITECOMBINED;
	err = nvkm_gsp_rm_ctrl_wr(&tmp_subdev, wim);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: WRITE_INST_MEM err=%d\n", err);
		goto fail_device;
	}

	/* (4) NV04_DISPLAY_COMMON (objcom). */
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

	err = nvkm_gsp_disp_brightc_state_load(sc, &tmp_subdev);
	if (err != 0)
		nvkm_infof(sc->dev,
		    "gsp_disp: INIT_BRIGHTC_STATE_LOAD err=%d\n", err);

	/* (4b) DP_SET_MANUAL_DISPLAYPORT (0x731365): r535 issues this in oneinit
	 * before reading heads -- tells GSP the driver drives DP/modeset
	 * manually (vs GSP auto). params = { u32 subDeviceInstance; }. */
	{
		struct { uint32_t subDeviceInstance; } *dpm;

		dpm = nvkm_gsp_rm_ctrl_get(&disp->objcom, 0x731365u, sizeof(*dpm));
		if (dpm != NULL) {
			dpm->subDeviceInstance = 0;
			if (nvkm_gsp_rm_ctrl_wr(&disp->objcom, dpm) != 0)
				nvkm_infof(sc->dev,
				    "gsp_disp: DP_SET_MANUAL_DISPLAYPORT failed\n");
			else
				nvkm_infof(sc->dev,
				    "gsp_disp: DP_SET_MANUAL_DISPLAYPORT ok\n");
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
	/* GSP-RM uses r535_disp_init(), not tu102_disp_init(): the GSP owns the
	 * FE/caps/supervisor routing.  Host-side tu102_disp_init() is the
	 * non-GSP path documented in nouveau_disp.txt only to explain what the
	 * firmware runs internally. */
	nvkm_infof(sc->dev,
	    "gsp_disp: skip host tu102_disp_init; using r535/GSP display init\n");

	/* (5) r535 oneinit output enumeration: GET_SUPPORTED -> OR_GET_INFO ->
	 * GET_CONNECTOR_DATA -> DP_GET_CAPS for DP outputs. This runs before the
	 * display RAMHT and user-visible EVO channels are created in nouveau. */
	err = nvkm_gsp_disp_outp_oneinit(sc);
	if (err != 0)
		nvkm_infof(sc->dev, "gsp_disp: outp oneinit err=%d\n", err);

	/* (6) Register for runtime hotplug/DP-IRQ events. */
	(void)nvkm_gsp_disp_register_hotplug(sc);

	/* (7) Initial probe: read+print EDID of already-connected outputs.
	 * Inline on the attach thread (single-threaded bring-up); runtime
	 * re-probes use the same probe_connected() from the hotplug worker. */
	nvkm_gsp_disp_probe_connected(sc);

	/* (8) Display instmem object model (M4b): RAMHT + ctxdma descriptors in
	 * the display RAMIN, needed to resolve EVO context-DMA handles. */
	(void)nvkm_gsp_disp_instmem_init(sc);

	/* (8b) Create notifier/fb backing before channel init; per-channel RAMHT
	 * entries are bound by core/window channel creation, matching nouveau's
	 * nv50_dmac_create() + r535_dmac_bind() ordering. */
	err = nvkm_gsp_disp_nouveau_ramht_setup(sc,
	    (uint64_t)1920u * 4u * 1080u);
	if (err != 0)
		nvkm_infof(sc->dev,
		    "gsp_disp: early display backing setup err=%d\n", err);

	/* (9) Core display channel (M2a): TU102_DISP root + NVC57D DMAC. */
	(void)nvkm_gsp_disp_core_init(sc);

	/* (10) corec57d_init immediately follows core channel creation in nouveau. */
	(void)nvkm_gsp_disp_corec57d_init(sc);

	/* (11) GV100_DISP_CAPS maps the host BAR0 caps page, not a GSP channel. */
	(void)nvkm_gsp_disp_caps_init(sc);

	/* (12) Window + WIMM display channels: NVC57E/NVC57B per window. */
	(void)nvkm_gsp_disp_window_init(sc);

	/* (13) Cursor PIO channels: NVC57A per head. */
	(void)nvkm_gsp_disp_cursor_init(sc);

	return (0);

fail_device:
	nvkm_gsp_device_dtor(&disp->device);
fail_client:
	nvkm_gsp_client_dtor(&disp->client);
fail_free:
	kfree(disp);
	return (err);
}
