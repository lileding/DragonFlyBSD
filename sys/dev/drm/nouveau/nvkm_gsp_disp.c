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
#define TU102_DISP_WINDOW_CHANNEL_DMA		0x0000c57eu	/* window (NVC57E) */
#define NVKM_RM_DISP_WINDOW			0xc57e0000u
/* Window image methods (clc57e.h, MIT). */
#define NVC57E_SET_SIZE				0x00000224u
#define NVC57E_SET_SIZE_OUT			0x000002a4u
/* Core notifier methods (clc57d.h/clc37d.h, MIT). NVC57D reuses the C37D
 * method offsets for these. */
#define NVC57D_SET_CONTEXT_DMA_NOTIFIER		0x00000208u
#define NVC57D_SET_NOTIFIER_CONTROL		0x0000020cu	/* MODE[0]/OFFSET[11:4]/NOTIFY[12] */
#define NVC57D_UPDATE				0x00000200u
#define NVC57D_SET_INTERLOCK_FLAGS		0x00000218u
#define NVC57D_SET_WINDOW_INTERLOCK_FLAGS	0x0000021cu
#define NV50_DISP_HANDLE_SYNCBUF		0xf0000000u	/* notifier ctxdma handle */
#define NV50_DISP_HANDLE_VRAM			0xf0000001u	/* core whole-VRAM ctxdma */
/* NV_DISP_NOTIFIER dword0 STATUS field (bits 31:30): NOT_BEGUN=0, FINISHED=2. */
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
#define NVC57D_HEAD_SET_CONTROL(h)		(0x00002008u + (h) * 0x400u)
#define NVC57D_HEAD_SET_OLUT_CONTROL(h)		(0x00002280u + (h) * 0x400u)
#define NVC57D_HEAD_SET_OLUT_FP_NORM_SCALE(h)	(0x00002284u + (h) * 0x400u)
#define NVC57D_HEAD_SET_CONTEXT_DMA_OLUT(h)	(0x00002288u + (h) * 0x400u)
#define NVC57D_HEAD_SET_OFFSET_OLUT(h)		(0x0000228cu + (h) * 0x400u)
#define NVC57D_WINDOW_SET_CONTROL(w)		(0x00001000u + (w) * 0x80u)
#define NVC57D_WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(w) \
						(0x00001008u + (w) * 0x80u)
#define NVC57D_WINDOW_SET_WINDOW_USAGE_BOUNDS(w)	(0x00001010u + (w) * 0x80u)
#define NVC57D_SOR_SET_CONTROL(s)		(0x00000300u + (s) * 0x20u)
/* Window image methods (clc57e.h). */
#define NVC57E_SET_PRESENT_CONTROL		0x00000308u
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
#define NVC57E_SET_ILUT_CONTROL		0x00000440u
#define NVC57E_SET_CONTEXT_DMA_ILUT		0x00000444u
#define NVC57E_SET_OFFSET_ILUT			0x00000448u
#define NVC57E_UPDATE				0x00000200u
#define NVC57E_SET_INTERLOCK_FLAGS		0x00000370u
#define NVC57E_SET_WINDOW_INTERLOCK_FLAGS	0x00000374u
#define NVC57E_SET_PARAMS_FORMAT_A8R8G8B8	0x000000cfu
#define NVC57E_SET_STORAGE_MEMORY_LAYOUT_PITCH	(1u << 4)
#define NVC57E_SET_PRESENT_CONTROL_INTERVAL_1	0x00000001u
#define NVC57E_ILUT_CONTROL_IDENTITY_DIRECT10	((2u << 2) | ((4u + 1024u + 1u) << 8))
#define NVC57E_COMPOSITION_DEPTH_PRIMARY	(0xffu << 4)
#define NVC57E_COMPOSITION_ALPHA_OPAQUE		0x000000ffu
#define NVC57E_COMPOSITION_FACTOR_PIXEL_NONE	0x00004422u
#define NVC57E_KEY_RANGE_FULL			0xffff0000u
#define NVC57D_SOR_PROTOCOL_SINGLE_TMDS_A	(1u << 8)
#define NVC57D_SOR_PROTOCOL_SINGLE_TMDS_B	(2u << 8)
#define NVC57D_OUTPUT_RESOURCE_PIXEL_DEPTH_BPP_24_444 (4u << 4)
#define NVC57D_OLUT_CONTROL_IDENTITY_DIRECT10	((1u << 0) | (2u << 2) | ((4u + 1024u + 1u) << 8))
#define DISP_OLUT_VRAM_SIZE			0x3000u
#define DISP_ILUT_VRAM_SIZE			0x3000u
#define NV50_DISP_HANDLE_WNDW_ISO		0xfb000000u	/* window ISO ctxdma handle */

/* NV0073_CTRL_CMD_DFP_ASSIGN_SOR (MIT, open-gpu 570.144 via nvrm/disp.h). The
 * NvU8/NvU32 mix relies on natural alignment matching the firmware struct. */
#define NV0073_CTRL_CMD_DFP_ASSIGN_SOR		0x00731152u
#define DFP_ASSIGN_SOR_MAX_SORS			4u
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
#define EVO_WINDOW_USAGE_BOUNDS			0x00117fffu

static inline uint32_t
evo_method_hdr(uint32_t mthd, uint32_t count)
{
	/* OPCODE_METHOD(0) | METHOD_COUNT[27:18] | METHOD_OFFSET[13:2]. */
	return ((count & 0x3ffu) << 18) | (mthd & 0x3ffcu);
}

/* M4a smoke: keep the core channel pristine for the first real UPDATE. Older
 * smoke batches touched per-window usage methods and can leave a stale core FE
 * exception before modeset diagnostics run; the modeset batch below now proves
 * core PUT/GET consumption directly.
 */
static int
nvkm_gsp_disp_core_push_smoke(struct nvkm_softc *sc)
{
	nvkm_infof(sc->dev,
	    "gsp_disp: M4a core push: deferred until modeset UPDATE\n");
	return (0);
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
	uint32_t client = disp->client.object.handle & 0x3fffu;
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
	uint32_t client = disp->client.object.handle & 0x3fffu;
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

static int
disp_chan_emit(struct nvkm_softc *sc, volatile uint32_t *pb, uint32_t put_reg,
    uint32_t bufdw, const uint32_t *data, uint32_t ndw, uint32_t *out_put)
{
	uint32_t base = nvkm_rd32(sc, put_reg);
	uint32_t i, put_dw;

	if (base + ndw >= bufdw)
		return (ENOSPC);
	for (i = 0; i < ndw; i++)
		pb[base + i] = data[i];
	cpu_sfence();
	put_dw = base + ndw;
	nvkm_wr32(sc, put_reg, put_dw);
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

/* Append a method batch, kick PUT, and wait for GET to catch PUT. Methods that
 * latch (UPDATE) take effect; arm methods just sit until a later UPDATE. */
static int
disp_chan_push(struct nvkm_softc *sc, volatile uint32_t *pb, uint32_t put_reg,
    uint32_t bufdw, const uint32_t *data, uint32_t ndw)
{
	uint32_t put_dw, get;
	int err;

	err = disp_chan_emit(sc, pb, put_reg, bufdw, data, ndw, &put_dw);
	if (err != 0)
		return (err);
	get = disp_chan_wait_get(sc, put_reg, put_dw, 2000000);
	return (get == put_dw ? 0 : ETIMEDOUT);
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
		uint32_t client = disp->client.object.handle & 0x3fffu;
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

/* ===== window display channel (M4c) =====
 * Allocate the NVC57E window channel (instance 0, drives head 0) with a
 * coherent-sysmem pushbuffer under the TU102_DISP root, mirroring the core
 * channel. The window is what plane updates push image methods to; first light
 * (the framebuffer ISO ctxdma + full image push + head + UPDATE) is M4d. M4c
 * stops at allocation + a method-push smoke proving the window channel consumes
 * host-written methods (GET catches PUT), like the M4a core smoke.
 */
static int
nvkm_gsp_disp_window_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct disp_channel_pushbuffer_params *pb;
	struct disp_channeldma_alloc_params *ca;
	volatile uint32_t *wpb;
	uint32_t handle, base, put_dw, get, n;
	int err, us;

	if (disp->dispclass.handle == 0)	/* needs the TU102_DISP root */
		return (ENXIO);

	/* (1) Window pushbuffer: 4KB coherent sysmem. */
	disp->window_push_size = 0x1000;
	disp->window_push_kva = contigmalloc(disp->window_push_size, M_NVKM_DISP,
	    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
	if (disp->window_push_kva == NULL)
		return (ENOMEM);
	disp->window_push_paddr = vtophys(disp->window_push_kva);

	/* (2) Tell GSP the window channel's pushbuffer. */
	nvkm_gsp_disp_internal_subdev(sc, &tmp_client, &tmp_subdev);
	pb = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER, sizeof(*pb));
	if (pb == NULL)
		return (ENOMEM);
	memset(pb, 0, sizeof(*pb));
	pb->addressSpace = DISP_ADDR_SYSMEM;
	pb->cacheSnoop = 1;
	pb->pbTargetAperture = DISP_PHYS_PCI_COHERENT;
	pb->physicalAddr = disp->window_push_paddr;
	pb->limit = disp->window_push_size - 1;
	pb->hclass = TU102_DISP_WINDOW_CHANNEL_DMA;
	pb->channelInstance = 0;
	pb->valid = 1;
	pb->subDeviceId = 1u;		/* BIT(0) */
	err = nvkm_gsp_rm_ctrl_wr(&tmp_subdev, pb);
	if (err != 0) {
		nvkm_infof(sc->dev, "gsp_disp: window set_pushbuf err=%d\n", err);
		return (err);
	}

	/* (3) Allocate the NVC57E window channel under the display root. */
	handle = nvkm_gsp_client_child_handle(&disp->client, NVKM_RM_DISP_WINDOW);
	ca = nvkm_gsp_rm_alloc_get(&disp->dispclass, handle,
	    TU102_DISP_WINDOW_CHANNEL_DMA, sizeof(*ca), &disp->window);
	if (ca == NULL)
		return (ENOMEM);
	memset(ca, 0, sizeof(*ca));
	ca->channelInstance = 0;
	ca->offset = 0;
	ca->subDeviceId = 1u;		/* BIT(0) */
	err = nvkm_gsp_rm_alloc_wr(&disp->window, ca);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: NVC57E window alloc (handle=0x%x) err=%d\n",
		    disp->window.handle, err);
		return (err);
	}

	/* Window channel control regs: PUT/GET at BAR0 0x690000 + inst*0x1000. */
	disp->window_put_reg = 0x690000;
	nvkm_infof(sc->dev,
	    "gsp_disp: window channel up -- handle=0x%x pb@0x%llx PUT=0x%x GET=0x%x\n",
	    disp->window.handle, (unsigned long long)disp->window_push_paddr,
	    nvkm_rd32(sc, disp->window_put_reg),
	    nvkm_rd32(sc, disp->window_put_reg + 4));

	/* (M4c smoke) Push harmless arm methods (SET_SIZE / SET_SIZE_OUT), kick
	 * PUT, and confirm the window channel consumes them (GET catches PUT).
	 * Arm-only -- no UPDATE, no head ownership, so no scanout side effect. */
	wpb = disp->window_push_kva;
	base = nvkm_rd32(sc, disp->window_put_reg);
	if (base + 16u >= disp->window_push_size / 4) {
		nvkm_infof(sc->dev,
		    "gsp_disp: M4c window smoke skipped (PUT=%u)\n", base);
		return (0);
	}
	n = base;
	wpb[n++] = evo_method_hdr(NVC57E_SET_SIZE, 1);
	wpb[n++] = 1920u | (1080u << 16);
	wpb[n++] = evo_method_hdr(NVC57E_SET_SIZE_OUT, 1);
	wpb[n++] = 1920u | (1080u << 16);
	cpu_sfence();
	put_dw = n;
	nvkm_wr32(sc, disp->window_put_reg, put_dw);

	get = base;
	for (us = 0; us < 100000; us += 10) {
		get = nvkm_rd32(sc, disp->window_put_reg + 4);
		if (get == put_dw)
			break;
		DELAY(10);
	}
	nvkm_infof(sc->dev,
	    "gsp_disp: M4c window push: 2 methods [%u..%u) PUT=%u GET=%u -> %s\n",
	    base, put_dw, put_dw, get,
	    get == put_dw ? "consumed (GET caught PUT)" : "STUCK (GET != PUT)");
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

	err = disp_ramht_bind_nouveau(sc, NV50_DISP_HANDLE_SYNCBUF, 0,
	    DISP_DMAOBJ_FLAGS0_VRAM_RW_SP, disp->notifier_paddr,
	    disp->notifier_paddr + 0xfff, "core-syncbuf");
	if (err != 0)
		return (err);
	err = disp_ramht_bind_nouveau(sc, NV50_DISP_HANDLE_VRAM, 0,
	    DISP_DMAOBJ_FLAGS0_VRAM_RW_SP, 0, vram_limit, "core-vram");
	if (err != 0)
		return (err);
	err = disp_ramht_bind_nouveau(sc, NV50_DISP_HANDLE_VRAM, 1,
	    DISP_DMAOBJ_FLAGS0_VRAM_RW_SP, 0, vram_limit, "window-vram");
	if (err != 0)
		return (err);
	err = disp_ramht_bind_nouveau(sc, NV50_DISP_HANDLE_WNDW_ISO, 1,
	    DISP_DMAOBJ_FLAGS0_VRAM_RW_LP, 0, vram_limit, "window-pitch");
	if (err != 0)
		return (err);

	nvkm_gsp_bar1_flush(sc);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4e nouveau RAMHT ready ntfy@0x%llx fb@0x%llx "
	    "ram_user=0x%llx ctxdma_limit=0x%llx\n",
	    (unsigned long long)disp->notifier_paddr,
	    (unsigned long long)disp->fb_paddr,
	    (unsigned long long)sc->fb_usable_size,
	    (unsigned long long)vram_limit);
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
disp_dump_exception_chid(struct nvkm_softc *sc, uint32_t chid)
{
	uint32_t stat = nvkm_rd32(sc, 0x611020 + chid * 12u);
	uint32_t data = nvkm_rd32(sc, 0x611024 + chid * 12u);
	uint32_t code = nvkm_rd32(sc, 0x611028 + chid * 12u);
	uint32_t type = (stat & 0x00007000u) >> 12;
	uint32_t method = (stat & 0x00000fffu) << 2;

	nvkm_infof(sc->dev,
	    "gsp_disp: M4y EXC chid=%u stat=0x%x reason=%u[%s] "
	    "method=0x%x data=0x%x code=0x%x\n",
	    chid, stat, type, disp_exception_reason(type), method, data, code);
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
	disp_dump_exception_chid(sc, 0);
	disp_dump_exception_chid(sc, 1);
}

static void
disp_dump_gv100_sor_route_state(struct nvkm_softc *sc, const char *tag)
{
	nvkm_infof(sc->dev,
	    "gsp_disp: %s SOR live[0..3]=0x%x 0x%x 0x%x 0x%x "
	    "arm[0..3]=0x%x 0x%x 0x%x 0x%x\n",
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
	uint32_t head_live = 0x682000;
	uint32_t head_arm = head_live + 0x8000;
	uint32_t win_live = 0x681000;
	uint32_t win_arm = win_live + 0x8000;

	nvkm_infof(sc->dev,
	    "gsp_disp: %s head0 live ctrl=0x%x out=0x%x clk=0x%x "
	    "display=0x%x clkmax=0x%x usage=0x%x\n",
	    tag,
	    nvkm_rd32(sc, head_live + 0x008),
	    nvkm_rd32(sc, head_live + 0x004),
	    nvkm_rd32(sc, head_live + 0x00c),
	    nvkm_rd32(sc, head_live + 0x020),
	    nvkm_rd32(sc, head_live + 0x028),
	    nvkm_rd32(sc, head_live + 0x030));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s head0 live viewin=0x%x viewout=0x%x "
	    "raster=0x%x sync=0x%x blanke=0x%x blanks=0x%x blank2=0x%x\n",
	    tag,
	    nvkm_rd32(sc, head_live + 0x04c),
	    nvkm_rd32(sc, head_live + 0x058),
	    nvkm_rd32(sc, head_live + 0x064),
	    nvkm_rd32(sc, head_live + 0x068),
	    nvkm_rd32(sc, head_live + 0x06c),
	    nvkm_rd32(sc, head_live + 0x070),
	    nvkm_rd32(sc, head_live + 0x074));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s head0 arm ctrl=0x%x out=0x%x clk=0x%x "
	    "display=0x%x clkmax=0x%x usage=0x%x\n",
	    tag,
	    nvkm_rd32(sc, head_arm + 0x008),
	    nvkm_rd32(sc, head_arm + 0x004),
	    nvkm_rd32(sc, head_arm + 0x00c),
	    nvkm_rd32(sc, head_arm + 0x020),
	    nvkm_rd32(sc, head_arm + 0x028),
	    nvkm_rd32(sc, head_arm + 0x030));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s head0 arm viewin=0x%x viewout=0x%x "
	    "raster=0x%x sync=0x%x blanke=0x%x blanks=0x%x blank2=0x%x\n",
	    tag,
	    nvkm_rd32(sc, head_arm + 0x04c),
	    nvkm_rd32(sc, head_arm + 0x058),
	    nvkm_rd32(sc, head_arm + 0x064),
	    nvkm_rd32(sc, head_arm + 0x068),
	    nvkm_rd32(sc, head_arm + 0x06c),
	    nvkm_rd32(sc, head_arm + 0x070),
	    nvkm_rd32(sc, head_arm + 0x074));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s win-core live ctl[0..3]=0x%x 0x%x 0x%x 0x%x "
	    "usage[0..3]=0x%x 0x%x 0x%x 0x%x\n",
	    tag,
	    nvkm_rd32(sc, win_live + 0 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_live + 1 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_live + 2 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_live + 3 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_live + 0 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_live + 1 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_live + 2 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_live + 3 * 0x80 + 0x010));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s win-core arm ctl[0..3]=0x%x 0x%x 0x%x 0x%x "
	    "usage[0..3]=0x%x 0x%x 0x%x 0x%x\n",
	    tag,
	    nvkm_rd32(sc, win_arm + 0 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_arm + 1 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_arm + 2 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_arm + 3 * 0x80 + 0x000),
	    nvkm_rd32(sc, win_arm + 0 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_arm + 1 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_arm + 2 * 0x80 + 0x010),
	    nvkm_rd32(sc, win_arm + 3 * 0x80 + 0x010));
	nvkm_infof(sc->dev,
	    "gsp_disp: %s win0-core live fmt=0x%x rot=0x%x data0c=0x%x "
	    "arm fmt=0x%x rot=0x%x data0c=0x%x\n",
	    tag,
	    nvkm_rd32(sc, win_live + 0x004),
	    nvkm_rd32(sc, win_live + 0x008),
	    nvkm_rd32(sc, win_live + 0x00c),
	    nvkm_rd32(sc, win_arm + 0x004),
	    nvkm_rd32(sc, win_arm + 0x008),
	    nvkm_rd32(sc, win_arm + 0x00c));
}

static uint32_t
disp_retarget_gv100_route(struct nvkm_softc *sc, uint32_t old_orid,
    uint32_t new_orid)
{
	uint32_t old_sor = old_orid + 1u;
	uint32_t new_sor = new_orid + 1u;
	uint32_t changed = 0;

	if (old_orid == 0xffu || new_orid == 0xffu || old_orid == new_orid)
		return (0);

	for (uint32_t i = 0; i < 8; i++) {
		uint32_t addr = 0x612308 + i * 0x80;
		uint32_t before = nvkm_rd32(sc, addr);
		uint32_t after;

		if ((before & 0x0000000fu) != old_sor)
			continue;

		after = (before & ~0x0000000fu) | new_sor;
		nvkm_wr32(sc, addr, after);
		nvkm_infof(sc->dev,
		    "gsp_disp: M4y route retarget[%u] @0x%x 0x%x -> 0x%x "
		    "(sor%u -> sor%u)\n",
		    i, addr, before, after, old_orid, new_orid);
		changed++;
	}

	if (changed == 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: M4y route retarget: no route entry for inherited sor%u\n",
		    old_orid);
	}
	return (changed);
}

/* ===== M4e: minimal modeset (light the screen) =====
 * Assemble the full first-light path on head 0 + the connected HDMI: assign a
 * SOR, program head 0 timing for 1080p60, route head 0 -> SOR (TMDS), own
 * window 0 by head 0, point the core at a notifier, then core UPDATE (which
 * brings up the pixel clock and signals the notifier). Then push the window 0
 * image over a whole-VRAM ISO ctxdma + window UPDATE with visible color bars.
 */
#define DISP_HDMI_DISPLAY_ID	0x400u	/* connected HDMI (from M1 probe) */

static int
nvkm_gsp_disp_modeset_test(struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct disp_dfp_assign_sor_params *sor;
	volatile uint32_t *cpb, *wpb;
	uint32_t cmds[160], n, st0, status, i;
	uint32_t orid = 0xff, inherited_orid = 0xff, inherited_proto = 0;
	uint32_t assigned_sor_arm, assigned_proto, sormask, sor_proto;
	uint32_t sor_ctrl[DFP_ASSIGN_SOR_MAX_SORS];
	uint32_t mode_raster, mode_sync, mode_blanke, mode_blanks, mode_blank2;
	uint32_t wput = 0, wget = 0;
	uint64_t fb_paddr = 0, fb_size;
	void *p;
	int err, us, cpush, wpush;
	const uint32_t HEAD = 0, WIN = 0;
	const uint32_t h_active = 2200, h_synce = 43, h_blanke = 191, h_blanks = 2111;
	const uint32_t v_active = 1125, v_synce = 4, v_blanke = 40, v_blanks = 1120;
	const uint32_t fb_w = 1920, fb_h = 1080, fb_pitch = 1920u * 4u;

	if (disp->core.handle == 0 || disp->window.handle == 0 ||
	    disp->instmem_gva[0] == 0)
		return (ENXIO);
	cpb = disp->core_push_kva;
	wpb = disp->window_push_kva;

	/* (1) Assign a SOR to the HDMI display (GSP picks the OR). */
	sor = nvkm_gsp_rm_ctrl_get(&disp->objcom, NV0073_CTRL_CMD_DFP_ASSIGN_SOR,
	    sizeof(*sor));
	if (sor == NULL)
		return (ENOMEM);
	memset(sor, 0, sizeof(*sor));
	sor->subDeviceInstance = 0;
	sor->displayId = DISP_HDMI_DISPLAY_ID;
	sor->sorExcludeMask = 0;
	p = sor;
	err = nvkm_gsp_rm_ctrl_rd(&disp->objcom, &p, sizeof(*sor));
	if (err != 0 || p == NULL) {
		nvkm_infof(sc->dev, "gsp_disp: DFP_ASSIGN_SOR err=%d\n", err);
		return (EIO);
	}
	for (i = 0; i < DFP_ASSIGN_SOR_MAX_SORS; i++) {
		uint32_t dm = ((struct disp_dfp_assign_sor_params *)p)->
		    sorAssignListWithTag[i].displayMask;
		if (dm & DISP_HDMI_DISPLAY_ID) {
			orid = i;
			break;
		}
	}
	sormask = 0;
	for (i = 0; i < DFP_ASSIGN_SOR_MAX_SORS; i++)
		sormask |= ((struct disp_dfp_assign_sor_params *)p)->
		    sorAssignList[i] & 0xffu;
	nvkm_gsp_rm_ctrl_done(&disp->objcom, p);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4y ASSIGN_SOR display=0x%x -> or=%u (listmask=0x%x)\n",
	    DISP_HDMI_DISPLAY_ID, orid, sormask);
	if (orid == 0xff)
		orid = 0;	/* fall back; the log shows if assignment failed */
	sor_proto = NVC57D_SOR_PROTOCOL_SINGLE_TMDS_A;
	for (i = 0; i < DFP_ASSIGN_SOR_MAX_SORS; i++)
		sor_ctrl[i] = nvkm_rd32(sc, 0x6101c4 + i * 0x20);
	for (i = 0; i < DFP_ASSIGN_SOR_MAX_SORS; i++) {
		if (sor_ctrl[i] != 0xbadf5040 && (sor_ctrl[i] & 0x00000f00u) != 0) {
			inherited_orid = i;
			inherited_proto = sor_ctrl[i] & 0x00000f00u;
			break;
		}
	}
	assigned_sor_arm = nvkm_rd32(sc, 0x680300 + 0x8000 + orid * 0x20);
	assigned_proto = assigned_sor_arm & 0x00000f00u;
	if (assigned_proto == NVC57D_SOR_PROTOCOL_SINGLE_TMDS_A ||
	    assigned_proto == NVC57D_SOR_PROTOCOL_SINGLE_TMDS_B)
		sor_proto = assigned_proto;
	nvkm_infof(sc->dev,
	    "gsp_disp: M4y SOR ctrl[0..3]=0x%x 0x%x 0x%x 0x%x "
	    "inherited_or=%u inherited_proto=0x%x assigned_arm=0x%x "
	    "-> use assigned_or=%u proto=0x%x\n",
	    sor_ctrl[0], sor_ctrl[1], sor_ctrl[2], sor_ctrl[3],
	    inherited_orid, inherited_proto, assigned_sor_arm, orid, sor_proto);
	disp_dump_gv100_sor_route_state(sc, "M4y pre-route");
	(void)disp_retarget_gv100_route(sc, inherited_orid, orid);
	disp_dump_gv100_sor_route_state(sc, "M4y post-route");

	/* (2) RM-allocate the framebuffer + notifier, then write nouveau-style
	 * RAMHT entries that the display HW resolves from EVO ctxdma handles. */
	fb_size = (uint64_t)fb_pitch * fb_h;
	err = nvkm_gsp_disp_nouveau_ramht_setup(sc, fb_size);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "gsp_disp: M4e nouveau RAMHT setup err=%d\n", err);
		return (err);
	}
	fb_paddr = disp->fb_paddr;
	if (fb_paddr == 0)
		return (ENXIO);
	err = disp_fb_fill_bars(sc, fb_paddr, fb_pitch, fb_w, fb_h);
	if (err != 0)
		return (err);
	disp_dump_gv100_head_window_state(sc, "M4z pre-core");
	mode_raster = h_active | (v_active << 16);
	mode_sync = h_synce | (v_synce << 16);
	mode_blanke = h_blanke | (v_blanke << 16);
	mode_blanks = h_blanks | (v_blanks << 16);
	mode_blank2 = 0x00000001u;
	/* M5d: do NOT inherit arm blank values. M5c latched raster/sync/blanke
	 * from the mode batch but RASTER_BLANK_END/START failed (FE 0x206c): the
	 * boot arm holds an interlaced blank2 (0x25c0694) and v-blank values that
	 * are inconsistent with our progressive CONTROL(interlace=0). Use
	 * nouveau's progressive computation (blanke=0x2800bf blanks=0x460083f
	 * blank2=0x1) instead. */
	nvkm_infof(sc->dev,
	    "gsp_disp: M5d computed timing raster=0x%x sync=0x%x blanke=0x%x "
	    "blanks=0x%x blank2=0x%x\n",
	    mode_raster, mode_sync, mode_blanke, mode_blanks, mode_blank2);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4aa core scope: window%u bounds/control only\n", WIN);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4ac core scope: inherit head0 timing/view from arm\n");
	nvkm_infof(sc->dev,
	    "gsp_disp: M4ad core scope: head%u notifier/update only\n", HEAD);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4ae core scope: bare UPDATE, notifier disabled\n");

	/* (3) M6a: activate the HDMI encoder (SET_HDMI_ENABLE), then push one full
	 * single-shot head modeset + UPDATE. A-direction finding: GSP owns the disp
	 * supervisor (r535 disp func has no .super/.intr); the host only pushes EVO
	 * methods plus the output-path RM controls. We had ASSIGN_SOR but never
	 * enabled HDMI, so the head timing latched (M5e had raster/sync/blanke/
	 * blanks all correct in live) yet no TMDS output was driven. SET_HDMI_ENABLE
	 * is the missing activation. */
	{
		struct disp_set_hdmi_enable_params *he;
		he = nvkm_gsp_rm_ctrl_get(&disp->objcom,
		    NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_ENABLE, sizeof(*he));
		if (he != NULL) {
			memset(he, 0, sizeof(*he));
			he->subDeviceInstance = 0;
			he->displayId = DISP_HDMI_DISPLAY_ID;
			he->enable = 1;
			err = nvkm_gsp_rm_ctrl_wr(&disp->objcom, he);
			nvkm_infof(sc->dev,
			    "gsp_disp: M6a SET_HDMI_ENABLE displayId=0x%x err=%d\n",
			    DISP_HDMI_DISPLAY_ID, err);
		}
	}

	/* M7a: allocate + fill an identity OLUT in VRAM. The core vram ctxdma
	 * (NV50_DISP_HANDLE_VRAM, already bound) covers it from offset 0, so
	 * OFFSET_OLUT = olut_paddr>>8. */
	if (disp->olut_paddr == 0) {
		disp->olut_paddr = nvkm_gsp_vram_alloc(sc, DISP_OLUT_VRAM_SIZE, 0x1000);
		if (disp->olut_paddr != 0)
			(void)disp_olut_fill_identity(sc, disp->olut_paddr);
		nvkm_infof(sc->dev, "gsp_disp: M7a identity olut@0x%llx\n",
		    (unsigned long long)disp->olut_paddr);
	}

	nvkm_wr32(sc, 0x611020 + 0 * 12u, 0x90000000u);
#define M(off, val) do { cmds[n++] = evo_method_hdr((off), 1); cmds[n++] = (val); } while (0)
	n = 0;
	/* nouveau headc57d_mode set (multi-method raster, M5e-validated) + output
	 * binding (display_id + SOR route + window owner) so the GSP supervisor has
	 * a complete output target. No CONTROL_OUTPUT_RESOURCE: M5g showed pushing
	 * it (0xfc000040) regressed the latch; boot arm already holds out=0x40. */
	cmds[n++] = evo_method_hdr(NVC57D_HEAD_SET_RASTER_SIZE(HEAD), 4);
	cmds[n++] = mode_raster;
	cmds[n++] = mode_sync;
	cmds[n++] = mode_blanke;
	cmds[n++] = mode_blanks;
	M(NVC57D_HEAD_SET_RASTER_VERT_BLANK2(HEAD), mode_blank2);
	M(NVC57D_HEAD_SET_CONTROL(HEAD), 0x00000000u);
	M(NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(HEAD),
	    nvkm_rd32(sc, 0x682000 + 0x8000 + 0x00c) & 0x7fffffffu);
	M(NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(HEAD),
	    nvkm_rd32(sc, 0x682000 + 0x8000 + 0x00c) & 0x7fffffffu);
	M(NVC57D_HEAD_SET_HEAD_USAGE_BOUNDS(HEAD),
	    0x4u | (1u << 4) | (1u << 8) | (1u << 12));	/* +OLUT_ALLOWED */
	/* M7a: identity OLUT. nouveau headc57d always sets an identity OLUT for an
	 * active head (olut_identity=true, USAGE OLUT_ALLOWED=TRUE). Without it the
	 * head state is incomplete and the GSP supervisor REJECTED the whole modeset
	 * (UPDATE INVALID_ARG, notifier never flipped, screen black). OLUT ctxdma
	 * handle = the core vram ctxdma (NV50_DISP_HANDLE_VRAM, already bound),
	 * offset = olut_paddr>>8 (vram ctxdma covers VRAM from 0). */
	M(NVC57D_HEAD_SET_OLUT_CONTROL(HEAD), NVC57D_OLUT_CONTROL_IDENTITY_DIRECT10);
	M(NVC57D_HEAD_SET_OLUT_FP_NORM_SCALE(HEAD), 0xffffffffu);
	M(NVC57D_HEAD_SET_CONTEXT_DMA_OLUT(HEAD), NV50_DISP_HANDLE_VRAM);
	M(NVC57D_HEAD_SET_OFFSET_OLUT(HEAD), (uint32_t)(disp->olut_paddr >> 8));
	M(NVC57D_HEAD_SET_DISPLAY_ID(HEAD), DISP_HDMI_DISPLAY_ID);
	M(NVC57D_SOR_SET_CONTROL(orid), sor_proto | (1u << HEAD));
	M(NVC57D_WINDOW_SET_CONTROL(WIN), HEAD);
	/* M6d: single atomic ntfy UPDATE. Hypothesis: the per-increment latch is
	 * because we never armed the core completion notifier. Add SET_CONTEXT_DMA_
	 * NOTIFIER(SYNCBUF) + SET_NOTIFIER_CONTROL(MODE_WRITE|NOTIFY_ENABLE=0x1000)
	 * around UPDATE, then DISABLE after. If one UPDATE latches everything and the
	 * notifier flips FINISHED, the 24-UPDATE workaround retires. */
	M(NVC57D_SET_CONTEXT_DMA_NOTIFIER, NV50_DISP_HANDLE_SYNCBUF);
	M(NVC57D_SET_NOTIFIER_CONTROL, (1u << 12));	/* MODE_WRITE|OFFSET0|NOTIFY_ENABLE */
	M(NVC57D_SET_INTERLOCK_FLAGS, 0x00000000u);
	M(NVC57D_SET_WINDOW_INTERLOCK_FLAGS, 0x00000000u);
	M(NVC57D_UPDATE, 0x1u);
	M(NVC57D_SET_NOTIFIER_CONTROL, 0x00000000u);	/* NOTIFY_DISABLE */
	nvkm_wr32(sc, 0x611020 + 0 * 12u, 0x90000000u);
	cpush = disp_chan_push(sc, cpb, disp->core_put_reg,
	    disp->core_push_size / 4, cmds, n);
	{
		uint32_t nt = 0;
		int us2;
		for (us2 = 0; us2 < 500000; us2 += 100) {
			nt = nvkm_gsp_bar1_rd32(sc, disp->notifier_gva + 0x0);
			if (((nt >> 30) & 0x3u) == DISP_NOTIFIER_STATUS_FINISHED)
				break;
			DELAY(100);
		}
		nvkm_infof(sc->dev,
		    "gsp_disp: M6d single ntfy UPDATE: push=%d coreGET=%u ntfy=0x%x "
		    "exc=0x%x raster=0x%x clk=0x%x -> %s\n",
		    cpush, nvkm_rd32(sc, disp->core_put_reg + 4), nt,
		    nvkm_rd32(sc, 0x611020),
		    nvkm_rd32(sc, 0x682000 + 0x064),
		    nvkm_rd32(sc, 0x682000 + 0x00c),
		    ((nt >> 30) & 0x3u) == DISP_NOTIFIER_STATUS_FINISHED ?
		    "NOTIFIER FINISHED (atomic latch!)" : "no notifier (per-increment)");
	}
	disp_dump_gv100_sor_route_state(sc, "M6d");
	/* M7a: NO 24-UPDATE workaround. The single ntfy UPDATE above is the real
	 * test: success == notifier flips FINISHED (supervisor committed). The
	 * 24-UPDATE loop only copied arm->live registers without a real commit
	 * (black screen). With the identity OLUT added, the head state should now
	 * be complete enough for the supervisor to accept the UPDATE. */
	disp_dump_gv100_head_window_state(sc, "M7a-final");
	disp_dump_gv100_sor_route_state(sc, "M7a-final");

	st0 = 0;
	for (us = 0; us < 3000000; us += 50) {
		st0 = nvkm_gsp_bar1_rd32(sc, disp->notifier_gva + 0x0);
		if (((st0 >> 30) & 0x3u) == DISP_NOTIFIER_STATUS_FINISHED)
			break;
		DELAY(50);
	}
	status = (st0 >> 30) & 0x3u;
	nvkm_infof(sc->dev,
	    "gsp_disp: M4y core first update: push=%d coreGET=%u ntfy=0x%x "
	    "status=%u -> %s\n",
	    cpush, nvkm_rd32(sc, disp->core_put_reg + 4), st0, status,
	    status == DISP_NOTIFIER_STATUS_FINISHED ? "PIPE UP" : "no notifier");
	disp_dump_gv100_head_window_state(sc, "M4z post-core");
	disp_dump_gv100_exception_state(sc);

	/* (4) Window batch: image over the ISO ctxdma + window UPDATE. */
	n = 0;
	M(NVC57E_SET_PRESENT_CONTROL, NVC57E_SET_PRESENT_CONTROL_INTERVAL_1);
	M(NVC57E_SET_SIZE, fb_w | (fb_h << 16));
	M(NVC57E_SET_STORAGE, NVC57E_SET_STORAGE_MEMORY_LAYOUT_PITCH);
	M(NVC57E_SET_PARAMS, NVC57E_SET_PARAMS_FORMAT_A8R8G8B8);
	M(NVC57E_SET_PLANAR_STORAGE(0), fb_pitch >> 6);
	M(NVC57E_SET_CONTEXT_DMA_ISO(0), NV50_DISP_HANDLE_WNDW_ISO);
	M(NVC57E_SET_OFFSET(0), (uint32_t)(fb_paddr >> 8));
	M(NVC57E_SET_POINT_IN(0), 0x00000000u);
	M(NVC57E_SET_SIZE_IN, fb_w | (fb_h << 16));
	M(NVC57E_SET_SIZE_OUT, fb_w | (fb_h << 16));
	M(NVC57E_SET_COMPOSITION_CONTROL, NVC57E_COMPOSITION_DEPTH_PRIMARY);
	M(NVC57E_SET_COMPOSITION_CONSTANT_ALPHA, NVC57E_COMPOSITION_ALPHA_OPAQUE);
	M(NVC57E_SET_COMPOSITION_FACTOR_SELECT, NVC57E_COMPOSITION_FACTOR_PIXEL_NONE);
	M(NVC57E_SET_KEY_ALPHA, NVC57E_KEY_RANGE_FULL);
	M(NVC57E_SET_KEY_RED_CR, NVC57E_KEY_RANGE_FULL);
	M(NVC57E_SET_KEY_GREEN_Y, NVC57E_KEY_RANGE_FULL);
	M(NVC57E_SET_KEY_BLUE_CB, NVC57E_KEY_RANGE_FULL);
	M(NVC57E_SET_INTERLOCK_FLAGS, 0x00000001u);
	M(NVC57E_SET_WINDOW_INTERLOCK_FLAGS, 1u << WIN);
	M(NVC57E_UPDATE, 0x1u);
	wpush = disp_chan_emit(sc, wpb, disp->window_put_reg,
	    disp->window_push_size / 4, cmds, n, &wput);
#undef M
	if (wpush == 0)
		wget = disp_chan_wait_get(sc, disp->window_put_reg, wput, 2000000);
	else
		wget = nvkm_rd32(sc, disp->window_put_reg + 4);
	nvkm_infof(sc->dev,
	    "gsp_disp: M4y window image: emit=%d winPUT=%u winGET=%u fb@0x%llx "
	    "iso=whole-vram\n",
	    wpush, wput, wget, (unsigned long long)fb_paddr);
	disp_dump_gv100_head_window_state(sc, "M4z post-window");

	/* (DIAG) Read display HW state to tell "pipe latched, ctxdma-only fail"
	 * from "UPDATE never completed", without a monitor:
	 *  - SOR control 0x6101c4 + or*0x20: proto[11:8]/owner[7:0] (did SOR latch?)
	 *  - channel state 0x610664 + chid*4: bits[19:16]==4 => idle/healthy. */
	nvkm_infof(sc->dev,
	    "gsp_disp: M4y DIAG SOR ctrl[0..3]=0x%x 0x%x 0x%x 0x%x\n",
	    nvkm_rd32(sc, 0x6101c4 + 0 * 0x20),
	    nvkm_rd32(sc, 0x6101c4 + 1 * 0x20),
	    nvkm_rd32(sc, 0x6101c4 + 2 * 0x20),
	    nvkm_rd32(sc, 0x6101c4 + 3 * 0x20));
	nvkm_infof(sc->dev,
	    "gsp_disp: M4y DIAG chan state[0..5]=0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
	    nvkm_rd32(sc, 0x610664 + 0 * 4), nvkm_rd32(sc, 0x610664 + 1 * 4),
	    nvkm_rd32(sc, 0x610664 + 2 * 4), nvkm_rd32(sc, 0x610664 + 3 * 4),
	    nvkm_rd32(sc, 0x610664 + 4 * 4), nvkm_rd32(sc, 0x610664 + 5 * 4));
	disp_dump_gv100_exception_state(sc);
	disp_dump_gv100_sor_route_state(sc, "M4y DIAG");
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

	/* (1) Display client + device (needed to RM-allocate the inst mem). */
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

	/* (7) Display instmem object model (M4b): RAMHT + ctxdma descriptors in
	 * the display RAMIN, needed to resolve EVO context-DMA handles. */
	(void)nvkm_gsp_disp_instmem_init(sc);

	/* (8) Core display channel (M2a). */
	(void)nvkm_gsp_disp_core_init(sc);

	/* (9) Window display channel (M4c): NVC57E channel for plane images. */
	(void)nvkm_gsp_disp_window_init(sc);

	/* (10) M4e smoke: nouveau-style RAMHT ctxdma bind + minimal modeset. */
	(void)nvkm_gsp_disp_modeset_test(sc);
	return (0);

fail_device:
	nvkm_gsp_device_dtor(&disp->device);
fail_client:
	nvkm_gsp_client_dtor(&disp->client);
fail_free:
	kfree(disp);
	return (err);
}
