/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Internal definitions shared across nvkm translation units.
 */

#ifndef _NVKM_PRIV_H_
#define _NVKM_PRIV_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/serialize.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <machine/atomic.h>
#include <sys/proc.h>

#include <drm/drm_mm.h>

#define NVKM_PCI_VENDOR_NVIDIA	0x10de

enum nvkm_vram_kind {
	NVKM_VRAM_UNKNOWN = 0,
	NVKM_VRAM_GEM,
	NVKM_VRAM_BAR1_SPT,
	NVKM_VRAM_BAR1_PAGE,
	NVKM_VRAM_BAR2_ROOT,
	NVKM_VRAM_BAR2_PT,
	NVKM_VRAM_BAR2_FLUSH,
	NVKM_VRAM_BAR2_TEST,
	NVKM_VRAM_VMM_PT,
	NVKM_VRAM_CHANNEL_GOLDEN,
	NVKM_VRAM_CHANNEL_INST,
	NVKM_VRAM_CHANNEL_USERD,
	NVKM_VRAM_CHANNEL_SUBMIT_PT,
	NVKM_VRAM_GR_CTXBUF_GLOBAL,
	NVKM_VRAM_GR_CTXBUF_CHANNEL,
	NVKM_VRAM_KIND_COUNT
};

struct nvkm_vram_alloc;
TAILQ_HEAD(nvkm_vram_alloc_head, nvkm_vram_alloc);

struct nvkm_vram_alloc {
	TAILQ_ENTRY(nvkm_vram_alloc) alloc_link;
	struct drm_mm_node node;
	uint64_t paddr;
	uint64_t size;
	uint64_t align;
	enum nvkm_vram_kind kind;
	void *owner;
	bool free;
};

const char *nvkm_vram_kind_name(enum nvkm_vram_kind kind);

struct nvkm_softc;
extern int nvkm_debug;
void	nvkm_debugf(device_t dev, const char *fmt, ...) __printflike(2, 3);
void	nvkm_infof(device_t dev, const char *fmt, ...) __printflike(2, 3);
void	nvkm_drm_exec_fault_channel_locked(struct nvkm_softc *sc,
	    uint32_t chid, int error);

/* Initial supported device. Phase 0 targets only TU102. */
#define NVKM_PCI_DEVICE_TU102	0x1e07

/* PMC: NVIDIA Master Control. Chip identification. */
#define NV_PMC_BOOT_0		0x00000000

/*
 * TU102 SEC2 engine BAR0 offsets.
 *   NV_PSEC                 = 0x840000..0x843fff
 *   NV_PSEC_FBIF_BASE       = 0x840600
 *   NV_PSEC_FALCON_ENGINE   = 0x8403c0
 * Turing SEC2 is Falcon-only (no RISC-V), so the second register block
 * is unused. From open-rm dev_sec_pri.h / dev_sec_addendum.h.
 */
#define NVKM_TU102_SEC2_BASE	0x00840000
#define NVKM_TU102_SEC2_FBIF	0x00840600

/*
 * TU102 GSP-Falcon (GSP-lite) BAR0 offsets. The GSP engine on Turing
 * contains both a Falcon core (used for HS firmware like FwSec) and a
 * RISC-V core (used for GSP-RM itself). FwSec-FRTS and FwSec-SB run
 * on the Falcon; the booter then resets GSP into RISC-V mode.
 *   NV_PGSP                 = 0x110000..0x113fff
 *   NV_PGSP_FBIF_BASE       = 0x110600
 *   NV_FALCON2_GSP_BASE     = 0x111000 (RISC-V control regs)
 * From open-rm dev_gsp.h / dev_gsp_addendum.h / dev_riscv_pri.h.
 */
#define NVKM_TU102_GSP_BASE	0x00110000
#define NVKM_TU102_GSP_FBIF	0x00110600
#define NVKM_TU102_GSP_RISCV	0x00111000

/*
 * PCI cfg-space mirror inside BAR0. cfg.addr is the same (0x088000) for the
 * entire gp100 family and later, including Turing. See linux/drivers/gpu/drm/
 * nouveau/nvkm/subdev/pci/gp100.c.
 */
#define NV_PMC_PCI_CFG		0x00088000
#define NV_PMC_ROM_SHADOW	(NV_PMC_PCI_CFG + 0x50)
#define   NV_PMC_ROM_SHADOW_EN	0x00000001

/* PROM aperture in BAR0: SPI ROM contents visible as MMIO. */
#define NV_PROM			0x00300000
#define NV_PROM_SIZE		0x00100000	/* 1 MiB aperture */

/*
 * PRAMIN aperture: a BAR0-visible 1 MiB window into VRAM. The window's
 * VRAM base is set via NV_PBUS_PRAMIN @ 0x001700 (value is base >> 16).
 * GPU bootcode places the active VBIOS image in VRAM at attach; reading
 * via PRAMIN gets the GPU's working copy, which differs from the raw
 * PROM contents on cards that "stitch" pointers at boot time.
 */
#define NV_PRAMIN		0x00700000
#define NV_PRAMIN_SIZE		0x00100000
#define NV_PBUS_PRAMIN		0x00001700	/* window base, shifted right 16 */

/*
 * Display control regs used to discover where the GPU has staged its
 * runtime VBIOS copy in VRAM. (See nouveau shadowramin.c.)
 *   NV_PDISP_VGA_CR  : present for Volta and Turing (we use Turing path)
 *   bit 3            : aperture enabled
 *   bits 1:0 == 1    : aperture target is VRAM
 *   bits 31:8        : staging address >> 8
 */
#define NV_PDISP_VGA_CR			0x00625f04
#define   NV_PDISP_VGA_CR_TARGET_VRAM	0x00000001u
#define   NV_PDISP_VGA_CR_TARGET_MASK	0x00000003u
#define   NV_PDISP_VGA_CR_ENABLED	0x00000008u
#define NV_PDISP_GENERAL_CTL		0x00021c04
#define   NV_PDISP_GENERAL_CTL_DISABLED	0x00000001u

/*
 * VBIOS image cache. PCI Option ROMs are chained: a legacy x86 image,
 * one or more UEFI images, and (on NVIDIA cards) private images chained
 * via NPDE that carry the FwSec firmware needed by GSP boot. NVIDIA's
 * own reader sizes this to the full 1 MiB PROM aperture.
 */
#define NVKM_VBIOS_MAX_SIZE	0x100000

#define NVKM_NUM_BARS		6

struct firmware;
struct nvkm_falcon;

/*
 * A chunk of system memory that is mapped for DMA and visible to both
 * the host CPU (via kva) and the GPU (via paddr). Used for booter and
 * GSP image staging, WPR descriptors, msgq rings, and similar.
 */
struct nvkm_dmamem {
	void		*kva;
	bus_addr_t	paddr;
	bus_size_t	size;
	bus_dma_tag_t	tag;
	bus_dmamap_t	map;
};

/*
 * Parsed view of a booter / HS-signed Falcon ucode container.
 * All offsets are bytes from the start of the blob.
 */
struct nvkm_booter_info {
	const uint8_t	*blob;
	uint32_t	blob_size;
	uint32_t	data_offset;	/* code+data section start in blob */
	uint32_t	data_size;

	uint32_t	nmem_offset;	/* non-secure code, in blob */
	uint32_t	nmem_size;
	uint32_t	imem_offset;	/* secure code, in blob */
	uint32_t	imem_size;
	uint32_t	dmem_offset;	/* DMEM data, in blob */
	uint32_t	dmem_size;
	uint32_t	boot_addr;	/* Falcon BOOTVEC value */

	uint32_t	sig_prod_offset;
	uint32_t	sig_prod_size;
	uint32_t	patch_loc;
	uint32_t	patch_sig;
	uint32_t	num_sig;
};

#define NVKM_GSP_NTFY_MAX 16

struct nvkm_gsp_client;
struct nvkm_gsp_device;
struct nvkm_gsp_vaspace;
struct nvkm_gsp_chgrp;
struct nvkm_gsp_chan;
struct nvkm_gsp_vmm;
struct dma_fence;

/* Per-RPC pending entry: queued on sc->gsp_pending while waiting
 * for a reply that matches function. The msgq drainer (ISR or
 * another lwkt) sets done + wakeup; the issuer then consumes
 * reply_buf and removes from the list. */
struct nvkm_gsp_pending {
	LIST_ENTRY(nvkm_gsp_pending) link;
	uint32_t  fn;
	uint32_t  seq;
	volatile u_int done;     /* 0 = pending, 1 = reply ready; use atomic_*_acq/rel */
	void     *reply_buf;
	uint32_t  reply_len;
};
LIST_HEAD(nvkm_gsp_pending_list, nvkm_gsp_pending);

struct nvkm_drm_exec_pending {
	LIST_ENTRY(nvkm_drm_exec_pending) link;
	/* Borrowed channel pointer. Its submit pages outlive this pending
	 * record because channel teardown cancels matching pending records
	 * before calling nvkm_gsp_chan_dtor().
	 */
	struct nvkm_gsp_chan *chan;
	/* Borrowed semaphore slot inside chan->submit_sema. The slot remains
	 * owned by this pending record until interrupt completion or channel
	 * cancellation releases post_slot.
	 */
	volatile uint32_t *sema;
	uint32_t payload;
	uint32_t post_slot;
	uint64_t trace_seq;
	uint32_t trace_first;
	uint32_t trace_count;
	/* Owned fence references. The submit ioctl may drop its local signal
	 * array and exec_fence references immediately after doorbell.
	 */
	struct dma_fence *fences[64];
	uint32_t fence_count;
	volatile u_int done;
};
LIST_HEAD(nvkm_drm_exec_pending_list, nvkm_drm_exec_pending);

#define NVKM_DRM_EXEC_TRACE_COUNT	1024

struct nvkm_drm_exec_trace {
	uint64_t seq;
	uint32_t channel;
	uint32_t chid;
	uint32_t post_slot;
	uint32_t gpf_index;
	uint32_t push_index;
	uint32_t push_count;
	uint32_t flags;
	uint64_t va;
	uint32_t va_len;
	uint64_t binding_addr;
	uint64_t binding_size;
	uint64_t bo_paddr;
	uint32_t bo_domain;
	uintptr_t obj;
	uint8_t cpu_mapped;
	uint8_t completed;
	int error;
};

#define NVKM_DRM_VM_TRACE_COUNT	4096

enum nvkm_drm_vm_trace_action {
	NVKM_DRM_VM_TRACE_MAP = 1,
	NVKM_DRM_VM_TRACE_UNMAP,
	NVKM_DRM_VM_TRACE_MAP_NULL,
	NVKM_DRM_VM_TRACE_MAP_SPARSE,
	NVKM_DRM_VM_TRACE_UNMAP_SPARSE,
};

struct nvkm_drm_vm_trace {
	uint64_t seq;
	uint32_t action;
	uint32_t flags;
	uint32_t handle;
	uint64_t addr;
	uint64_t range;
	uint64_t bo_offset;
	uint64_t bo_size;
	uint64_t bo_paddr;
	uint32_t bo_domain;
	uintptr_t obj;
	uint8_t cpu_mapped;
	int error;
};


/* BAR1 GVA layout. USERD at fixed slot 0; bar1_alloc_page reuses
 * page-sized slots inside a fixed high BAR1 window owned by this driver. */
#define BAR1_GVA_USERD		0x0ULL
#define BAR1_PD0_MANAGED_FIRST	96u
#define BAR1_PD0_MANAGED_LAST	127u
#define BAR1_PD0_MANAGED_COUNT \
	(BAR1_PD0_MANAGED_LAST - BAR1_PD0_MANAGED_FIRST + 1u)
#define BAR1_GVA_ALLOC_BASE \
	((uint64_t)BAR1_PD0_MANAGED_FIRST * (2ULL << 20))
#define BAR1_GVA_ALLOC_PAGES	(BAR1_PD0_MANAGED_COUNT * 512u)
#define BAR1_GVA_ALLOC_BITMAP_SIZE	(BAR1_GVA_ALLOC_PAGES / 8u)

/* BAR1 host-managed vmm -- see nvkm_gsp_bar1.c.
 * All PT pages live in VRAM (fresh allocations the walker has never
 * touched), set up via PRAMIN. After bar1_init we repoint 0xb80f40
 * at our new inst block. */
struct nvkm_gsp_bar1 {
	uint64_t	spt_bar2_gva;
	uint64_t	pd3_paddr;
	uint64_t	pd2_paddr;
	uint64_t	pd1_paddr;
	uint64_t	pd0_paddr;
	uint64_t	spt_paddr[32];
	uint64_t	next_gva;
	uint8_t		gva_used[BAR1_GVA_ALLOC_BITMAP_SIZE];
	uint64_t	flush_vram_paddr;
	bool		ready;
};

struct nvkm_gsp_bar2_pt {
	LIST_ENTRY(nvkm_gsp_bar2_pt) link;
	uint8_t		level;
	uint16_t	pd2_idx;
	uint16_t	pd1_idx;
	uint16_t	pd0_idx;
	uint64_t	paddr;
};
LIST_HEAD(nvkm_gsp_bar2_pt_list, nvkm_gsp_bar2_pt);

struct nvkm_gsp_bar2 {
	uint64_t	pd3_paddr;	/* GSP\'s BAR2 PDB (we adopt) */
	uint64_t	pd2_paddr;
	uint64_t	pd1_paddr;
	uint64_t	pd0_paddr;
	uint64_t	spt_paddr;
	uint64_t	aperture_size;
	uint64_t	next_gva;
	uint64_t	flush_vram_paddr;
	struct nvkm_gsp_bar2_pt_list pt_pages;
	bool		ready;
};


/* A VRAM page paired with a BAR1 GVA mapping so the host can read/write
 * the page L2-coherently. Created by nvkm_gsp_bar1_alloc_page; do not
 * fill fields manually. */
struct nvkm_bar1_page {
	uint64_t	vram_paddr;
	uint64_t	bar1_gva;
	enum nvkm_vram_kind kind;
	void		*owner;
};

struct nvkm_softc {
	device_t		dev;

	int			bar_rid[NVKM_NUM_BARS];
	struct resource		*bar_res[NVKM_NUM_BARS];

	uint8_t			*vbios;
	uint32_t		vbios_size;

	const struct firmware	*fw_booter_load;

	struct nvkm_falcon	*sec2;
	struct nvkm_falcon	*gsp;	/* GSP-Falcon (HS host) */

	struct nvkm_booter_info	booter;
	struct nvkm_dmamem	booter_dma;	/* staged booter image */
	struct nvkm_dmamem	wpr_meta;	/* GspFwWprMeta in sysmem */
	struct nvkm_dmamem	gsp_image;	/* GSP-RM .fwimage section */
	struct nvkm_dmamem	gsp_radix3;	/* 3-level page table for image */
	struct nvkm_dmamem	gsp_bl;		/* GSP RISC-V bootloader */
	struct nvkm_dmamem	gsp_sig;	/* .fwsignature_tu10x section */
	uint32_t		gsp_sig_size;
	uint32_t		gsp_fwimage_off; /* offset in original ELF */
	struct nvkm_dmamem	gsp_libos;	/* LibOS init args (4 KiB) */
	struct nvkm_dmamem	gsp_loginit;	/* LibOS LOGINIT ring (64 KiB) */
	struct nvkm_dmamem	gsp_logintr;	/* LibOS LOGINTR ring (64 KiB) */
	struct nvkm_dmamem	gsp_logrm;	/* LibOS LOGRM   ring (64 KiB) */
	struct nvkm_dmamem	gsp_shm;	/* cmdq/msgq shared mem (PTEs+512KiB) */
	struct nvkm_dmamem	gsp_rmargs;	/* GSP_ARGUMENTS_CACHED (4 KiB) */
	uint32_t		gsp_shm_ptes_nr;
	uint32_t		gsp_shm_ptes_size;	/* bytes of PTE region */
	uint32_t		gsp_shm_cmdq_off;	/* = ptes_size */
	uint32_t		gsp_shm_msgq_off;	/* = ptes_size + cmdq_size */
	uint32_t		gsp_cmdq_seq;		/* RPC envelope sequence */
	/* Extracted from GspStaticConfigInfo reply post-INIT_DONE: */
	uint32_t		gsp_internal_client;	/* hInternalClient */
	uint32_t		gsp_internal_device;	/* hInternalDevice */
	uint32_t		gsp_internal_subdevice;	/* hInternalSubdevice */
	uint64_t		gsp_bar1_pdb;		/* bar1PdeBase */
	uint64_t		gsp_bar2_pdb;		/* bar2PdeBase */
	bool			gsp_running;		/* set true when GSP_INIT_DONE seen */
	struct nvkm_gsp_ntfy_table {
		struct {
			uint32_t fn;
			int (*func)(void *priv, uint32_t fn, void *repv, uint32_t repc);
			void *priv;
		} tab[NVKM_GSP_NTFY_MAX];
		uint32_t cnt;
	}			gsp_ntfy;
	uint32_t		gsp_rpc_seq;		/* inner RPC sequence (matches reply) */
	uint32_t		gsp_msgq_rptr;		/* host-side msgq read cursor */

	/* Phase 5: RPC token + pending list. gsp_tok serialises
	 * cmdq writes and msgq reads across ioctl lwkts + ithread. */
	struct lwkt_token       gsp_tok;
	struct lwkt_token       chid_tok;       /* per-fifo chid pool lock */
	uint64_t                chid_used[32];  /* 2048-bit chid bitmap */
	struct nvkm_gsp_pending_list gsp_pending;
	struct nvkm_drm_exec_pending_list exec_pending;

	/* IRQ resource + ithread serializer (DragonFly native model). */
	int			irq_rid;
	bool			irq_msi;
	struct thread		*gsp_drain_td;
	bool			gsp_drain_exit;
	struct thread		*gsp_test_td;
	bool			gsp_test_done;
	struct resource		*irq_res;
	void			*irq_cookie;
	struct lwkt_serialize	irq_serialize;
	uint64_t		irq_isr_count;
	uint64_t		irq_msi_rearm_count;

	/* DRM driver registration (Phase 3). */
	struct drm_device	*drm_dev;
	struct pci_dev		*drm_pdev;
	uint64_t		fence_context;
	uint32_t		fence_seqno;

	/* M5 observability counters. */
	uint64_t		exec_submit_count;
	uint64_t		exec_signal_only_count;
	uint64_t		exec_timeout_count;
	uint64_t		exec_internal_fence_count;
	uint64_t		exec_signal_fence_count;
	uint64_t		exec_resv_attach_calls;
	uint64_t		exec_resv_attach_bos;
	uint64_t		exec_async_pending_count;
	uint64_t		exec_async_complete_count;
	uint64_t		exec_async_wait_count;
	uint64_t		exec_async_wait_error_count;
	uint64_t		sync_wait_count;
	uint64_t		sync_wait_error_count;
	uint64_t		sync_signal_count;
	uint64_t		sync_signal_error_count;
	uint64_t		gsp_post_event_count;
	uint64_t		gsp_post_event_short_count;
	uint64_t		gsp_post_event_bad_size_count;
	uint64_t		gsp_post_event_unhandled_count;
	uint64_t		gsp_post_event_nonstall_count;
	uint32_t		gsp_post_event_last_client;
	uint32_t		gsp_post_event_last_event;
	uint32_t		gsp_post_event_last_notify_index;
	uint32_t		gsp_post_event_last_data;
	uint32_t		gsp_post_event_last_status;
	uint32_t		gsp_post_event_last_data_size;
	uint64_t		gsp_nonstall_event_register_count;
	uint64_t		gsp_nonstall_event_register_error_count;
	uint32_t		gsp_nonstall_event_handle;
	uint32_t		gsp_nonstall_event_last_error;
	uint32_t		gsp_nonstall_leaf_mask[8];
	uint64_t		gsp_nonstall_intr_count;
	uint32_t		gsp_nonstall_intr_last_leaf;
	uint32_t		gsp_nonstall_intr_last_mask;
	uint32_t		gsp_nonstall_intr_last_top;
	uint64_t		rc_triggered_count;
	uint32_t		rc_last_engine_type;
	uint32_t		rc_last_chid;
	uint32_t		rc_last_except_level;
	uint32_t		rc_last_except_type;
	uint32_t		rc_last_scope;
	uint64_t		rc_last_mmu_fault_addr;
	uint32_t		rc_last_mmu_fault_type;
	uint32_t		rc_last_journal_size;
	uint64_t		bo_resv_wait_count;
	uint64_t		bo_resv_wait_error_count;
	uint64_t		vm_bind_wait_count;
	uint64_t		vm_bind_wait_error_count;
	uint64_t		cpu_prep_wait_count;
	uint64_t		cpu_prep_wait_error_count;
	uint64_t		exec_profile_token_wait_us;
	uint64_t		exec_profile_wait_sync_us;
	uint64_t		exec_profile_push_build_us;
	uint64_t		exec_profile_prepare_signal_us;
	uint64_t		exec_profile_attach_resv_us;
	uint64_t		exec_profile_flush_cpu_us;
	uint64_t		exec_profile_cache_flush_us;
	uint64_t		exec_profile_doorbell_us;
	uint64_t		exec_profile_poll_us;
	uint64_t		exec_profile_cleanup_us;
	uint64_t		exec_profile_poll_iters;
	uint64_t		exec_profile_pushes;
	uint64_t		exec_cpu_flush_seq;
	uint64_t		exec_profile_cpu_bind_scanned;
	uint64_t		exec_profile_cpu_bind_flushed;
	uint32_t		exec_trace_next;
	struct nvkm_drm_exec_trace exec_trace[NVKM_DRM_EXEC_TRACE_COUNT];
	uint64_t		vm_trace_seq;
	uint32_t		vm_trace_next;
	struct nvkm_drm_vm_trace vm_trace[NVKM_DRM_VM_TRACE_COUNT];

	/* Phase 5: GSP-RM resource manager root client. */
	struct nvkm_gsp_vmm	*gsp_vmm;
	struct nvkm_gsp_chan	*gsp_chan;
	struct nvkm_gsp_bar1	bar1;	/* host BAR1 vmm */
	struct nvkm_gsp_bar2	bar2;	/* host BAR2 vmm */

	/* Phase 5: usable VRAM range parsed from GspStaticConfigInfo
	 * fbRegionInfoParams (set in nvkm_gsp_get_static_info). */
	uint64_t		fb_usable_base;
	uint32_t		mthdbuf_size;	/* CE fault method buffer size from NV2080 ctrl */
	uint64_t		fb_usable_size;

	/* Phase 5: VRAM bump allocator. Hands out physical VRAM
	 * addresses for channel inst block, USERD, etc. */
	uint64_t		vram_bump_base;
	uint64_t		vram_bump_next;
	uint64_t		vram_bump_limit;
	struct drm_mm		vram_mm;
	struct lock		vram_lock;
	struct nvkm_vram_alloc_head vram_allocs;
	uint64_t		vram_alloc_bytes[NVKM_VRAM_KIND_COUNT];
	uint32_t		vram_alloc_count[NVKM_VRAM_KIND_COUNT];

	struct nvkm_gsp_gr_ctxbuf *gr_ctxbuf_mem;
	uint8_t			gr_ctxbuf_nr;
};

static __inline uint32_t
nvkm_rd32(struct nvkm_softc *sc, uint32_t offset)
{
	return (bus_read_4(sc->bar_res[0], offset));
}

static __inline void
nvkm_wr32(struct nvkm_softc *sc, uint32_t offset, uint32_t val)
{
	bus_write_4(sc->bar_res[0], offset, val);
}

/* Little-endian byte-buffer accessors used by ROM/VBIOS parsers. */
static __inline uint16_t
nvkm_le16(const uint8_t *p)
{
	return ((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static __inline uint32_t
nvkm_le32(const uint8_t *p)
{
	return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static __inline uint32_t
nvkm_le24(const uint8_t *p)
{
	return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16));
}

/* nvkm_bios.c */
struct sysctl_ctx_list;
struct sysctl_oid;
int	nvkm_bios_init(struct nvkm_softc *sc);
void	nvkm_bios_fini(struct nvkm_softc *sc);
void	nvkm_bios_publish_sysctl(struct nvkm_softc *sc,
	    struct sysctl_ctx_list *ctx, struct sysctl_oid *parent);

/* nvkm_fw.c */
int	nvkm_fw_init(struct nvkm_softc *sc);
void	nvkm_fw_fini(struct nvkm_softc *sc);

/* nvkm_sec2.c */
int	nvkm_sec2_init(struct nvkm_softc *sc);
void	nvkm_sec2_fini(struct nvkm_softc *sc);

/* nvkm_gsp.c -- GSP-Falcon engine (HS host for FwSec) */
int	nvkm_gsp_init(struct nvkm_softc *sc);
void	nvkm_gsp_fini(struct nvkm_softc *sc);

/* nvkm_mem.c -- DMA-coherent memory helpers */
int	nvkm_dmamem_alloc(struct nvkm_softc *sc, bus_size_t size,
	    bus_size_t alignment, struct nvkm_dmamem *out);
void	nvkm_dmamem_free(struct nvkm_softc *sc, struct nvkm_dmamem *mem);

/* nvkm_booter.c -- HS Falcon container parser + boot */
struct firmware;
int	nvkm_booter_parse(struct nvkm_softc *sc, const struct firmware *fw,
	    struct nvkm_booter_info *info);
int	nvkm_booter_load_and_start(struct nvkm_softc *sc);
void	nvkm_booter_release(struct nvkm_softc *sc);

/* nvkm_gsp_meta.c */
#define NVKM_GSP_FW_WPR_META_MAGIC    0xdc3aae21371a60b3ULL
#define NVKM_GSP_FW_WPR_META_REVISION 1ULL
#define NVKM_GSP_FW_WPR_META_SIZE     256

struct nvkm_gsp_wpr_meta {
	uint64_t magic;
	uint64_t revision;

	uint64_t sysmemAddrOfRadix3Elf;
	uint64_t sizeOfRadix3Elf;
	uint64_t sysmemAddrOfBootloader;
	uint64_t sizeOfBootloader;
	uint64_t bootloaderCodeOffset;
	uint64_t bootloaderDataOffset;
	uint64_t bootloaderManifestOffset;
	uint64_t sysmemAddrOfSignature;
	uint64_t sizeOfSignature;

	uint64_t gspFwRsvdStart;
	uint64_t nonWprHeapOffset;
	uint64_t nonWprHeapSize;
	uint64_t gspFwWprStart;
	uint64_t gspFwHeapOffset;
	uint64_t gspFwHeapSize;
	uint64_t gspFwOffset;
	uint64_t bootBinOffset;
	uint64_t frtsOffset;
	uint64_t frtsSize;
	uint64_t gspFwWprEnd;
	uint64_t fbSize;

	uint64_t vgaWorkspaceOffset;
	uint64_t vgaWorkspaceSize;
	uint64_t bootCount;

	uint64_t partitionRpcAddr;
	uint16_t partitionRpcRequestOffset;
	uint16_t partitionRpcReplyOffset;
	uint32_t elfCodeOffset;
	uint32_t elfDataOffset;
	uint32_t elfCodeSize;
	uint32_t elfDataSize;
	uint32_t lsUcodeVersion;

	uint8_t  gspFwHeapVfPartitionCount;
	uint8_t  flags;
	uint8_t  padding[2];
	uint32_t pmuReservedSize;

	uint64_t verified;
} __packed;

int	nvkm_gsp_meta_init(struct nvkm_softc *sc);
void	nvkm_gsp_meta_fini(struct nvkm_softc *sc);

/* nvkm_gsp_boot.c */
int	nvkm_gsp_boot_prepare(struct nvkm_softc *sc);
void	nvkm_gsp_boot_release(struct nvkm_softc *sc);

/* nvkm_gsp_libos.c -- libos init args, message queue, RM args */
int	nvkm_gsp_libos_prepare(struct nvkm_softc *sc);
int	nvkm_gsp_rpc_set_system_info(struct nvkm_softc *sc);
int	nvkm_gsp_rpc_set_registry(struct nvkm_softc *sc);

enum {
	NVKM_GSP_RPC_REPLY_NOWAIT = 0,
	NVKM_GSP_RPC_REPLY_NOSEQ  = 1,
	NVKM_GSP_RPC_REPLY_RECV   = 2,
};

/* Phase 2 RPC framework -- mirrors r570 (uses r535_rpc vtable). */
typedef int (*nvkm_gsp_msg_ntfy_func)(void *priv, uint32_t fn,
    void *repv, uint32_t repc);

void	nvkm_gsp_msg_ntfy_init(struct nvkm_softc *sc);
int	nvkm_gsp_msg_ntfy_add(struct nvkm_softc *sc, uint32_t fn,
	    nvkm_gsp_msg_ntfy_func handler, void *priv);

void   *nvkm_gsp_rpc_get(struct nvkm_softc *sc, uint32_t fn, uint32_t argc);
void   *nvkm_gsp_rpc_push(struct nvkm_softc *sc, void *params, int policy,
	    uint32_t repc);
void	nvkm_gsp_rpc_done(struct nvkm_softc *sc, void *params);

/* Convenience: alloc+send and wait for reply. Returns reply params ptr
 * (caller frees with nvkm_gsp_rpc_done) or NULL on error. */
static __inline void *
nvkm_gsp_rpc_rd(struct nvkm_softc *sc, uint32_t fn, uint32_t argc)
{
	void *p = nvkm_gsp_rpc_get(sc, fn, argc);
	if (p == NULL) return (NULL);
	return nvkm_gsp_rpc_push(sc, p, NVKM_GSP_RPC_REPLY_RECV, argc);
}

/* Convenience: send the prepared params buffer, no reply needed. */
static __inline int
nvkm_gsp_rpc_wr(struct nvkm_softc *sc, void *params, int policy)
{
	void *r = nvkm_gsp_rpc_push(sc, params, policy, 0);
	if (r == NULL) return (EIO);
	nvkm_gsp_rpc_done(sc, r);
	return (0);
}

int	nvkm_gsp_msg_dispatch_all(struct nvkm_softc *sc);

/* DRM driver registration. */
int	nvkm_drm_register(struct nvkm_softc *sc);
void	nvkm_drm_unregister(struct nvkm_softc *sc);
int	nvkm_gsp_get_static_info(struct nvkm_softc *sc);

/* sequencer event handler -- registered via nvkm_gsp_msg_ntfy_add */
int	nvkm_gsp_seq_msg_handler(void *priv, uint32_t fn,
	    void *repv, uint32_t repc);
void	nvkm_gsp_debug_publish_sysctl(struct nvkm_softc *sc, struct sysctl_ctx_list *ctx, struct sysctl_oid *parent);
void	nvkm_gsp_bar1_count_gva(struct nvkm_softc *sc, uint32_t *used,
	    uint32_t *total);
void	nvkm_gsp_vmm_snapshot(struct nvkm_gsp_vmm *vmm, uint32_t *pd0_count,
	    uint32_t *pt_count, uint64_t *valid_pte_count,
	    uint32_t *sparse_region_count);
void	nvkm_gsp_libos_release(struct nvkm_softc *sc);

/* nvkm_fwsec.c */
int	nvkm_fwsec_run_cmd(struct nvkm_softc *sc, uint32_t init_cmd,
	    uint64_t frts_addr, uint32_t frts_size);
#define	NVKM_FWSEC_CMD_FRTS	0x00000015u
#define	NVKM_FWSEC_CMD_SB	0x00000019u





/* === GMMU VER2 PDE/PTE encoding (Pascal+) ===
 * Refs:
 *   vmmgp100.c:237-251 gp100_vmm_pde -- PDE aperture
 *   tu102/dev_mmu.h:79-83 NV_MMU_PTE_APERTURE -- PTE aperture
 *   gp100/dev_mmu.h:120-151 NV_MMU_VER2_PTE -- bit layout
 */
#define NV_PT_ADDR_SHIFT          4

#define NV_PDE_APERTURE_INVALID   0ULL
#define NV_PDE_APERTURE_VRAM      (1ULL << 1)
#define NV_PDE_APERTURE_SYS_COH   (2ULL << 1)
#define NV_PDE_APERTURE_SYS_NCOH  (3ULL << 1)
#define NV_PDE_VOL                (1ULL << 3)

#define NV_PTE_VALID              (1ULL << 0)
#define NV_PTE_APERTURE_VRAM      0ULL
#define NV_PTE_APERTURE_PEER      (1ULL << 1)
#define NV_PTE_APERTURE_SYS_COH   (2ULL << 1)
#define NV_PTE_APERTURE_SYS_NCOH  (3ULL << 1)
#define NV_PTE_VOL                (1ULL << 3)
#define NV_PTE_PRIV               (1ULL << 5)
#define NV_PTE_RO                 (1ULL << 6)
#define NV_PTE_KIND_INVALID_TURING 0x07ULL

/* Pascal+ 16K-page 5-level GMMU (gp100_vmm_16, vmmgp100.c:603-608). */
#define NVKM_GMMU_PD3_SHIFT       47
#define NVKM_GMMU_PD2_SHIFT       38
#define NVKM_GMMU_PD1_SHIFT       29
#define NVKM_GMMU_PD0_SHIFT       21
#define NVKM_GMMU_SPT_SHIFT       12
#define NVKM_GMMU_PD2_ENTRIES    512
#define NVKM_GMMU_PD1_ENTRIES    512
#define NVKM_GMMU_PD0_ENTRIES    256
#define NVKM_GMMU_SPT_ENTRIES    512
#define NVKM_GMMU_PD0_ENTRY_SIZE  16   /* dual entry: small + big */
#define NVKM_GMMU_PT_PAGE_SIZE 0x1000

static __inline uint64_t
nvkm_pde_to_sysmem(uint64_t pt_paddr)
{
	return ((uint64_t)pt_paddr >> NV_PT_ADDR_SHIFT)
	     | NV_PDE_APERTURE_SYS_COH | NV_PDE_VOL;
}

static __inline uint64_t
nvkm_pte_to_sysmem(uint64_t paddr)
{
	return ((uint64_t)paddr >> NV_PT_ADDR_SHIFT)
	     | NV_PTE_APERTURE_SYS_COH | NV_PTE_VOL | NV_PTE_VALID;
}

static __inline uint64_t
nvkm_pde_to_vram(uint64_t pt_paddr)
{
	return ((uint64_t)pt_paddr >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM;
}

static __inline uint64_t
nvkm_pte_to_vram(uint64_t paddr)
{
	return ((uint64_t)paddr >> NV_PT_ADDR_SHIFT)
	     | NV_PTE_APERTURE_VRAM | NV_PTE_VALID;
}

static __inline uint64_t
nvkm_pte_to_vram_flags(uint64_t paddr, uint8_t priv, uint8_t ro)
{
	uint64_t pte = nvkm_pte_to_vram(paddr);

	if (priv)
		pte |= NV_PTE_PRIV;
	if (ro)
		pte |= NV_PTE_RO;
	return (pte);
}

static __inline uint64_t
nvkm_pte_to_sparse(void)
{
	return NV_PTE_VOL;
}

/* === VMM VA layout ===
 *
 * Server-managed: handed to GSP via COPY_SERVER_RESERVED_PDES. GSP
 *   maintains PDEs in this range; host MUST NOT modify any PT entry
 *   inside it. Matches nouveau SPLIT_VAS_SERVER_RM_MANAGED_VA_START/_SIZE
 *   (r535/nvrm/vmm.h:28-29) = [4 GiB, 4.5 GiB) -> PD1[8].
 *
 * Client-managed: host owns PT entries. We map sysmem BOs (push,
 *   gpfifo, sema, ...) here. 16 GiB-aligned so the first GVA lands
 *   on PD1[32], well clear of server's PD1[8].
 */
#define NVKM_VMM_RM_BASE         0x000100000000ULL   /* 4 GiB */
#define NVKM_VMM_RM_SIZE         0x000020000000ULL   /* 512 MiB */
#define NVKM_VMM_CLIENT_BASE     0x000400000000ULL   /* 16 GiB */
#define NVKM_VMM_CLIENT_SIZE     0x001000000000ULL   /* 64 GiB */


/* === BAR2 host-managed vmm (nvkm_gsp_bar2.c) === */
int	nvkm_gsp_bar2_init(struct nvkm_softc *sc);
void	nvkm_gsp_bar2_fini(struct nvkm_softc *sc);
int	nvkm_gsp_bar2_map_vram(struct nvkm_softc *sc, uint64_t bar2_gva,
	    uint64_t vram_paddr);
void	nvkm_gsp_bar2_wr32(struct nvkm_softc *sc, uint64_t bar2_gva,
	    uint32_t val);
uint32_t nvkm_gsp_bar2_rd32(struct nvkm_softc *sc, uint64_t bar2_gva);
void	nvkm_gsp_bar2_flush(struct nvkm_softc *sc);
void	nvkm_gsp_bar2_invalidate(struct nvkm_softc *sc);
int	nvkm_gsp_pramin_rd64(struct nvkm_softc *sc, uint64_t paddr, uint64_t *out);
void	nvkm_gsp_bar2_wr64(struct nvkm_softc *sc, uint64_t bar2_gva, uint64_t val);
uint64_t nvkm_gsp_bar2_rd64(struct nvkm_softc *sc, uint64_t bar2_gva);

#define BAR2_GVA_FLUSH		0x0ULL    /* nouveau flush slot */
#define BAR2_GVA_ALLOC_BASE	0x1000ULL


/* === BAR1 host-managed vmm (nvkm_gsp_bar1.c) === */
int	nvkm_gsp_bar1_init(struct nvkm_softc *sc);
void	nvkm_gsp_bar1_fini(struct nvkm_softc *sc);
void	nvkm_gsp_bar1_flush(struct nvkm_softc *sc);
void	nvkm_gsp_bar1_invalidate(struct nvkm_softc *sc);
int	nvkm_gsp_bar1_map_vram(struct nvkm_softc *sc, uint64_t bar1_gva,
	    uint64_t vram_paddr);
void	nvkm_gsp_bar1_wr32(struct nvkm_softc *sc, uint64_t bar1_gva,
	    uint32_t val);
uint32_t nvkm_gsp_bar1_rd32(struct nvkm_softc *sc, uint64_t bar1_gva);
void	nvkm_gsp_bar1_wr64(struct nvkm_softc *sc, uint64_t bar1_gva,
	    uint64_t val);
uint64_t nvkm_gsp_bar1_rd64(struct nvkm_softc *sc, uint64_t bar1_gva);

/* Allocate a 4 KiB VRAM page and map it into BAR1 at the next free
 * GVA. Fills *page with the VRAM paddr (for PDE/PTE encoding) and the
 * BAR1 GVA (for host reads/writes via bar1_{wr,rd}{32,64}). */
int	nvkm_gsp_bar1_alloc_page_kind(struct nvkm_softc *sc,
	    struct nvkm_bar1_page *page, enum nvkm_vram_kind kind, void *owner);
int	nvkm_gsp_bar1_alloc_page(struct nvkm_softc *sc,
	    struct nvkm_bar1_page *page);
void	nvkm_gsp_bar1_free_page(struct nvkm_softc *sc, struct nvkm_bar1_page *page);
void	nvkm_gsp_bar1_dump_pt(struct nvkm_softc *sc, uint64_t target_paddr, uint32_t target_off);

void	nvkm_drm_exec_complete_intr(struct nvkm_softc *sc);


/* BAR1 PDB control register (tu102_bar.c references 0xb80f40
 * for tu102_bar_bar1_fini). */
#define NV_BAR1_PDB_REG		0xb80f40u

#endif /* _NVKM_PRIV_H_ */
