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

struct nvkm_object;

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
	struct nvkm_gsp		*gsp;
};

struct nvkm_memory;

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

/* Bring up imported nouveau core/r535 GSP display.
 * Runs on the attach thread (blockable; synchronous GSP RPCs). */
int	 nvkm_gsp_disp_init(struct nvkm_softc *sc);
uint32_t nvkm_gsp_disp_supported_mask(struct nvkm_softc *sc);
uint32_t nvkm_gsp_disp_head_count(struct nvkm_softc *sc);

struct nvkm_gsp_disp_output_info {
	uint32_t display_id;
	uint32_t heads;
	uint8_t output_type;
	uint8_t connector_type;
	uint8_t output_location;
	uint8_t connector_location;
	uint8_t or_mask;
	uint8_t link;
	bool is_dp;
	bool mst_capable;
	bool dp_interlace_capable;
};

/*
 * Ownership:
 *   Writes a scalar snapshot of the GSP/RM output that owns display_id into
 *   caller-owned storage. No nvkm_outp or nvkm_conn pointer escapes.
 *
 * Lifetime:
 *   The returned fields are valid as a point-in-time capability snapshot. A
 *   later hotplug or display re-enumeration may require a fresh query.
 *
 * Threading:
 *   Call during KMS object construction or another blockable display context
 *   where sc->disp is stable. This function does not issue GSP RPCs.
 */
int	 nvkm_gsp_disp_output_info(struct nvkm_softc *sc, uint32_t display_id,
	     struct nvkm_gsp_disp_output_info *info);

/* Probe connected outputs and print their EDID. Blockable context only. */
void	 nvkm_gsp_disp_probe_connected(struct nvkm_softc *sc);
/* Is the given GSP displayId currently connected? 1/0, <0 on error. Blockable. */
int	 nvkm_gsp_disp_connected(struct nvkm_softc *sc, uint32_t display_id);
/* Read EDID for a displayId into out (<=*outlen); sets *outlen. Blockable. */
int	 nvkm_gsp_disp_read_edid(struct nvkm_softc *sc, uint32_t display_id,
	     uint8_t *out, uint32_t *outlen);
int	 nvkm_gsp_disp_channel_pushbuf(struct nvkm_softc *sc, int32_t oclass,
	     int inst, struct nvkm_memory *memory);
int	 nvkm_gsp_disp_dmac_alloc(struct nvkm_softc *sc, uint32_t oclass,
		     int inst, uint32_t put_offset, struct nvkm_gsp_object *object);
int	 nvkm_gsp_disp_pio_alloc(struct nvkm_softc *sc, uint32_t oclass,
		     int inst, struct nvkm_gsp_object *object);
int	 nvkm_gsp_disp_dmac_bind(struct nvkm_softc *sc, uint32_t oclass,
		     int inst, struct nvkm_object *object, uint32_t handle);
void	 nvkm_gsp_disp_dmac_unbind(struct nvkm_softc *sc, int cookie);

/* dispnv50 bridge: adapt committed DragonFly DRM state to imported emitters. */
struct nvkm_dispnv50_hdmi_info {
	bool has_infoframe;
	bool scdc_supported;
	bool scdc_scrambling;
	bool scdc_low_rates;
};

enum nvkm_dispnv50_dither_mode {
	NVKM_DISPNV50_DITHER_MODE_OFF = 0,
	NVKM_DISPNV50_DITHER_MODE_ON = 1,
	NVKM_DISPNV50_DITHER_MODE_DYNAMIC2X2 = 513,
	NVKM_DISPNV50_DITHER_MODE_STATIC2X2 = 769,
	NVKM_DISPNV50_DITHER_MODE_TEMPORAL = 1025,
	NVKM_DISPNV50_DITHER_MODE_AUTO = 1026,
};

enum nvkm_dispnv50_dither_depth {
	NVKM_DISPNV50_DITHER_DEPTH_6BPC = 0,
	NVKM_DISPNV50_DITHER_DEPTH_8BPC = 16,
	NVKM_DISPNV50_DITHER_DEPTH_AUTO = 17,
};

enum nvkm_dispnv50_underscan_mode {
	NVKM_DISPNV50_UNDERSCAN_OFF = 0,
	NVKM_DISPNV50_UNDERSCAN_ON = 1,
	NVKM_DISPNV50_UNDERSCAN_AUTO = 2,
};

/*
 * Head programming parameters decoded by KMS from connector atomic state.
 *
 * Ownership:
 *   This structure owns only scalar values. Callers retain ownership of DRM
 *   state and connector objects; the bridge must not store their pointers.
 *
 * Lifetime:
 *   Valid for one display programming callback. Prepared output routes store
 *   their own full head-config snapshot, so commit consume never needs to read
 *   DRM connector state again.
 *
 * Threading:
 *   Produced while DRM atomic locks protect the connector/CRTC state and
 *   consumed synchronously by the display commit path.
 */
struct nvkm_dispnv50_head_config {
	struct nvkm_dispnv50_hdmi_info hdmi;
	uint8_t bpc;
	uint32_t dither_mode;
	uint32_t dither_depth;
	uint32_t scaling_mode;
	uint32_t underscan_mode;
	uint32_t underscan_hborder;
	uint32_t underscan_vborder;
	bool underscan_auto_is_hdmi;
};

#define NVKM_DISPNV50_DP_DPCD_SIZE	16U

struct nvkm_dispnv50_dp_sst_candidate {
	bool valid;
	uint8_t link_bw;
	uint8_t lanes;
	uint32_t watermark;
	uint32_t hblank_symbols;
	uint32_t vblank_symbols;
};

/*
 * Prepared output route consumed by a single KMS atomic commit.
 *
 * Ownership:
 *   The KMS atomic state owns this storage.  The bridge fills scalar snapshots
 *   only; no nvkm_outp, nvkm_ior, DRM connector, CRTC, or framebuffer pointer
 *   is stored here.  Any SOR/IOR acquired while preparing the route is released
 *   by abort unless the matching commit consumes it.
 *
 * Lifetime:
 *   Valid only between nvkm_dispnv50_output_prepare() and the matching
 *   atomic_enable callback in the same commit.  The refcounted atomic state
 *   keeps it alive across nonblocking commit_work, and atomic_state_clear must
 *   abort or clear it before the atomic state is released.
 *
 * Threading:
 *   Writers run before swap_state while DRM modeset locks are held.  After
 *   swap_state, only the commit worker owning the atomic-state reference may
 *   consume or clear it.  It must not be shared with IRQ, HPD, or async cursor
 *   paths.
 */
struct nvkm_dispnv50_output_prepare {
	bool valid;
	bool acquired;
	bool consumed;
	uint32_t display_id;
	uint32_t head;
	uint8_t output_type;
	uint8_t outp_index;
	int ior_id;
	uint8_t ior_link;
	uint32_t sor_proto;
	struct nvkm_dispnv50_head_config config;
	uint8_t dp_dpcd[NVKM_DISPNV50_DP_DPCD_SIZE];
	uint32_t dp_max_rate;
	uint32_t dp_min_rate;
	uint8_t dp_max_lanes;
	bool dp_enhanced_framing;
	struct nvkm_dispnv50_dp_sst_candidate dp_sst;
};

struct drm_crtc;
struct drm_display_mode;
int	 nvkm_dispnv50_output_prepare(struct nvkm_softc *sc,
	     struct drm_display_mode *mode, uint32_t head,
	     uint32_t display_id,
	     const struct nvkm_dispnv50_head_config *config,
	     struct nvkm_dispnv50_output_prepare *prepare);
/*
 * Validate output-specific mode capability without programming display state.
 *
 * Ownership:
 *   Borrows sc, display_id, and the caller-owned mode for the duration of the
 *   call.  No DRM object, nvkm_outp pointer, or prepared route escapes.
 *
 * Lifetime:
 *   The returned value is a DRM MODE_* status integer for this point-in-time
 *   output/sink capability snapshot.  A later hotplug or DPCD change requires
 *   a fresh validation.
 *
 * Threading:
 *   Runs in blockable KMS probe/check context.  It may issue read-only AUX
 *   transactions for DP, but it must not acquire SORs, train links, or program
 *   EVO/GSP display state.
 */
int	 nvkm_dispnv50_output_mode_valid(struct nvkm_softc *sc,
	     uint32_t display_id, const struct drm_display_mode *mode,
	     uint8_t bpc, bool dp_interlace_capable);
void	 nvkm_dispnv50_output_prepare_abort(struct nvkm_softc *sc,
	     struct nvkm_dispnv50_output_prepare *prepare);
int	 nvkm_dispnv50_atomic_enable(struct nvkm_softc *sc,
	     struct drm_crtc *crtc, uint32_t head, uint32_t win,
	     uint32_t display_id,
	     const struct nvkm_dispnv50_head_config *config);
int	 nvkm_dispnv50_atomic_enable_prepared(struct nvkm_softc *sc,
	     struct drm_crtc *crtc, uint32_t win,
	     struct nvkm_dispnv50_output_prepare *prepare);
/*
 * Ownership:
 *   Borrows the committed DRM CRTC state and the connector-derived HEAD
 *   configuration for one core-channel update.  No framebuffer, connector, or
 *   output route ownership is transferred.
 *
 * Lifetime:
 *   Used for active connector private-property commits that do not require a
 *   full modeset.  The caller supplies the nouveau-style change mask: scaler
 *   and underscan changes emit only VIEW, while dither/bpc changes emit only
 *   DITHER.  The function submits the same notifying core UPDATE boundary that
 *   nouveau uses for normal HEAD set commits; notifier timeout is diagnostic
 *   and is handled by the bridge commit helper.
 *
 * Threading:
 *   Called from the serialized atomic commit tail and may sleep.  It must not
 *   be called from interrupt context or async cursor paths.
 */
int	 nvkm_dispnv50_head_update(struct nvkm_softc *sc,
	     struct drm_crtc *crtc, uint32_t head,
	     const struct nvkm_dispnv50_head_config *config, bool update_view,
	     bool update_dither);
/*
 * Ownership:
 *   Borrows sc plus scalar KMS routing state for one modeset-disable commit.
 *   The bridge owns no DRM connector, CRTC, plane, or framebuffer reference
 *   through this call.
 *
 * Lifetime:
 *   display_id must describe the output routed to head in the old committed
 *   CRTC state, or be zero to let the bridge use its last audited route.  The
 *   window/head/output clear methods are submitted as one interlocked EVO
 *   transaction. Output release happens after the core UPDATE is accepted
 *   and its notifier wait has been attempted; notifier timeout is reported
 *   but does not abort the disable release path, matching nouveau atomic
 *   tail semantics.
 *
 * Threading:
 *   Called from the atomic commit tail under DRM modeset serialization. The
 *   function may block on display notifier completion and must not be called
 *   from interrupt context.
 */
int	 nvkm_dispnv50_modeset_disable(struct nvkm_softc *sc, uint32_t head,
	    uint32_t win, uint32_t display_id, bool submit_disable);
/*
 * Ownership:
 *   Borrows the committed DRM CRTC/primary plane state. The bridge never owns
 *   the framebuffer BO; prepare_fb and cleanup_fb own the scanout pin record.
 *
 * Lifetime:
 *   display_id must describe the output route active for this CRTC update, or
 *   be zero only for internal rescue paths that rely on the audited route.
 *
 * Threading:
 *   Called from atomic commit tail under DRM modeset serialization. The normal
 *   page-flip path must not block on window notifier completion. When
 *   color_update is true, the call owns the whole image + color display commit
 *   for that atomic state and may block on the notifiers needed by that commit.
 */
int	 nvkm_dispnv50_plane_update(struct nvkm_softc *sc,
	     struct drm_crtc *crtc, uint32_t win, uint32_t display_id,
	     bool color_update);
/*
 * Ownership:
 *   Borrows the dispnv50 bridge audit records inside sc. It copies only scalar
 *   state from pending to current and owns no DRM object, BO, channel, or
 *   notifier reference.
 *
 * Lifetime:
 *   Call after the DRM commit-tail flip-done barrier has completed. At that
 *   point an async primary-plane update has reached the same completion point
 *   as the page-flip event/out-fence, so the pending audit may become current.
 *
 * Threading:
 *   Called from the serialized atomic commit tail. Sysctl readers may sample
 *   the transition locklessly; the audit record is diagnostic state only.
 */
void	 nvkm_dispnv50_publish_pending_flip(struct nvkm_softc *sc,
	     bool publish);
int	 nvkm_dispnv50_plane_disable(struct nvkm_softc *sc, uint32_t win);
/*
 * Ownership:
 *   Borrows the committed DRM CRTC/cursor plane state and the framebuffer BO
 *   reference held by atomic KMS. The bridge never owns that BO; prepare_fb and
 *   cleanup_fb own the matching scanout pin record.
 *
 * Lifetime:
 *   The borrowed CRTC/plane state must remain the current committed state for
 *   the duration of the call. The cursor channel and head state live inside
 *   sc->dispnv50 until display teardown.
 *
 * Threading:
 *   Runs from KMS commit context. It may sleep while pushing EVO/core updates
 *   and, for non-legacy commits, waiting for the core notifier. Legacy cursor
 *   ioctls must use nouveau's non-notifying core update path so a cursor-only
 *   state change does not wait on the full display completion path.
 */
int	 nvkm_dispnv50_cursor_update(struct nvkm_softc *sc,
	     struct drm_crtc *crtc, uint32_t head, bool legacy_cursor_update);
/*
 * Ownership:
 *   Borrows the current cursor plane state and only updates hardware position.
 *   The caller remains responsible for updating drm_plane->state after success.
 *
 * Lifetime:
 *   Requires a cursor image already programmed for the same CRTC/fb lifetime;
 *   it does not pin, unpin, or replace the cursor BO.
 *
 * Threading:
 *   Runs from DRM's legacy cursor async path. It writes the cursor USER
 *   aperture directly and must not take locks that would invert atomic commit
 *   ordering.
 */
int	 nvkm_dispnv50_cursor_async_update(struct nvkm_softc *sc,
	     struct drm_crtc *crtc, uint32_t head, int32_t x, int32_t y);
/*
 * Ownership:
 *   Borrows only the head/core display objects owned by sc->dispnv50.
 *
 * Lifetime:
 *   Disables the hardware cursor latch for the head; BO lifetime cleanup is
 *   still handled by the KMS plane cleanup_fb path.
 *
 * Threading:
 *   Runs from KMS commit context and may sleep waiting for the core notifier
 *   for non-legacy commits. Legacy cursor ioctl disables use nouveau's
 *   non-notifying core update path.
 */
int	 nvkm_dispnv50_cursor_disable(struct nvkm_softc *sc, uint32_t head,
	     bool legacy_cursor_update);
void	 nvkm_dispnv50_fini(struct nvkm_softc *sc);

/* Register DRIVER_MODESET objects backed by imported GSP display discovery. */
int	 nvkm_drm_kms_init(struct drm_device *dev, struct nvkm_softc *sc);
void	 nvkm_drm_kms_fini(struct nvkm_softc *sc);
int	 nvkm_drm_kms_schedule(struct nvkm_softc *sc, const char *reason);

/* Driver-internal atomic modeset: drive the first connected output's preferred
 * mode through the full atomic path (-> crtc atomic_enable -> EVO modeset).
 * Used by the auto-KMS task and by the debug sysctl. Returns 0 / errno. */
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
	uint32_t		gpf_free;	/* cached free submit_gpf entries */
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

	/* GPU-submit data pages: pre-allocated before channel alloc so we
	 * can pass a real gpFifoOffset to GSP.  The GVA mappings are installed
	 * through the owning VMM; channel code must not keep a parallel PT tree. */
	struct nvkm_dmamem	submit_push;	/* DMA-coherent sysmem push page */
	struct nvkm_dmamem	submit_gpf;	/* DMA-coherent sysmem GPFIFO page */
	struct nvkm_dmamem	submit_sema;	/* DMA-coherent sysmem semaphore page */
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

#define NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE	0x20802068U
#define NV2080_CTRL_PERF_PSTATES_UNDEFINED	0x00000000U
#define NV2080_CTRL_PERF_PSTATES_P0		0x00000001U
#define NV2080_CTRL_PERF_PSTATES_P8		0x00000100U

struct NV2080_CTRL_PERF_GET_CURRENT_PSTATE_PARAMS_r570 {
	uint32_t currPstate;
};

int	 nvkm_gsp_query_perf_current_pstate(struct nvkm_softc *sc,
	    uint32_t *pstate);


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
