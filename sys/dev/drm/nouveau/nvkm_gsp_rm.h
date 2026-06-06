/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM resource manager RPC helpers.
 *
 * Wraps NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC (103) and GSP_RM_CONTROL (76)
 * around the existing nvkm_gsp_rpc_* primitives. Envelope structs are
 * the v03_00 layout (stable from r535 through r570 — verified against
 * Linux nouveau r535/r570 sources and open-rm 570.144 headers).
 *
 * Per-class params structs MUST be sourced from r570/nvrm/ headers,
 * not r535 — several classes added fields between versions
 * (e.g. NV0000_ALLOC_PARAMETERS gained pOsPidInfo in r570).
 */

#ifndef _NVKM_GSP_RM_H_
#define _NVKM_GSP_RM_H_

#include "nvkm_priv.h"
#include <sys/taskqueue.h>	/* struct task (nvkm_gsp_disp hotplug worker) */

/* GSP-RM RPC function numbers — Linux nouveau r570/nvrm/rpcfn.h. */
#define NV_VGPU_MSG_FUNCTION_FREE		10
#define NV_VGPU_MSG_FUNCTION_DUP_OBJECT		21
#define NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL	76
#define NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC	103

/* Stable envelope (v03_00) — same in r535 and r570. */
struct rpc_gsp_rm_alloc_v03_00 {
	uint32_t hClient;
	uint32_t hParent;
	uint32_t hObject;
	uint32_t hClass;
	uint32_t status;	/* OUT */
	uint32_t paramsSize;
	uint32_t flags;
	uint8_t  reserved[4];
	uint8_t  params[];
};

struct rpc_gsp_rm_control_v03_00 {
	uint32_t hClient;
	uint32_t hObject;
	uint32_t cmd;
	uint32_t status;	/* OUT */
	uint32_t paramsSize;
	uint32_t flags;
	uint8_t  params[];
};

struct NVOS00_PARAMETERS_v03_00 {
	uint32_t hRoot;
	uint32_t hObjectParent;
	uint32_t hObjectOld;
	int32_t  status;
};

struct rpc_free_v03_00 {
	struct NVOS00_PARAMETERS_v03_00 params;
};

/* Resource handle tracking. Mirrors nouveau's nvkm_gsp_client +
 * nvkm_gsp_object. A client is itself an object whose parent is itself
 * (the RM_ALLOC for NV01_ROOT uses the same handle for client/parent/obj). */
struct nvkm_gsp_object {
	struct nvkm_gsp_client	*client;
	struct nvkm_gsp_object	*parent;	/* NULL for client root */
	uint32_t		 handle;
};

struct nvkm_gsp_client {
	struct nvkm_gsp_object	 object;	/* embedded; .client = self */
	struct nvkm_softc	*sc;
};

static __inline uint32_t
nvkm_gsp_client_child_handle(struct nvkm_gsp_client *client, uint32_t base)
{
	/* GSP treats RM object handles as global enough that fixed child
	 * handles collide across our primary and golden clients.  Preserve
	 * the readable nouveau-style base while making per-client children
	 * distinct. */
	return (base | (client->object.handle & 0x00000fffu));
}

/* === RM_ALLOC ===
 * Allocate a new RM resource under `parent`. `new_obj` is the caller's
 * uninitialised nvkm_gsp_object; we fill it in. Returned pointer is the
 * class-specific params region — caller fills it then calls _wr. */
void	*nvkm_gsp_rm_alloc_get(struct nvkm_gsp_object *parent,
	    uint32_t handle, uint32_t oclass, uint32_t params_size,
	    struct nvkm_gsp_object *new_obj);

/* Submit the prepared alloc params. Returns 0 on GSP-side success. */
int	 nvkm_gsp_rm_alloc_wr(struct nvkm_gsp_object *obj, void *params);
/* Alloc and return the reply params (for [OUT] fields); free via _done. */
int	 nvkm_gsp_rm_alloc_rd(struct nvkm_gsp_object *obj, void **params,
	    uint32_t repc);

/* Discard the prepared buffer without sending (e.g. error path). */
void	 nvkm_gsp_rm_alloc_done(struct nvkm_gsp_object *obj, void *params);

/* Free an allocated RM resource. */
int	 nvkm_gsp_rm_free(struct nvkm_gsp_object *obj);

/* === RM_CONTROL ===
 * Prepare a control request against an existing object. */
void	*nvkm_gsp_rm_ctrl_get(struct nvkm_gsp_object *obj, uint32_t cmd,
	    uint32_t params_size);

/* Send + wait + return params with OUT fields filled (caller frees via _done). */
int	 nvkm_gsp_rm_ctrl_rd(struct nvkm_gsp_object *obj, void **params,
	    uint32_t repc);

/* Send + ignore reply (still RECV but discard). */
int	 nvkm_gsp_rm_ctrl_wr(struct nvkm_gsp_object *obj, void *params);

void	 nvkm_gsp_rm_ctrl_done(struct nvkm_gsp_object *obj, void *params);

/* === Convenience === */
/* Query NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE on GSP's
 * internal subdevice; stores into sc->mthdbuf_size. */
int	 nvkm_gsp_query_mthdbuf_size(struct nvkm_softc *sc);
int	 nvkm_gsp_intr_get_kernel_table(struct nvkm_softc *sc);
int	 nvkm_gsp_register_nonstall_event(struct nvkm_gsp_vmm *vmm);
void	 nvkm_gsp_unregister_nonstall_event(struct nvkm_gsp_vmm *vmm);

/* Host-side chid pool. Mirrors nouveau chid.c:nvkm_chid_new with
 * nr=2048, first=1, count=2047 -- chid 0 is reserved. */
void	  nvkm_chid_init(struct nvkm_softc *sc);
int	  nvkm_chid_alloc(struct nvkm_softc *sc);
void	  nvkm_chid_free(struct nvkm_softc *sc, int chid);

int	 nvkm_gsp_client_ctor(struct nvkm_softc *sc, uint32_t handle,
	    struct nvkm_gsp_client *client);
int	 nvkm_gsp_client_dtor(struct nvkm_gsp_client *client);

/* Allocated device + subdevice tree under a client. */
struct nvkm_gsp_device {
	struct nvkm_gsp_object	object;		/* NV01_DEVICE_0 */
	struct nvkm_gsp_object	subdevice;	/* NV20_SUBDEVICE_0 */
};

int	 nvkm_gsp_device_ctor(struct nvkm_gsp_client *client,
	    struct nvkm_gsp_device *device);
int	 nvkm_gsp_device_dtor(struct nvkm_gsp_device *device);

#define NVKM_GSP_DISP_WINDOW_NR	8

/* Display subsystem (Phase 2): NV04_DISPLAY_COMMON + connector/EDID.
 * Allocated by nvkm_gsp_disp_init at attach; sc->gsp_disp points here. */
struct nvkm_gsp_disp {
	struct nvkm_softc	*sc;
	struct nvkm_gsp_client	client;
	struct nvkm_gsp_device	device;
	struct nvkm_gsp_object	objcom;		/* NV04_DISPLAY_COMMON */
	uint64_t		inst_paddr;	/* display instance RAM (RAMIN) */
	uint32_t		num_heads;
	uint32_t		head_mask;
	uint32_t		window_mask;
	uint32_t		supported_mask;	/* GET_SUPPORTED displayId mask */
		/* Display events (r535 oneinit): GSP POST_EVENT -> ithread matches
		 * hpd_event_handle -> enqueues hpd_task on a background lwkt, which
		 * re-probes + logs EDID. DP IRQ is registered as a second event object
		 * to mirror nouveau, but currently only logged by the generic handler. */
		struct nvkm_gsp_object	hpd_event;	/* NV01_EVENT_KERNEL_CALLBACK_EX */
		struct nvkm_gsp_object	dp_irq_event;	/* NV01_EVENT_KERNEL_CALLBACK_EX */
		uint32_t		hpd_event_handle;
		uint32_t		dp_irq_event_handle;
		struct task		hpd_task;
	/* Core display channel (M2a): TU102_DISP root + NVC57D core channel. */
	struct nvkm_gsp_object	dispclass;	/* TU102_DISP (0xc570) root */
	struct nvkm_gsp_object	core;		/* NVC57D core channel DMAC */
	void			*core_push_kva;	/* core pushbuffer (coherent sysmem) */
	uint64_t		core_push_paddr;
	uint32_t		core_push_size;
	uint32_t		core_put_reg;	/* BAR0 MMIO PUT (0x680000); GET at +4 */
	uint32_t		core_put_cur;	/* nouveau dmac->put software cursor */
	int			core_assign_windows;
	/* instmem (M4b): RAMHT + ctxdma descriptors live in the display RAMIN
	 * (inst_paddr). The display HW reads them physically; BAR1 is only our
	 * CPU write window. RAMHT occupies RAMIN [0, 0x1000); ctxdma descriptors
	 * are bump-allocated (32-byte slots) from descr_next. */
	uint64_t		instmem_gva[4];	/* BAR1 GVAs of the first 4 RAMIN pages */
	uint32_t		descr_next;	/* next free ctxdma RAMIN byte offset */
	/* Window display channels (M4c): NVC57E instance N.  r535_dmac_init()
	 * creates one DMA channel per window; chid.user = 1 + N and PUT/GET are
	 * at BAR0 0x690000 + N*0x1000. */
	struct nvkm_gsp_disp_window {
		struct nvkm_gsp_object	object;		/* NVC57E window channel */
		void			*push_kva;	/* coherent sysmem PB */
		uint64_t		push_paddr;
		uint32_t		push_size;
		uint32_t		put_reg;
		uint32_t		put_cur;
		uint32_t		id;
		uint32_t		heads;
		uint32_t		interlock_data;
		uint32_t		interlock_wimm;
		uint32_t		ntfy;
		uint32_t		armed_ntfy;
		uint32_t		sema;
		uint32_t		data;
		int			ramht_ready;
		int			ready;
	} window[NVKM_GSP_DISP_WINDOW_NR];
	struct nvkm_gsp_disp_wimm {
		struct nvkm_gsp_object	object;		/* NVC57B window-immediate channel */
		void			*push_kva;
		uint64_t		push_paddr;
		uint32_t		push_size;
		uint32_t		put_reg;
		uint32_t		put_cur;
		uint32_t		id;
		int			ready;
	} wimm[NVKM_GSP_DISP_WINDOW_NR];
	struct nvkm_gsp_disp_curs {
		struct nvkm_gsp_object	object;		/* NVC57A cursor PIO channel */
		uint32_t		put_reg;
		uint32_t		id;
		int			ready;
	} curs[NVKM_GSP_DISP_WINDOW_NR];
	/* Core completion notifier (M4d-1): VRAM page + ctxdma the core channel
	 * signals on UPDATE; host polls STATUS via BAR1. */
	uint64_t		notifier_paddr;	/* notifier VRAM page (physical) */
	uint64_t		notifier_gva;	/* BAR1 GVA of the notifier page */
	int			ramht_ready;	/* RAMHT ctxdma handles are valid */
	uint64_t		fb_paddr;	/* M4d test framebuffer (VRAM physical) */
	uint64_t		olut_paddr;	/* M4q identity output LUT (VRAM physical) */
	uint64_t		ilut_paddr;	/* M4t identity window input LUT (VRAM physical) */
	/* M4d option Y: RM ContextDma delivery. Each surface is an
	 * NV01_MEMORY_LOCAL_USER object plus an NV01_CONTEXT_DMA explicitly
	 * bound to its display channel. */
	struct nvkm_gsp_object	inst_mem;	/* RM-allocated display inst mem (RAMIN) */
	struct nvkm_gsp_object	iso_mem;	/* fb memory object */
	struct nvkm_gsp_object	olut_mem;	/* output LUT memory object */
	struct nvkm_gsp_object	iso_dma;	/* fb ctxdma object */
	struct nvkm_gsp_object	ntfy_mem;	/* notifier memory object */
	struct nvkm_gsp_object	ntfy_dma;	/* notifier ctxdma object */
	struct nvkm_gsp_object	caps;		/* GV100_DISP_CAPS host BAR0 map */
};

/* Bring up the GSP display subsystem + read EDID of connected outputs.
 * Runs on the attach thread (blockable; synchronous GSP RPCs). */
int	 nvkm_gsp_disp_init(struct nvkm_softc *sc);
/* Probe connected outputs and print their EDID. Blockable context only. */
void	 nvkm_gsp_disp_probe_connected(struct nvkm_softc *sc);
/* Is the given GSP displayId currently connected? 1/0, <0 on error. Blockable. */
int	 nvkm_gsp_disp_connected(struct nvkm_softc *sc, uint32_t display_id);
/* Read EDID for a displayId into out (<=*outlen); sets *outlen. Blockable. */
int	 nvkm_gsp_disp_read_edid(struct nvkm_softc *sc, uint32_t display_id,
	     uint8_t *out, uint32_t *outlen);

/* ===== nouveau_disp.txt display state machine =====
 * The drm atomic hooks in nvkm_drm_kms.c call a single translated commit-tail
 * entry point instead of individually sequencing display helper blocks. */

/* Per-head HW timing, computed from a drm_display_mode by the crtc atomic
 * hook (nouveau headc57d convention), packed as the EVO methods expect. */
struct nvkm_disp_mode {
	uint32_t raster;	/* h.active | v.active<<16 */
	uint32_t sync;		/* h.synce  | v.synce<<16  */
	uint32_t blanke;	/* h.blanke | v.blanke<<16 */
	uint32_t blanks;	/* h.blanks | v.blanks<<16 */
	uint32_t blank2;	/* v.blank2e<<16 | v.blank2s (progressive => 1) */
	uint32_t clk;		/* pixel clock, Hz */
	uint32_t iw, ih;	/* viewport-in  active w,h */
	uint32_t ow, oh;	/* viewport-out active w,h */
	uint32_t interlace;
	uint32_t nhsync;
	uint32_t nvsync;
};

/* Per-window scanout state from the DRM plane atom.  The display engine code
 * converts this to NVC57E SET_IMAGE/POINT/COMPOSITION methods. */
struct nvkm_disp_scanout {
	uint64_t paddr;		/* VRAM physical address; 0 => internal test fb */
	uint64_t modifier;	/* DRM_FORMAT_MOD_* */
	uint32_t format;	/* DRM fourcc */
	uint32_t pitch;		/* bytes */
	uint32_t width, height;	/* framebuffer size */
	uint32_t src_x, src_y;	/* pixels */
	uint32_t src_w, src_h;	/* pixels */
	uint32_t crtc_x, crtc_y;	/* pixels */
	uint32_t crtc_w, crtc_h;	/* pixels */
	uint32_t async_flip;
};

/* Driver-internal first-light state machine translated from nouveau_disp.txt's
 * nv50_disp_atomic_commit_tail active-enable path. */
int	 nvkm_gsp_disp_nouveau_commit_tail(struct nvkm_softc *sc,
	     uint32_t head, uint32_t win, uint32_t display_id,
	     const struct nvkm_disp_mode *m,
	     const struct nvkm_disp_scanout *scanout);

/* Dump host-side disp state after a modeset (diagnostic). */
void	 nvkm_gsp_disp_dump_state(struct nvkm_softc *sc);

/* KMS skeleton (M3a): register DRIVER_MODESET + mode_config + connectors. */
int	 nvkm_drm_kms_init(struct drm_device *dev, struct nvkm_softc *sc);

/* Driver-internal atomic modeset: drive the first connected output's preferred
 * mode through the full atomic path (-> crtc atomic_enable -> EVO modeset).
 * Triggered via the dev.drm.<n>.kms_lightup sysctl. Returns 0 / errno. */
int	 nvkm_drm_kms_light_up(struct nvkm_softc *sc);

/* Allocated VA space (FERMI_VASPACE_A) — server-managed PDE flavour
 * (external = false). Caller still has to call COPY_SERVER_RESERVED_PDES
 * to obtain the page-table copies before issuing map ops. */
#define FERMI_VASPACE_A			0x000090f1U
#define NVKM_RM_VASPACE			0x90f10000u
#define NV_VASPACE_ALLOCATION_INDEX_GPU_NEW	0x00U

struct NV_VASPACE_ALLOCATION_PARAMETERS_r535 {
	uint32_t index;
	int32_t  flags;
	uint64_t vaSize;
	uint64_t vaStartInternal;
	uint64_t vaLimitInternal;
	uint32_t bigPageSize;
	uint8_t  _pad[4];
	uint64_t vaBase;
};

struct nvkm_gsp_vaspace {
	struct nvkm_gsp_object	object;
};

int	 nvkm_gsp_vaspace_ctor(struct nvkm_gsp_device *device,
	    struct nvkm_gsp_vaspace *vas);
int	 nvkm_gsp_vaspace_dtor(struct nvkm_gsp_vaspace *vas);

/* VRAM range allocator.
 *
 * Ownership model:
 * - nvkm_gsp_vram_alloc() currently returns owned VRAM backing to the
 *   caller. The caller's object lifetime owns that backing.
 * - Any BAR1/BAR2/GPU-VA mapping created from the returned paddr is a borrow.
 *   The mapping may be read-borrowed by multiple users, but writable access
 *   must be exclusive at the object level.
 * - A borrow must not outlive its owner. VM_BIND records therefore borrow GEM
 *   BO backing through a held GEM object reference; they must unmap/drop that
 *   reference, not free the backing allocation directly.
 * - Internal RM-visible objects such as BAR page tables, VMM page tables,
 *   channel inst/USERD, and GR ctxbufs are pinned until their explicit owner
 *   lifetime ends. They must never be returned by the GEM free path.
 *
 * The window is set in nvkm_gsp_vram_init from GSP static-info usable FB
 * ranges and managed with drm_mm. All allocations are PAGE_SIZE-aligned; alloc
 * returns the physical VRAM offset to hand to GSP-RM as
 * NV_MEMORY_DESC_PARAMS.base. Runtime VRAM frees are accepted only through the
 * typed GEM path after owner validation.
 */
int	  nvkm_gsp_vram_init(struct nvkm_softc *sc);
uint64_t nvkm_gsp_vram_alloc_kind(struct nvkm_softc *sc, uint64_t size,
	    uint64_t align, enum nvkm_vram_kind kind, void *owner);
uint64_t nvkm_gsp_vram_alloc(struct nvkm_softc *sc, uint64_t size,
	    uint64_t align);
struct nvkm_vram_alloc *nvkm_gsp_vram_alloc_ref(struct nvkm_softc *sc,
	    uint64_t size, uint64_t align, enum nvkm_vram_kind kind,
	    void *owner);
void	  nvkm_gsp_vram_free_gem(struct nvkm_softc *sc,
	    struct nvkm_vram_alloc *alloc, void *owner);
void	  nvkm_gsp_vram_free_kind(struct nvkm_softc *sc, uint64_t paddr,
	    enum nvkm_vram_kind kind, void *owner);

/* RM_ENGINE_TYPE values used as channel/cgrp engineType field. */
#define NV2080_ENGINE_TYPE_GRAPHICS	0x00000001U
#define NV2080_ENGINE_TYPE_COPY0	0x00000009U
#define NV2080_ENGINE_TYPE_COPY1	0x0000000aU
#define NV2080_ENGINE_TYPE_COPY2	0x0000000bU

/* Channel group (KEPLER_CHANNEL_GROUP_A / TSG). */
struct nvkm_gsp_chgrp {
	struct nvkm_gsp_object	object;
};

int	 nvkm_gsp_chgrp_ctor(struct nvkm_gsp_device *device,
	    struct nvkm_gsp_vaspace *vas, uint32_t engine_type,
	    struct nvkm_gsp_chgrp *grp);
int	 nvkm_gsp_chgrp_dtor(struct nvkm_gsp_chgrp *grp);

/* GPFIFO channel (TURING_CHANNEL_GPFIFO_A for our Turing target).
 * Owns the VRAM backing for inst/USERD and the sysmem mthdbuf. */
/* Sysmem-allocated 4 KiB page for GART-style data BOs. */
struct nvkm_gsp_sysmem_page {
	void			*kva;
	vm_paddr_t		paddr;
};

#define NVKM_GSP_GR_MAX_CTXBUFS	16

enum nvkm_gsp_gr_ctxbuf_target {
	NVKM_GSP_GR_CTXBUF_TARGET_INST = 0,
	NVKM_GSP_GR_CTXBUF_TARGET_INST_SR_LOST = 1,
};

struct nvkm_gsp_gr_ctxbuf {
	void			*kva;
	vm_paddr_t		paddr;
	uint64_t		size;
	uint64_t		gva;
	uint32_t		buffer_id;
	uint8_t			target;
	uint8_t			init;
	uint8_t			ro;
	uint8_t			nonmapped;
};

struct nvkm_gsp_chan {
	struct nvkm_gsp_object	object;
	/* Borrowed back-pointer to the VMM this channel was created in.
	 * Used by submit-mapping teardown and the RC fault PTE walk to act
	 * on the correct (per-file) address space. */
	struct nvkm_gsp_vmm	*vmm;
	uint64_t		inst_vram;	/* VRAM physical base */
	uint64_t		userd_vram;	/* VRAM physical base */
	uint64_t		userd_bar2_gva;	/* BAR2 GVA for host access */
	uint64_t		inst_bar1_gva;	/* BAR1 GVA for L2-coherent inst write */
	void			*mthdbuf_kva;	/* sysmem kva (contigmalloc) */
	uint64_t		mthdbuf_paddr;	/* sysmem physical base */
	uint32_t		mthdbuf_size;	/* alloc size for contigfree */
	int			chid;		/* allocated chid (>=1) */
	uint32_t		gsp_token;	/* GSP-issued doorbell token */
	uint32_t		gpf_put;	/* software GP_PUT for submit_gpf ring */
	uint32_t		submit_post_slot;	/* next EXEC post/sema ring slot */
	uint64_t		submit_post_slots_busy;	/* active EXEC post/sema slots */
	uint8_t			faulted;
	int			fault_error;
	struct nvkm_gsp_object	ce_obj;	/* TURING_DMA_COPY_A engine obj */
	struct nvkm_gsp_object	usermode_obj;	/* TURING_USERMODE_A */
	uint64_t		submit_gva_push;
	uint64_t		submit_gva_gpf;
	uint64_t		submit_gva_sema;
	uint8_t			gr_ctx_promoted;
	uint8_t			gr_ctxbuf_nr;
	struct nvkm_gsp_gr_ctxbuf gr_ctxbuf[NVKM_GSP_GR_MAX_CTXBUFS];

	/* GPU-submit BOs: pre-allocated before channel alloc so we
	 * can pass a real gpFifoOffset to GSP. PD0/SPT hold the host
	 * PT under PD1[8] (covers GVA 0x100000000..). */
	/* PT + data BOs all in VRAM, accessed via BAR1 (nvkm_bar1_page). */
	struct nvkm_bar1_page	submit_pd0;	/* VRAM (PT page) */
	struct nvkm_bar1_page	submit_lpt;	/* VRAM (empty 64 KiB LPT) */
	struct nvkm_bar1_page	submit_spt;	/* VRAM (PT page) */
	struct nvkm_gsp_sysmem_page submit_push;	/* sysmem (matches nouveau GART) */
	struct nvkm_gsp_sysmem_page submit_gpf;	/* sysmem */
	struct nvkm_gsp_sysmem_page submit_sema;	/* sysmem */
};

int	 nvkm_gsp_chan_ctor(struct nvkm_gsp_vmm *vmm,
	    uint32_t engine_type, struct nvkm_gsp_chan *chan);
int	 nvkm_gsp_chan_dtor(struct nvkm_gsp_chan *chan);
int	 nvkm_gsp_chan_alloc_obj(struct nvkm_gsp_chan *chan,
	    uint32_t handle, uint32_t oclass, struct nvkm_gsp_object *obj);
int	 nvkm_gsp_chan_promote_gr_ctx(struct nvkm_gsp_vmm *vmm,
	    struct nvkm_gsp_chan *chan, uint8_t golden);
int	 nvkm_gsp_gr_oneinit(struct nvkm_gsp_vmm *vmm);

/* NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO — same struct as in static_info,
 * but cleaner to query via RM_CONTROL on the subdevice. */
#define NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO	0x20801320U
#define NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_MAX_ENTRIES	16U
#define NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_MEM_TYPES	17U

struct nvkm_fb_region_info {
	uint64_t base;
	uint64_t limit;
	uint64_t reserved;
	uint32_t performance;
	uint8_t  supportCompressed;
	uint8_t  supportISO;
	uint8_t  bProtected;
	uint8_t  blackList[17];
};

struct NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_PARAMS_r570 {
	uint32_t numFBRegions;
	uint8_t  _pad[4];
	struct nvkm_fb_region_info fbRegion[16];
};


/* NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE.
 * Source: nouveau r535/nvrm/fifo.h:11-34, fifo.c:469-490. */
#define NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE	0x20801112U
#define NV2080_FIFO_DEV_INFO_MAX_ENTRIES		32U
#define NV2080_FIFO_DEV_INFO_ENG_DATA_TYPES		16U
#define NV2080_FIFO_DEV_INFO_ENG_MAX_PBDMA		2U
#define NV2080_FIFO_DEV_INFO_ENG_MAX_NAME_LEN		16U

#define ENGINE_INFO_TYPE_RM_ENGINE_TYPE			2U
#define ENGINE_INFO_TYPE_RUNLIST			3U
#define RM_ENGINE_TYPE_COPY0				9U

struct NV2080_CTRL_FIFO_DEVICE_ENTRY_r570 {
	uint32_t engineData[NV2080_FIFO_DEV_INFO_ENG_DATA_TYPES];
	uint32_t pbdmaIds[NV2080_FIFO_DEV_INFO_ENG_MAX_PBDMA];
	uint32_t pbdmaFaultIds[NV2080_FIFO_DEV_INFO_ENG_MAX_PBDMA];
	uint32_t numPbdmas;
	char     engineName[NV2080_FIFO_DEV_INFO_ENG_MAX_NAME_LEN];
};

struct NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS_r570 {
	uint32_t baseIndex;
	uint32_t numEntries;
	uint8_t  bMore;
	uint8_t  _pad[3];
	struct NV2080_CTRL_FIFO_DEVICE_ENTRY_r570
	    entries[NV2080_FIFO_DEV_INFO_MAX_ENTRIES];
};

/* Returns 0 + writes *runl on success. */
int	 nvkm_gsp_query_ce0_runlist(struct nvkm_softc *sc, uint32_t *runl);

/* End-to-end smoke test: build host PT for 3 sysmem pages
 * (pushbuf/gpfifo/sema) at fixed GVAs, encode an NVC36F SEM
 * RELEASE pushbuf, kick + doorbell, poll sema for 0xdeadbeef. */
int	 nvkm_gsp_submit_test(struct nvkm_softc *sc);

#endif /* _NVKM_GSP_RM_H_ */
