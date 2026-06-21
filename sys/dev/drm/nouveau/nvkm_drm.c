/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM driver registration. Phase 3 step 1: just get the driver
 * registered so that /dev/dri/cardN and /dev/dri/renderDN appear.
 * No nouveau-specific ioctls yet — those come in subsequent steps.
 *
 * Pattern follows dfly amdgpu (drm/amd/amdgpu/amdgpu_drv.c) but
 * stripped to render-only essentials (no modeset, no vblank, no
 * connector/crtc/encoder).
 */

#include "nvkm_priv.h"
#include "nvkm_bo.h"
#include "nvkm_gsp_vmm.h"
#include "nvkm_ttm.h"

#include <drm/drmP.h>
#include <drm/drm_drv.h>
#include <drm/drm_prime.h>
#include <drm/drm_syncobj.h>
#include <drm/ttm/ttm_placement.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-fence-chain.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/reservation.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <machine/pmap.h>
#include <sys/tree.h>

/* Identify ourselves as "nouveau" so unmodified Mesa NVK accepts us.
 * Version >= 1.0.3.1 is what NVK requires (winsys/nouveau_device.c). */
#define NVKM_DRM_NAME		"nouveau"
#define NVKM_DRM_DESC		"nVidia Riva/TNT/GeForce (dfly GSP-RM)"
#define NVKM_DRM_DATE		"20260522"
#define NVKM_DRM_MAJOR		1
#define NVKM_DRM_MINOR		3
#define NVKM_DRM_PATCH		1
#define NVKM_DRM_MAX_CHANNELS	64
#define NVKM_DRM_MAX_CHAN_OBJS	16

/*
 * Minimal file_operations. On DragonFly the cdevsw layer (drm_cdevsw in
 * drm_drv.c) is what actually services /dev/dri reads/ioctls; the Linux
 * file_operations struct here is just for compat shape. Keep empty.
 */
static const struct file_operations nvkm_drm_fops = {
	.owner = THIS_MODULE,
};

/* Forward decl: ioctl table defined at end of file. */
static const struct drm_ioctl_desc nvkm_drm_ioctls[];
struct nvkm_drm_file;
struct nvkm_drm_job;
struct nvkm_drm_vm_binding;
TAILQ_HEAD(nvkm_drm_job_list, nvkm_drm_job);
static struct nvkm_softc *nvkm_drm_sc(struct drm_device *ddev);
static int nvkm_drm_open(struct drm_device *ddev, struct drm_file *file_priv);
static void nvkm_drm_postclose(struct drm_device *ddev,
    struct drm_file *file_priv);
static void nvkm_drm_lastclose(struct drm_device *ddev);
static unsigned int nvkm_drm_primary_client_count(struct drm_device *ddev);
static void nvkm_drm_exec_pending_cancel_channel(struct nvkm_softc *sc,
    struct nvkm_gsp_chan *chan, int error);
static void nvkm_drm_job_work(struct work_struct *work);
static void nvkm_drm_jobs_flush(struct nvkm_drm_file *nfile);
static void nvkm_drm_jobs_close(struct nvkm_drm_file *nfile);

/* NVIF ioctl is variable-size; encode with size=0 since dispatch
 * matches by NR only and the actual copy size comes from userspace. */
#define DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_ALLOC, struct drm_nouveau_channel_alloc)
#define DRM_IOCTL_NOUVEAU_CHANNEL_FREE \
    DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_FREE, struct drm_nouveau_channel_free)
#define DRM_IOCTL_NOUVEAU_GEM_NEW \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_NEW, struct drm_nouveau_gem_new)
#define DRM_IOCTL_NOUVEAU_GEM_INFO \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_INFO, struct drm_nouveau_gem_info)
#define DRM_IOCTL_NOUVEAU_GEM_CPU_PREP \
    DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_CPU_PREP, struct drm_nouveau_gem_cpu_prep)
#define DRM_IOCTL_NOUVEAU_GEM_CPU_FINI \
    DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_CPU_FINI, struct drm_nouveau_gem_cpu_fini)
#define DRM_IOCTL_NOUVEAU_VM_BIND \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_BIND, struct drm_nouveau_vm_bind)
#define DRM_IOCTL_NOUVEAU_EXEC \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_EXEC, struct drm_nouveau_exec)
#define DRM_IOCTL_NOUVEAU_NVIF \
    _IOC(IOC_INOUT, DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_NOUVEAU_NVIF, 0)

/* Raster scanout position for precise vblank timestamps (TU102 = gv100):
 * armed head timing at 0x6820xx, raster generator line at 0x616330/4. Lets
 * the drm vblank core place page-flip events on the correct vblank. */
static bool
nvkm_drm_get_scanout_position(struct drm_device *dev, unsigned int pipe,
    bool in_vblank_irq, int *vpos, int *hpos, ktime_t *stime, ktime_t *etime,
    const struct drm_display_mode *mode)
{
	struct nvkm_softc *sc = nvkm_drm_sc(dev);
	uint32_t rg = pipe * 0x800u;
	uint32_t st = 0x8000u + pipe * 0x400u;	/* armed head state */
	int vtotal, vblanks, vblanke, line;

	(void)in_vblank_irq;
	(void)mode;
	if (sc == NULL)
		return false;

	vtotal  = (nvkm_rd32(sc, 0x682064 + st) >> 16) & 0xffff;
	vblanke = (nvkm_rd32(sc, 0x68206c + st) >> 16) & 0xffff;
	vblanks = (nvkm_rd32(sc, 0x682070 + st) >> 16) & 0xffff;
	if (vtotal == 0)
		return false;

	if (stime != NULL)
		*stime = ktime_get();
	/* Reading vline (0x616330) latches hline (0x616334). */
	line  = nvkm_rd32(sc, 0x616330 + rg) & 0xffff;
	*hpos = nvkm_rd32(sc, 0x616334 + rg) & 0xffff;
	if (etime != NULL)
		*etime = ktime_get();

	/* nouveau calc(): raster line -> vpos relative to active scanout. */
	if (vblanke >= vblanks) {
		if (line >= vblanks)
			line -= vtotal;
	} else {
		if (line >= vblanks)
			line -= vtotal;
		line -= vblanke + 1;
	}
	*vpos = line;
	return true;
}

void
nvkm_proc_snapshot(pid_t *pid, char *comm, size_t comm_len)
{
	struct proc *proc;

	if (pid != NULL)
		*pid = -1;
	if (comm != NULL && comm_len != 0)
		strlcpy(comm, "<kernel>", comm_len);

	proc = curproc;
	if (proc == NULL)
		return;

	lwkt_gettoken_shared(&proc->p_token);
	if (pid != NULL)
		*pid = proc->p_pid;
	if (comm != NULL && comm_len != 0)
		strlcpy(comm, proc->p_comm, comm_len);
	lwkt_reltoken(&proc->p_token);
}

static struct nvkm_hotproc_slot *
nvkm_hotproc_find_slot_locked(struct nvkm_softc *sc, pid_t pid,
    const char *comm)
{
	struct nvkm_hotproc_slot *empty = NULL;

	for (uint32_t i = 0; i < NVKM_HOTPROC_OVERFLOW_SLOT; i++) {
		struct nvkm_hotproc_slot *slot = &sc->hotproc[i];

		if (!slot->active) {
			if (empty == NULL)
				empty = slot;
			continue;
		}
		if (slot->pid == pid && strcmp(slot->comm, comm) == 0)
			return (slot);
	}

	if (empty != NULL) {
		empty->active = true;
		empty->pid = pid;
		strlcpy(empty->comm, comm, sizeof(empty->comm));
		return (empty);
	}

	sc->hotproc[NVKM_HOTPROC_OVERFLOW_SLOT].active = true;
	sc->hotproc[NVKM_HOTPROC_OVERFLOW_SLOT].pid = -2;
	strlcpy(sc->hotproc[NVKM_HOTPROC_OVERFLOW_SLOT].comm, "<overflow>",
	    sizeof(sc->hotproc[NVKM_HOTPROC_OVERFLOW_SLOT].comm));
	return (&sc->hotproc[NVKM_HOTPROC_OVERFLOW_SLOT]);
}

void
nvkm_hotproc_record(struct nvkm_softc *sc, enum nvkm_hotproc_event event,
    uint32_t op_count)
{
	struct nvkm_hotproc_slot *slot;
	char comm[MAXCOMLEN + 1];
	pid_t pid;

	if (sc == NULL)
		return;

	nvkm_proc_snapshot(&pid, comm, sizeof(comm));

	spin_lock(&sc->hotproc_lock);
	slot = nvkm_hotproc_find_slot_locked(sc, pid, comm);
	switch (event) {
	case NVKM_HOTPROC_VM_BIND:
		slot->vm_bind_ioctl_count++;
		slot->vm_bind_op_count += op_count;
		break;
	case NVKM_HOTPROC_PRIME_HANDLE_TO_FD:
		slot->prime_handle_to_fd_count++;
		break;
	case NVKM_HOTPROC_GEM_NEW:
		slot->gem_new_count++;
		break;
	default:
		break;
	}
	spin_unlock(&sc->hotproc_lock);
}

static void
nvkm_hotproc_record_prime_handle_to_fd(struct nvkm_softc *sc, uint32_t handle)
{
	struct nvkm_hotproc_slot *slot;
	char comm[MAXCOMLEN + 1];
	pid_t pid;
	bool found = false;

	if (sc == NULL)
		return;

	nvkm_proc_snapshot(&pid, comm, sizeof(comm));

	spin_lock(&sc->hotproc_lock);
	slot = nvkm_hotproc_find_slot_locked(sc, pid, comm);
	slot->prime_handle_to_fd_count++;
	for (uint32_t i = 0; i < slot->prime_handle_count; i++) {
		if (slot->prime_handles[i] == handle) {
			found = true;
			break;
		}
	}
	if (found) {
		slot->prime_handle_repeat_count++;
	} else if (slot->prime_handle_count <
	    NVKM_HOTPROC_PRIME_HANDLE_SLOT_COUNT) {
		slot->prime_handles[slot->prime_handle_count++] = handle;
		slot->prime_handle_seen_count++;
	} else {
		slot->prime_handle_overflow_count++;
	}
	spin_unlock(&sc->hotproc_lock);
}

void
nvkm_hotproc_snapshot(struct nvkm_softc *sc, struct nvkm_hotproc_slot *dst,
    uint32_t dst_count)
{
	uint32_t count;

	if (sc == NULL || dst == NULL || dst_count == 0)
		return;

	count = dst_count;
	if (count > NVKM_HOTPROC_SLOT_COUNT)
		count = NVKM_HOTPROC_SLOT_COUNT;

	spin_lock(&sc->hotproc_lock);
	memcpy(dst, sc->hotproc, sizeof(sc->hotproc[0]) * count);
	spin_unlock(&sc->hotproc_lock);
}

static int
nvkm_drm_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file_priv,
    uint32_t handle, uint32_t flags, int *prime_fd)
{
	struct nvkm_softc *sc = nvkm_drm_sc(dev);
	int ret;

	if (sc != NULL) {
		sc->prime_handle_to_fd_count++;
		nvkm_hotproc_record_prime_handle_to_fd(sc, handle);
	}
	ret = drm_gem_prime_handle_to_fd(dev, file_priv, handle, flags,
	    prime_fd);
	if (ret != 0 && sc != NULL)
		sc->prime_handle_to_fd_error_count++;
	return (ret);
}

static int
nvkm_drm_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
    int prime_fd, uint32_t *handle)
{
	struct nvkm_softc *sc = nvkm_drm_sc(dev);
	int ret;

	if (sc != NULL)
		sc->prime_fd_to_handle_count++;
	ret = drm_gem_prime_fd_to_handle(dev, file_priv, prime_fd, handle);
	if (ret != 0 && sc != NULL)
		sc->prime_fd_to_handle_error_count++;
	return (ret);
}

static struct dma_buf *
nvkm_drm_gem_prime_export(struct drm_device *dev, struct drm_gem_object *obj,
    int flags)
{
	struct nvkm_softc *sc = nvkm_drm_sc(dev);
	struct nvkm_bo *bo = to_nvkm_bo(obj);
	struct dma_buf *dmabuf;

	if (bo->no_share)
		return (ERR_PTR(-EPERM));

	dmabuf = drm_gem_prime_export(dev, obj, flags);
	if (!IS_ERR(dmabuf) && sc != NULL)
		sc->prime_dma_buf_export_count++;

	return (dmabuf);
}

static struct reservation_object *
nvkm_drm_gem_prime_res_obj(struct drm_gem_object *obj)
{
	return (nvkm_bo_resv(to_nvkm_bo(obj)));
}

static struct drm_driver nvkm_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ |
	    DRIVER_PRIME | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops    = &nvkm_drm_fops,
	.get_scanout_position = nvkm_drm_get_scanout_position,
	.get_vblank_timestamp = drm_calc_vbltimestamp_from_scanoutpos,
	.ioctls  = nvkm_drm_ioctls,
	.num_ioctls = 0x45 /* sparse: max index DRM_NOUVEAU_GEM_INFO(0x44)+1 */,
	.name    = NVKM_DRM_NAME,
	.desc    = NVKM_DRM_DESC,
	.date    = NVKM_DRM_DATE,
	.major   = NVKM_DRM_MAJOR,
	.minor   = NVKM_DRM_MINOR,
	.patchlevel = NVKM_DRM_PATCH,
	.open = nvkm_drm_open,
	.postclose = nvkm_drm_postclose,
	.lastclose = nvkm_drm_lastclose,
	.gem_vm_ops = &nvkm_gem_pager_ops,
	.gem_free_object_unlocked = nvkm_bo_gem_free,
	/*
	 * PRIME self-import only (wlroots requires DRM_PRIME_CAP_IMPORT even
	 * for software rendering): same-device dmabuf round-trips resolve to
	 * the original GEM object in drm_gem_prime_import_dev().  Cross-device
	 * sharing needs gem_prime_get_sg_table/import_sg_table, which are
	 * deliberately absent -- those paths fail with an errno, not a crash.
	 */
	.prime_handle_to_fd = nvkm_drm_prime_handle_to_fd,
	.prime_fd_to_handle = nvkm_drm_prime_fd_to_handle,
	.gem_prime_export = nvkm_drm_gem_prime_export,
	.gem_prime_res_obj = nvkm_drm_gem_prime_res_obj,
	.gem_prime_import = drm_gem_prime_import,
	.dumb_create = nvkm_bo_dumb_create,
	.dumb_map_offset = nvkm_bo_dumb_map_offset,
	.dumb_destroy = nvkm_bo_dumb_destroy,
};

static uint64_t
nvkm_drm_profile_now_us(struct nvkm_softc *sc)
{
	if (sc == NULL || sc->sync_diag_enable == 0)
		return (0);
	return ((uint64_t)ktime_to_us(ktime_get()));
}

static void
nvkm_drm_profile_add_us(struct nvkm_softc *sc, uint64_t *total,
    uint64_t start_us)
{
	uint64_t end_us;

	if (sc == NULL || sc->sync_diag_enable == 0 || start_us == 0)
		return;

	end_us = nvkm_drm_profile_now_us(sc);
	if (end_us >= start_us)
		*total += end_us - start_us;
}

struct nvkm_drm_vm_binding {
	LIST_ENTRY(nvkm_drm_vm_binding) link;
	LIST_ENTRY(nvkm_drm_vm_binding) bo_link;
	LIST_ENTRY(nvkm_drm_vm_binding) validate_link;
	RB_ENTRY(nvkm_drm_vm_binding) rb_link;
	uint64_t addr;
	uint64_t size;
	uint64_t bo_offset;
	struct nvkm_drm_file *owner;
	struct drm_gem_object *obj;
	uint8_t pte_kind;
	uint8_t page_shift;
	bool pte_installed;
	bool bo_pinned;
	bool bo_no_evict_pinned;
	bool bo_linked;
	bool validate_linked;
};

struct nvkm_drm_vm_bind_segment {
	uint64_t addr;
	uint64_t size;
	uint64_t bo_offset;
	uint64_t paddr;
	vm_paddr_t *sysmem_paddrs;
	uint32_t sysmem_page_count;
	uint8_t page_shift;
};

struct nvkm_drm_vm_bind_segment_plan {
	struct nvkm_drm_vm_bind_segment *segments;
	uint32_t count;
	uint32_t capacity;
};

#define NVKM_DRM_VM_DIRTY_RANGE_INLINE	16

struct nvkm_drm_vm_dirty_range {
	uint64_t start;
	uint64_t end;
};

struct nvkm_drm_vm_dirty_set {
	struct nvkm_drm_vm_dirty_range ranges[NVKM_DRM_VM_DIRTY_RANGE_INLINE];
	uint64_t first;
	uint64_t last;
	uint64_t range_count;
	uint64_t pages;
	uint32_t merged_count;
	bool dirty;
	bool overflow;
};

struct nvkm_drm_vm_bind_2m_split {
	uint64_t addr;
	struct nvkm_gsp_vmm_user_pt *pt;
	bool committed;
};

struct nvkm_drm_vm_bind_2m_split_plan {
	struct nvkm_drm_vm_bind_2m_split *splits;
	uint32_t count;
	uint32_t capacity;
};
LIST_HEAD(nvkm_drm_vm_binding_list, nvkm_drm_vm_binding);

struct nvkm_drm_vm_materialize_entry {
	struct nvkm_drm_vm_binding *binding;
	struct nvkm_drm_vm_bind_segment_plan segments;
	struct nvkm_drm_vm_bind_2m_split_plan split_plan;
	struct nvkm_drm_vm_binding_list new_bindings;
	bool is_2m;
	bool committed;
};

struct nvkm_drm_vm_materialize_plan {
	struct nvkm_drm_vm_materialize_entry *entries;
	uint32_t count;
};

struct nvkm_drm_vm_remove_plan {
	struct nvkm_drm_vm_binding_list tail_bindings;
	struct nvkm_drm_vm_materialize_plan materialize_plan;
	uint64_t addr;
	uint64_t size;
	bool clear_empty_range;
	bool preserve_target_pts;
	uint8_t preserve_page_shift;
	bool materialize_full_cover;
	uint32_t clear_action;
};

struct nvkm_drm_vm_clear_plan {
	struct nvkm_drm_vm_remove_plan remove_plan;
	struct nvkm_gsp_vmm_sparse_unmap_plan *sparse_clear_plan;
	uint64_t addr;
	uint64_t size;
	uint32_t action;
};

struct nvkm_drm_vm_segment_remove_plan {
	struct nvkm_drm_vm_binding_list tail_bindings;
	struct nvkm_drm_vm_materialize_plan materialize_plan;
	const struct nvkm_drm_vm_bind_segment *segments;
	uint32_t segment_count;
	uint64_t addr;
	uint64_t size;
	uint32_t clear_action;
};

struct nvkm_drm_vm_valid_map_plan {
	struct nvkm_drm_vm_binding_list new_bindings;
	struct nvkm_drm_vm_binding_list replace_tails;
	struct nvkm_drm_vm_materialize_plan materialize_plan;
	struct nvkm_gsp_vmm_sparse_unmap_plan *sparse_clear_plan;
	bool committed;
};

struct nvkm_drm_vm_sparse_map_plan {
	struct nvkm_drm_vm_bind_segment_plan segment_plan;
	struct nvkm_drm_vm_segment_remove_plan target_clear_plan;
	struct nvkm_gsp_vmm_sparse_region **sparse_regions;
	struct nvkm_gsp_vmm_sparse_unmap_plan *sparse_clear_plan;
	uint64_t addr;
	uint64_t size;
};
RB_HEAD(nvkm_drm_vm_binding_tree, nvkm_drm_vm_binding);

static int
nvkm_drm_vm_binding_tree_cmp(struct nvkm_drm_vm_binding *a,
    struct nvkm_drm_vm_binding *b)
{
	if (a->addr < b->addr)
		return (-1);
	if (a->addr > b->addr)
		return (1);
	return (0);
}
RB_PROTOTYPE_STATIC(nvkm_drm_vm_binding_tree, nvkm_drm_vm_binding,
    rb_link, nvkm_drm_vm_binding_tree_cmp);
RB_GENERATE_STATIC(nvkm_drm_vm_binding_tree, nvkm_drm_vm_binding,
    rb_link, nvkm_drm_vm_binding_tree_cmp);

static void nvkm_drm_vm_binding_insert_sorted(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_binding *binding);
static int nvkm_drm_vm_binding_pin(struct nvkm_drm_vm_binding *binding);
static void nvkm_drm_vm_binding_free(struct nvkm_drm_vm_binding *binding);
static void nvkm_drm_vm_binding_unlink_retire(
    struct nvkm_drm_vm_binding *binding,
    struct nvkm_drm_vm_binding_list *retired_bindings);
static void nvkm_drm_vm_binding_validate_clear(
    struct nvkm_drm_vm_binding *binding);
static void nvkm_drm_vm_binding_validate_mark(
    struct nvkm_drm_vm_binding *binding);
static void nvkm_drm_vm_bindings_free_prepared(
    struct nvkm_drm_vm_binding_list *bindings);
static void nvkm_drm_vm_bind_note_map_shape(struct nvkm_softc *sc,
	    uint32_t flags, uint64_t size);
static void nvkm_drm_vm_bind_note_clear_shape(struct nvkm_softc *sc,
	    const struct nvkm_bo *bo, uint8_t pte_kind, uint8_t page_shift,
	    uint64_t size);
static int nvkm_drm_vm_bind_segment_check_writer_args(
    const struct nvkm_drm_vm_bind_segment *segment, const struct nvkm_bo *bo);

/*
 * VM_BIND retire cleanup.
 *
 * Ownership:
 *   Owned by one VM_BIND job until the job signals its done fence.  After
 *   that point, ownership moves to system_unbound_wq and the job drops the
 *   pointer.  sc is borrowed for debug counters only and does not affect
 *   cleanup ownership.
 *
 * Lifetime:
 *   Contains old live bindings removed from the GPUVA tracker by a VM_BIND
 *   operation.  The bindings stay alive until the done fence is observable.
 *
 * Threading:
 *   The job worker only moves bindings onto this list while holding the
 *   drm_file VM token.  The cleanup worker later owns the detached list
 *   exclusively and may sleep while GEM/TTM waits reservation fences.
 */
struct nvkm_drm_vm_bind_retire {
	struct work_struct work;
	struct nvkm_drm_vm_binding_list bindings;
	struct nvkm_softc *sc;
};

struct nvkm_drm_chan_obj {
	uint32_t handle;
	uint32_t oclass;
	uint64_t nvif_object;	/* NVIF object id (nvif_ioctl_v0.object) for DEL */
	struct nvkm_gsp_object object;
};

struct nvkm_drm_chan {
	LIST_ENTRY(nvkm_drm_chan) link;
	uint32_t id;
	uint32_t engine_type;
	struct nvkm_gsp_chan *chan;
	struct nvkm_drm_chan_obj obj[NVKM_DRM_MAX_CHAN_OBJS];
};
LIST_HEAD(nvkm_drm_chan_list, nvkm_drm_chan);

struct nvkm_drm_file {
	/*
	 * Per-file VM lifetime anchor.
	 *
	 * Ownership:
	 *   The drm_file owns the base reference from open to postclose.  Future
	 *   BO reverse-map snapshots may take temporary references while they
	 *   prepare TTM rebind work for this VM.
	 *
	 * Lifetime:
	 *   Protects the nvkm_drm_file object, its per-file VMM pointer, tokens,
	 *   and reservation object.  Postclose removes live jobs/channels/bindings
	 *   before dropping the base reference; final teardown runs only after all
	 *   temporary references are gone.
	 *
	 * Threading:
	 *   The kref itself is atomic.  It does not serialize VM mutations; users
	 *   still need vm_token/job_token/gsp_tok for the protected state.
	 */
	struct kref refcount;
	struct nvkm_softc *sc;

	/* Per-file GPU address space. Each drm_file owns its own VMM (RM
	 * vaspace + page-table tree) so concurrent NVK processes cannot
	 * collide on GPU VAs. NULL only if vmm_ctor failed during open. */
	struct nvkm_gsp_vmm *vmm;
	struct nvkm_drm_vm_binding_list vm_bindings;
	/* Live GPUVA bindings whose BO was moved away from its preferred TTM
	 * placement.  EXEC validates these BOs through DragonFly TTM before
	 * ringing the doorbell; the list only tracks VM state. */
	struct nvkm_drm_vm_binding_list vm_validate_bindings;
	struct nvkm_drm_vm_binding_tree vm_binding_tree;
	uint64_t vm_bindings_max_end;
	struct nvkm_drm_chan_list channels;
	/* Serializes VM_BIND page-table mutation against other remaps. EXEC
	 * jobs take the same token before ringing the doorbell so they never
	 * observe a half-written page-table update, but VM_BIND does not drain
	 * already submitted GPU work. Nouveau relies on userspace fences for
	 * that ordering, and synchronous VM_BIND only waits for its own bind job.
	 * Order is always vm_token -> gsp_tok. */
	struct lwkt_token vm_token;

		/* Private GPUVM reservation object.
		 *
		 * Linux nouveau publishes EXEC fences through drm_gpuvm_exec using
		 * dma_resv usage metadata. DragonFly's current reservation_object has no
		 * usage classes, so VM-wide fences live here instead of being exported
		 * as per-BO dma-buf content fences.  VM_BIND bookkeeping fences may also
		 * be visible here through no-share BO aliases, so TTM move must not wait
		 * on this object when rebinding live GPUVA PTEs.
		 */
		struct reservation_object vm_resv;

		/* EXEC-only GPUVM reservation object.
		 *
		 * Ownership:
		 *   Owned by this drm_file and updated only by the EXEC submit path.
		 *
		 * Lifetime:
		 *   Initialized with the file and destroyed after all channels, jobs, and
		 *   reverse-map snapshots have released their references.
		 *
		 * Threading:
		 *   TTM live-bound move waits this object before rewriting PTEs so it
		 *   only orders against already submitted GPU work.  VM_BIND job fences
		 *   are deliberately excluded to avoid a VM_BIND job waiting for its own
		 *   completion fence during TTM eviction or validation.
		 */
		struct reservation_object vm_exec_resv;

	/*
	 * Serializes the userspace-visible submit point for EXEC and VM_BIND.
	 *
	 * Ownership:
	 *   Owned by the drm_file; callers borrow it only for the duration of a
	 *   single ioctl submit sequence.
	 *
	 * Lifetime:
	 *   Initialized at open and valid until postclose has drained and
	 *   cancelled this file's job queue.
	 *
	 * Threading:
	 *   Guards the order in which a job publishes reservation fences, publishes
	 *   out-sync fences, and becomes visible to the per-file job queue.  Worker
	 *   execution does not take this lock, matching Linux nouveau's scheduler
	 *   submit mutex rather than turning execution itself into a global lock.
	 */
	struct lock job_submit_lock;

	/* Ordered per-file GPU job queue.  IOCTL handlers copy user data and
	 * publish output fences before enqueueing; the worker waits dependency
	 * fences and performs all EXEC work plus synchronous/asynchronous VM_BIND
	 * work.  A synchronous VM_BIND ioctl waits for this job to complete; EXEC
	 * ioctls never execute inline. */
	struct work_struct job_work;
	struct lwkt_token job_token;
	struct nvkm_drm_job_list job_queue;
	bool job_work_queued;
	bool job_closing;
	uint64_t job_epoch;
};

struct drm_nouveau_exec_push {
	uint64_t va;
	uint32_t va_len;
	uint32_t flags;
};

static volatile u_int nvkm_drm_next_channel = 1;

/* Per-file RM client handle allocator. Each drm_file's VMM needs a unique
 * client handle; child object handles are derived from it. Kept clear of the
 * kernel VMM (0xc1d00001) and golden VMM (0xc1d00002). Monotonic; reuse only
 * after 2^16 opens, by which point earlier files are long gone. */
static volatile uint32_t nvkm_drm_next_client_handle = 0xc1d10000u;

static struct nvkm_drm_file *
nvkm_drm_file_priv(struct drm_file *file_priv)
{
	return (file_priv->driver_priv);
}

/*
 * nvkm_drm_file_free()
 *
 * Ownership:
 *   Consumes the final nvkm_drm_file reference.  All live VM bindings,
 *   channels, and queued jobs must already have been removed by postclose.
 *
 * Lifetime:
 *   Tears down the per-file VMM and reservation object only after no external
 *   snapshot can still dereference the file.  This is the lifetime boundary
 *   future TTM rebind snapshots will rely on.
 *
 * Threading:
 *   May run from postclose today.  Future callers must not hold vm_token,
 *   bo->vm_mapping_token, or gsp_tok when dropping the last reference, because
 *   VMM destruction takes gsp_tok internally.
 */
static void
nvkm_drm_file_free(struct kref *kref)
{
	struct nvkm_drm_file *nfile =
	    container_of(kref, struct nvkm_drm_file, refcount);
	struct nvkm_softc *sc = nfile->sc;

	if (nfile->vmm != NULL) {
		lwkt_gettoken(&sc->gsp_tok);
		nvkm_gsp_vmm_dtor(nfile->vmm);
		lwkt_reltoken(&sc->gsp_tok);
		kfree(nfile->vmm);
		nfile->vmm = NULL;
	}

	reservation_object_fini(&nfile->vm_exec_resv);
	reservation_object_fini(&nfile->vm_resv);
	kfree(nfile);
}

static void
nvkm_drm_file_get(struct nvkm_drm_file *nfile)
{
	kref_get(&nfile->refcount);
}

static void
nvkm_drm_file_put(struct nvkm_drm_file *nfile)
{
	if (nfile != NULL)
		kref_put(&nfile->refcount, nvkm_drm_file_free);
}

/* Exposed to nvkm_bo so a no_share BO can alias its fence-wait resv to this
 * file's VM-wide reservation set. */
struct reservation_object *
nvkm_drm_file_vm_resv(struct drm_file *file_priv)
{
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);

	if (nfile == NULL)
		return (NULL);
	return (&nfile->vm_resv);
}

/*
 * nvkm_drm_vm_range_end()
 *
 * Ownership:
 *   Borrows scalar VA range arguments and writes the exclusive end to the
 *   caller-owned output slot.
 *
 * Lifetime:
 *   The returned end is a pure value.  A false result means the range is empty
 *   or not representable as [addr, addr + size).
 *
 * Threading:
 *   Pure arithmetic helper; callers own any VM serialization needed for the
 *   objects whose ranges they are checking.
 */
static bool
nvkm_drm_vm_range_end(uint64_t addr, uint64_t size, uint64_t *end)
{
	if (size == 0 || addr > UINT64_MAX - size)
		return (false);
	*end = addr + size;
	return (true);
}

static bool
nvkm_drm_vm_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs)
{
	uint64_t ae, be;

	if (as == 0 || bs == 0)
		return (false);
	if (!nvkm_drm_vm_range_end(a, as, &ae) ||
	    !nvkm_drm_vm_range_end(b, bs, &be))
		return (true);
	return (a < be && b < ae);
}

static bool
nvkm_drm_gpu_va_fits_binding(const struct nvkm_drm_vm_binding *binding,
    uint64_t va, uint64_t size, uint64_t *binding_offset)
{
	uint64_t offset;

	if (va < binding->addr)
		return (false);
	offset = va - binding->addr;
	if (offset > binding->size || size > binding->size - offset)
		return (false);
	*binding_offset = offset;
	return (true);
}

static void
nvkm_drm_vm_binding_assert(const struct nvkm_drm_vm_binding *binding)
{
	KASSERT(binding->owner != NULL,
	    ("nvkm_drm: VM binding without owner"));
	KASSERT(binding->obj != NULL,
	    ("nvkm_drm: VM binding without GEM object"));
	KASSERT(binding->page_shift == NVKM_GMMU_SPT_SHIFT ||
	    binding->page_shift == NVKM_GMMU_LPT_SHIFT ||
	    binding->page_shift == NVKM_GMMU_PD0_SHIFT,
	    ("nvkm_drm: VM binding with invalid page shift"));
}

static struct nvkm_drm_vm_binding *
nvkm_drm_vm_binding_alloc(struct nvkm_drm_file *nfile, uint64_t addr,
    uint64_t size, struct drm_gem_object *obj, uint64_t bo_offset,
    uint8_t pte_kind, uint8_t page_shift)
{
	struct nvkm_drm_vm_binding *binding;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (binding == NULL)
		return (NULL);

	binding->addr = addr;
	binding->size = size;
	binding->bo_offset = bo_offset;
	binding->owner = nfile;
	binding->obj = obj;
	binding->pte_kind = pte_kind;
	binding->page_shift = page_shift;
	binding->pte_installed = true;
	return (binding);
}

/*
 * nvkm_drm_vm_binding_tree_lower_bound()
 *
 * Ownership:
 *   Borrows nfile and returns a borrowed live binding pointer.  It does not
 *   acquire or release GEM, BO, or VMM ownership.
 *
 * Lifetime:
 *   The returned binding is valid only while the caller keeps the VM token and
 *   does not unlink the binding from nfile's live tracker.
 *
 * Threading:
 *   Requires nfile->vm_token.  The rb-tree is a VM-local lookup index and has
 *   no independent locking.
 */
static struct nvkm_drm_vm_binding *
nvkm_drm_vm_binding_tree_lower_bound(struct nvkm_drm_file *nfile,
    uint64_t addr)
{
	struct nvkm_drm_vm_binding *binding;
	struct nvkm_drm_vm_binding *best = NULL;

	binding = RB_ROOT(&nfile->vm_binding_tree);
	while (binding != NULL) {
		if (binding->addr < addr) {
			binding = RB_RIGHT(binding, rb_link);
		} else {
			best = binding;
			binding = RB_LEFT(binding, rb_link);
		}
	}
	return (best);
}

/*
 * nvkm_drm_vm_binding_first_overlap()
 *
 * Ownership:
 *   Borrows nfile and returns a borrowed live binding pointer.  The caller
 *   keeps ownership of the query range.
 *
 * Lifetime:
 *   The pointer is stable only while the caller holds nfile->vm_token and does
 *   not unlink that binding.  Use next_overlap before mutating the current
 *   binding in a destructive walk.
 *
 * Threading:
 *   Requires nfile->vm_token.  This is the VA interval-manager lookup path for
 *   VM_BIND remap planning and replaces whole-list overlap scans.
 */
static struct nvkm_drm_vm_binding *
nvkm_drm_vm_binding_first_overlap(struct nvkm_drm_file *nfile,
    uint64_t addr, uint64_t size)
{
	struct nvkm_drm_vm_binding *binding, *prev;
	uint64_t end;

	if (size == 0 || addr >= nfile->vm_bindings_max_end)
		return (NULL);
	if (!nvkm_drm_vm_range_end(addr, size, &end))
		end = UINT64_MAX;

	binding = nvkm_drm_vm_binding_tree_lower_bound(nfile, addr);
	if (binding != NULL)
		prev = nvkm_drm_vm_binding_tree_RB_PREV(binding);
	else
		prev = nvkm_drm_vm_binding_tree_RB_MINMAX(
		    &nfile->vm_binding_tree, 1);

	if (prev != NULL &&
	    nvkm_drm_vm_ranges_overlap(addr, size, prev->addr, prev->size))
		return (prev);

	while (binding != NULL && binding->addr < end) {
		if (nvkm_drm_vm_ranges_overlap(addr, size, binding->addr,
		    binding->size))
			return (binding);
		binding = nvkm_drm_vm_binding_tree_RB_NEXT(binding);
	}
	return (NULL);
}

static struct nvkm_drm_vm_binding *
nvkm_drm_vm_binding_next_overlap(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_binding *binding, uint64_t addr, uint64_t size)
{
	uint64_t end;

	if (!nvkm_drm_vm_range_end(addr, size, &end))
		end = UINT64_MAX;
	binding = nvkm_drm_vm_binding_tree_RB_NEXT(binding);
	while (binding != NULL && binding->addr < end) {
		if (nvkm_drm_vm_ranges_overlap(addr, size, binding->addr,
		    binding->size))
			return (binding);
		binding = nvkm_drm_vm_binding_tree_RB_NEXT(binding);
	}
	return (NULL);
}

static void
nvkm_drm_vm_binding_tree_insert(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_drm_vm_binding *conflict;

	conflict = nvkm_drm_vm_binding_first_overlap(nfile, binding->addr,
	    binding->size);
	KASSERT(conflict == NULL,
	    ("nvkm_drm: overlapping VM binding insert addr=0x%016jx size=0x%016jx old=0x%016jx+0x%016jx",
	    (uintmax_t)binding->addr, (uintmax_t)binding->size,
	    (uintmax_t)conflict->addr, (uintmax_t)conflict->size));
	conflict = nvkm_drm_vm_binding_tree_RB_INSERT(
	    &nfile->vm_binding_tree, binding);
	KASSERT(conflict == NULL,
	    ("nvkm_drm: duplicate VM binding insert addr=0x%016jx",
	    (uintmax_t)binding->addr));
}

static void
nvkm_drm_vm_binding_tree_remove(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_drm_vm_binding *removed;

	removed = nvkm_drm_vm_binding_tree_RB_REMOVE(
	    &nfile->vm_binding_tree, binding);
	KASSERT(removed == binding,
	    ("nvkm_drm: VM binding tree remove missed addr=0x%016jx",
	    (uintmax_t)binding->addr));
}

/*
 * nvkm_drm_vm_binding_bo_attach()
 *
 * Ownership:
 *   Borrows one live VM binding and links it into the owning BO's reverse GPUVA
 *   list.  The binding's existing GEM reference and VM_BIND pin keep the BO
 *   alive and immobile; this helper does not take another reference.
 *
 * Lifetime:
 *   The BO reverse link exists only while the binding is present in the live
 *   per-file VM tracker.  Retired bindings keep their GEM/pin ownership until
 *   the done fence is visible, but they are no longer live GPUVA mappings and
 *   must already be detached from this list.
 *
 * Threading:
 *   Called while nfile->vm_token serializes the VM tracker.  The BO-local
 *   token serializes the reverse list across different drm_file VMs.
 */
static void
nvkm_drm_vm_binding_bo_attach(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	struct nvkm_softc *sc = binding->obj->dev->dev_private;

	KASSERT(!binding->bo_linked,
	    ("nvkm_drm: double BO reverse attach addr=0x%016jx",
	    (uintmax_t)binding->addr));
	lwkt_gettoken(&bo->vm_mapping_token);
	LIST_INSERT_HEAD(&bo->vm_mappings, binding, bo_link);
	bo->vm_mapping_count++;
	binding->bo_linked = true;
	lwkt_reltoken(&bo->vm_mapping_token);

	sc->vm_bind_bo_reverse_link_count++;
	sc->vm_bind_bo_reverse_live_count++;
	if (sc->vm_bind_bo_reverse_max_live_count <
	    sc->vm_bind_bo_reverse_live_count)
		sc->vm_bind_bo_reverse_max_live_count =
		    sc->vm_bind_bo_reverse_live_count;
}

/*
 * nvkm_drm_vm_binding_bo_detach()
 *
 * Ownership:
 *   Removes a live VM binding from the owning BO's reverse GPUVA list.  GEM
 *   reference and VM_BIND pin ownership stay with the binding and are released
 *   later by nvkm_drm_vm_binding_free().
 *
 * Lifetime:
 *   After detach, the binding may move to a retired list or be freed.  It must
 *   not be used for BO reverse lookup again unless it is reinserted as a live
 *   mapping.
 *
 * Threading:
 *   Called under nfile->vm_token for the VM-side mutation and under the BO
 *   token for the BO-side list mutation.
 */
static void
nvkm_drm_vm_binding_bo_detach(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	struct nvkm_softc *sc = binding->obj->dev->dev_private;

	if (!binding->bo_linked)
		return;

	nvkm_drm_vm_binding_validate_clear(binding);
	lwkt_gettoken(&bo->vm_mapping_token);
	KASSERT(bo->vm_mapping_count > 0,
	    ("nvkm_drm: BO reverse mapping count underflow"));
	LIST_REMOVE(binding, bo_link);
	bo->vm_mapping_count--;
	binding->bo_linked = false;
	lwkt_reltoken(&bo->vm_mapping_token);

	sc->vm_bind_bo_reverse_unlink_count++;
	if (sc->vm_bind_bo_reverse_live_count > 0)
		sc->vm_bind_bo_reverse_live_count--;
}

/*
 * nvkm_drm_vm_binding_validate_mark()
 *
 * Ownership:
 *   Borrows one live VM binding and links it into its drm_file validate list.
 *   The binding keeps owning its GEM reference and VM_BIND pin; this helper
 *   takes no additional TTM or GEM ownership.
 *
 * Lifetime:
 *   The mark exists only while the binding is live in the VM tracker.  It is
 *   cleared when TTM validates the BO back to preferred placement or when the
 *   mapping leaves the live tracker.
 *
 * Threading:
 *   Called while the owning nfile->vm_token serializes VM state.  It never
 *   takes a TTM reservation lock and never sleeps.
 */
static void
nvkm_drm_vm_binding_validate_mark(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_drm_file *nfile = binding->owner;
	struct nvkm_softc *sc = binding->obj->dev->dev_private;

	if (binding->validate_linked)
		return;
	LIST_INSERT_HEAD(&nfile->vm_validate_bindings, binding, validate_link);
	binding->validate_linked = true;
	sc->ttm_vm_validate_mark_count++;
	sc->ttm_vm_validate_live_count++;
	if (sc->ttm_vm_validate_max_live_count <
	    sc->ttm_vm_validate_live_count)
		sc->ttm_vm_validate_max_live_count =
		    sc->ttm_vm_validate_live_count;
}

/*
 * nvkm_drm_vm_binding_validate_clear()
 *
 * Ownership:
 *   Borrows one VM binding and removes any validate-list membership created
 *   by nvkm_drm_vm_binding_validate_mark().  It does not release the binding's
 *   GEM reference or VM_BIND pin.
 *
 * Lifetime:
 *   May be called repeatedly; only the first call after a mark mutates the
 *   list.  The binding must still be allocated.
 *
 * Threading:
 *   Called under nfile->vm_token or from paths that still exclusively own a
 *   detached binding.  It does not sleep and does not touch TTM state.
 */
static void
nvkm_drm_vm_binding_validate_clear(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_softc *sc;

	if (!binding->validate_linked)
		return;
	sc = binding->obj->dev->dev_private;
	LIST_REMOVE(binding, validate_link);
	binding->validate_linked = false;
	sc->ttm_vm_validate_clear_count++;
	if (sc->ttm_vm_validate_live_count > 0)
		sc->ttm_vm_validate_live_count--;
}

void
nvkm_drm_bo_vm_snapshot_init(struct nvkm_drm_bo_vm_snapshot *snapshot)
{
	snapshot->entries = NULL;
	snapshot->count = 0;
	snapshot->capacity = 0;
}

/*
 * nvkm_drm_bo_vm_snapshot_fini()
 *
 * Ownership:
 *   Consumes all owner/GEM references held by snapshot entries, then releases
 *   the caller-owned entry array.
 *
 * Lifetime:
 *   Must be called exactly once for every initialized snapshot after a
 *   successful or partially successful collect attempt.  After return the
 *   snapshot is empty and can be reused.
 *
 * Threading:
 *   Must not be called while holding bo->vm_mapping_token, nfile->vm_token, or
 *   gsp_tok.  Dropping the last owner or GEM reference can run object teardown.
 */
void
nvkm_drm_bo_vm_snapshot_fini(struct nvkm_drm_bo_vm_snapshot *snapshot)
{
	for (uint32_t i = 0; i < snapshot->count; i++) {
		drm_gem_object_put_unlocked(snapshot->entries[i].obj);
		nvkm_drm_file_put(snapshot->entries[i].owner);
	}
	kfree(snapshot->entries);
	snapshot->entries = NULL;
	snapshot->count = 0;
	snapshot->capacity = 0;
}

static int
nvkm_drm_bo_vm_snapshot_reserve(struct nvkm_drm_bo_vm_snapshot *snapshot,
    uint32_t required)
{
	struct nvkm_drm_bo_vm_snapshot_entry *entries;
	uint32_t capacity;

	if (required <= snapshot->capacity)
		return (0);

	capacity = snapshot->capacity != 0 ? snapshot->capacity : 4;
	while (capacity < required) {
		if (capacity > UINT32_MAX / 2)
			return (-ENOMEM);
		capacity *= 2;
	}

	entries = kmalloc_array(capacity, sizeof(*entries), GFP_KERNEL);
	if (entries == NULL)
		return (-ENOMEM);
	if (snapshot->entries != NULL) {
		memcpy(entries, snapshot->entries,
		    snapshot->count * sizeof(*entries));
		kfree(snapshot->entries);
	}
	snapshot->entries = entries;
	snapshot->capacity = capacity;
	return (0);
}

/*
 * nvkm_drm_bo_vm_snapshot_collect()
 *
 * Ownership:
 *   Borrows bo and every live reverse-map binding, then records an owned
 *   snapshot of each binding's VM owner, GEM object, and immutable mapping
 *   metadata.  The snapshot never stores live binding pointers.
 *
 * Lifetime:
 *   Owner and GEM references remain held until
 *   nvkm_drm_bo_vm_snapshot_fini().  A successful empty snapshot proves the BO
 *   had no live GPUVA mappings at the instant the BO token was held.
 *
 * Threading:
 *   Takes bo->vm_mapping_token only while copying metadata and taking refs.
 *   It must not take nfile->vm_token, because VM_BIND attach/detach uses
 *   vm_token -> bo token ordering.  Callers must release the snapshot outside
 *   the BO token.
 */
int
nvkm_drm_bo_vm_snapshot_collect(struct nvkm_bo *bo,
    struct nvkm_drm_bo_vm_snapshot *snapshot)
{
	struct nvkm_drm_vm_binding *binding;
	uint32_t required;
	int err;

	if (snapshot->count != 0)
		return (-EBUSY);

	for (;;) {
		lwkt_gettoken(&bo->vm_mapping_token);
		required = bo->vm_mapping_count;
		lwkt_reltoken(&bo->vm_mapping_token);

		err = nvkm_drm_bo_vm_snapshot_reserve(snapshot, required);
		if (err != 0)
			return (err);

		lwkt_gettoken(&bo->vm_mapping_token);
		if (bo->vm_mapping_count > snapshot->capacity) {
			lwkt_reltoken(&bo->vm_mapping_token);
			continue;
		}

		LIST_FOREACH(binding, &bo->vm_mappings, bo_link) {
			struct nvkm_drm_bo_vm_snapshot_entry *entry =
			    &snapshot->entries[snapshot->count];

			nvkm_drm_file_get(binding->owner);
			drm_gem_object_get(binding->obj);
			entry->owner = binding->owner;
			entry->obj = binding->obj;
			entry->addr = binding->addr;
			entry->size = binding->size;
			entry->bo_offset = binding->bo_offset;
			entry->pte_kind = binding->pte_kind;
			entry->page_shift = binding->page_shift;
			snapshot->count++;
		}
		lwkt_reltoken(&bo->vm_mapping_token);
		return (0);
	}
}

/*
 * nvkm_drm_bo_live_vm_mapping_count()
 *
 * Ownership:
 *   Borrows bo and returns a diagnostic live GPUVA count.  Internally it uses
 *   the same owned snapshot path as future TTM rebind preparation, then drops
 *   the refs before returning.
 *
 * Lifetime:
 *   The returned count is only a snapshot.  A non-zero value means this BO
 *   must not be moved without a full unmap/move/remap plan.
 *
 * Threading:
 *   Does not retain refs after return.  Snapshot finalization happens after
 *   bo->vm_mapping_token has been released.
 */
uint32_t
nvkm_drm_bo_live_vm_mapping_count(struct nvkm_bo *bo)
{
	struct nvkm_drm_bo_vm_snapshot snapshot;
	uint32_t count;

	nvkm_drm_bo_vm_snapshot_init(&snapshot);
	if (nvkm_drm_bo_vm_snapshot_collect(bo, &snapshot) != 0) {
		nvkm_drm_bo_vm_snapshot_fini(&snapshot);
		return (UINT32_MAX);
	}
	count = snapshot.count;
	nvkm_drm_bo_vm_snapshot_fini(&snapshot);
	return (count);
}

/*
 * nvkm_drm_vm_binding_rekey_tail()
 *
 * Ownership:
 *   Borrows a live binding and mutates its VA key in place.  GEM/BO ownership
 *   remains with the binding.
 *
 * Lifetime:
 *   The binding stays on the sorted list.  The new address is the old tail
 *   after a split, so list order is preserved; only the rb-tree key changes.
 *
 * Threading:
 *   Requires nfile->vm_token and must be used for every live binding addr
 *   mutation so the VA interval index stays isomorphic with the list.
 */
static void
nvkm_drm_vm_binding_rekey_tail(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_binding *binding, uint64_t addr, uint64_t size,
    uint64_t bo_offset)
{
	nvkm_drm_vm_binding_tree_remove(nfile, binding);
	binding->addr = addr;
	binding->size = size;
	binding->bo_offset = bo_offset;
	nvkm_drm_vm_binding_tree_insert(nfile, binding);
}

/*
 * nvkm_drm_vm_bindings_match_map_range()
 *
 * Ownership:
 *   Borrows the live binding tree and the MAP operation's GEM object.  It
 *   consumes no GEM, BO, VMM, or binding ownership.
 *
 * Lifetime:
 *   The returned bool is valid only for the current serialized VM_BIND check.
 *   Borrowed binding pointers are not retained past the call.
 *
 * Threading:
 *   Requires nfile->vm_token.  The helper walks the rb-tree interval index but
 *   does not write hardware PTEs or reservation state.
 */
static bool
nvkm_drm_vm_bindings_match_map_range(struct nvkm_drm_file *nfile,
    struct drm_gem_object *obj,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    uint8_t pte_kind)
{
	struct nvkm_drm_vm_binding *binding;

	for (uint32_t i = 0; i < segment_count; i++) {
		uint64_t cur = segments[i].addr;
		uint64_t end = segments[i].addr + segments[i].size;

		binding = nvkm_drm_vm_binding_first_overlap(nfile,
		    segments[i].addr, segments[i].size);
		while (cur < end) {
			uint64_t binding_end, covered_end, binding_bo_offset;
			uint64_t target_bo_offset;

			if (binding == NULL || binding->addr > cur)
				return (false);
			if (binding->obj != obj || binding->pte_kind != pte_kind)
				return (false);
			if (binding->page_shift < segments[i].page_shift)
				return (false);

			binding_end = binding->addr + binding->size;
			covered_end = binding_end < end ? binding_end : end;
			binding_bo_offset = binding->bo_offset + cur -
			    binding->addr;
			target_bo_offset = segments[i].bo_offset + cur -
			    segments[i].addr;
			if (binding_bo_offset != target_bo_offset)
				return (false);
			if (covered_end <= cur)
				return (false);

			cur = covered_end;
			if (cur < end)
				binding = nvkm_drm_vm_binding_next_overlap(nfile,
				    binding, segments[i].addr, segments[i].size);
		}
	}
	return (true);
}

/*
 * nvkm_drm_vm_bindings_match_exact_map_op()
 *
 * Ownership:
 *   Borrows the live VM binding tree and the MAP operation's GEM object.  It
 *   consumes no GEM, BO, VMM, or binding ownership and never prepares segment
 *   snapshots.
 *
 * Lifetime:
 *   The result is valid only while the caller keeps VM_BIND serialization and
 *   does not mutate the live binding tree.  Covered bindings remain borrowed
 *   and are not retained.
 *
 * Threading:
 *   Requires nfile->vm_token.  This is the prepare-stage fast exact-noop
 *   predicate used before BO pin/populate work; sparse-region absence must be
 *   checked separately so valid MAP over sparse reservation still runs the
 *   sparse-clear plan.
 */
static bool
nvkm_drm_vm_bindings_match_exact_map_op(struct nvkm_drm_file *nfile,
    struct drm_gem_object *obj, uint64_t addr, uint64_t size,
    uint64_t bo_offset, uint8_t pte_kind)
{
	struct nvkm_drm_vm_binding *binding;
	uint64_t cur, end;

	if (!nvkm_drm_vm_range_end(addr, size, &end))
		return (false);
	cur = addr;
	binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	while (cur < end) {
		uint64_t binding_end, covered_end, binding_delta;
		uint64_t target_delta, binding_bo_offset, target_bo_offset;

		if (binding == NULL || binding->addr > cur)
			return (false);
		if (binding->obj != obj || binding->pte_kind != pte_kind)
			return (false);
		if (!binding->pte_installed || !binding->bo_pinned)
			return (false);
		if (binding->page_shift != NVKM_GMMU_SPT_SHIFT &&
		    binding->page_shift != NVKM_GMMU_LPT_SHIFT &&
		    binding->page_shift != NVKM_GMMU_PD0_SHIFT)
			return (false);
		if (binding->addr > UINT64_MAX - binding->size)
			return (false);

		binding_end = binding->addr + binding->size;
		covered_end = binding_end < end ? binding_end : end;
		if (covered_end <= cur)
			return (false);
		binding_delta = cur - binding->addr;
		target_delta = cur - addr;
		if (binding->bo_offset > UINT64_MAX - binding_delta ||
		    bo_offset > UINT64_MAX - target_delta)
			return (false);
		binding_bo_offset = binding->bo_offset + binding_delta;
		target_bo_offset = bo_offset + target_delta;
		if (binding_bo_offset != target_bo_offset)
			return (false);

		cur = covered_end;
		if (cur < end)
			binding = nvkm_drm_vm_binding_next_overlap(nfile,
			    binding, addr, size);
	}
	return (true);
}

/*
 * nvkm_drm_vm_bind_segment_plan_*()
 *
 * Ownership:
 *   The plan owns a dynamically sized segment array.  Segments own no GEM,
 *   BO, or VMM references; they are value data for the current MAP op.
 *
 * Lifetime:
 *   The plan must be initialized before use and finalized on every MAP path.
 *   Prepared live bindings copy the segment fields before the plan is freed.
 *
 * Threading:
 *   Runs in VM_BIND prepare while the caller holds normal VM_BIND
 *   serialization.  Allocation may sleep, but no PTEs are written here.
 */
static void
nvkm_drm_vm_bind_segment_plan_init(
    struct nvkm_drm_vm_bind_segment_plan *plan)
{
	plan->segments = NULL;
	plan->count = 0;
	plan->capacity = 0;
}

static void
nvkm_drm_vm_bind_segment_plan_fini(
    struct nvkm_drm_vm_bind_segment_plan *plan)
{
	for (uint32_t i = 0; i < plan->count; i++)
		kfree(plan->segments[i].sysmem_paddrs);
	kfree(plan->segments);
	plan->segments = NULL;
	plan->count = 0;
	plan->capacity = 0;
}

static int
nvkm_drm_vm_bind_segment_plan_reserve(
    struct nvkm_drm_vm_bind_segment_plan *plan, uint32_t required)
{
	struct nvkm_drm_vm_bind_segment *segments;
	uint32_t capacity;

	if (required <= plan->capacity)
		return (0);

	capacity = plan->capacity != 0 ? plan->capacity : 4;
	while (capacity < required) {
		if (capacity > UINT32_MAX / 2)
			return (-ENOMEM);
		capacity *= 2;
	}

	segments = kmalloc_array(capacity, sizeof(*segments), GFP_KERNEL);
	if (segments == NULL)
		return (-ENOMEM);
	if (plan->segments != NULL) {
		memcpy(segments, plan->segments,
		    plan->count * sizeof(*segments));
		kfree(plan->segments);
	}
	plan->segments = segments;
	plan->capacity = capacity;
	return (0);
}

static int
nvkm_drm_vm_bind_segment_add(struct nvkm_drm_vm_bind_segment_plan *plan,
    uint64_t addr, uint64_t size, uint64_t bo_offset, uint64_t paddr,
    uint8_t page_shift)
{
	int err;

	KASSERT(size != 0, ("nvkm_drm: empty VM_BIND MAP segment"));
	err = nvkm_drm_vm_bind_segment_plan_reserve(plan, plan->count + 1);
	if (err != 0)
		return (err);

	plan->segments[plan->count].addr = addr;
	plan->segments[plan->count].size = size;
	plan->segments[plan->count].bo_offset = bo_offset;
	plan->segments[plan->count].paddr = paddr;
	plan->segments[plan->count].sysmem_paddrs = NULL;
	plan->segments[plan->count].sysmem_page_count = 0;
	plan->segments[plan->count].page_shift = page_shift;
	plan->count++;
	return (0);
}

/*
 * nvkm_drm_vm_bind_segment_add_sysmem()
 *
 * Ownership:
 *   Allocates a temporary physical-page snapshot owned by the segment plan.
 *   The live VM binding does not retain this array; it only records VA, BO,
 *   bo_offset, kind, and page_shift after commit succeeds.
 *
 * Lifetime:
 *   The paddr array is valid until nvkm_drm_vm_bind_segment_plan_fini().  It is
 *   consumed by the prepared-only VMM writer during the same VM_BIND operation.
 *
 * Threading:
 *   Called in VM_BIND prepare while the BO is VM_BIND pinned and, for TTM TT
 *   memory, already populated.  It does not write PTEs and may fail before the
 *   no-fail commit section starts.
 */
static int
nvkm_drm_vm_bind_segment_add_sysmem(
    struct nvkm_drm_vm_bind_segment_plan *plan, const struct nvkm_bo *bo,
    uint64_t addr, uint64_t size, uint64_t bo_offset, uint8_t page_shift)
{
	vm_paddr_t *paddrs;
	uint64_t page_size;
	uint64_t page_count64;
	uint32_t page_count;
	int err;

	KASSERT(size != 0, ("nvkm_drm: empty VM_BIND sysmem segment"));
	if (page_shift == NVKM_GMMU_SPT_SHIFT)
		page_size = NVKM_GMMU_PT_PAGE_SIZE;
	else if (page_shift == NVKM_GMMU_LPT_SHIFT)
		page_size = NVKM_GMMU_LPT_PAGE_SIZE;
	else if (page_shift == NVKM_GMMU_PD0_SHIFT)
		page_size = NVKM_GMMU_PD0_PAGE_SIZE;
	else
		return (-EINVAL);
	if (((addr | size | bo_offset) & (page_size - 1)) != 0)
		return (-EINVAL);
	page_count64 = size / NVKM_GMMU_PT_PAGE_SIZE;
	if (page_count64 == 0 || page_count64 > UINT32_MAX)
		return (-ENOMEM);
	page_count = (uint32_t)page_count64;

	paddrs = kmalloc_array(page_count, sizeof(*paddrs), GFP_KERNEL);
	if (paddrs == NULL)
		return (-ENOMEM);

	for (uint32_t i = 0; i < page_count; i++) {
		err = nvkm_bo_paddr_at(bo,
		    bo_offset + (uint64_t)i * NVKM_GMMU_PT_PAGE_SIZE,
		    &paddrs[i]);
		if (err != 0) {
			kfree(paddrs);
			return (-err);
		}
	}
	if (page_shift != NVKM_GMMU_SPT_SHIFT) {
		uint32_t pages_per_leaf =
		    (uint32_t)(page_size / NVKM_GMMU_PT_PAGE_SIZE);

		if ((page_count % pages_per_leaf) != 0 ||
		    ((uint64_t)paddrs[0] & (page_size - 1)) != 0) {
			kfree(paddrs);
			return (-EINVAL);
		}
		for (uint32_t i = 1; i < page_count; i++) {
			if ((uint64_t)paddrs[i] !=
			    (uint64_t)paddrs[0] +
			    (uint64_t)i * NVKM_GMMU_PT_PAGE_SIZE) {
				kfree(paddrs);
				return (-EINVAL);
			}
		}
	}

	err = nvkm_drm_vm_bind_segment_add(plan, addr, size, bo_offset,
	    paddrs[0], page_shift);
	if (err != 0) {
		kfree(paddrs);
		return (err);
	}
	plan->segments[plan->count - 1].sysmem_paddrs = paddrs;
	plan->segments[plan->count - 1].sysmem_page_count = page_count;
	return (0);
}

/*
 * nvkm_drm_vm_bind_segment_add_sysmem_mem()
 *
 * Ownership:
 *   Allocates a temporary physical-page snapshot for a caller-supplied TTM
 *   placement.  It borrows bo and mem and never changes BO reservation,
 *   placement, GEM references, or live VM mapping ownership.
 *
 * Lifetime:
 *   mem must remain valid for the prepare call.  The copied paddr array lives
 *   in plan until nvkm_drm_vm_bind_segment_plan_fini().  This lets TTM move
 *   code prepare PTEs for new_mem before bo->tbo.mem is published.
 *
 * Threading:
 *   Pure prepare helper.  It does not sleep except for allocation, does not
 *   reserve the BO, and does not write page tables.
 */
static int
nvkm_drm_vm_bind_segment_add_sysmem_mem(
    struct nvkm_drm_vm_bind_segment_plan *plan, const struct nvkm_bo *bo,
    const struct ttm_mem_reg *mem, uint64_t addr, uint64_t size,
    uint64_t bo_offset, uint8_t page_shift)
{
	vm_paddr_t *paddrs;
	uint64_t page_size;
	uint64_t page_count64;
	uint32_t page_count;
	int err;

	KASSERT(size != 0, ("nvkm_drm: empty TTM rebind sysmem segment"));
	if (page_shift == NVKM_GMMU_SPT_SHIFT)
		page_size = NVKM_GMMU_PT_PAGE_SIZE;
	else if (page_shift == NVKM_GMMU_LPT_SHIFT)
		page_size = NVKM_GMMU_LPT_PAGE_SIZE;
	else if (page_shift == NVKM_GMMU_PD0_SHIFT)
		page_size = NVKM_GMMU_PD0_PAGE_SIZE;
	else
		return (-EINVAL);
	if (((addr | size | bo_offset) & (page_size - 1)) != 0)
		return (-EINVAL);
	page_count64 = size / NVKM_GMMU_PT_PAGE_SIZE;
	if (page_count64 == 0 || page_count64 > UINT32_MAX)
		return (-ENOMEM);
	page_count = (uint32_t)page_count64;

	paddrs = kmalloc_array(page_count, sizeof(*paddrs), GFP_KERNEL);
	if (paddrs == NULL)
		return (-ENOMEM);

	for (uint32_t i = 0; i < page_count; i++) {
		err = nvkm_bo_paddr_at_mem(bo, mem,
		    bo_offset + (uint64_t)i * NVKM_GMMU_PT_PAGE_SIZE,
		    &paddrs[i]);
		if (err != 0) {
			kfree(paddrs);
			return (-err);
		}
	}
	if (page_shift != NVKM_GMMU_SPT_SHIFT) {
		uint32_t pages_per_leaf =
		    (uint32_t)(page_size / NVKM_GMMU_PT_PAGE_SIZE);

		if ((page_count % pages_per_leaf) != 0 ||
		    ((uint64_t)paddrs[0] & (page_size - 1)) != 0) {
			kfree(paddrs);
			return (-EINVAL);
		}
		for (uint32_t i = 1; i < page_count; i++) {
			if ((uint64_t)paddrs[i] !=
			    (uint64_t)paddrs[0] +
			    (uint64_t)i * NVKM_GMMU_PT_PAGE_SIZE) {
				kfree(paddrs);
				return (-EINVAL);
			}
		}
	}

	err = nvkm_drm_vm_bind_segment_add(plan, addr, size, bo_offset,
	    paddrs[0], page_shift);
	if (err != 0) {
		kfree(paddrs);
		return (err);
	}
	plan->segments[plan->count - 1].sysmem_paddrs = paddrs;
	plan->segments[plan->count - 1].sysmem_page_count = page_count;
	return (0);
}

/*
 * nvkm_drm_vm_bind_2m_split_plan_*()
 *
 * Ownership:
 *   Owns prepared, unlinked child PTs returned by the VMM prepare helper.
 *   Once a split is committed, ownership transfers to vmm and the plan drops
 *   its pointer.
 *
 * Lifetime:
 *   The plan spans the prepare and commit halves of one 2 MiB materialize.
 *   Fini releases only PTs that were prepared but never committed.
 *
 * Threading:
 *   Used while nfile->vm_token serializes the VM.  Prepare may allocate; commit
 *   only links already prepared PTs and performs deterministic VMM writes.
 */
static void
nvkm_drm_vm_bind_2m_split_plan_init(
    struct nvkm_drm_vm_bind_2m_split_plan *plan)
{
	plan->splits = NULL;
	plan->count = 0;
	plan->capacity = 0;
}

static void
nvkm_drm_vm_bind_2m_split_plan_fini(struct nvkm_gsp_vmm *vmm,
    struct nvkm_drm_vm_bind_2m_split_plan *plan)
{
	for (uint32_t i = 0; i < plan->count; i++) {
		if (!plan->splits[i].committed)
			nvkm_gsp_vmm_abort_split_vram_2m(vmm,
			    plan->splits[i].pt);
	}
	kfree(plan->splits);
	plan->splits = NULL;
	plan->count = 0;
	plan->capacity = 0;
}

static int
nvkm_drm_vm_bind_2m_split_plan_reserve(
    struct nvkm_drm_vm_bind_2m_split_plan *plan, uint32_t required)
{
	struct nvkm_drm_vm_bind_2m_split *splits;
	uint32_t capacity;

	if (required <= plan->capacity)
		return (0);

	capacity = plan->capacity != 0 ? plan->capacity : 4;
	while (capacity < required) {
		if (capacity > UINT32_MAX / 2)
			return (-ENOMEM);
		capacity *= 2;
	}

	splits = kmalloc_array(capacity, sizeof(*splits), GFP_KERNEL);
	if (splits == NULL)
		return (-ENOMEM);
	if (plan->splits != NULL) {
		memcpy(splits, plan->splits,
		    plan->count * sizeof(*splits));
		kfree(plan->splits);
	}
	plan->splits = splits;
	plan->capacity = capacity;
	return (0);
}

static int
nvkm_drm_vm_bind_2m_split_plan_prepare(struct nvkm_gsp_vmm *vmm,
    struct nvkm_drm_vm_bind_2m_split_plan *plan, uint64_t addr)
{
	struct nvkm_gsp_vmm_user_pt *pt;
	int err;

	for (uint32_t i = 0; i < plan->count; i++) {
		if (plan->splits[i].addr == addr)
			return (0);
	}

	err = nvkm_drm_vm_bind_2m_split_plan_reserve(plan, plan->count + 1);
	if (err != 0)
		return (err);

	err = nvkm_gsp_vmm_prepare_split_vram_2m(vmm, addr, &pt);
	if (err != 0)
		return (-err);

	plan->splits[plan->count].addr = addr;
	plan->splits[plan->count].pt = pt;
	plan->splits[plan->count].committed = false;
	plan->count++;
	return (0);
}

static int
nvkm_drm_vm_bind_2m_split_plan_commit(struct nvkm_gsp_vmm *vmm,
    struct nvkm_drm_vm_bind_2m_split_plan *plan, uint64_t addr)
{
	int err;

	for (uint32_t i = 0; i < plan->count; i++) {
		if (plan->splits[i].addr != addr)
			continue;
		if (plan->splits[i].committed)
			return (0);
		err = nvkm_gsp_vmm_commit_split_vram_2m_noflush(vmm, addr,
		    plan->splits[i].pt);
		if (err != 0)
			return (-err);
		plan->splits[i].committed = true;
		plan->splits[i].pt = NULL;
		return (0);
	}
	return (-ENOENT);
}

/*
 * nvkm_drm_vm_bind_2m_split_plan_preflight()
 *
 * Ownership:
 *   Borrows the caller-owned split plan and VMM.  It does not consume prepared
 *   PTs, link child tables, write PD0 slots, or mutate the split plan.
 *
 * Lifetime:
 *   A successful result proves every uncommitted split in the plan can be
 *   consumed by the later commit pass while VM_BIND serialization is held.
 *
 * Threading:
 *   Runs before materialize plan commit starts writing hardware state.  It may
 *   acquire vmm->tok through the VMM check helper, but performs only read-only
 *   validation.
 */
static int
nvkm_drm_vm_bind_2m_split_plan_preflight(struct nvkm_gsp_vmm *vmm,
    const struct nvkm_drm_vm_bind_2m_split_plan *plan)
{
	int err;

	for (uint32_t i = 0; i < plan->count; i++) {
		if (plan->splits[i].committed)
			continue;
		err = nvkm_gsp_vmm_check_split_vram_2m(vmm,
		    plan->splits[i].addr, plan->splits[i].pt);
		if (err != 0)
			return (-err);
	}
	return (0);
}

/*
 * nvkm_drm_vm_materialize_entry_*()
 *
 * Ownership:
 *   The entry owns all detached bindings, split PTs, and paddr snapshots
 *   prepared for one large live binding.  On commit, detached binding ownership
 *   moves into the live VM tracker and committed split PTs move into the VMM.
 *
 * Lifetime:
 *   Entries live inside one outer materialize plan.  Fini releases only
 *   resources that were prepared but not consumed by a successful commit.
 *
 * Threading:
 *   Prepare runs before the no-fail materialize commit and may allocate or pin.
 *   Commit runs while VM_BIND and GSP serialization are held and only consumes
 *   prepared resources.
 */
static void
nvkm_drm_vm_materialize_entry_init(
    struct nvkm_drm_vm_materialize_entry *entry)
{
	entry->binding = NULL;
	nvkm_drm_vm_bind_segment_plan_init(&entry->segments);
	nvkm_drm_vm_bind_2m_split_plan_init(&entry->split_plan);
	LIST_INIT(&entry->new_bindings);
	entry->is_2m = false;
	entry->committed = false;
}

static void
nvkm_drm_vm_materialize_entry_fini(struct nvkm_gsp_vmm *vmm,
    struct nvkm_drm_vm_materialize_entry *entry)
{
	if (!entry->committed)
		nvkm_drm_vm_bindings_free_prepared(&entry->new_bindings);
	nvkm_drm_vm_bind_2m_split_plan_fini(vmm, &entry->split_plan);
	nvkm_drm_vm_bind_segment_plan_fini(&entry->segments);
	entry->binding = NULL;
}

static uint32_t
nvkm_drm_vm_bind_page_shift_bucket(uint8_t page_shift)
{
	switch (page_shift) {
	case NVKM_GMMU_SPT_SHIFT:
		return (NVKM_DRM_VM_BIND_PAGE_SHIFT_4K);
	case NVKM_GMMU_LPT_SHIFT:
		return (NVKM_DRM_VM_BIND_PAGE_SHIFT_64K);
	case NVKM_GMMU_PD0_SHIFT:
		return (NVKM_DRM_VM_BIND_PAGE_SHIFT_2M);
	default:
		return (NVKM_DRM_VM_BIND_PAGE_SHIFT_4K);
	}
}

static uint32_t
nvkm_drm_vm_bind_domain_bucket(const struct nvkm_bo *bo)
{
	if (bo != NULL && (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0)
		return (NVKM_DRM_VM_BIND_DOMAIN_VRAM);
	return (NVKM_DRM_VM_BIND_DOMAIN_HOST);
}

static void
nvkm_drm_vm_bind_note_reject(struct nvkm_softc *sc, uint32_t reason,
    uint64_t size)
{
	uint64_t pages = size / NVKM_GMMU_PT_PAGE_SIZE;

	if (reason >= NVKM_DRM_VM_BIND_REJECT_REASON_COUNT)
		return;
	sc->vm_bind_reject_reason_count[reason]++;
	sc->vm_bind_reject_reason_pages[reason] += pages;
}

/*
 * nvkm_drm_vm_bind_note_dirty_range()
 *
 * Ownership:
 *   Borrows sc and the caller-owned dirty set, then records aggregate
 *   diagnostics for a PTE/PDE range already written by the caller.  It does not
 *   retain any VM, BO, or binding state.
 *
 * Lifetime:
 *   The range is caller-owned and used only for this commit's dirty set and
 *   aggregate counters.  The dirty set is cleared by the enclosing VM_BIND
 *   batch plan after the final flush boundary.
 *
 * Threading:
 *   Called from serialized VM_BIND mutation paths after a noflush VMM helper
 *   succeeds.  The counters are diagnostic only and are not part of the UAPI.
 */
static void
nvkm_drm_vm_dirty_set_init(struct nvkm_drm_vm_dirty_set *set)
{
	memset(set, 0, sizeof(*set));
}

static void
nvkm_drm_vm_dirty_set_remove_range(struct nvkm_drm_vm_dirty_set *set,
    uint32_t index)
{
	KASSERT(index < set->merged_count,
	    ("nvkm_drm: dirty range index %u count %u", index,
	    set->merged_count));
	for (uint32_t i = index + 1; i < set->merged_count; i++)
		set->ranges[i - 1] = set->ranges[i];
	set->merged_count--;
}

static void
nvkm_drm_vm_dirty_set_collapse(struct nvkm_drm_vm_dirty_set *set)
{
	if (!set->dirty)
		return;
	set->ranges[0].start = set->first;
	set->ranges[0].end = set->last;
	set->merged_count = 1;
	set->overflow = true;
}

static void
nvkm_drm_vm_dirty_set_insert(struct nvkm_drm_vm_dirty_set *set,
    uint64_t start, uint64_t end)
{
	uint32_t insert_at = 0;

	for (uint32_t i = 0; i < set->merged_count;) {
		struct nvkm_drm_vm_dirty_range *range = &set->ranges[i];

		if (end < range->start || start > range->end) {
			i++;
			continue;
		}
		if (range->start < start)
			start = range->start;
		if (range->end > end)
			end = range->end;
		nvkm_drm_vm_dirty_set_remove_range(set, i);
	}

	if (set->merged_count == NVKM_DRM_VM_DIRTY_RANGE_INLINE) {
		nvkm_drm_vm_dirty_set_collapse(set);
		return;
	}
	while (insert_at < set->merged_count &&
	    set->ranges[insert_at].start < start)
		insert_at++;
	for (uint32_t i = set->merged_count; i > insert_at; i--)
		set->ranges[i] = set->ranges[i - 1];
	set->ranges[insert_at].start = start;
	set->ranges[insert_at].end = end;
	set->merged_count++;
}

static uint64_t
nvkm_drm_vm_dirty_set_merged_pages(const struct nvkm_drm_vm_dirty_set *set)
{
	uint64_t pages = 0;

	for (uint32_t i = 0; i < set->merged_count; i++) {
		if (set->ranges[i].end <= set->ranges[i].start)
			continue;
		pages += (set->ranges[i].end - set->ranges[i].start) /
		    NVKM_GMMU_PT_PAGE_SIZE;
	}
	return (pages);
}

/*
 * nvkm_drm_vm_dirty_set_flush()
 *
 * Ownership:
 *   Borrows the DRM-local dirty set and converts its scalar ranges into the
 *   backend VMM dirty-set shape.  It does not transfer ownership of the VM_BIND
 *   batch plan or retain any range storage after return.
 *
 * Lifetime:
 *   The stack range array lives only for nvkm_gsp_vmm_flush_dirty().  The flush
 *   completes the PTE/PDE visibility and invalidate boundary before returning.
 *
 * Threading:
 *   Called from VM_BIND/release commit paths before fence publish or retired
 *   BO ref release.  The VMM backend takes its own token for BAR1 visibility
 *   and hardware invalidate.
 */
static void
nvkm_drm_vm_dirty_set_flush(struct nvkm_gsp_vmm *vmm,
    const struct nvkm_drm_vm_dirty_set *set)
{
	struct nvkm_gsp_vmm_dirty_range ranges[NVKM_DRM_VM_DIRTY_RANGE_INLINE];
	struct nvkm_gsp_vmm_dirty_set dirty;
	uint32_t count;

	if (vmm == NULL || set == NULL || !set->dirty)
		return;
	count = set->merged_count;
	KASSERT(count <= NVKM_DRM_VM_DIRTY_RANGE_INLINE,
	    ("nvkm_drm: dirty set range count %u", count));
	if (count == 0) {
		ranges[0].start = set->first;
		ranges[0].end = set->last;
		count = 1;
	} else {
		for (uint32_t i = 0; i < count; i++) {
			ranges[i].start = set->ranges[i].start;
			ranges[i].end = set->ranges[i].end;
		}
	}

	memset(&dirty, 0, sizeof(dirty));
	dirty.ranges = ranges;
	dirty.range_count = count;
	dirty.page_count = nvkm_drm_vm_dirty_set_merged_pages(set);
	dirty.overflow = set->overflow ? 1 : 0;
	nvkm_gsp_vmm_flush_dirty(vmm, &dirty);
}

static void
nvkm_drm_vm_dirty_set_publish(struct nvkm_softc *sc,
    const struct nvkm_drm_vm_dirty_set *set)
{
	if (!set->dirty)
		return;
	sc->vm_bind_dirty_set_count++;
	sc->vm_bind_dirty_set_range_count += set->merged_count;
	sc->vm_bind_dirty_set_pages += nvkm_drm_vm_dirty_set_merged_pages(set);
	if (set->range_count > set->merged_count)
		sc->vm_bind_dirty_set_merged_count +=
		    set->range_count - set->merged_count;
	if (set->overflow)
		sc->vm_bind_dirty_set_overflow_count++;
}

static void
nvkm_drm_vm_bind_note_dirty_range(struct nvkm_softc *sc,
    struct nvkm_drm_vm_dirty_set *dirty_set, uint64_t addr, uint64_t size)
{
	uint64_t end;

	if (size == 0)
		return;
	if (!nvkm_drm_vm_range_end(addr, size, &end))
		end = UINT64_MAX;
	if (dirty_set != NULL) {
		if (!dirty_set->dirty) {
			dirty_set->first = addr;
			dirty_set->last = end;
			dirty_set->dirty = true;
		} else {
			if (addr < dirty_set->first)
				dirty_set->first = addr;
			if (end > dirty_set->last)
				dirty_set->last = end;
		}
		dirty_set->range_count++;
		dirty_set->pages += size / NVKM_GMMU_PT_PAGE_SIZE;
		if (dirty_set->overflow) {
			dirty_set->ranges[0].start = dirty_set->first;
			dirty_set->ranges[0].end = dirty_set->last;
			dirty_set->merged_count = 1;
		} else {
			nvkm_drm_vm_dirty_set_insert(dirty_set, addr, end);
		}
	}
	sc->vm_bind_dirty_range_count++;
	sc->vm_bind_dirty_range_pages += size / NVKM_GMMU_PT_PAGE_SIZE;
}

/*
 * nvkm_drm_vm_bind_note_materialize()
 *
 * Ownership:
 *   Borrows sc and records one large-leaf materialize operation.  The caller
 *   keeps ownership of the binding and hardware page-table state.
 *
 * Lifetime:
 *   The byte range is copied into aggregate counters only.
 *
 * Threading:
 *   Called only after materialize-to-4K PTE writes succeed, while VM_BIND
 *   serialization is held.
 */
static void
nvkm_drm_vm_bind_note_materialize(struct nvkm_softc *sc,
    struct nvkm_drm_vm_dirty_set *dirty_set, uint64_t addr, uint64_t size)
{
	sc->vm_bind_materialize_count++;
	sc->vm_bind_materialize_pages += size / NVKM_GMMU_PT_PAGE_SIZE;
	nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set, addr, size);
}

/*
 * nvkm_drm_vm_bind_vram_run_has_aligned_middle()
 *
 * Ownership:
 *   Borrows caller-owned MAP coordinates.  It does not retain any BO, VMM, or
 *   binding state.
 *
 * Lifetime:
 *   All inputs are copied by value and used only during the current planner
 *   call.
 *
 * Threading:
 *   Pure planner helper.  The caller must already hold any serialization
 *   needed to make the physical run stable for this VM_BIND operation.
 */
static bool
nvkm_drm_vm_bind_vram_run_has_aligned_middle(uint64_t addr,
    uint64_t bo_offset, uint64_t paddr, uint64_t size, uint64_t page_size)
{
	uint64_t mask;
	uint64_t addr_mod;
	uint64_t prefix;
	uint64_t middle;

	if (page_size == 0)
		return (false);
	mask = page_size - 1;
	if ((page_size & mask) != 0)
		return (false);
	addr_mod = addr & mask;
	if ((bo_offset & mask) != addr_mod || (paddr & mask) != addr_mod)
		return (false);
	prefix = (page_size - addr_mod) & mask;
	if (prefix >= size)
		return (false);
	middle = (size - prefix) & ~mask;
	return (middle != 0);
}

/*
 * nvkm_drm_vm_bind_note_vram_2m_reject()
 *
 * Ownership:
 *   Borrows sc and scalar MAP coordinates.  It records diagnostic counters only
 *   and retains no BO, VMM, or binding state.
 *
 * Lifetime:
 *   Inputs are snapshots from the current VM_BIND planner pass.  The selected
 *   reject reason is not UAPI and may not be used by userspace for control flow.
 *
 * Threading:
 *   Called while VM_BIND serialization protects the planner counters.  It does
 *   not sleep and performs no page-table or GEM operations.
 */
static void
nvkm_drm_vm_bind_note_vram_2m_reject(struct nvkm_softc *sc, uint64_t addr,
    uint64_t bo_offset, uint64_t paddr, uint64_t size)
{
	uint64_t mask = NVKM_GMMU_PD0_PAGE_SIZE - 1;
	uint64_t addr_mod = addr & mask;
	uint64_t prefix;
	uint32_t reason;

	if ((bo_offset & mask) != addr_mod) {
		reason = addr_mod != 0 ?
		    NVKM_DRM_VM_BIND_REJECT_VA_ALIGN :
		    NVKM_DRM_VM_BIND_REJECT_BO_OFFSET_ALIGN;
	} else if ((paddr & mask) != addr_mod) {
		reason = NVKM_DRM_VM_BIND_REJECT_PADDR_ALIGN;
	} else {
		prefix = (NVKM_GMMU_PD0_PAGE_SIZE - addr_mod) & mask;
		if (prefix >= size || ((size - prefix) & ~mask) == 0)
			reason = NVKM_DRM_VM_BIND_REJECT_RANGE_ALIGN;
		else
			reason = NVKM_DRM_VM_BIND_REJECT_PADDR_RUN;
	}
	nvkm_drm_vm_bind_note_reject(sc, reason, size);
}

/*
 * nvkm_drm_vm_bind_build_vram_run_segments_64k()
 *
 * Ownership:
 *   Borrows bo and copies one physically contiguous VRAM run into MAP plan
 *   segments.  It does not acquire or release GEM, BO, VMM, or binding
 *   ownership.
 *
 * Lifetime:
 *   Added segments are owned by the caller's per-op plan until the MAP
 *   operation finishes or aborts.
 *
 * Threading:
 *   Pure planner helper.  The caller must keep the BO backing stable for the
 *   current VM_BIND operation and serialize access to sc counters.
 */
static int
nvkm_drm_vm_bind_build_vram_run_segments_64k(struct nvkm_softc *sc,
    struct nvkm_drm_vm_bind_segment_plan *plan, const struct nvkm_bo *bo,
    uint64_t addr, uint64_t bo_offset, uint64_t paddr, uint64_t size)
{
	uint64_t mask = NVKM_GMMU_LPT_PAGE_SIZE - 1;
	uint64_t addr_mod, bo_mod, paddr_mod;
	uint64_t prefix, middle, suffix;
	int err;

	if (bo->base.size < NVKM_GMMU_LPT_PAGE_SIZE) {
		nvkm_drm_vm_bind_note_reject(sc,
		    NVKM_DRM_VM_BIND_REJECT_RANGE_ALIGN, size);
		return (nvkm_drm_vm_bind_segment_add(plan, addr, size,
		    bo_offset, paddr, NVKM_GMMU_SPT_SHIFT));
	}

	addr_mod = addr & mask;
	bo_mod = bo_offset & mask;
	paddr_mod = paddr & mask;
	if (addr_mod != bo_mod || addr_mod != paddr_mod) {
		uint32_t reason;

		if (addr_mod != bo_mod)
			reason = addr_mod != 0 ?
			    NVKM_DRM_VM_BIND_REJECT_VA_ALIGN :
			    NVKM_DRM_VM_BIND_REJECT_BO_OFFSET_ALIGN;
		else
			reason = NVKM_DRM_VM_BIND_REJECT_PADDR_ALIGN;
		nvkm_drm_vm_bind_note_reject(sc, reason, size);
		return (nvkm_drm_vm_bind_segment_add(plan, addr, size,
		    bo_offset, paddr, NVKM_GMMU_SPT_SHIFT));
	}

	prefix = (NVKM_GMMU_LPT_PAGE_SIZE - addr_mod) & mask;
	if (prefix >= size) {
		nvkm_drm_vm_bind_note_reject(sc,
		    NVKM_DRM_VM_BIND_REJECT_RANGE_ALIGN, size);
		return (nvkm_drm_vm_bind_segment_add(plan, addr, size,
		    bo_offset, paddr, NVKM_GMMU_SPT_SHIFT));
	}
	middle = (size - prefix) & ~mask;
	if (middle == 0) {
		nvkm_drm_vm_bind_note_reject(sc,
		    NVKM_DRM_VM_BIND_REJECT_RANGE_ALIGN, size);
		return (nvkm_drm_vm_bind_segment_add(plan, addr, size,
		    bo_offset, paddr, NVKM_GMMU_SPT_SHIFT));
	}
	suffix = size - prefix - middle;

	if (prefix != 0) {
		err = nvkm_drm_vm_bind_segment_add(plan, addr, prefix,
		    bo_offset, paddr, NVKM_GMMU_SPT_SHIFT);
		if (err != 0)
			return (err);
	}
	err = nvkm_drm_vm_bind_segment_add(plan, addr + prefix,
	    middle, bo_offset + prefix, paddr + prefix,
	    NVKM_GMMU_LPT_SHIFT);
	if (err != 0)
		return (err);
	if (suffix != 0) {
		err = nvkm_drm_vm_bind_segment_add(plan,
		    addr + prefix + middle, suffix,
		    bo_offset + prefix + middle, paddr + prefix + middle,
		    NVKM_GMMU_SPT_SHIFT);
		if (err != 0)
			return (err);
	}
	return (0);
}

/*
 * nvkm_drm_vm_bind_build_map_segments()
 *
 * Ownership:
 *   Borrows bo and copies MAP range values.  It does not acquire or release
 *   GEM, BO, VMM, or binding ownership.
 *
 * Lifetime:
 *   Returned segments are valid only for the current MAP operation.  Each
 *   segment is later materialized into an independent live binding with its
 *   own GEM reference and VM_BIND pin.
 *
 * Threading:
 *   Called while VM_BIND holds at least one BO pin, so bo->domain and
 *   bo->paddr are stable for this VM_BIND.  The helper is otherwise pure.
 */
static int
nvkm_drm_vm_bind_build_vram_run_segments(struct nvkm_softc *sc,
    struct nvkm_drm_vm_bind_segment_plan *plan, const struct nvkm_bo *bo,
    uint64_t addr, uint64_t bo_offset, uint64_t paddr, uint64_t size)
{
	uint64_t mask = NVKM_GMMU_PD0_PAGE_SIZE - 1;
	uint64_t addr_mod;
	uint64_t prefix, middle, suffix;
	int err;

	if (bo->base.size >= NVKM_GMMU_PD0_PAGE_SIZE &&
	    nvkm_drm_vm_bind_vram_run_has_aligned_middle(addr, bo_offset,
	    paddr, size, NVKM_GMMU_PD0_PAGE_SIZE)) {
		if (sc->vm_bind_map_2m_enable == 0) {
			nvkm_drm_vm_bind_note_reject(sc,
			    NVKM_DRM_VM_BIND_REJECT_CAPABILITY_GATE, size);
			return (nvkm_drm_vm_bind_build_vram_run_segments_64k(sc,
			    plan, bo, addr, bo_offset, paddr, size));
		}

		addr_mod = addr & mask;
		prefix = (NVKM_GMMU_PD0_PAGE_SIZE - addr_mod) & mask;
		middle = (size - prefix) & ~mask;
		suffix = size - prefix - middle;

		if (prefix != 0) {
			err = nvkm_drm_vm_bind_build_vram_run_segments_64k(sc,
			    plan, bo, addr, bo_offset, paddr, prefix);
			if (err != 0)
				return (err);
		}
		err = nvkm_drm_vm_bind_segment_add(plan, addr + prefix,
		    middle, bo_offset + prefix, paddr + prefix,
		    NVKM_GMMU_PD0_SHIFT);
		if (err != 0)
			return (err);
		if (suffix != 0) {
			err = nvkm_drm_vm_bind_build_vram_run_segments_64k(sc,
			    plan, bo, addr + prefix + middle,
			    bo_offset + prefix + middle,
			    paddr + prefix + middle, suffix);
			if (err != 0)
				return (err);
		}
		return (0);
	}

	if (bo->base.size >= NVKM_GMMU_PD0_PAGE_SIZE)
		nvkm_drm_vm_bind_note_vram_2m_reject(sc, addr, bo_offset,
		    paddr, size);
	return (nvkm_drm_vm_bind_build_vram_run_segments_64k(sc, plan, bo,
	    addr, bo_offset, paddr, size));
}

/*
 * nvkm_drm_vm_bind_build_sysmem_run_segments_64k()
 *
 * Ownership:
 *   Borrows bo and copies one physically contiguous HOST/GART run into MAP
 *   plan segments.  Each segment owns its own 4 KiB physical-page snapshot so
 *   commit never has to query BO backing.
 *
 * Lifetime:
 *   Added segments are owned by the caller's per-op plan until the MAP
 *   operation finishes or aborts.
 *
 * Threading:
 *   Pure planner helper except for diagnostic counters.  The caller must keep
 *   the BO backing pinned and populated for the current VM_BIND operation.
 */
static int
nvkm_drm_vm_bind_build_sysmem_run_segments_64k(struct nvkm_softc *sc,
    struct nvkm_drm_vm_bind_segment_plan *plan, const struct nvkm_bo *bo,
    uint64_t addr, uint64_t bo_offset, uint64_t paddr, uint64_t size)
{
	uint64_t mask = NVKM_GMMU_LPT_PAGE_SIZE - 1;
	uint64_t addr_mod, bo_mod, paddr_mod;
	uint64_t prefix, middle, suffix;
	int err;

	if (bo->base.size < NVKM_GMMU_LPT_PAGE_SIZE) {
		nvkm_drm_vm_bind_note_reject(sc,
		    NVKM_DRM_VM_BIND_REJECT_RANGE_ALIGN, size);
		return (nvkm_drm_vm_bind_segment_add_sysmem(plan, bo, addr,
		    size, bo_offset, NVKM_GMMU_SPT_SHIFT));
	}

	addr_mod = addr & mask;
	bo_mod = bo_offset & mask;
	paddr_mod = paddr & mask;
	if (addr_mod != bo_mod || addr_mod != paddr_mod) {
		uint32_t reason;

		if (addr_mod != bo_mod)
			reason = addr_mod != 0 ?
			    NVKM_DRM_VM_BIND_REJECT_VA_ALIGN :
			    NVKM_DRM_VM_BIND_REJECT_BO_OFFSET_ALIGN;
		else
			reason = NVKM_DRM_VM_BIND_REJECT_PADDR_ALIGN;
		nvkm_drm_vm_bind_note_reject(sc, reason, size);
		return (nvkm_drm_vm_bind_segment_add_sysmem(plan, bo, addr,
		    size, bo_offset, NVKM_GMMU_SPT_SHIFT));
	}

	prefix = (NVKM_GMMU_LPT_PAGE_SIZE - addr_mod) & mask;
	if (prefix >= size) {
		nvkm_drm_vm_bind_note_reject(sc,
		    NVKM_DRM_VM_BIND_REJECT_RANGE_ALIGN, size);
		return (nvkm_drm_vm_bind_segment_add_sysmem(plan, bo, addr,
		    size, bo_offset, NVKM_GMMU_SPT_SHIFT));
	}
	middle = (size - prefix) & ~mask;
	if (middle == 0) {
		nvkm_drm_vm_bind_note_reject(sc,
		    NVKM_DRM_VM_BIND_REJECT_RANGE_ALIGN, size);
		return (nvkm_drm_vm_bind_segment_add_sysmem(plan, bo, addr,
		    size, bo_offset, NVKM_GMMU_SPT_SHIFT));
	}
	suffix = size - prefix - middle;

	if (prefix != 0) {
		err = nvkm_drm_vm_bind_segment_add_sysmem(plan, bo, addr,
		    prefix, bo_offset, NVKM_GMMU_SPT_SHIFT);
		if (err != 0)
			return (err);
	}
	err = nvkm_drm_vm_bind_segment_add_sysmem(plan, bo, addr + prefix,
	    middle, bo_offset + prefix, NVKM_GMMU_LPT_SHIFT);
	if (err != 0)
		return (err);
	if (suffix != 0) {
		err = nvkm_drm_vm_bind_segment_add_sysmem(plan, bo,
		    addr + prefix + middle, suffix,
		    bo_offset + prefix + middle, NVKM_GMMU_SPT_SHIFT);
		if (err != 0)
			return (err);
	}
	return (0);
}

/*
 * nvkm_drm_vm_bind_build_sysmem_run_segments_2m()
 *
 * Ownership:
 *   Borrows a pinned HOST/GART BO and appends caller-owned MAP segments.  Each
 *   appended sysmem segment owns its paddr snapshot through the segment plan.
 *
 * Lifetime:
 *   The helper only creates prepare-stage value data.  The snapshots are
 *   consumed by the prepared sysmem writer during this VM_BIND op and released
 *   by nvkm_drm_vm_bind_segment_plan_fini().
 *
 * Threading:
 *   Runs while VM_BIND serialization keeps the BO backing stable.  It does no
 *   PTE writes and may fail before the no-fail commit section begins.
 */
static int
nvkm_drm_vm_bind_build_sysmem_run_segments_2m(struct nvkm_softc *sc,
    struct nvkm_drm_vm_bind_segment_plan *plan, const struct nvkm_bo *bo,
    uint64_t addr, uint64_t bo_offset, uint64_t paddr, uint64_t size)
{
	uint64_t mask = NVKM_GMMU_PD0_PAGE_SIZE - 1;
	uint64_t addr_mod;
	uint64_t prefix, middle, suffix;
	int err;

	addr_mod = addr & mask;
	prefix = (NVKM_GMMU_PD0_PAGE_SIZE - addr_mod) & mask;
	middle = (size - prefix) & ~mask;
	suffix = size - prefix - middle;

	if (prefix != 0) {
		err = nvkm_drm_vm_bind_build_sysmem_run_segments_64k(sc,
		    plan, bo, addr, bo_offset, paddr, prefix);
		if (err != 0)
			return (err);
	}
	err = nvkm_drm_vm_bind_segment_add_sysmem(plan, bo, addr + prefix,
	    middle, bo_offset + prefix, NVKM_GMMU_PD0_SHIFT);
	if (err != 0)
		return (err);
	if (suffix != 0) {
		err = nvkm_drm_vm_bind_build_sysmem_run_segments_64k(sc,
		    plan, bo, addr + prefix + middle,
		    bo_offset + prefix + middle, paddr + prefix + middle,
		    suffix);
		if (err != 0)
			return (err);
	}
	return (0);
}

static int
nvkm_drm_vm_bind_build_sysmem_run_segments(struct nvkm_softc *sc,
    struct nvkm_drm_vm_bind_segment_plan *plan, const struct nvkm_bo *bo,
    uint64_t addr, uint64_t bo_offset, uint64_t paddr, uint64_t size)
{
	bool has_2m;
	bool has_64k;

	has_2m = nvkm_drm_vm_bind_vram_run_has_aligned_middle(addr, bo_offset,
	    paddr, size, NVKM_GMMU_PD0_PAGE_SIZE);
	has_64k = nvkm_drm_vm_bind_vram_run_has_aligned_middle(addr, bo_offset,
	    paddr, size, NVKM_GMMU_LPT_PAGE_SIZE);
	if (has_2m) {
		if (sc->vm_bind_map_host_large_enable == 0 ||
		    sc->vm_bind_map_2m_enable == 0) {
			nvkm_drm_vm_bind_note_reject(sc,
			    NVKM_DRM_VM_BIND_REJECT_CAPABILITY_GATE, size);
		}
	}
	if (sc->vm_bind_map_host_large_enable == 0) {
		if (has_64k && !has_2m) {
			nvkm_drm_vm_bind_note_reject(sc,
			    NVKM_DRM_VM_BIND_REJECT_CAPABILITY_GATE, size);
		}
		return (nvkm_drm_vm_bind_segment_add_sysmem(plan, bo, addr,
		    size, bo_offset, NVKM_GMMU_SPT_SHIFT));
	}
	if (has_2m && sc->vm_bind_map_2m_enable != 0) {
		return (nvkm_drm_vm_bind_build_sysmem_run_segments_2m(sc,
		    plan, bo, addr, bo_offset, paddr, size));
	}

	return (nvkm_drm_vm_bind_build_sysmem_run_segments_64k(sc, plan, bo,
	    addr, bo_offset, paddr, size));
}

static int
nvkm_drm_vm_bind_build_map_segments(struct nvkm_softc *sc,
    const struct nvkm_bo *bo, uint64_t addr, uint64_t bo_offset, uint64_t size,
    struct nvkm_drm_vm_bind_segment_plan *plan)
{
	uint64_t off, run_size;
	uint64_t pending_addr = 0, pending_bo_offset = 0, pending_size = 0;
	vm_paddr_t paddr;
	uint32_t run_count = 0;
	int err;

	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) == 0) {
		for (off = 0; off < size; off += run_size) {
			bool has_2m, has_64k, large_capable;

			err = nvkm_bo_paddr_run_at(bo, bo_offset + off,
			    size - off, &paddr, &run_size);
			if (err != 0)
				return (-err);
			if (run_size == 0)
				return (-EIO);
			run_count++;
			has_2m = nvkm_drm_vm_bind_vram_run_has_aligned_middle(
			    addr + off, bo_offset + off, paddr, run_size,
			    NVKM_GMMU_PD0_PAGE_SIZE);
			has_64k = nvkm_drm_vm_bind_vram_run_has_aligned_middle(
			    addr + off, bo_offset + off, paddr, run_size,
			    NVKM_GMMU_LPT_PAGE_SIZE);
			if (sc->vm_bind_map_host_large_enable == 0 &&
			    (has_2m || has_64k)) {
				nvkm_drm_vm_bind_note_reject(sc,
				    NVKM_DRM_VM_BIND_REJECT_CAPABILITY_GATE,
				    run_size);
			}
			large_capable =
			    sc->vm_bind_map_host_large_enable != 0 &&
			    (has_64k ||
			    (sc->vm_bind_map_2m_enable != 0 && has_2m));
			if (large_capable) {
				if (pending_size != 0) {
					err = nvkm_drm_vm_bind_segment_add_sysmem(
					    plan, bo, pending_addr,
					    pending_size, pending_bo_offset,
					    NVKM_GMMU_SPT_SHIFT);
					if (err != 0)
						return (err);
					pending_size = 0;
				}
				err = nvkm_drm_vm_bind_build_sysmem_run_segments(
				    sc, plan, bo, addr + off, bo_offset + off,
				    paddr, run_size);
				if (err != 0)
					return (err);
			} else {
				if (pending_size == 0) {
					pending_addr = addr + off;
					pending_bo_offset = bo_offset + off;
				}
				pending_size += run_size;
			}
		}
		if (pending_size != 0) {
			err = nvkm_drm_vm_bind_segment_add_sysmem(plan, bo,
			    pending_addr, pending_size, pending_bo_offset,
			    NVKM_GMMU_SPT_SHIFT);
			if (err != 0)
				return (err);
		}
		if (run_count > 1) {
			sc->vm_bind_paddr_run_split_count++;
			sc->vm_bind_paddr_run_split_pages +=
			    size / NVKM_GMMU_PT_PAGE_SIZE;
		}
		return (0);
	}

	for (off = 0; off < size; off += run_size) {
		err = nvkm_bo_paddr_run_at(bo, bo_offset + off, size - off,
		    &paddr, &run_size);
		if (err != 0)
			return (-err);
		if (run_size == 0)
			return (-EIO);
		run_count++;
		err = nvkm_drm_vm_bind_build_vram_run_segments(sc, plan, bo,
		    addr + off, bo_offset + off, paddr, run_size);
		if (err != 0)
			return (err);
	}

	if (run_count > 1) {
		sc->vm_bind_paddr_run_split_count++;
		sc->vm_bind_paddr_run_split_pages +=
		    size / NVKM_GMMU_PT_PAGE_SIZE;
	}
	return (0);
}

/*
 * nvkm_drm_vm_bind_sparse_has_aligned_middle()
 *
 * Ownership:
 *   Borrows scalar VM_BIND sparse coordinates.  It does not retain VM, BO, or
 *   VMM state.
 *
 * Lifetime:
 *   The result is valid only for the current planner call.
 *
 * Threading:
 *   Pure helper.  The caller owns any VM_BIND serialization needed for
 *   diagnostic counter updates.
 */
static bool
nvkm_drm_vm_bind_sparse_has_aligned_middle(uint64_t addr, uint64_t size,
    uint64_t page_size)
{
	uint64_t mask, addr_mod, prefix, middle;

	if (page_size == 0)
		return (false);
	mask = page_size - 1;
	if ((page_size & mask) != 0)
		return (false);

	addr_mod = addr & mask;
	prefix = (page_size - addr_mod) & mask;
	if (prefix >= size)
		return (false);
	middle = (size - prefix) & ~mask;
	return (middle != 0);
}

/*
 * nvkm_drm_vm_bind_build_sparse_segments_64k()
 *
 * Ownership:
 *   Appends caller-owned value segments to plan.  No sparse-region object is
 *   allocated here; later prepare_sparse_region_page() consumes these values.
 *
 * Lifetime:
 *   The segments live only for the enclosing VM_BIND op and are released with
 *   the segment plan.
 *
 * Threading:
 *   Pure planner helper.  It does not inspect VMM state or write PTEs.
 */
static int
nvkm_drm_vm_bind_build_sparse_segments_64k(
    struct nvkm_drm_vm_bind_segment_plan *plan, uint64_t addr, uint64_t size)
{
	uint64_t mask = NVKM_GMMU_LPT_PAGE_SIZE - 1;
	uint64_t prefix, middle, suffix;
	int err;

	prefix = (NVKM_GMMU_LPT_PAGE_SIZE - (addr & mask)) & mask;
	if (prefix >= size)
		return (nvkm_drm_vm_bind_segment_add(plan, addr, size, 0, 0,
		    NVKM_GMMU_SPT_SHIFT));
	middle = (size - prefix) & ~mask;
	suffix = size - prefix - middle;

	if (prefix != 0) {
		err = nvkm_drm_vm_bind_segment_add(plan, addr, prefix, 0, 0,
		    NVKM_GMMU_SPT_SHIFT);
		if (err != 0)
			return (err);
	}
	if (middle != 0) {
		err = nvkm_drm_vm_bind_segment_add(plan, addr + prefix,
		    middle, 0, 0, NVKM_GMMU_LPT_SHIFT);
		if (err != 0)
			return (err);
	}
	if (suffix != 0) {
		err = nvkm_drm_vm_bind_segment_add(plan, addr + prefix +
		    middle, suffix, 0, 0, NVKM_GMMU_SPT_SHIFT);
		if (err != 0)
			return (err);
	}
	return (0);
}

/*
 * nvkm_drm_vm_bind_build_sparse_segments_2m()
 *
 * Ownership:
 *   Appends sparse target segments only.  The caller owns the plan and later
 *   sparse-region allocation for each segment.
 *
 * Lifetime:
 *   Segment values survive until the enclosing VM_BIND op finishes preparing
 *   and committing sparse regions.
 *
 * Threading:
 *   Pure planner helper.  It encodes page-size intent but does not mutate
 *   mapping or GMMU state.
 */
static int
nvkm_drm_vm_bind_build_sparse_segments_2m(
    struct nvkm_drm_vm_bind_segment_plan *plan, uint64_t addr, uint64_t size)
{
	uint64_t mask = NVKM_GMMU_PD0_PAGE_SIZE - 1;
	uint64_t prefix, middle, suffix;
	int err;

	prefix = (NVKM_GMMU_PD0_PAGE_SIZE - (addr & mask)) & mask;
	if (prefix >= size)
		return (nvkm_drm_vm_bind_build_sparse_segments_64k(plan,
		    addr, size));
	middle = (size - prefix) & ~mask;
	suffix = size - prefix - middle;

	if (prefix != 0) {
		err = nvkm_drm_vm_bind_build_sparse_segments_64k(plan, addr,
		    prefix);
		if (err != 0)
			return (err);
	}
	if (middle != 0) {
		err = nvkm_drm_vm_bind_segment_add(plan, addr + prefix,
		    middle, 0, 0, NVKM_GMMU_PD0_SHIFT);
		if (err != 0)
			return (err);
	}
	if (suffix != 0) {
		err = nvkm_drm_vm_bind_build_sparse_segments_64k(plan,
		    addr + prefix + middle, suffix);
		if (err != 0)
			return (err);
	}
	return (0);
}

/*
 * nvkm_drm_vm_bind_build_sparse_segments()
 *
 * Ownership:
 *   Converts one userspace MAP|SPARSE op into sparse target segments.  Segments
 *   are value data owned by the caller's segment plan; no sparse-region record
 *   is allocated here.
 *
 * Lifetime:
 *   The plan lives only for the current VM_BIND op.  Prepared sparse-region
 *   records later copy addr/size/page_shift from these segments.
 *
 * Threading:
 *   Pure planner helper except for diagnostic reject counters.  Large sparse
 *   is hidden behind explicit debug gates and defaults to the legacy 4 KiB
 *   path.
 */
static int
nvkm_drm_vm_bind_build_sparse_segments(struct nvkm_softc *sc,
    uint64_t addr, uint64_t size, struct nvkm_drm_vm_bind_segment_plan *plan)
{
	bool has_2m = nvkm_drm_vm_bind_sparse_has_aligned_middle(addr, size,
	    NVKM_GMMU_PD0_PAGE_SIZE);
	bool has_64k = nvkm_drm_vm_bind_sparse_has_aligned_middle(addr, size,
	    NVKM_GMMU_LPT_PAGE_SIZE);

	if ((has_2m || has_64k) && sc->vm_bind_sparse_large_enable == 0) {
		nvkm_drm_vm_bind_note_reject(sc,
		    NVKM_DRM_VM_BIND_REJECT_CAPABILITY_GATE, size);
		return (nvkm_drm_vm_bind_segment_add(plan, addr, size, 0, 0,
		    NVKM_GMMU_SPT_SHIFT));
	}

	if (has_2m) {
		if (sc->vm_bind_sparse_2m_enable != 0)
			return (nvkm_drm_vm_bind_build_sparse_segments_2m(
			    plan, addr, size));
		nvkm_drm_vm_bind_note_reject(sc,
		    NVKM_DRM_VM_BIND_REJECT_CAPABILITY_GATE, size);
	}

	if (has_64k || has_2m)
		return (nvkm_drm_vm_bind_build_sparse_segments_64k(plan,
		    addr, size));
	return (nvkm_drm_vm_bind_segment_add(plan, addr, size, 0, 0,
	    NVKM_GMMU_SPT_SHIFT));
}

/*
 * nvkm_drm_vm_bind_prepare_segment_bindings()
 *
 * Ownership:
 *   Takes one GEM reference and one VM_BIND pin for every segment binding it
 *   creates.  On failure it releases all ownership already acquired.
 *
 * Lifetime:
 *   On success, new_bindings owns detached binding nodes.  The caller must
 *   either insert them into the live VM tracker after PTE install succeeds, or
 *   free them with nvkm_drm_vm_bindings_free_prepared().
 *
 * Threading:
 *   Runs in VM_BIND prepare while nfile->vm_token is held.  It may sleep while
 *   pinning the BO, but it does not touch hardware PTEs.
 */
static int
nvkm_drm_vm_bind_prepare_segment_bindings(struct nvkm_drm_file *nfile,
    struct drm_gem_object *obj,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    uint8_t pte_kind, struct nvkm_drm_vm_binding_list *new_bindings)
{
	struct nvkm_drm_vm_binding *binding;
	int err;

	LIST_INIT(new_bindings);
	for (uint32_t i = 0; i < segment_count; i++) {
		drm_gem_object_get(obj);
		binding = nvkm_drm_vm_binding_alloc(nfile, segments[i].addr,
		    segments[i].size, obj, segments[i].bo_offset, pte_kind,
		    segments[i].page_shift);
		if (binding == NULL) {
			drm_gem_object_put_unlocked(obj);
			err = -ENOMEM;
			goto fail;
		}
		err = nvkm_drm_vm_binding_pin(binding);
		if (err != 0) {
			nvkm_drm_vm_binding_free(binding);
			err = -err;
			goto fail;
		}
		LIST_INSERT_HEAD(new_bindings, binding, link);
	}
	return (0);

fail:
	nvkm_drm_vm_bindings_free_prepared(new_bindings);
	return (err);
}

static void
nvkm_drm_vm_bindings_insert_prepared(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_binding_list *new_bindings)
{
	struct nvkm_drm_vm_binding *binding;

	while ((binding = LIST_FIRST(new_bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_insert_sorted(nfile, binding);
	}
}

/*
 * nvkm_drm_vm_bind_map_segments_preflight()
 *
 * Ownership:
 *   Borrows the MAP segment plan, target BO, and file VMM.  It does not retain
 *   references, allocate objects, or write hardware PTEs.
 *
 * Lifetime:
 *   The caller keeps VM_BIND serialization held until the later prepared
 *   writers run.  A successful preflight proves that every segment has valid
 *   writer arguments and that all target PT storage is present before the
 *   first segment is written.
 *
 * Threading:
 *   May take vmm->tok through nvkm_gsp_vmm_check_prepared_pt_range().  This is
 *   still a prepare/pre-commit gate; it must run before any PTE write in the
 *   MAP commit section.
 */
static int
nvkm_drm_vm_bind_map_segments_preflight_target(struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    bool target_vram)
{
	int err;

	for (uint32_t i = 0; i < segment_count; i++) {
		const struct nvkm_drm_vm_bind_segment *segment = &segments[i];
		uint64_t page_size;

		if (segment->size == 0)
			return (EINVAL);
		if (!target_vram) {
			if (segment->page_shift == NVKM_GMMU_SPT_SHIFT)
				page_size = NVKM_GMMU_PT_PAGE_SIZE;
			else if (segment->page_shift == NVKM_GMMU_LPT_SHIFT)
				page_size = NVKM_GMMU_LPT_PAGE_SIZE;
			else if (segment->page_shift == NVKM_GMMU_PD0_SHIFT)
				page_size = NVKM_GMMU_PD0_PAGE_SIZE;
			else
				return (EINVAL);
			if (segment->sysmem_paddrs == NULL ||
			    segment->sysmem_page_count !=
			    segment->size / NVKM_GMMU_PT_PAGE_SIZE ||
			    ((segment->addr | segment->size |
			    segment->bo_offset) &
			    (page_size - 1)) != 0)
				return (EINVAL);
			if (segment->page_shift != NVKM_GMMU_SPT_SHIFT) {
				uint32_t pages_per_leaf =
				    (uint32_t)(page_size /
				    NVKM_GMMU_PT_PAGE_SIZE);

				if ((segment->paddr & (page_size - 1)) != 0 ||
				    (segment->sysmem_page_count %
				    pages_per_leaf) != 0)
					return (EINVAL);
				for (uint32_t j = 1;
				    j < segment->sysmem_page_count; j++) {
					if ((uint64_t)segment->sysmem_paddrs[j] !=
					    (uint64_t)segment->sysmem_paddrs[0] +
					    (uint64_t)j *
					    NVKM_GMMU_PT_PAGE_SIZE)
						return (EINVAL);
				}
			}
		} else {
			if (segment->page_shift == NVKM_GMMU_SPT_SHIFT)
				page_size = NVKM_GMMU_PT_PAGE_SIZE;
			else if (segment->page_shift == NVKM_GMMU_LPT_SHIFT)
				page_size = NVKM_GMMU_LPT_PAGE_SIZE;
			else if (segment->page_shift == NVKM_GMMU_PD0_SHIFT)
				page_size = NVKM_GMMU_PD0_PAGE_SIZE;
			else
				return (EINVAL);
			if ((segment->addr | segment->paddr |
			    segment->size) & (page_size - 1))
				return (EINVAL);
		}
		err = nvkm_gsp_vmm_check_prepared_pt_range(nfile->vmm,
		    segment->addr, segment->size, segment->page_shift);
		if (err != 0)
			return (err);
	}
	return (0);
}

static int
nvkm_drm_vm_bind_map_segments_preflight(struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    const struct nvkm_bo *bo)
{
	return (nvkm_drm_vm_bind_map_segments_preflight_target(nfile,
	    segments, segment_count,
	    (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0));
}

/*
 * nvkm_drm_vm_bind_map_segments_target_write_noflush()
 *
 * Ownership:
 *   Borrows prepared segment storage, nfile, and dirty_set.  It does not own
 *   GEM refs, BO pins, or VM binding records, and it does not allocate page
 *   table storage.
 *
 * Lifetime:
 *   All preflight that can allocate or fail predictably must already be done
 *   by the caller.  This helper is the commit-side writer used after TTM
 *   rebind has invalidated old PTEs; it only consumes prepared mapping
 *   metadata and records the written dirty ranges.
 *
 * Threading:
 *   Called while the caller holds nfile->vm_token and the enclosing GSP/PTE
 *   write serialization.  It does not flush or publish fences.
 */
static int
nvkm_drm_vm_bind_map_segments_target_write_noflush(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    bool target_vram, uint8_t pte_kind,
    struct nvkm_drm_vm_dirty_set *dirty_set)
{
	int err;

	for (uint32_t i = 0; i < segment_count; i++) {
		if (target_vram) {
			err = nvkm_gsp_vmm_map_vram_flags_page_prepared_noflush(
			    nfile->vmm, segments[i].addr, segments[i].paddr,
			    segments[i].size, 0, 0, pte_kind,
			    segments[i].page_shift);
		} else {
			err =
			    nvkm_gsp_vmm_map_sysmem_paddrs_page_prepared_noflush(
			    nfile->vmm, segments[i].addr,
			    segments[i].sysmem_paddrs,
			    segments[i].sysmem_page_count, pte_kind,
			    segments[i].page_shift);
		}
		if (err != 0)
			return (err);
		nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set,
		    segments[i].addr, segments[i].size);
	}
	return (0);
}

static int
nvkm_drm_vm_bind_map_segments_target_noflush(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    bool target_vram, uint8_t pte_kind,
    struct nvkm_drm_vm_dirty_set *dirty_set)
{
	int err;

	err = nvkm_drm_vm_bind_map_segments_preflight_target(nfile, segments,
	    segment_count, target_vram);
	if (err != 0)
		return (err);
	return (nvkm_drm_vm_bind_map_segments_target_write_noflush(sc, nfile,
	    segments, segment_count, target_vram, pte_kind, dirty_set));
}

static int
nvkm_drm_vm_bind_map_segments_noflush(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    const struct nvkm_bo *bo, uint8_t pte_kind,
    struct nvkm_drm_vm_dirty_set *dirty_set)
{
	return (nvkm_drm_vm_bind_map_segments_target_noflush(sc, nfile,
	    segments, segment_count,
	    (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0, pte_kind,
	    dirty_set));
}

struct nvkm_drm_bo_vm_rebind_entry {
	struct nvkm_drm_vm_bind_segment_plan segment_plan;
	struct nvkm_drm_vm_bind_2m_split_plan split_plan;
	uint8_t target_page_shift;
};

static bool
nvkm_drm_ttm_mem_is_vram(const struct ttm_mem_reg *mem)
{
	return (mem != NULL && mem->mem_type == TTM_PL_VRAM);
}

/*
 * nvkm_drm_bo_vm_rebind_target_shift()
 *
 * Ownership:
 *   Borrows one owned snapshot entry plus a caller-owned target paddr/run
 *   snapshot.  It does not allocate, retain pointers, or mutate VM/BO state.
 *
 * Lifetime:
 *   The returned page shift is consumed immediately by the rebind prepare
 *   plan.  It is valid only for the same target placement snapshot whose
 *   paddr/run values were passed in.
 *
 * Threading:
 *   Pure prepare helper.  TTM move serialization keeps the target placement
 *   stable; no VM token, GSP token, or reservation lock is acquired here.
 */
static uint8_t
nvkm_drm_bo_vm_rebind_target_shift(
    const struct nvkm_drm_bo_vm_snapshot_entry *snapshot_entry, uint64_t paddr,
    uint64_t run_size, bool target_vram)
{
	if (!target_vram)
		return (NVKM_GMMU_SPT_SHIFT);
	if (snapshot_entry->size < NVKM_GMMU_LPT_PAGE_SIZE)
		return (NVKM_GMMU_SPT_SHIFT);
	if (run_size < snapshot_entry->size)
		return (NVKM_GMMU_SPT_SHIFT);
	if (((snapshot_entry->addr | snapshot_entry->bo_offset |
	    snapshot_entry->size | paddr) & (NVKM_GMMU_LPT_PAGE_SIZE - 1)) !=
	    0)
		return (NVKM_GMMU_SPT_SHIFT);

	/*
	 * Keep TTM rebind at 64 KiB for now.  Direct 2 MiB MAP/promotion has
	 * separate gates and soak evidence; the move callback must not bypass
	 * that policy just because a relocated VRAM run happens to be aligned.
	 */
	return (NVKM_GMMU_LPT_SHIFT);
}

/*
 * nvkm_drm_bo_vm_rebind_prepare_entry()
 *
 * Ownership:
 *   Borrows the BO, one owned snapshot entry, and a caller-owned TTM placement
 *   snapshot.  It allocates only temporary segment state for rebind_entry.
 *
 * Lifetime:
 *   The prepared segment plan is valid until
 *   nvkm_drm_bo_vm_rebind_entry_fini().  SYSTEM/TT placements deliberately
 *   use 4 KiB SPT geometry so arbitrary DMA page layouts are expressible.
 *   VRAM placements may restore 64 KiB LPT geometry when the target run and
 *   VA/BO offsets are fully aligned.  If the live binding is a 2 MiB PD0 leaf,
 *   the entry also owns the prepared split needed before lower-page writes.
 *
 * Threading:
 *   Runs outside nfile->vm_token and gsp_tok.  It does not mutate the live VM
 *   tree or BO placement.  The caller must hold TTM move serialization so mem
 *   continues to describe the target placement.
 */
static int
nvkm_drm_bo_vm_rebind_prepare_entry(struct nvkm_bo *bo,
    const struct nvkm_drm_bo_vm_snapshot_entry *snapshot_entry,
    const struct ttm_mem_reg *mem,
    struct nvkm_drm_bo_vm_rebind_entry *rebind_entry)
{
	uint64_t page_size;
	uint64_t old_page_size;
	vm_paddr_t paddr;
	uint64_t run_size;
	int err;

	nvkm_drm_vm_bind_segment_plan_init(&rebind_entry->segment_plan);
	nvkm_drm_vm_bind_2m_split_plan_init(&rebind_entry->split_plan);
	rebind_entry->target_page_shift = NVKM_GMMU_SPT_SHIFT;
	if (snapshot_entry->size == 0 ||
	    snapshot_entry->addr > UINT64_MAX - snapshot_entry->size ||
	    snapshot_entry->bo_offset > UINT64_MAX - snapshot_entry->size)
		return (-EINVAL);
	if (snapshot_entry->obj != &bo->base)
		return (-EINVAL);

	if (snapshot_entry->page_shift == NVKM_GMMU_SPT_SHIFT)
		old_page_size = NVKM_GMMU_PT_PAGE_SIZE;
	else if (snapshot_entry->page_shift == NVKM_GMMU_LPT_SHIFT)
		old_page_size = NVKM_GMMU_LPT_PAGE_SIZE;
	else if (snapshot_entry->page_shift == NVKM_GMMU_PD0_SHIFT)
		old_page_size = NVKM_GMMU_PD0_PAGE_SIZE;
	else
		return (-EINVAL);
	page_size = NVKM_GMMU_PT_PAGE_SIZE;
	if (((snapshot_entry->addr | snapshot_entry->size |
	    snapshot_entry->bo_offset) & (old_page_size - 1)) != 0)
		return (-EINVAL);
	if (((snapshot_entry->addr | snapshot_entry->size |
	    snapshot_entry->bo_offset) & (page_size - 1)) != 0)
		return (-EINVAL);

	if (nvkm_drm_ttm_mem_is_vram(mem)) {
		err = nvkm_bo_paddr_run_at_mem(bo, mem,
		    snapshot_entry->bo_offset, snapshot_entry->size, &paddr,
		    &run_size);
		if (err != 0)
			return (-err);
		rebind_entry->target_page_shift =
		    nvkm_drm_bo_vm_rebind_target_shift(snapshot_entry, paddr,
		    run_size, true);
		if (run_size < snapshot_entry->size ||
		    ((uint64_t)paddr & (page_size - 1)) != 0)
			return (-EINVAL);
		err = nvkm_drm_vm_bind_segment_add(
		    &rebind_entry->segment_plan, snapshot_entry->addr,
		    snapshot_entry->size, snapshot_entry->bo_offset, paddr,
		    rebind_entry->target_page_shift);
	} else {
		err = nvkm_drm_vm_bind_segment_add_sysmem_mem(
		    &rebind_entry->segment_plan, bo, mem, snapshot_entry->addr,
		    snapshot_entry->size, snapshot_entry->bo_offset,
		    rebind_entry->target_page_shift);
	}
	if (err != 0)
		return (err);

	if (snapshot_entry->page_shift == NVKM_GMMU_PD0_SHIFT &&
	    rebind_entry->target_page_shift < NVKM_GMMU_PD0_SHIFT) {
		uint64_t end = snapshot_entry->addr + snapshot_entry->size;

		for (uint64_t va = snapshot_entry->addr; va < end;
		    va += NVKM_GMMU_PD0_PAGE_SIZE) {
			err = nvkm_drm_vm_bind_2m_split_plan_prepare(
			    snapshot_entry->owner->vmm,
			    &rebind_entry->split_plan, va);
			if (err != 0)
				return (err);
		}
	}
	return (0);
}

/*
 * nvkm_drm_bo_vm_rebind_entry_fini()
 *
 * Ownership:
 *   Releases every temporary object owned by one TTM rebind entry.  Committed
 *   split PTs have already transferred to the VMM; uncommitted split PTs are
 *   aborted here.
 *
 * Lifetime:
 *   Called after success or failure of the enclosing snapshot rebind.  It does
 *   not touch live VM binding ownership, GEM refs, or BO pins.
 *
 * Threading:
 *   Runs after the caller has released VM/GSP tokens.  It may free private BAR1
 *   page-table allocations that were never linked into the VMM.
 */
static void
nvkm_drm_bo_vm_rebind_entry_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_bo_vm_rebind_entry *entry)
{
	nvkm_drm_vm_bind_2m_split_plan_fini(nfile->vmm, &entry->split_plan);
	nvkm_drm_vm_bind_segment_plan_fini(&entry->segment_plan);
}

/*
 * nvkm_drm_bo_vm_rebind_commit_splits()
 *
 * Ownership:
 *   Consumes only split PT ownership already prepared in entry.  On success,
 *   each committed child PT transfers to nfile->vmm; segment and binding
 *   ownership stay with the caller.
 *
 * Lifetime:
 *   Must run immediately before the prepared SPT writer that consumes the
 *   newly linked child PTs.  If it fails, any uncommitted split PTs remain
 *   owned by entry and will be released by rebind_entry_fini().
 *
 * Threading:
 *   Runs in the TTM rebind commit section while nfile->vm_token and gsp_tok
 *   are held.  It does not allocate, pin, or wait on BO reservations.
 */
static int
nvkm_drm_bo_vm_rebind_commit_splits(struct nvkm_drm_file *nfile,
    struct nvkm_drm_bo_vm_rebind_entry *entry)
{
	int err;

	for (uint32_t i = 0; i < entry->split_plan.count; i++) {
		err = nvkm_drm_vm_bind_2m_split_plan_commit(nfile->vmm,
		    &entry->split_plan, entry->split_plan.splits[i].addr);
		if (err != 0)
			return (err < 0 ? -err : err);
	}
	return (0);
}

static bool
nvkm_drm_bo_vm_snapshot_entry_matches_live(
    const struct nvkm_drm_bo_vm_snapshot_entry *entry,
    const struct nvkm_drm_vm_binding *binding)
{
	if (binding == NULL)
		return (false);
	return (binding->addr == entry->addr &&
	    binding->size == entry->size &&
	    binding->bo_offset == entry->bo_offset &&
	    binding->obj == entry->obj &&
	    binding->pte_kind == entry->pte_kind &&
	    binding->pte_installed &&
	    binding->bo_pinned);
}

/*
 * nvkm_drm_bo_vm_snapshot_owner_count()
 *
 * Ownership:
 *   Borrows snapshot and every owner pointer stored in it.  It does not take
 *   or drop references; snapshot already owns stable nfile refs.
 *
 * Lifetime:
 *   The returned count is telemetry for this rebind attempt only.  It must be
 *   consumed before nvkm_drm_bo_vm_snapshot_fini() releases the owner refs.
 *
 * Threading:
 *   Runs outside VM and GSP tokens.  Snapshot contents are immutable after
 *   collect, so no additional locking is required.
 */
static uint32_t
nvkm_drm_bo_vm_snapshot_owner_count(
    const struct nvkm_drm_bo_vm_snapshot *snapshot)
{
	uint32_t count = 0;

	for (uint32_t i = 0; i < snapshot->count; i++) {
		bool seen = false;

		for (uint32_t j = 0; j < i; j++) {
			if (snapshot->entries[j].owner ==
			    snapshot->entries[i].owner) {
				seen = true;
				break;
			}
		}
		if (!seen)
			count++;
	}
	return (count);
}

/*
 * nvkm_drm_bo_vm_snapshot_wait_exec_resv()
 *
 * Ownership:
 *   Borrows snapshot and the owner references already held by it.  The helper
 *   does not retain fences, VM owners, BOs, or GEM objects after return.
 *
 * Lifetime:
 *   Must be called before a TTM live-bound move rewrites PTEs.  It waits the
 *   EXEC-only reservation fence for each unique owner so already-published GPU
 *   work in that VM cannot keep using the old VA->BO interpretation while TTM
 *   unmaps/remaps the backing.  It deliberately ignores VM_BIND bookkeeping
 *   fences; otherwise a VM_BIND job that triggers TTM validation could wait on
 *   its own completion fence.
 *
 * Threading:
 *   Called without nfile->vm_token or sc->gsp_tok.  It may sleep unless
 *   no_wait is true.  The caller owns TTM move serialization and keeps the
 *   snapshot owner references valid.
 */
int
nvkm_drm_bo_vm_snapshot_wait_exec_resv(struct nvkm_softc *sc,
    const struct nvkm_drm_bo_vm_snapshot *snapshot, bool interruptible,
    bool no_wait)
{
	if (snapshot == NULL)
		return (-EINVAL);
	if (snapshot->count == 0)
		return (0);

	sc->ttm_rebind_exec_resv_wait_count++;
	for (uint32_t i = 0; i < snapshot->count; i++) {
		struct nvkm_drm_file *owner = snapshot->entries[i].owner;
		long ret;
		bool seen = false;

		for (uint32_t j = 0; j < i; j++) {
			if (snapshot->entries[j].owner == owner) {
				seen = true;
				break;
			}
		}
		if (seen)
			continue;

		sc->ttm_rebind_exec_resv_wait_owner_count++;
		if (no_wait) {
			if (reservation_object_test_signaled_rcu(
			    &owner->vm_exec_resv, true))
				continue;
			sc->ttm_rebind_exec_resv_wait_error_count++;
			sc->ttm_rebind_exec_resv_wait_last_error = -EBUSY;
			return (-EBUSY);
		}

		ret = reservation_object_wait_timeout_rcu(&owner->vm_exec_resv,
		    true, interruptible, 15 * HZ);
		if (ret < 0) {
			sc->ttm_rebind_exec_resv_wait_error_count++;
			sc->ttm_rebind_exec_resv_wait_last_error = (int)ret;
			return ((int)ret);
		}
		if (ret == 0) {
			sc->ttm_rebind_exec_resv_wait_error_count++;
			sc->ttm_rebind_exec_resv_wait_last_error = -EBUSY;
			return (-EBUSY);
		}
	}
	sc->ttm_rebind_exec_resv_wait_last_error = 0;
	return (0);
}

/*
 * nvkm_drm_bo_vm_rebind_note_page_shift()
 *
 * Ownership:
 *   Borrows sc and updates only diagnostic counters.
 *
 * Lifetime:
 *   Called after one live binding has been successfully rebound and its
 *   metadata now reflects the new page shift.  Failed prepare or commit paths
 *   must not call this helper.
 *
 * Threading:
 *   TTM rebind is serialized by the active move path.  These counters are
 *   diagnostic and follow the surrounding nvkm state counter convention.
 */
static void
nvkm_drm_bo_vm_rebind_note_page_shift(struct nvkm_softc *sc,
    uint8_t old_shift, uint8_t new_shift, uint64_t pages)
{
	uint32_t old_bucket = nvkm_drm_vm_bind_page_shift_bucket(old_shift);
	uint32_t new_bucket = nvkm_drm_vm_bind_page_shift_bucket(new_shift);

	sc->ttm_rebind_page_shift_old_count[old_bucket]++;
	sc->ttm_rebind_page_shift_old_pages[old_bucket] += pages;
	sc->ttm_rebind_page_shift_new_count[new_bucket]++;
	sc->ttm_rebind_page_shift_new_pages[new_bucket] += pages;
}

/*
 * nvkm_drm_bo_vm_rebind_note_error()
 *
 * Ownership:
 *   Borrows sc and an optional immutable snapshot entry.  It stores only
 *   scalar diagnostics and never retains snapshot, BO, VM, or mapping
 *   ownership.
 *
 * Lifetime:
 *   Values describe the most recent TTM rebind failure.  They are diagnostic
 *   breadcrumbs for dev.drm.0.state and do not participate in recovery.
 *
 * Threading:
 *   Called from the serialized TTM move/rebind path.  The counters are
 *   best-effort diagnostics and follow the existing nvkm state counter model.
 */
static void
nvkm_drm_bo_vm_rebind_note_error(struct nvkm_softc *sc,
    enum nvkm_drm_ttm_rebind_error_stage stage, int err, uint32_t index,
    uint32_t count, const struct nvkm_drm_bo_vm_snapshot_entry *snap,
    const struct nvkm_drm_bo_vm_rebind_entry *entry, bool target_vram)
{
	sc->ttm_rebind_last_error = err < 0 ? err : -err;
	sc->ttm_rebind_last_error_stage = stage;
	sc->ttm_rebind_last_error_index = index;
	sc->ttm_rebind_last_error_count = count;
	sc->ttm_rebind_last_target_vram = target_vram ? 1 : 0;
	sc->ttm_rebind_last_error_addr = 0;
	sc->ttm_rebind_last_error_size = 0;
	sc->ttm_rebind_last_error_bo_offset = 0;
	sc->ttm_rebind_last_old_shift = 0;
	sc->ttm_rebind_last_target_shift = 0;
	if (snap != NULL) {
		sc->ttm_rebind_last_error_addr = snap->addr;
		sc->ttm_rebind_last_error_size = snap->size;
		sc->ttm_rebind_last_error_bo_offset = snap->bo_offset;
		sc->ttm_rebind_last_old_shift = snap->page_shift;
	}
	if (entry != NULL)
		sc->ttm_rebind_last_target_shift = entry->target_page_shift;
}

/*
 * nvkm_drm_bo_vm_snapshot_rebind()
 *
 * Ownership:
 *   Borrows bo, snapshot, and mem.  snapshot owns stable owner/GEM references
 *   for every entry, but this helper does not consume them.  It owns temporary
 *   segment plans until return.
 *
 * Lifetime:
 *   mem must be an old_mem or new_mem snapshot from the active TTM move.  The
 *   helper first invalidates the old live PTEs and flushes that dirty range,
 *   then installs prepared target PTEs and flushes again.  It does not change
 *   GEM refs, VM_BIND pins, BO reverse-list membership, or VA interval nodes.
 *   The live binding's page_shift may be lowered to SPT after the prepared PTE
 *   write succeeds so software metadata stays isomorphic with hardware page
 *   tables.  Prepare preflights SPT/LPT invalidation with target PT
 *   preservation.  PD0 snapshots are preflighted as full-window clears because
 *   target SPT storage exists only after the prepared split commits.
 *
 * Threading:
 *   Prepare runs without VM/GSP tokens.  Commit takes each owner vm_token
 *   before gsp_tok, verifies the live binding still exactly matches the owned
 *   snapshot, writes prepared PTEs, and flushes before releasing tokens.  The
 *   caller is responsible for TTM move/evict ordering against GPU fences.
 */
int
nvkm_drm_bo_vm_snapshot_rebind(struct nvkm_bo *bo,
    const struct nvkm_drm_bo_vm_snapshot *snapshot,
    const struct ttm_mem_reg *mem, bool rollback)
{
	struct nvkm_softc *sc = bo->base.dev->dev_private;
	struct nvkm_drm_bo_vm_rebind_entry *entries;
	bool target_vram = nvkm_drm_ttm_mem_is_vram(mem);
	bool commit_started = false;
	int err = 0;

	if (snapshot == NULL || mem == NULL)
		return (-EINVAL);
	if (snapshot->count == 0)
		return (0);

	sc->ttm_rebind_prepare_count++;
	entries = kcalloc(snapshot->count, sizeof(*entries), GFP_KERNEL);
	if (entries == NULL) {
		sc->ttm_rebind_prepare_error_count++;
		sc->ttm_rebind_abort_count++;
		nvkm_drm_bo_vm_rebind_note_error(sc,
		    NVKM_DRM_TTM_REBIND_ERROR_ALLOC, -ENOMEM, 0,
		    snapshot->count, NULL, NULL, target_vram);
		return (-ENOMEM);
	}

	for (uint32_t i = 0; i < snapshot->count; i++) {
		const struct nvkm_drm_bo_vm_snapshot_entry *snap =
		    &snapshot->entries[i];
		bool unmap_preserve_ready;

		err = nvkm_drm_bo_vm_rebind_prepare_entry(bo,
		    snap, mem, &entries[i]);
		if (err != 0) {
			sc->ttm_rebind_prepare_error_count++;
			nvkm_drm_bo_vm_rebind_note_error(sc,
			    NVKM_DRM_TTM_REBIND_ERROR_ENTRY_PREPARE, err, i,
			    snapshot->count, snap, &entries[i], target_vram);
			goto out_fini;
		}
		unmap_preserve_ready =
		    snap->page_shift != NVKM_GMMU_PD0_SHIFT;
		err = nvkm_gsp_vmm_check_unmap_valid_range_page(
		    snap->owner->vmm, snap->addr, snap->size,
		    unmap_preserve_ready, entries[i].target_page_shift);
		if (err != 0) {
			sc->ttm_rebind_prepare_error_count++;
			nvkm_drm_bo_vm_rebind_note_error(sc,
			    NVKM_DRM_TTM_REBIND_ERROR_UNMAP_PREFLIGHT, err, i,
			    snapshot->count, snap, &entries[i], target_vram);
			err = -err;
			goto out_fini;
		}
		err = nvkm_drm_vm_bind_map_segments_preflight_target(
		    snap->owner, entries[i].segment_plan.segments,
		    entries[i].segment_plan.count, target_vram);
		if (err != 0 && entries[i].split_plan.count == 0) {
			sc->ttm_rebind_prepare_error_count++;
			nvkm_drm_bo_vm_rebind_note_error(sc,
			    NVKM_DRM_TTM_REBIND_ERROR_TARGET_PREFLIGHT, err, i,
			    snapshot->count, snap, &entries[i], target_vram);
			err = -err;
			goto out_fini;
		}
		err = nvkm_drm_vm_bind_2m_split_plan_preflight(
		    snapshot->entries[i].owner->vmm, &entries[i].split_plan);
		if (err != 0) {
			sc->ttm_rebind_prepare_error_count++;
			nvkm_drm_bo_vm_rebind_note_error(sc,
			    NVKM_DRM_TTM_REBIND_ERROR_SPLIT_PREFLIGHT, err, i,
			    snapshot->count, snap, &entries[i], target_vram);
			goto out_fini;
		}
	}

	if (rollback)
		sc->ttm_rebind_rollback_count++;
	else
		sc->ttm_rebind_map_count++;
	sc->ttm_rebind_vm_count +=
	    nvkm_drm_bo_vm_snapshot_owner_count(snapshot);
	commit_started = true;

	for (uint32_t i = 0; i < snapshot->count; i++) {
		const struct nvkm_drm_bo_vm_snapshot_entry *snap =
		    &snapshot->entries[i];
		struct nvkm_drm_file *nfile = snap->owner;
		struct nvkm_drm_vm_binding *binding;
		struct nvkm_drm_vm_dirty_set map_dirty;
		struct nvkm_drm_vm_dirty_set unmap_dirty;
		uint64_t pages = snap->size / NVKM_GMMU_PT_PAGE_SIZE;
		int write_err;
		enum nvkm_drm_ttm_rebind_error_stage write_stage =
		    NVKM_DRM_TTM_REBIND_ERROR_NONE;

		nvkm_drm_vm_dirty_set_init(&unmap_dirty);
		nvkm_drm_vm_dirty_set_init(&map_dirty);
		lwkt_gettoken(&nfile->vm_token);
		lwkt_gettoken(&sc->gsp_tok);
		binding = nvkm_drm_vm_binding_first_overlap(nfile,
		    snap->addr, snap->size);
		if (!nvkm_drm_bo_vm_snapshot_entry_matches_live(snap,
		    binding)) {
			write_err = ENOENT;
			write_stage = NVKM_DRM_TTM_REBIND_ERROR_COMMIT_LIVE;
		} else {
			write_err = nvkm_drm_bo_vm_rebind_commit_splits(nfile,
			    &entries[i]);
			if (write_err != 0)
				write_stage =
				    NVKM_DRM_TTM_REBIND_ERROR_COMMIT_SPLIT;
			if (write_err == 0) {
				write_err =
				    nvkm_gsp_vmm_unmap_valid_preserve_page_noflush(
				    nfile->vmm, snap->addr, snap->size,
				    entries[i].target_page_shift);
				if (write_err != 0)
					write_stage =
					    NVKM_DRM_TTM_REBIND_ERROR_COMMIT_UNMAP;
				if (write_err == 0)
					nvkm_drm_vm_bind_note_dirty_range(sc,
					    &unmap_dirty, snap->addr,
					    snap->size);
			}
			if (write_err == 0) {
				nvkm_drm_vm_dirty_set_publish(sc,
				    &unmap_dirty);
				if (unmap_dirty.dirty) {
					sc->ttm_rebind_unmap_count++;
					sc->ttm_rebind_unmap_pages += pages;
					sc->ttm_rebind_flush_count++;
					nvkm_drm_vm_dirty_set_flush(nfile->vmm,
					    &unmap_dirty);
				}
				write_err =
				    nvkm_drm_vm_bind_map_segments_target_write_noflush(
				    sc, nfile,
				    entries[i].segment_plan.segments,
				    entries[i].segment_plan.count, target_vram,
				    snap->pte_kind, &map_dirty);
				if (write_err != 0)
					write_stage =
					    NVKM_DRM_TTM_REBIND_ERROR_COMMIT_MAP;
			}
			if (write_err == 0)
				binding->page_shift =
				    entries[i].target_page_shift;
			if (write_err == 0) {
				if (target_vram ||
				    !nvkm_bo_ttm_prefers_vram(bo))
					nvkm_drm_vm_binding_validate_clear(
					    binding);
				else
					nvkm_drm_vm_binding_validate_mark(
					    binding);
			}
		}
		nvkm_drm_vm_dirty_set_publish(sc, &map_dirty);
		if (map_dirty.dirty) {
			sc->ttm_rebind_flush_count++;
			nvkm_drm_vm_dirty_set_flush(nfile->vmm, &map_dirty);
		}
		lwkt_reltoken(&sc->gsp_tok);
		lwkt_reltoken(&nfile->vm_token);

		if (write_err != 0) {
			if (rollback)
				sc->ttm_rebind_rollback_error_count++;
			else
				sc->ttm_rebind_map_error_count++;
			nvkm_drm_bo_vm_rebind_note_error(sc, write_stage,
			    write_err, i, snapshot->count, snap, &entries[i],
			    target_vram);
			err = -write_err;
			goto out_fini;
		}
		sc->ttm_rebind_binding_count++;
		sc->ttm_rebind_pages += pages;
		nvkm_drm_bo_vm_rebind_note_page_shift(sc, snap->page_shift,
		    entries[i].target_page_shift, pages);
	}

out_fini:
	if (err != 0 && !commit_started)
		sc->ttm_rebind_abort_count++;
	for (uint32_t i = 0; i < snapshot->count; i++)
		nvkm_drm_bo_vm_rebind_entry_fini(snapshot->entries[i].owner,
		    &entries[i]);
	kfree(entries);
	return (err);
}

static void
nvkm_drm_vm_bind_note_map_segments(struct nvkm_softc *sc, uint32_t flags,
	    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
	    const struct nvkm_bo *bo)
{
	uint64_t total_pages = 0;
	uint32_t domain_bucket = 0;
	bool valid_bo = bo != NULL;

	if (valid_bo)
		domain_bucket = nvkm_drm_vm_bind_domain_bucket(bo);

	for (uint32_t i = 0; i < segment_count; i++) {
		uint64_t pages = segments[i].size / NVKM_GMMU_PT_PAGE_SIZE;
		uint32_t bucket =
		    nvkm_drm_vm_bind_page_shift_bucket(segments[i].page_shift);

		nvkm_drm_vm_bind_note_map_shape(sc, flags, segments[i].size);
		sc->vm_bind_page_shift_map_count[bucket]++;
		sc->vm_bind_page_shift_map_pages[bucket] += pages;
		if (valid_bo) {
			sc->vm_bind_valid_page_shift_map_count[domain_bucket][bucket]++;
			sc->vm_bind_valid_page_shift_map_pages[domain_bucket][bucket] +=
			    pages;
		}
		total_pages += pages;
	}
	sc->vm_bind_map_segment_count += segment_count;
	sc->vm_bind_map_segment_pages += total_pages;
	if (segment_count > 1) {
		sc->vm_bind_map_split_count++;
		sc->vm_bind_map_split_pages += total_pages;
	}
}

static void
nvkm_drm_vm_bind_debug_segments(struct nvkm_softc *sc,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count)
{
	for (uint32_t i = 0; i < segment_count; i++) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND segment addr=0x%016jx size=0x%016jx bo_off=0x%016jx shift=%u\n",
		    (uintmax_t)segments[i].addr,
		    (uintmax_t)segments[i].size,
		    (uintmax_t)segments[i].bo_offset,
		    segments[i].page_shift);
	}
}

static void
nvkm_drm_vm_bind_mark_bo_tiled(struct nvkm_bo *bo, uint8_t pte_kind)
{
	if (pte_kind == 0)
		return;

	if (!bo->vm_bound_tiled)
		bo->vm_bound_kind = pte_kind;
	else if (bo->vm_bound_kind != pte_kind)
		bo->vm_bound_mixed_kind = true;
	bo->vm_bound_tiled = true;
}

static void
nvkm_drm_vm_bind_put_op_object(struct drm_gem_object *obj, bool job_object)
{
	if (obj == NULL)
		return;
	if (!job_object)
		drm_gem_object_put_unlocked(obj);
}

static void
nvkm_drm_vm_binding_insert_sorted(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_drm_vm_binding *prev;
	uint64_t end = binding->addr + binding->size;

	nvkm_drm_vm_binding_tree_insert(nfile, binding);
	if (nfile->vm_bindings_max_end < end)
		nfile->vm_bindings_max_end = end;

	prev = nvkm_drm_vm_binding_tree_RB_PREV(binding);
	if (prev != NULL)
		LIST_INSERT_AFTER(prev, binding, link);
	else
		LIST_INSERT_HEAD(&nfile->vm_bindings, binding, link);

	nvkm_drm_vm_binding_bo_attach(binding);
}

static void
nvkm_drm_vm_bindings_recalc_max_end(struct nvkm_drm_file *nfile)
{
	struct nvkm_drm_vm_binding *binding;
	uint64_t max_end = 0;

	LIST_FOREACH(binding, &nfile->vm_bindings, link) {
		uint64_t end = binding->addr + binding->size;

		if (max_end < end)
			max_end = end;
	}
	nfile->vm_bindings_max_end = max_end;
}

static int
nvkm_drm_vm_binding_pin(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_bo *bo;
	int err;

	nvkm_drm_vm_binding_assert(binding);
	if (binding->bo_pinned)
		return (0);

	bo = to_nvkm_bo(binding->obj);
	err = nvkm_bo_vm_bind_pin(bo, &binding->bo_no_evict_pinned);
	if (err != 0)
		return (err);
	binding->bo_pinned = true;
	return (0);
}

static int
nvkm_drm_vm_binding_unpin(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_bo *bo;
	int err;

	nvkm_drm_vm_binding_assert(binding);
	if (!binding->bo_pinned)
		return (0);

	bo = to_nvkm_bo(binding->obj);
	err = nvkm_bo_vm_bind_unpin(bo, binding->bo_no_evict_pinned);
	if (err == 0) {
		binding->bo_pinned = false;
		binding->bo_no_evict_pinned = false;
	}
	return (err);
}

static void
nvkm_drm_vm_binding_free(struct nvkm_drm_vm_binding *binding)
{
	struct nvkm_softc *sc = binding->obj->dev->dev_private;
	int err;

	nvkm_drm_vm_binding_assert(binding);
	KASSERT(!binding->bo_linked,
	    ("nvkm_drm: freeing live BO reverse mapping addr=0x%016jx",
	    (uintmax_t)binding->addr));
	KASSERT(!binding->validate_linked,
	    ("nvkm_drm: freeing validate-listed VM binding addr=0x%016jx",
	    (uintmax_t)binding->addr));
	err = nvkm_drm_vm_binding_unpin(binding);
	if (err != 0)
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND unpin failed obj=%p err=%d\n",
		    binding->obj, err);
	drm_gem_object_put_unlocked(binding->obj);
	kfree(binding);
}

static void
nvkm_drm_vm_binding_unlink_free(struct nvkm_drm_vm_binding *binding)
{
	nvkm_drm_vm_binding_bo_detach(binding);
	nvkm_drm_vm_binding_tree_remove(binding->owner, binding);
	LIST_REMOVE(binding, link);
	nvkm_drm_vm_binding_free(binding);
}

/*
 * nvkm_drm_vm_bindings_release()
 *
 * Ownership:
 *   Consumes every binding currently linked on bindings.  Each binding owns a
 *   GEM reference and may own a VM_BIND BO pin; both are released here.
 *
 * Lifetime:
 *   The list head remains owned by the caller and is empty on return.  The
 *   return value is the number of consumed bindings.
 *
 * Threading:
 *   The caller must own the detached list exclusively.  This helper may sleep
 *   through GEM/TTM destruction and reservation-object waits.
 */
static uint32_t
nvkm_drm_vm_bindings_release(
    struct nvkm_drm_vm_binding_list *bindings)
{
	struct nvkm_drm_vm_binding *binding;
	uint32_t count = 0;

	while ((binding = LIST_FIRST(bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_free(binding);
		count++;
	}
	return (count);
}

/*
 * nvkm_drm_vm_bind_retire_work()
 *
 * Ownership:
 *   Consumes the retire record handed to system_unbound_wq by
 *   nvkm_drm_vm_bind_retire_schedule().
 *
 * Lifetime:
 *   Runs after the VM_BIND done fence has been signaled.  Frees the detached
 *   list and then frees the retire record itself.
 *
 * Threading:
 *   Runs outside the ordered VM_BIND/EXEC job worker and may sleep in GEM/TTM
 *   destruction.
 */
static void
nvkm_drm_vm_bind_retire_work(struct work_struct *work)
{
	struct nvkm_drm_vm_bind_retire *retire =
	    container_of(work, struct nvkm_drm_vm_bind_retire, work);
	struct nvkm_softc *sc = retire->sc;
	uint32_t count;

	count = nvkm_drm_vm_bindings_release(&retire->bindings);
	if (sc != NULL) {
		sc->vm_bind_retire_work_count++;
		sc->vm_bind_retire_work_binding_count += count;
	}
	kfree(retire);
}

/*
 * nvkm_drm_vm_bind_retire_free()
 *
 * Ownership:
 *   Consumes retire when a VM_BIND job fails before scheduling cleanup.
 *
 * Lifetime:
 *   Used only while the job still owns retire.  A scheduled retire record must
 *   not be passed here because system_unbound_wq owns it.
 *
 * Threading:
 *   May sleep while releasing any bindings left on the list.  Normal failed
 *   submit paths call it with an empty list.
 */
static void
nvkm_drm_vm_bind_retire_free(struct nvkm_drm_vm_bind_retire *retire)
{
	struct nvkm_softc *sc;
	uint32_t count;

	if (retire == NULL)
		return;
	sc = retire->sc;
	count = nvkm_drm_vm_bindings_release(&retire->bindings);
	if (sc != NULL) {
		sc->vm_bind_retire_free_count++;
		sc->vm_bind_retire_free_binding_count += count;
	}
	kfree(retire);
}

/*
 * nvkm_drm_vm_bind_retire_schedule()
 *
 * Ownership:
 *   Consumes *pretire.  On success, system_unbound_wq owns the retire record
 *   and the caller's pointer is cleared.  Empty lists are freed immediately.
 *
 * Lifetime:
 *   Must be called only after the VM_BIND done fence has been signaled.
 *   Delaying old binding release until this point prevents no_share BO final
 *   put from waiting on the same VM_BIND fence that is currently completing.
 *
 * Threading:
 *   Intended for job-worker context.  Non-empty cleanup is queued to the
 *   unbound workqueue so the ordered VM_BIND/EXEC worker cannot block on
 *   reservation waits for later jobs published to the same per-file vm_resv.
 */
static void
nvkm_drm_vm_bind_retire_schedule(
    struct nvkm_drm_vm_bind_retire **pretire)
{
	struct nvkm_drm_vm_bind_retire *retire = *pretire;
	struct nvkm_softc *sc;

	if (retire == NULL)
		return;
	sc = retire->sc;
	if (sc != NULL)
		sc->vm_bind_retire_schedule_count++;
	*pretire = NULL;
	if (LIST_FIRST(&retire->bindings) == NULL) {
		if (sc != NULL)
			sc->vm_bind_retire_empty_count++;
		kfree(retire);
		return;
	}
	if (!queue_work(system_unbound_wq, &retire->work)) {
		if (sc != NULL)
			sc->vm_bind_retire_queue_error_count++;
		nvkm_drm_vm_bind_retire_free(retire);
	}
}

/*
 * nvkm_drm_vm_binding_unlink_retire()
 *
 * Ownership:
 *   Moves a live VM binding from nfile->vm_bindings into retired_bindings.
 *   The binding keeps owning its GEM reference and VM_BIND pin.
 *
 * Lifetime:
 *   Used after the VM_BIND job has updated PTEs but before its done fence is
 *   signaled.  The retired list is released after the done fence is visible.
 *
 * Threading:
 *   Requires the caller to hold the per-file vm_token that serializes the live
 *   binding tracker.  The retired list is detached from other threads.
 */
static void
nvkm_drm_vm_binding_unlink_retire(struct nvkm_drm_vm_binding *binding,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	nvkm_drm_vm_binding_bo_detach(binding);
	nvkm_drm_vm_binding_tree_remove(binding->owner, binding);
	LIST_REMOVE(binding, link);
	LIST_INSERT_HEAD(retired_bindings, binding, link);
}

/*
 * nvkm_drm_vm_bindings_can_merge()
 *
 * Ownership:
 *   Borrows two live binding records.  It does not acquire, transfer, or drop
 *   GEM references, BO pins, or VMM ownership.
 *
 * Lifetime:
 *   The result is valid only while both bindings stay linked in the same VM
 *   tracker and no caller mutates their VA or BO-offset ranges.
 *
 * Threading:
 *   Requires nfile->vm_token through the caller.  This is a pure predicate for
 *   software mapping coalescing and does not inspect or write hardware PTEs.
 */
static bool
nvkm_drm_vm_bindings_can_merge(const struct nvkm_drm_vm_binding *left,
    const struct nvkm_drm_vm_binding *right)
{
	if (left == NULL || right == NULL)
		return (false);
	if (left->owner != right->owner)
		return (false);
	if (!left->pte_installed || !right->pte_installed)
		return (false);
	if (!left->bo_pinned || !right->bo_pinned)
		return (false);
	if (left->addr > UINT64_MAX - left->size ||
	    left->bo_offset > UINT64_MAX - left->size)
		return (false);
	if (right->addr > UINT64_MAX - right->size ||
	    right->bo_offset > UINT64_MAX - right->size)
		return (false);
	if (left->size > UINT64_MAX - right->size)
		return (false);
	if (left->addr + left->size != right->addr)
		return (false);
	if (left->bo_offset + left->size != right->bo_offset)
		return (false);
	if (left->obj != right->obj)
		return (false);
	if (left->pte_kind != right->pte_kind)
		return (false);
	if (left->page_shift != right->page_shift)
		return (false);
	return (true);
}

/*
 * nvkm_drm_vm_bindings_merge_neighbors()
 *
 * Ownership:
 *   Mutates nfile's live mapping tracker and moves merged-away right-hand
 *   bindings to retired_bindings.  The surviving left binding keeps one GEM
 *   reference and one VM_BIND BO pin for the merged VA interval; retired
 *   bindings keep their original ownership until the VM_BIND done fence is
 *   visible.
 *
 * Lifetime:
 *   The helper never changes hardware PTEs.  It only coalesces adjacent
 *   software mappings whose PTE representation is already identical, so the
 *   mapping tree remains isomorphic with the existing page table leaves.
 *
 * Threading:
 *   Requires nfile->vm_token.  Call after the enclosing remap has installed
 *   PTEs and spliced new mappings, before publishing the VM_BIND done fence.
 */
static void
nvkm_drm_vm_bindings_merge_neighbors(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_binding *binding, *prev, *right;
	uint64_t end = addr + size;
	uint64_t merged_pages = 0;
	uint64_t merged_count = 0;

	if (size == 0 || LIST_FIRST(&nfile->vm_bindings) == NULL)
		return;
	if (addr > UINT64_MAX - size)
		return;

	binding = nvkm_drm_vm_binding_tree_lower_bound(nfile, addr);
	if (binding != NULL) {
		prev = nvkm_drm_vm_binding_tree_RB_PREV(binding);
		if (prev != NULL)
			binding = prev;
	} else {
		binding = nvkm_drm_vm_binding_tree_RB_MINMAX(
		    &nfile->vm_binding_tree, 1);
	}

	while (binding != NULL) {
		uint64_t right_size;

		if (binding->addr > end)
			break;
		right = nvkm_drm_vm_binding_tree_RB_NEXT(binding);
		if (right == NULL)
			break;
		if (!nvkm_drm_vm_bindings_can_merge(binding, right)) {
			binding = right;
			continue;
		}

		right_size = right->size;
		right->pte_installed = false;
		nvkm_drm_vm_binding_unlink_retire(right, retired_bindings);
		binding->size += right_size;
		merged_count++;
		merged_pages += right_size / NVKM_GMMU_PT_PAGE_SIZE;
	}

	if (merged_count == 0)
		return;
	sc->vm_bind_mapping_merge_count += merged_count;
	sc->vm_bind_mapping_merge_pages += merged_pages;
	nvkm_drm_vm_bindings_recalc_max_end(nfile);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND merged mappings count=%ju pages=%ju range=0x%016jx+0x%016jx\n",
	    (uintmax_t)merged_count, (uintmax_t)merged_pages,
	    (uintmax_t)addr, (uintmax_t)size);
}

static void
nvkm_drm_vm_bind_note_promote(struct nvkm_softc *sc, uint8_t page_shift,
    uint64_t size)
{
	uint32_t bucket = nvkm_drm_vm_bind_page_shift_bucket(page_shift);

	sc->vm_bind_promote_count[bucket]++;
	sc->vm_bind_promote_pages[bucket] += size / NVKM_GMMU_PT_PAGE_SIZE;
}

static void
nvkm_drm_vm_bind_note_promote_split(struct nvkm_softc *sc, uint64_t size)
{
	sc->vm_bind_promote_split_count++;
	sc->vm_bind_promote_split_pages += size / NVKM_GMMU_PT_PAGE_SIZE;
}

/*
 * nvkm_drm_vm_binding_find_promote_64k()
 *
 * Ownership:
 *   Borrows one live binding and its BO.  It does not acquire or release GEM
 *   references, BO pins, or VMM ownership.
 *
 * Lifetime:
 *   The predicate is valid only while the caller holds nfile->vm_token and the
 *   binding remains live.  Returned ranges are immediate values copied from
 *   the binding and BO.  The binding's existing VM_BIND pin keeps the BO
 *   physical run stable for the promotion decision.
 *
 * Threading:
 *   Pure VM_BIND normalize helper.  Hardware PTEs are not inspected here; the
 *   mapping tree is the software source of truth.
 */
static bool
nvkm_drm_vm_binding_find_promote_64k(
    const struct nvkm_drm_vm_binding *binding, bool allow_host_large,
    uint64_t *pmid_addr, uint64_t *pmid_size, uint64_t *pmid_bo_offset,
    uint64_t *ppaddr)
{
	const struct nvkm_bo *bo;
	vm_paddr_t paddr;
	uint64_t mask = NVKM_GMMU_LPT_PAGE_SIZE - 1;
	uint64_t old_end, mid_start, mid_end, mid_size, mid_bo_offset;
	uint64_t run_size;
	int err;

	if (binding == NULL || !binding->pte_installed ||
	    !binding->bo_pinned)
		return (false);
	if (binding->page_shift != NVKM_GMMU_SPT_SHIFT)
		return (false);
	if (binding->addr > UINT64_MAX - binding->size)
		return (false);
	if (binding->addr > UINT64_MAX - mask)
		return (false);
	old_end = binding->addr + binding->size;
	mid_start = (binding->addr + mask) & ~mask;
	mid_end = old_end & ~mask;
	if (mid_end <= mid_start)
		return (false);
	mid_size = mid_end - mid_start;
	if ((mid_start - binding->addr) > UINT64_MAX - binding->bo_offset)
		return (false);
	mid_bo_offset = binding->bo_offset + (mid_start - binding->addr);
	if (mid_bo_offset > binding->obj->size ||
	    mid_size > binding->obj->size - mid_bo_offset)
		return (false);
	bo = to_nvkm_bo(binding->obj);
	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) == 0 &&
	    !allow_host_large)
		return (false);
	err = nvkm_bo_paddr_run_at(bo, mid_bo_offset, mid_size, &paddr,
	    &run_size);
	if (err != 0 || run_size < mid_size)
		return (false);
	if (paddr & mask)
		return (false);
	if ((mid_bo_offset | mid_size) & mask)
		return (false);
	*pmid_addr = mid_start;
	*pmid_size = mid_size;
	*pmid_bo_offset = mid_bo_offset;
	*ppaddr = paddr;
	return (true);
}

/*
 * nvkm_drm_vm_binding_prepare_clone()
 *
 * Ownership:
 *   Takes one GEM reference and one VM_BIND BO pin for the cloned live
 *   mapping.  The source binding is borrowed and keeps its own ownership.
 *
 * Lifetime:
 *   On success, *pbinding owns a detached binding node.  The caller must
 *   either insert it into nfile's live VM tracker after hardware promotion
 *   succeeds, or free it with nvkm_drm_vm_binding_free().
 *
 * Threading:
 *   Called from VM_BIND normalize while nfile->vm_token is held.  It may sleep
 *   while pinning the BO, so callers must run it before the promotion commit
 *   point that rewrites hardware PTEs.
 */
static int
nvkm_drm_vm_binding_prepare_clone(struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_binding *source, uint64_t addr, uint64_t size,
    uint64_t bo_offset, uint8_t page_shift,
    struct nvkm_drm_vm_binding **pbinding)
{
	struct nvkm_drm_vm_binding *binding;
	int err;

	*pbinding = NULL;
	if (size == 0)
		return (0);
	drm_gem_object_get(source->obj);
	binding = nvkm_drm_vm_binding_alloc(nfile, addr, size, source->obj,
	    bo_offset, source->pte_kind, page_shift);
	if (binding == NULL) {
		drm_gem_object_put_unlocked(source->obj);
		return (-ENOMEM);
	}
	err = nvkm_drm_vm_binding_pin(binding);
	if (err != 0) {
		nvkm_drm_vm_binding_free(binding);
		return (-err);
	}
	*pbinding = binding;
	return (0);
}

/*
 * nvkm_drm_vm_binding_promote_64k_one()
 *
 * Ownership:
 *   Mutates one live binding and may insert cloned middle/suffix bindings.
 *   Any clone takes its own GEM reference and VM_BIND pin before hardware PTEs
 *   are changed.  On VMM failure, all clones are freed and the source binding
 *   remains unchanged.
 *
 * Lifetime:
 *   The source binding stays live for the whole call.  After success, the
 *   software mapping ranges match the hardware shape: 4 KiB prefix/suffix
 *   remain SPT mappings and the promoted middle is an LPT mapping.
 *
 * Threading:
 *   Requires nfile->vm_token and the caller's GSP/VMM serialization.  All
 *   fallible ownership work is complete before nvkm_gsp_vmm_promote_* writes
 *   hardware PTEs; after that point, only deterministic tracker splices occur.
 */
static int
nvkm_drm_vm_binding_promote_64k_one(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_binding *binding,
    uint64_t mid_addr, uint64_t mid_size, uint64_t mid_bo_offset,
    uint64_t paddr)
{
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	struct nvkm_drm_vm_bind_segment_plan sysmem_plan;
	struct nvkm_drm_vm_binding *middle = NULL, *suffix = NULL;
	uint64_t old_start = binding->addr;
	uint64_t old_end = binding->addr + binding->size;
	uint64_t mid_end = mid_addr + mid_size;
	uint64_t prefix_size = mid_addr - old_start;
	uint64_t suffix_size = old_end - mid_end;
	uint64_t suffix_bo_offset = mid_bo_offset + mid_size;
	bool is_vram = (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0;
	int err;

	nvkm_drm_vm_bind_segment_plan_init(&sysmem_plan);
	if (!is_vram) {
		err = nvkm_drm_vm_bind_segment_add_sysmem(&sysmem_plan, bo,
		    mid_addr, mid_size, mid_bo_offset, NVKM_GMMU_LPT_SHIFT);
		if (err != 0)
			goto fail;
		KASSERT(sysmem_plan.count == 1,
		    ("nvkm_drm: sysmem promote64 segment count %u",
		    sysmem_plan.count));
	}

	if (prefix_size != 0) {
		err = nvkm_drm_vm_binding_prepare_clone(nfile, binding,
		    mid_addr, mid_size, mid_bo_offset, NVKM_GMMU_LPT_SHIFT,
		    &middle);
		if (err != 0)
			goto fail;
	}
	if (suffix_size != 0) {
		err = nvkm_drm_vm_binding_prepare_clone(nfile, binding,
		    mid_end, suffix_size, suffix_bo_offset,
		    NVKM_GMMU_SPT_SHIFT, &suffix);
		if (err != 0)
			goto fail;
	}

	if (is_vram) {
		err = nvkm_gsp_vmm_promote_vram_64k_noflush(nfile->vmm,
		    mid_addr, paddr, mid_size, 0, 0, binding->pte_kind);
	} else {
		struct nvkm_drm_vm_bind_segment *segment =
		    &sysmem_plan.segments[0];

		err =
		    nvkm_gsp_vmm_map_sysmem_paddrs_page_prepared_noflush(
		    nfile->vmm, segment->addr, segment->sysmem_paddrs,
		    segment->sysmem_page_count, binding->pte_kind,
		    NVKM_GMMU_LPT_SHIFT);
	}
	if (err != 0)
		goto fail;

	if (prefix_size != 0) {
		binding->size = prefix_size;
		nvkm_drm_vm_binding_insert_sorted(nfile, middle);
	} else {
		binding->size = mid_size;
		binding->page_shift = NVKM_GMMU_LPT_SHIFT;
	}
	if (suffix != NULL)
		nvkm_drm_vm_binding_insert_sorted(nfile, suffix);
	nvkm_drm_vm_bind_note_promote(sc, NVKM_GMMU_LPT_SHIFT, mid_size);
	if (prefix_size != 0 || suffix_size != 0)
		nvkm_drm_vm_bind_note_promote_split(sc, mid_size);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND promote64 addr=0x%016jx size=0x%016jx obj=%p split=%u\n",
	    (uintmax_t)mid_addr, (uintmax_t)mid_size, binding->obj,
	    prefix_size != 0 || suffix_size != 0);
	nvkm_drm_vm_bind_segment_plan_fini(&sysmem_plan);
	return (0);

fail:
	if (middle != NULL)
		nvkm_drm_vm_binding_free(middle);
	if (suffix != NULL)
		nvkm_drm_vm_binding_free(suffix);
	nvkm_drm_vm_bind_segment_plan_fini(&sysmem_plan);
	return (err);
}

/*
 * nvkm_drm_vm_bindings_promote_64k()
 *
 * Ownership:
 *   Mutates eligible live bindings in nfile's VM tracker.  Whole-binding
 *   promotion keeps ownership on the same binding; split-promotion allocates
 *   cloned middle/suffix bindings with independent GEM references and BO pins
 *   before hardware PTEs are changed.
 *
 * Lifetime:
 *   On success for a middle range, the hardware representation has been
 *   rewritten from 4 KiB SPT leaves to 64 KiB LPT leaves and the live mapping
 *   tree is split/updated before the enclosing VM_BIND publishes its final
 *   flush/fence.
 *
 * Threading:
 *   Requires nfile->vm_token and the caller's GSP/VMM serialization.  This is
 *   a best-effort normalize step; fallible preparation or unexpected VMM
 *   mismatch is counted and skipped without changing the VM_BIND UAPI result.
 */
static bool
nvkm_drm_vm_bindings_promote_64k(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size)
{
	struct nvkm_drm_vm_binding *binding, *next;
	uint64_t raw_end, start, end;
	bool promoted = false;

	if (size == 0 || addr > UINT64_MAX - size)
		return (false);
	raw_end = addr + size;
	start = addr & ~(NVKM_GMMU_LPT_PAGE_SIZE - 1);
	if (raw_end > UINT64_MAX - (NVKM_GMMU_LPT_PAGE_SIZE - 1))
		end = raw_end;
	else
		end = (raw_end + NVKM_GMMU_LPT_PAGE_SIZE - 1) &
		    ~(NVKM_GMMU_LPT_PAGE_SIZE - 1);
	if (end <= start)
		return (false);

	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, start,
	    end - start); binding != NULL; binding = next) {
		uint64_t mid_addr = 0, mid_size = 0, mid_bo_offset = 0;
		uint64_t paddr = 0;
		int err;

		next = nvkm_drm_vm_binding_next_overlap(nfile, binding,
		    start, end - start);
		if (!nvkm_drm_vm_binding_find_promote_64k(binding,
		    sc->vm_bind_map_host_large_enable != 0,
		    &mid_addr, &mid_size, &mid_bo_offset, &paddr))
			continue;

		err = nvkm_drm_vm_binding_promote_64k_one(sc, nfile,
		    binding, mid_addr, mid_size, mid_bo_offset, paddr);
		if (err != 0) {
			sc->vm_bind_promote_error_count++;
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND promote64 skipped addr=0x%016jx size=0x%016jx err=%d\n",
			    (uintmax_t)binding->addr,
			    (uintmax_t)binding->size, err);
			continue;
		}
		promoted = true;
	}
	return (promoted);
}

/*
 * nvkm_drm_vm_binding_find_promote_2m()
 *
 * Ownership:
 *   Borrows one live 64 KiB binding and its BO.  It takes no GEM reference,
 *   BO pin, or VMM ownership; the binding's existing VM_BIND pin keeps the
 *   physical run stable while the caller holds VM_BIND serialization.
 *
 * Lifetime:
 *   Returned range values are immediate copies.  The binding pointer remains
 *   borrowed and must stay live until the caller either skips promotion or
 *   completes the corresponding tracker splice.
 *
 * Threading:
 *   Pure normalize predicate under nfile->vm_token.  It uses the BO physical
 *   run helper so a 2 MiB promotion never crosses a discontiguous backing run.
 */
static bool
nvkm_drm_vm_binding_find_promote_2m(
    const struct nvkm_drm_vm_binding *binding, bool allow_host_large,
    uint64_t *pmid_addr, uint64_t *pmid_size, uint64_t *pmid_bo_offset,
    uint64_t *ppaddr)
{
	const struct nvkm_bo *bo;
	vm_paddr_t paddr;
	uint64_t mask = (1ULL << NVKM_GMMU_PD0_SHIFT) - 1;
	uint64_t old_end, mid_start, mid_end, mid_size, mid_bo_offset;
	uint64_t run_size;
	int err;

	if (binding == NULL || !binding->pte_installed ||
	    !binding->bo_pinned)
		return (false);
	if (binding->page_shift != NVKM_GMMU_LPT_SHIFT)
		return (false);
	if (binding->addr > UINT64_MAX - binding->size)
		return (false);
	if (binding->addr > UINT64_MAX - mask)
		return (false);
	old_end = binding->addr + binding->size;
	mid_start = (binding->addr + mask) & ~mask;
	mid_end = old_end & ~mask;
	if (mid_end <= mid_start)
		return (false);
	mid_size = mid_end - mid_start;
	if ((mid_start - binding->addr) > UINT64_MAX - binding->bo_offset)
		return (false);
	mid_bo_offset = binding->bo_offset + (mid_start - binding->addr);
	if (mid_bo_offset > binding->obj->size ||
	    mid_size > binding->obj->size - mid_bo_offset)
		return (false);
	bo = to_nvkm_bo(binding->obj);
	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) == 0 &&
	    !allow_host_large)
		return (false);
	err = nvkm_bo_paddr_run_at(bo, mid_bo_offset, mid_size, &paddr,
	    &run_size);
	if (err != 0 || run_size < mid_size)
		return (false);
	if (paddr & mask)
		return (false);
	if ((mid_bo_offset | mid_size) & mask)
		return (false);

	*pmid_addr = mid_start;
	*pmid_size = mid_size;
	*pmid_bo_offset = mid_bo_offset;
	*ppaddr = paddr;
	return (true);
}

/*
 * nvkm_drm_vm_binding_promote_2m_one()
 *
 * Ownership:
 *   Mutates one live 64 KiB binding and may insert cloned middle/suffix
 *   bindings.  Any clone takes its own GEM reference and VM_BIND pin before
 *   hardware page tables are changed.  On VMM failure, all clones are freed
 *   and the source binding remains unchanged.
 *
 * Lifetime:
 *   After success, software mapping shape and hardware page-table shape are
 *   isomorphic: LPT prefix/suffix remain 64 KiB mappings, and the promoted
 *   middle is a 2 MiB PD0 mapping.
 *
 * Threading:
 *   Requires nfile->vm_token and VM_BIND serialization.  Fallible ownership
 *   work finishes before the VMM promote writer rewrites the child table into
 *   a PD0 leaf.  HOST/GART promotion snapshots paddr runs here and remains
 *   behind the host-large gate.
 */
static int
nvkm_drm_vm_binding_promote_2m_one(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_binding *binding,
    uint64_t mid_addr, uint64_t mid_size, uint64_t mid_bo_offset,
    uint64_t paddr)
{
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	struct nvkm_drm_vm_bind_segment_plan sysmem_plan;
	struct nvkm_drm_vm_binding *middle = NULL, *suffix = NULL;
	uint64_t old_start = binding->addr;
	uint64_t old_end = binding->addr + binding->size;
	uint64_t mid_end = mid_addr + mid_size;
	uint64_t prefix_size = mid_addr - old_start;
	uint64_t suffix_size = old_end - mid_end;
	uint64_t suffix_bo_offset = mid_bo_offset + mid_size;
	bool is_vram = (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0;
	int err;

	nvkm_drm_vm_bind_segment_plan_init(&sysmem_plan);
	if (!is_vram) {
		err = nvkm_drm_vm_bind_segment_add_sysmem(&sysmem_plan, bo,
		    mid_addr, mid_size, mid_bo_offset, NVKM_GMMU_PD0_SHIFT);
		if (err != 0)
			goto fail;
		KASSERT(sysmem_plan.count == 1,
		    ("nvkm_drm: sysmem promote2m segment count %u",
		    sysmem_plan.count));
	}

	if (prefix_size != 0) {
		err = nvkm_drm_vm_binding_prepare_clone(nfile, binding,
		    mid_addr, mid_size, mid_bo_offset, NVKM_GMMU_PD0_SHIFT,
		    &middle);
		if (err != 0)
			goto fail;
	}
	if (suffix_size != 0) {
		err = nvkm_drm_vm_binding_prepare_clone(nfile, binding,
		    mid_end, suffix_size, suffix_bo_offset,
		    NVKM_GMMU_LPT_SHIFT, &suffix);
		if (err != 0)
			goto fail;
	}

	if (is_vram) {
		err = nvkm_gsp_vmm_promote_vram_2m_noflush(nfile->vmm,
		    mid_addr, paddr, mid_size, 0, 0, binding->pte_kind);
	} else {
		struct nvkm_drm_vm_bind_segment *segment =
		    &sysmem_plan.segments[0];

		err = nvkm_gsp_vmm_promote_sysmem_2m_noflush(nfile->vmm,
		    segment->addr, segment->sysmem_paddrs,
		    segment->sysmem_page_count, binding->pte_kind);
	}
	if (err != 0)
		goto fail;

	if (prefix_size != 0) {
		binding->size = prefix_size;
		nvkm_drm_vm_binding_insert_sorted(nfile, middle);
	} else {
		binding->size = mid_size;
		binding->page_shift = NVKM_GMMU_PD0_SHIFT;
	}
	if (suffix != NULL)
		nvkm_drm_vm_binding_insert_sorted(nfile, suffix);
	nvkm_drm_vm_bind_note_promote(sc, NVKM_GMMU_PD0_SHIFT, mid_size);
	if (prefix_size != 0 || suffix_size != 0)
		nvkm_drm_vm_bind_note_promote_split(sc, mid_size);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND promote2m addr=0x%016jx size=0x%016jx obj=%p split=%u\n",
	    (uintmax_t)mid_addr, (uintmax_t)mid_size, binding->obj,
	    prefix_size != 0 || suffix_size != 0);
	nvkm_drm_vm_bind_segment_plan_fini(&sysmem_plan);
	return (0);

fail:
	if (middle != NULL)
		nvkm_drm_vm_binding_free(middle);
	if (suffix != NULL)
		nvkm_drm_vm_binding_free(suffix);
	nvkm_drm_vm_bind_segment_plan_fini(&sysmem_plan);
	return (err);
}

/*
 * nvkm_drm_vm_bindings_promote_2m()
 *
 * Ownership:
 *   Mutates eligible live mappings when explicitly enabled.  It is part of
 *   the 2 MiB feature gate: the code is compiled and type-checked, but the
 *   caller keeps it out of the hot path until partial 2 MiB materialize and
 *   runtime validation are complete.
 *
 * Lifetime:
 *   Successful ranges are converted from 64 KiB LPT mappings to 2 MiB PD0
 *   mappings before the enclosing VM_BIND publishes its final flush/fence.
 *
 * Threading:
 *   Requires nfile->vm_token and VM_BIND serialization.  Promotion is
 *   best-effort; failures increment diagnostics and preserve VM_BIND UAPI
 *   result semantics.
 */
static bool
nvkm_drm_vm_bindings_promote_2m(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size)
{
	struct nvkm_drm_vm_binding *binding, *next;
	uint64_t page_size = 1ULL << NVKM_GMMU_PD0_SHIFT;
	uint64_t raw_end, start, end;
	bool promoted = false;

	if (size == 0 || addr > UINT64_MAX - size)
		return (false);
	raw_end = addr + size;
	start = addr & ~(page_size - 1);
	if (raw_end > UINT64_MAX - (page_size - 1))
		end = raw_end;
	else
		end = (raw_end + page_size - 1) & ~(page_size - 1);
	if (end <= start)
		return (false);

	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, start,
	    end - start); binding != NULL; binding = next) {
		uint64_t mid_addr = 0, mid_size = 0, mid_bo_offset = 0;
		uint64_t paddr = 0;
		int err;

		next = nvkm_drm_vm_binding_next_overlap(nfile, binding,
		    start, end - start);
		if (!nvkm_drm_vm_binding_find_promote_2m(binding,
		    sc->vm_bind_map_host_large_enable != 0,
		    &mid_addr, &mid_size, &mid_bo_offset, &paddr))
			continue;

		err = nvkm_drm_vm_binding_promote_2m_one(sc, nfile,
		    binding, mid_addr, mid_size, mid_bo_offset, paddr);
		if (err != 0) {
			sc->vm_bind_promote_error_count++;
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND promote2m skipped addr=0x%016jx size=0x%016jx err=%d\n",
			    (uintmax_t)binding->addr,
			    (uintmax_t)binding->size, err);
			continue;
		}
		promoted = true;
	}
	return (promoted);
}

/*
 * nvkm_drm_vm_bindings_normalize()
 *
 * Ownership:
 *   Borrows the VM_BIND remap range and mutates nfile's live mapping tracker
 *   only when an already-installed valid mapping can be merged or promoted.
 *   Merged-away bindings are moved to retired_bindings and keep their BO/GEM
 *   ownership until the caller publishes the VM_BIND completion fence.
 *
 * Lifetime:
 *   Call after the current remap has made hardware PTE/PDE state and software
 *   mappings isomorphic.  The helper may rewrite eligible valid PTEs to a
 *   larger page class before the caller's final flush/invalidate boundary.
 *
 * Threading:
 *   Requires nfile->vm_token and the caller's VMM/GSP serialization.  This is
 *   the common remap normalize tail for MAP, UNMAP, MAP_NULL, and MAP_SPARSE.
 */
static bool
nvkm_drm_vm_bindings_normalize(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	bool promoted = false;

	if (size == 0 || addr > UINT64_MAX - size)
		return (false);

	nvkm_drm_vm_bindings_merge_neighbors(sc, nfile, addr, size,
	    retired_bindings);
	if (nvkm_drm_vm_bindings_promote_64k(sc, nfile, addr, size)) {
		nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set, addr, size);
		promoted = true;
		nvkm_drm_vm_bindings_merge_neighbors(sc, nfile, addr, size,
		    retired_bindings);
	}
	if (sc->vm_bind_promote_2m_enable != 0 &&
	    nvkm_drm_vm_bindings_promote_2m(sc, nfile, addr, size)) {
		nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set, addr, size);
		promoted = true;
		nvkm_drm_vm_bindings_merge_neighbors(sc, nfile, addr, size,
		    retired_bindings);
	}
	return (promoted);
}

/*
 * nvkm_drm_vm_binding_reclaim_noflush()
 *
 * Ownership:
 *   Moves one live VM binding out of the VM tree/list and BO reverse list into
 *   caller-owned release_bindings.  The binding keeps owning its GEM reference
 *   and VM_BIND pin until the caller releases that detached list.
 *
 * Lifetime:
 *   Used by file release after channels and queued jobs for the file have been
 *   closed.  A successful PTE clear is recorded in dirty_set for the caller's
 *   final flush.  If the clear reports an invariant error, the caller still
 *   destroys this file's VMM immediately afterwards, so the binding ownership
 *   must not be leaked.
 *
 * Threading:
 *   The caller holds the file VM token and then the GSP/VMM mutation token for
 *   live tree/list and PTE mutation.  This helper must not release GEM refs or
 *   VM_BIND pins because unpinning may reserve the TTM BO; release_bindings is
 *   consumed only after the caller drops VM/GSP tokens.  The helper does not
 *   flush; the caller publishes dirty PTE writes once after the release batch.
 */
static int
nvkm_drm_vm_binding_reclaim_noflush(struct nvkm_softc *sc,
    struct nvkm_drm_vm_binding *binding,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *release_bindings)
{
	int err = 0;

	nvkm_drm_vm_binding_assert(binding);

	if (binding->pte_installed) {
		err = nvkm_gsp_vmm_unmap_valid_page_noflush(binding->owner->vmm,
		    binding->addr, binding->size, binding->page_shift);
		if (err != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND unmap failed addr=0x%016jx size=0x%016jx obj=%p err=%d\n",
			    (uintmax_t)binding->addr,
			    (uintmax_t)binding->size, binding->obj, err);
			err = -err;
		} else {
			binding->pte_installed = false;
			nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set,
			    binding->addr, binding->size);
		}
	}

	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND reclaim addr=0x%016jx size=0x%016jx obj=%p err=%d\n",
	    (uintmax_t)binding->addr, (uintmax_t)binding->size,
	    binding->obj, err);
	nvkm_drm_vm_binding_unlink_retire(binding, release_bindings);
	return (err);
}

/*
 * nvkm_drm_vm_bind_note_empty_clear_skip()
 *
 * Ownership:
 *   Borrows sc for counter updates only; it does not acquire references to the
 *   VMM, GEM objects, or VM binding records.
 *
 * Lifetime:
 *   The range is an ioctl-local value that remains owned by the caller.  The
 *   helper stores only aggregate diagnostic counters.
 *
 * Threading:
 *   Called from VM_BIND mutation paths while the caller holds the drm_file's
 *   vm_token.  Counters follow the driver's existing best-effort debug counter
 *   model and are not part of the ABI.
 */
static void
nvkm_drm_vm_bind_note_empty_clear_skip(struct nvkm_softc *sc, uint64_t size)
{
	sc->vm_bind_empty_clear_skip_count++;
	sc->vm_bind_empty_clear_skip_pages += size / NVKM_GMMU_PT_PAGE_SIZE;
}

/*
 * nvkm_drm_vm_bind_note_replace_clear_skip()
 *
 * Ownership:
 *   Borrows sc for diagnostic counter updates only.  It does not acquire or
 *   release VM binding, VMM, or GEM ownership.
 *
 * Lifetime:
 *   The byte range is caller-owned and used only to update aggregate counters.
 *
 * Threading:
 *   Called while the drm_file VM token serializes VM_BIND mutations.  Counters
 *   follow the driver's best-effort debug accounting and are not UAPI.
 */
static void
nvkm_drm_vm_bind_note_replace_clear_skip(struct nvkm_softc *sc, uint64_t size)
{
	sc->vm_bind_replace_clear_skip_count++;
	sc->vm_bind_replace_clear_skip_pages += size / NVKM_GMMU_PT_PAGE_SIZE;
}

/*
 * nvkm_drm_vm_bind_size_bucket()
 *
 * Ownership:
 *   Borrows no objects and changes no state. The pages value is copied by
 *   value and is only used for debug-counter classification.
 *
 * Lifetime:
 *   The returned bucket is an immediate value. It carries no reference to VM,
 *   VMM, or GEM state.
 *
 * Threading:
 *   Pure helper; callers provide any required serialization for the counters
 *   they update with the returned bucket.
 */
static uint32_t
nvkm_drm_vm_bind_size_bucket(uint64_t pages)
{
	uint32_t bucket = 0;

	if (pages == 0)
		return (0);
	pages--;
	while (pages != 0 &&
	    bucket + 1 < NVKM_DRM_VM_BIND_SIZE_BUCKET_COUNT) {
		pages >>= 1;
		bucket++;
	}
	return (bucket);
}

/*
 * nvkm_drm_vm_bind_note_map_shape()
 *
 * Ownership:
 *   Borrows sc for debug counter updates only. It does not acquire or release
 *   VM_BIND, VMM, GEM, or BO ownership.
 *
 * Lifetime:
 *   The range and flags are copied into aggregate counters only. No pointer
 *   or user-provided state is retained.
 *
 * Threading:
 *   Called after a MAP PTE write succeeds while VM_BIND serialization is held.
 *   Counters are best-effort diagnostics and are not part of the ABI.
 */
static void
nvkm_drm_vm_bind_note_map_shape(struct nvkm_softc *sc, uint32_t flags,
    uint64_t size)
{
	uint64_t pages = size / NVKM_GMMU_PT_PAGE_SIZE;
	uint32_t bucket = nvkm_drm_vm_bind_size_bucket(pages);
	uint8_t pte_kind = flags & 0xff;

	sc->vm_bind_map_kind_count[pte_kind]++;
	sc->vm_bind_map_kind_pages[pte_kind] += pages;
	sc->vm_bind_map_size_count[bucket]++;
	sc->vm_bind_map_size_pages[bucket] += pages;
}

/*
 * nvkm_drm_vm_bind_note_clear_shape()
 *
 * Ownership:
 *   Borrows sc and the pte_kind copied from a live binding. It does not own
 *   or retain the binding, GEM object, or VMM range.
 *
 * Lifetime:
 *   The covered range is reduced to aggregate debug counters before return.
 *   No live-binding state escapes this call.
 *
 * Threading:
 *   Called immediately before clearing tracked valid PTEs while the drm_file
 *   VM token serializes live-binding mutations. Counters are diagnostics only.
 */
static void
nvkm_drm_vm_bind_note_clear_shape(struct nvkm_softc *sc, const struct nvkm_bo *bo,
    uint8_t pte_kind, uint8_t page_shift, uint64_t size)
{
	uint64_t pages = size / NVKM_GMMU_PT_PAGE_SIZE;
	uint32_t bucket = nvkm_drm_vm_bind_size_bucket(pages);
	uint32_t shift_bucket = nvkm_drm_vm_bind_page_shift_bucket(page_shift);
	uint32_t domain_bucket = nvkm_drm_vm_bind_domain_bucket(bo);

	sc->vm_bind_clear_kind_count[pte_kind]++;
	sc->vm_bind_clear_kind_pages[pte_kind] += pages;
	sc->vm_bind_clear_size_count[bucket]++;
	sc->vm_bind_clear_size_pages[bucket] += pages;
	sc->vm_bind_page_shift_unmap_count[shift_bucket]++;
	sc->vm_bind_page_shift_unmap_pages[shift_bucket] += pages;
	sc->vm_bind_valid_page_shift_unmap_count[domain_bucket][shift_bucket]++;
	sc->vm_bind_valid_page_shift_unmap_pages[domain_bucket][shift_bucket] +=
	    pages;
}

/*
 * nvkm_drm_vm_bind_note_sparse_clear_shape()
 *
 * Ownership:
 *   Borrows sc through the callback arg and receives one prepared sparse-clear
 *   range by value.  It does not own or retain any VMM sparse-unmap plan state.
 *
 * Lifetime:
 *   The scalar range values are consumed immediately into aggregate counters.
 *   No pointer from the VMM plan escapes the callback.
 *
 * Threading:
 *   Called after a sparse clear commit succeeds while VM_BIND serialization is
 *   still held.  Counters are diagnostics only and do not affect ABI or fence
 *   semantics.
 */
static void
nvkm_drm_vm_bind_note_sparse_clear_shape(void *arg, uint64_t addr __unused,
    uint64_t size, uint8_t page_shift)
{
	struct nvkm_softc *sc = arg;
	uint64_t pages = size / NVKM_GMMU_PT_PAGE_SIZE;
	uint32_t shift_bucket;

	if (size == 0)
		return;
	shift_bucket = nvkm_drm_vm_bind_page_shift_bucket(page_shift);
	sc->vm_bind_page_shift_unmap_count[shift_bucket]++;
	sc->vm_bind_page_shift_unmap_pages[shift_bucket] += pages;
}

/*
 * nvkm_drm_vm_bind_note_sparse_clear_shapes()
 *
 * Ownership:
 *   Borrows the committed sparse-unmap plan before fini consumes it.  The VMM
 *   plan remains owned by the caller.
 *
 * Lifetime:
 *   Must be called after successful sparse clear commit and before
 *   nvkm_gsp_vmm_fini_unmap_sparse_range() releases the plan.  Only scalar
 *   page-shift counters survive the call.
 *
 * Threading:
 *   Runs inside the same VM_BIND commit serialization as the sparse clear.  It
 *   only updates debug counters and does not touch hardware PTEs.
 */
static void
nvkm_drm_vm_bind_note_sparse_clear_shapes(struct nvkm_softc *sc,
    const struct nvkm_gsp_vmm_sparse_unmap_plan *plan)
{
	nvkm_gsp_vmm_sparse_unmap_plan_for_each_clear(plan,
	    nvkm_drm_vm_bind_note_sparse_clear_shape, sc);
}

/*
 * nvkm_drm_vm_bind_note_op()
 *
 * Ownership:
 *   Borrows sc for aggregate diagnostics.  No VM_BIND, BO, or VMM ownership is
 *   changed.
 *
 * Lifetime:
 *   The range is caller-owned and not retained.
 *
 * Threading:
 *   Called while a VM_BIND ioctl/job is applying under the per-file VM token.
 */
static void
nvkm_drm_vm_bind_note_op(struct nvkm_softc *sc, uint32_t action,
    uint64_t size)
{
	uint64_t pages = size / NVKM_GMMU_PT_PAGE_SIZE;

	switch (action) {
	case NVKM_DRM_VM_TRACE_MAP:
		sc->vm_bind_map_count++;
		sc->vm_bind_map_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_UNMAP:
		sc->vm_bind_unmap_count++;
		sc->vm_bind_unmap_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_MAP_NULL:
		sc->vm_bind_map_null_count++;
		sc->vm_bind_map_null_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_MAP_SPARSE:
		sc->vm_bind_map_sparse_count++;
		sc->vm_bind_map_sparse_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_UNMAP_SPARSE:
		sc->vm_bind_unmap_sparse_count++;
		sc->vm_bind_unmap_sparse_pages += pages;
		break;
	}
}

/*
 * nvkm_drm_vm_bind_note_clear()
 *
 * Ownership:
 *   Borrows sc for aggregate diagnostics only.
 *
 * Lifetime:
 *   The range is caller-owned and not retained.
 *
 * Threading:
 *   Called immediately before remove_range writes invalid/sparse PTEs for a
 *   tracked overlap, while VM_BIND serialization is held.
 */
static void
nvkm_drm_vm_bind_note_clear(struct nvkm_softc *sc, uint32_t action,
    uint64_t size)
{
	uint64_t pages = size / NVKM_GMMU_PT_PAGE_SIZE;

	switch (action) {
	case NVKM_DRM_VM_TRACE_UNMAP:
		sc->vm_bind_clear_unmap_count++;
		sc->vm_bind_clear_unmap_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_MAP_NULL:
		sc->vm_bind_clear_map_null_count++;
		sc->vm_bind_clear_map_null_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_MAP_SPARSE:
		sc->vm_bind_clear_map_sparse_count++;
		sc->vm_bind_clear_map_sparse_pages += pages;
		break;
	case NVKM_DRM_VM_TRACE_UNMAP_SPARSE:
		sc->vm_bind_clear_unmap_sparse_count++;
		sc->vm_bind_clear_unmap_sparse_pages += pages;
		break;
	}
}

/*
 * nvkm_drm_vm_bind_clear_is_conflict()
 *
 * Ownership:
 *   Consumes only the scalar VM trace action.  It does not inspect or retain
 *   VM, BO, sparse-plan, or mapping ownership.
 *
 * Lifetime:
 *   The returned boolean is valid for the caller's current remap operation:
 *   MAP and MAP_SPARSE clear old state only to make room for a final target
 *   leaf, while UNMAP, MAP_NULL, and UNMAP_SPARSE are semantic final invalid.
 *
 * Threading:
 *   Pure helper; no locks, waits, or side effects.
 */
static bool
nvkm_drm_vm_bind_clear_is_conflict(uint32_t action)
{
	return (action == NVKM_DRM_VM_TRACE_MAP ||
	    action == NVKM_DRM_VM_TRACE_MAP_SPARSE);
}

/*
 * nvkm_drm_vm_bind_segments_min_page_shift()
 *
 * Ownership:
 *   Borrows a caller-owned segment plan and returns a scalar page class.  It
 *   does not retain the plan or inspect BO/VMM state.
 *
 * Lifetime:
 *   The returned shift is valid only for the current remap plan.  It is used
 *   to cap sparse-clear page size so later prepared target writers can consume
 *   child PT storage produced by the same plan.
 *
 * Threading:
 *   Pure helper.  The caller owns VM_BIND serialization and segment lifetime.
 */
static uint8_t
nvkm_drm_vm_bind_segments_min_page_shift(
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count)
{
	uint8_t page_shift = NVKM_GMMU_PD0_SHIFT;

	for (uint32_t i = 0; i < segment_count; i++) {
		if (segments[i].page_shift < page_shift)
			page_shift = segments[i].page_shift;
	}
	return (page_shift);
}

enum nvkm_drm_vm_sparse_clear_mode {
	NVKM_DRM_VM_SPARSE_CLEAR_FINAL,
	NVKM_DRM_VM_SPARSE_CLEAR_TARGET_OVERWRITE,
	NVKM_DRM_VM_SPARSE_CLEAR_METADATA_ONLY,
};

/*
 * nvkm_drm_vm_bind_prepare_sparse_clear()
 *
 * Ownership:
 *   Prepares a caller-owned sparse clear/removal plan for the range.  FINAL
 *   plans own invalid PTE writes; TARGET_OVERWRITE plans own only sparse
 *   metadata removal plus child storage needed by the following target writer;
 *   METADATA_ONLY plans own stale sparse metadata removal only.
 *
 * Lifetime:
 *   The caller must keep VM remap serialization until commit or fini because
 *   the plan borrows old sparse-region pointers from the VMM.  A range with no
 *   sparse regions returns success with *pplan == NULL.  clear_page_shift caps
 *   the cut range page size when a later valid/sparse target writer needs
 *   lower child PT storage from the same plan.  METADATA_ONLY is used only
 *   after exact valid-map no-op proof, so it must not dirty hardware PTE/PDEs.
 *
 * Threading:
 *   Called before PTE/PDE mutation for the enclosing op.  It may allocate
 *   inside the VMM prepare path, but performs no GEM lookup, BO pinning, or
 *   fence waits.
 */
static int
nvkm_drm_vm_bind_prepare_sparse_clear(struct nvkm_drm_file *nfile,
	    uint64_t addr, uint64_t size, uint8_t clear_page_shift,
	    enum nvkm_drm_vm_sparse_clear_mode mode,
	    struct nvkm_gsp_vmm_sparse_unmap_plan **pplan)
{
	int err;

	switch (mode) {
	case NVKM_DRM_VM_SPARSE_CLEAR_FINAL:
		err = nvkm_gsp_vmm_prepare_unmap_sparse_range_page(nfile->vmm,
		    addr, size, clear_page_shift, 0, pplan);
		break;
	case NVKM_DRM_VM_SPARSE_CLEAR_TARGET_OVERWRITE:
		err = nvkm_gsp_vmm_prepare_overwrite_sparse_range_page(nfile->vmm,
		    addr, size, clear_page_shift, pplan);
		break;
	case NVKM_DRM_VM_SPARSE_CLEAR_METADATA_ONLY:
		err = nvkm_gsp_vmm_prepare_metadata_sparse_range(nfile->vmm,
		    addr, size, pplan);
		break;
	default:
		return (-EINVAL);
	}
	if (err != 0)
		return (-err);
	return (0);
}

/*
 * nvkm_drm_vm_bind_commit_sparse_clear_noflush()
 *
 * Ownership:
 *   Consumes the prepared sparse clear plan on success and releases plan
 *   storage before return.  Passing a NULL plan is a no-op.
 *
 * Lifetime:
 *   Sparse PTE/PDE clears become visible only after the enclosing VM_BIND
 *   flush/TLB invalidate.  On failure, remaining prepared objects are released
 *   before returning the negative errno.
 *
 * Threading:
 *   Called in the nofail VM_BIND commit section.  It does not allocate, lookup
 *   GEM handles, pin BOs, or wait on fences.
 */
static int
nvkm_drm_vm_bind_commit_sparse_clear_noflush(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    struct nvkm_gsp_vmm_sparse_unmap_plan **pplan, uint32_t action,
    uint64_t addr, uint64_t size, struct nvkm_drm_vm_dirty_set *dirty_set)
{
	struct nvkm_gsp_vmm_sparse_unmap_plan *plan = *pplan;
	bool wrote_hw;
	int err;

	if (plan == NULL)
		return (0);
	if (nvkm_drm_vm_bind_clear_is_conflict(action)) {
		err = nvkm_gsp_vmm_commit_unmap_sparse_range_conflict_noflush(
		    nfile->vmm, plan);
	} else {
		err = nvkm_gsp_vmm_commit_unmap_sparse_range_noflush(
		    nfile->vmm, plan);
	}
	if (err != 0) {
		nvkm_gsp_vmm_fini_unmap_sparse_range(nfile->vmm, plan);
		*pplan = NULL;
		return (-err);
	}
	wrote_hw = nvkm_gsp_vmm_sparse_unmap_plan_wrote_hw(plan);
	if (wrote_hw)
		nvkm_drm_vm_bind_note_sparse_clear_shapes(sc, plan);
	nvkm_gsp_vmm_fini_unmap_sparse_range(nfile->vmm, plan);
	*pplan = NULL;
	if (wrote_hw) {
		nvkm_drm_vm_bind_note_clear(sc, action, size);
		nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set, addr, size);
	}
	return (0);
}

static void
nvkm_drm_vm_bind_abort_sparse_clear(struct nvkm_drm_file *nfile,
    struct nvkm_gsp_vmm_sparse_unmap_plan **pplan)
{
	if (*pplan == NULL)
		return;
	nvkm_gsp_vmm_fini_unmap_sparse_range(nfile->vmm, *pplan);
	*pplan = NULL;
}

/*
 * nvkm_drm_vm_materialize_entry_add_4k_keep_range()
 *
 * Ownership:
 *   Borrows one live large-page binding and appends a prepared SPT segment for
 *   a target-outside keep range.  The helper does not publish mapping ownership
 *   or write PTEs; the enclosing materialize entry owns the segment plan.
 *
 * Lifetime:
 *   Only keep ranges may be passed here.  Target-middle ranges that will become
 *   final valid, sparse, or invalid are deliberately skipped so split and final
 *   install/clear do not rewrite the same VA.
 *
 * Threading:
 *   Runs in VM_BIND prepare under nfile->vm_token.  It may snapshot BO backing
 *   and allocate segment storage, but performs no BAR1 writes and waits on no
 *   GPU work.
 */
static int
nvkm_drm_vm_materialize_entry_add_4k_keep_range(
    struct nvkm_drm_vm_materialize_entry *entry, struct nvkm_bo *bo,
    const struct nvkm_drm_vm_binding *binding, uint64_t keep_start,
    uint64_t keep_end)
{
	uint64_t bo_offset, keep_size;
	int err;

	if (keep_start >= keep_end)
		return (0);
	if (keep_start < binding->addr)
		return (-EINVAL);
	keep_size = keep_end - keep_start;
	bo_offset = binding->bo_offset + (keep_start - binding->addr);
	if ((keep_start | keep_size | bo_offset) &
	    (NVKM_GMMU_PT_PAGE_SIZE - 1))
		return (-EINVAL);

	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0) {
		vm_paddr_t paddr;
		uint64_t run_size;

		err = nvkm_bo_paddr_run_at(bo, bo_offset, keep_size, &paddr,
		    &run_size);
		if (err != 0 || run_size < keep_size)
			return (err != 0 ? -err : -EIO);
		return (nvkm_drm_vm_bind_segment_add(&entry->segments,
		    keep_start, keep_size, bo_offset, paddr,
		    NVKM_GMMU_SPT_SHIFT));
	}

	return (nvkm_drm_vm_bind_segment_add_sysmem(&entry->segments, bo,
	    keep_start, keep_size, bo_offset, NVKM_GMMU_SPT_SHIFT));
}

static int
nvkm_drm_vm_materialize_entry_prepare_4k(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_materialize_entry *entry,
    struct nvkm_drm_vm_binding *binding, uint64_t target_addr,
    uint64_t target_size)
{
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	uint64_t old_start, old_end, target_end, cut_start, cut_end;
	int err;

	entry->binding = binding;
	entry->is_2m = false;
	if (binding->page_shift <= NVKM_GMMU_SPT_SHIFT)
		return (0);
	if (binding->size == 0 || target_size == 0 ||
	    binding->addr > UINT64_MAX - binding->size ||
	    target_addr > UINT64_MAX - target_size)
		return (-EINVAL);

	old_start = binding->addr;
	old_end = old_start + binding->size;
	target_end = target_addr + target_size;
	cut_start = old_start > target_addr ? old_start : target_addr;
	cut_end = old_end < target_end ? old_end : target_end;
	if (cut_start >= cut_end)
		return (nvkm_drm_vm_materialize_entry_add_4k_keep_range(entry,
		    bo, binding, old_start, old_end));

	err = nvkm_drm_vm_materialize_entry_add_4k_keep_range(entry, bo,
	    binding, old_start, cut_start);
	if (err != 0)
		return (err);
	err = nvkm_drm_vm_materialize_entry_add_4k_keep_range(entry, bo,
	    binding, cut_end, old_end);
	if (err != 0)
		return (err);
	(void)nfile;
	return (0);
}

/*
 * nvkm_drm_vm_materialize_entry_add_vram_keep_range()
 *
 * Ownership:
 *   Borrows a live VRAM binding and appends prepared segment descriptors for
 *   the old BO range that must survive a remap.  The helper does not pin,
 *   retain, or publish mapping ownership; the enclosing materialize entry owns
 *   the segment plan.
 *
 * Lifetime:
 *   Generated segments describe only target-outside keep ranges.  Target-middle
 *   ranges that will be overwritten by valid/sparse or cleared by final invalid
 *   are deliberately skipped so split and install do not rewrite the same VA.
 *
 * Threading:
 *   Runs in VM_BIND prepare while nfile->vm_token serializes the binding tree.
 *   It only snapshots VRAM physical runs and may not write PTEs/PDEs.
 */
static int
nvkm_drm_vm_materialize_entry_add_vram_keep_range(
    struct nvkm_drm_vm_materialize_entry *entry, struct nvkm_bo *bo,
    const struct nvkm_drm_vm_binding *binding, uint64_t keep_start,
    uint64_t keep_end)
{
	uint64_t page_64k = NVKM_GMMU_LPT_PAGE_SIZE;
	uint64_t old_start = binding->addr;
	uint64_t cur;
	int err;

	for (cur = keep_start; cur < keep_end;) {
		uint64_t bo_offset = binding->bo_offset + (cur - old_start);
		uint64_t remaining = keep_end - cur;
		uint64_t chunk, next_64k;
		uint8_t page_shift;
		vm_paddr_t paddr;
		uint64_t run_size;

		if (((cur | bo_offset) & (page_64k - 1)) == 0 &&
		    remaining >= page_64k) {
			chunk = page_64k;
			page_shift = NVKM_GMMU_LPT_SHIFT;
		} else {
			next_64k = (cur + page_64k) & ~(page_64k - 1);
			if (next_64k <= cur)
				next_64k = cur + page_64k;
			chunk = MIN(remaining, next_64k - cur);
			chunk &= ~(NVKM_GMMU_PT_PAGE_SIZE - 1);
			if (chunk == 0)
				chunk = NVKM_GMMU_PT_PAGE_SIZE;
			page_shift = NVKM_GMMU_SPT_SHIFT;
		}

		err = nvkm_bo_paddr_run_at(bo, bo_offset, chunk, &paddr,
		    &run_size);
		if (err != 0 || run_size < chunk)
			return (err != 0 ? -err : -EIO);
		err = nvkm_drm_vm_bind_segment_add(&entry->segments, cur,
		    chunk, bo_offset, paddr, page_shift);
		if (err != 0)
			return (err);
		cur += chunk;
	}
	return (0);
}

static int
nvkm_drm_vm_materialize_entry_prepare_2m(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_materialize_entry *entry,
    struct nvkm_drm_vm_binding *binding, uint64_t target_addr,
    uint64_t target_size, bool materialize_full_cover)
{
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	uint64_t page_2m = 1ULL << NVKM_GMMU_PD0_SHIFT;
	uint64_t page_64k = NVKM_GMMU_LPT_PAGE_SIZE;
	uint64_t old_start = binding->addr;
	uint64_t old_end;
	uint64_t target_end;
	uint64_t window;
	int err;

	entry->binding = binding;
	entry->is_2m = true;
	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) == 0)
		return (nvkm_drm_vm_materialize_entry_prepare_4k(nfile,
		    entry, binding, target_addr, target_size));
	if (binding->page_shift != NVKM_GMMU_PD0_SHIFT)
		return (nvkm_drm_vm_materialize_entry_prepare_4k(nfile,
		    entry, binding, target_addr, target_size));
	if (binding->size == 0 || target_size == 0 ||
	    old_start > UINT64_MAX - binding->size ||
	    target_addr > UINT64_MAX - target_size)
		return (-EINVAL);
	old_end = old_start + binding->size;
	target_end = target_addr + target_size;
	if ((old_start | binding->size | binding->bo_offset) &
	    (page_2m - 1))
		return (-EINVAL);
	(void)materialize_full_cover;

	for (window = old_start; window < old_end; window += page_2m) {
		uint64_t window_end = window + page_2m;
		uint64_t cut_start = window > target_addr ? window :
		    target_addr;
		uint64_t cut_end = window_end < target_end ? window_end :
		    target_end;
		uint64_t window_bo_offset = binding->bo_offset +
		    (window - old_start);

		if (cut_start >= cut_end) {
			vm_paddr_t paddr;
			uint64_t run_size;

			err = nvkm_bo_paddr_run_at(bo, window_bo_offset,
			    page_2m, &paddr, &run_size);
			if (err != 0 || run_size < page_2m)
				return (err != 0 ? -err : -EIO);
			err = nvkm_drm_vm_bind_segment_add(&entry->segments,
			    window, page_2m, window_bo_offset, paddr,
			    NVKM_GMMU_PD0_SHIFT);
			if (err != 0)
				return (err);
			continue;
		}

		err = nvkm_drm_vm_bind_2m_split_plan_prepare(nfile->vmm,
		    &entry->split_plan, window);
		if (err != 0)
			return (err);
		if (window < cut_start) {
			err = nvkm_drm_vm_materialize_entry_add_vram_keep_range(
			    entry, bo, binding, window, cut_start);
			if (err != 0)
				return (err);
		}
		if (cut_end < window_end) {
			err = nvkm_drm_vm_materialize_entry_add_vram_keep_range(
			    entry, bo, binding, cut_end, window_end);
			if (err != 0)
				return (err);
		}
	}

	err = nvkm_drm_vm_bind_prepare_segment_bindings(nfile, binding->obj,
	    entry->segments.segments, entry->segments.count,
	    binding->pte_kind, &entry->new_bindings);
	if (err != 0)
		return (err);

	for (uint32_t i = 0; i < entry->segments.count; i++) {
		uint64_t split = entry->segments.segments[i].addr &
		    ~(page_2m - 1);

		if (entry->segments.segments[i].page_shift ==
		    NVKM_GMMU_PD0_SHIFT)
			continue;
		err = nvkm_drm_vm_bind_2m_split_plan_prepare(nfile->vmm,
		    &entry->split_plan, split);
		if (err != 0)
			return (err);
	}
	return (0);
}

static int
nvkm_drm_vm_materialize_entry_prepare(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_materialize_entry *entry,
    struct nvkm_drm_vm_binding *binding, uint64_t target_addr,
    uint64_t target_size, bool materialize_full_cover)
{
	if (binding->page_shift == NVKM_GMMU_PD0_SHIFT)
		return (nvkm_drm_vm_materialize_entry_prepare_2m(nfile,
		    entry, binding, target_addr, target_size,
		    materialize_full_cover));
	return (nvkm_drm_vm_materialize_entry_prepare_4k(nfile, entry,
	    binding, target_addr, target_size));
}

/*
 * nvkm_drm_vm_materialize_entry_preflight()
 *
 * Ownership:
 *   Borrows one prepared materialize entry and the file VMM.  It does not
 *   consume detached bindings, prepared split PTs, paddr snapshots, or live
 *   mapping ownership.
 *
 * Lifetime:
 *   The result is valid while VM_BIND serialization keeps the prepared entry
 *   and live VMM page-table state stable.  A successful result means the
 *   later entry commit should not discover missing target PTs or invalid 2 MiB
 *   split state after another entry has already mutated hardware.
 *
 * Threading:
 *   Runs before materialize plan commit starts.  It may acquire vmm->tok via
 *   prepared-writer and split preflight helpers, but performs no PTE/PDE writes
 *   and no software mapping splice.
 */
static int
nvkm_drm_vm_materialize_entry_preflight(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_materialize_entry *entry)
{
	struct nvkm_drm_vm_binding *binding = entry->binding;
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	int err;

	if (!entry->is_2m) {
		return (nvkm_drm_vm_bind_map_segments_preflight(nfile,
		    entry->segments.segments, entry->segments.count, bo));
	}

	err = nvkm_drm_vm_bind_2m_split_plan_preflight(nfile->vmm,
	    &entry->split_plan);
	if (err != 0)
		return (err);

	for (uint32_t i = 0; i < entry->segments.count; i++) {
		struct nvkm_drm_vm_bind_segment *segment =
		    &entry->segments.segments[i];

		if (segment->page_shift == NVKM_GMMU_PD0_SHIFT)
			continue;
		err = nvkm_drm_vm_bind_segment_check_writer_args(segment, bo);
		if (err != 0)
			return (err);
	}
	return (0);
}

static int
nvkm_drm_vm_materialize_entry_commit(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_materialize_entry *entry,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_binding *binding = entry->binding;
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	uint64_t page_2m = 1ULL << NVKM_GMMU_PD0_SHIFT;
	uint64_t materialized = 0;
	int err;

	if (!entry->is_2m) {
		err = nvkm_drm_vm_bind_map_segments_preflight(nfile,
		    entry->segments.segments, entry->segments.count, bo);
		if (err != 0)
			return (err);
		for (uint32_t i = 0; i < entry->segments.count; i++) {
			struct nvkm_drm_vm_bind_segment *segment =
			    &entry->segments.segments[i];

			if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) != 0) {
				err =
				    nvkm_gsp_vmm_map_vram_flags_page_prepared_noflush(
				    nfile->vmm, segment->addr, segment->paddr,
				    segment->size, 0, 0, binding->pte_kind,
				    NVKM_GMMU_SPT_SHIFT);
			} else {
				err =
				    nvkm_gsp_vmm_map_sysmem_paddrs_page_prepared_noflush(
				    nfile->vmm, segment->addr,
				    segment->sysmem_paddrs,
				    segment->sysmem_page_count, binding->pte_kind,
				    segment->page_shift);
			}
			if (err != 0)
				return (-err);
		}
		if (entry->segments.count != 0) {
			nvkm_drm_vm_bind_note_materialize(sc, dirty_set,
			    binding->addr, binding->size);
			binding->page_shift = NVKM_GMMU_SPT_SHIFT;
		}
		entry->committed = true;
		return (0);
	}

	for (uint32_t i = 0; i < entry->segments.count; i++) {
		struct nvkm_drm_vm_bind_segment *segment =
		    &entry->segments.segments[i];

		if (segment->page_shift == NVKM_GMMU_PD0_SHIFT)
			continue;
		err = nvkm_drm_vm_bind_segment_check_writer_args(segment, bo);
		if (err != 0)
			return (err);
	}
	for (uint32_t i = 0; i < entry->split_plan.count; i++) {
		uint64_t split = entry->split_plan.splits[i].addr;
		bool was_committed = entry->split_plan.splits[i].committed;

		err = nvkm_drm_vm_bind_2m_split_plan_commit(nfile->vmm,
		    &entry->split_plan, split);
		if (err != 0)
			return (err);
		if (!was_committed)
			materialized += page_2m;
	}
	for (uint32_t i = 0; i < entry->segments.count; i++) {
		struct nvkm_drm_vm_bind_segment *segment =
		    &entry->segments.segments[i];

		if (segment->page_shift == NVKM_GMMU_PD0_SHIFT)
			continue;
		err = nvkm_gsp_vmm_map_vram_flags_page_prepared_noflush(
		    nfile->vmm, segment->addr, segment->paddr,
		    segment->size, 0, 0, binding->pte_kind,
		    segment->page_shift);
		if (err != 0)
			return (-err);
	}

	nvkm_drm_vm_binding_unlink_retire(binding, retired_bindings);
	nvkm_drm_vm_bindings_insert_prepared(nfile, &entry->new_bindings);
	nvkm_drm_vm_bindings_recalc_max_end(nfile);
	if (materialized != 0)
		nvkm_drm_vm_bind_note_materialize(sc, dirty_set,
		    binding->addr, materialized);
	entry->committed = true;
	return (0);
}

static void
nvkm_drm_vm_materialize_plan_init(struct nvkm_drm_vm_materialize_plan *plan)
{
	plan->entries = NULL;
	plan->count = 0;
}

static void
nvkm_drm_vm_materialize_plan_fini(struct nvkm_gsp_vmm *vmm,
    struct nvkm_drm_vm_materialize_plan *plan)
{
	for (uint32_t i = 0; i < plan->count; i++)
		nvkm_drm_vm_materialize_entry_fini(vmm, &plan->entries[i]);
	kfree(plan->entries);
	plan->entries = NULL;
	plan->count = 0;
}

static int
nvkm_drm_vm_materialize_plan_alloc(
    struct nvkm_drm_vm_materialize_plan *plan, uint32_t count)
{
	if (count == 0)
		return (0);
	plan->entries = kmalloc_array(count, sizeof(*plan->entries),
	    GFP_KERNEL);
	if (plan->entries == NULL)
		return (-ENOMEM);
	plan->count = count;
	for (uint32_t i = 0; i < count; i++)
		nvkm_drm_vm_materialize_entry_init(&plan->entries[i]);
	return (0);
}

static bool
nvkm_drm_vm_binding_needs_range_materialize(
    const struct nvkm_drm_vm_binding *binding, uint64_t addr, uint64_t end,
    bool materialize_full_cover)
{
	uint64_t old_start, old_end, cut_start, cut_end;

	if (binding->page_shift <= NVKM_GMMU_SPT_SHIFT)
		return (false);
	old_start = binding->addr;
	old_end = binding->addr + binding->size;
	cut_start = old_start > addr ? old_start : addr;
	cut_end = old_end < end ? old_end : end;
	if (cut_start >= cut_end)
		return (false);
	if (!materialize_full_cover && cut_start == old_start &&
	    cut_end == old_end)
		return (false);
	return (true);
}

static int
nvkm_drm_vm_materialize_plan_prepare_range(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_materialize_plan *plan,
    uint64_t addr, uint64_t size, bool materialize_full_cover)
{
	struct nvkm_drm_vm_binding *binding;
	uint64_t end = addr + size;
	uint32_t count = 0;
	int err;

	(void)sc;
	nvkm_drm_vm_materialize_plan_init(plan);
	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL; binding = nvkm_drm_vm_binding_next_overlap(nfile,
	    binding, addr, size)) {
		if (nvkm_drm_vm_binding_needs_range_materialize(binding, addr,
		    end, materialize_full_cover))
			count++;
	}
	err = nvkm_drm_vm_materialize_plan_alloc(plan, count);
	if (err != 0)
		return (err);
	if (count == 0)
		return (0);

	count = 0;
	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL; binding = nvkm_drm_vm_binding_next_overlap(nfile,
	    binding, addr, size)) {
		if (!nvkm_drm_vm_binding_needs_range_materialize(binding, addr,
		    end, materialize_full_cover))
			continue;
		err = nvkm_drm_vm_materialize_entry_prepare(nfile,
		    &plan->entries[count], binding, addr, size,
		    materialize_full_cover);
		if (err != 0) {
			nvkm_drm_vm_materialize_plan_fini(nfile->vmm, plan);
			return (err);
		}
		count++;
	}
	return (0);
}

static int
nvkm_drm_vm_materialize_plan_preflight(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_materialize_plan *plan)
{
	int err;

	for (uint32_t i = 0; i < plan->count; i++) {
		err = nvkm_drm_vm_materialize_entry_preflight(nfile,
		    &plan->entries[i]);
		if (err != 0)
			return (err);
	}
	return (0);
}

static int
nvkm_drm_vm_materialize_plan_commit(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_materialize_plan *plan,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	int err;

	err = nvkm_drm_vm_materialize_plan_preflight(nfile, plan);
	if (err != 0)
		return (err);

	for (uint32_t i = 0; i < plan->count; i++) {
		err = nvkm_drm_vm_materialize_entry_commit(sc, nfile,
		    &plan->entries[i], dirty_set, retired_bindings);
		if (err != 0)
			return (err);
	}
	return (0);
}

static int
nvkm_drm_vm_bindings_materialize_large_partials(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size,
    bool materialize_full_cover, struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_materialize_plan plan;
	int err;

	if (addr >= nfile->vm_bindings_max_end)
		return (0);
	err = nvkm_drm_vm_materialize_plan_prepare_range(sc, nfile, &plan,
	    addr, size, materialize_full_cover);
	if (err != 0)
		return (err);
	err = nvkm_drm_vm_materialize_plan_commit(sc, nfile, &plan,
	    dirty_set, retired_bindings);
	nvkm_drm_vm_materialize_plan_fini(nfile->vmm, &plan);
	return (err);
}

static bool
nvkm_drm_vm_bind_segments_overlap_lower(
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    uint64_t addr, uint64_t size, uint8_t old_page_shift)
{
	uint64_t end = addr + size;

	for (uint32_t i = 0; i < segment_count; i++) {
		uint64_t seg_start = segments[i].addr;
		uint64_t seg_end = segments[i].addr + segments[i].size;

		if (seg_start < end && addr < seg_end &&
		    segments[i].page_shift < old_page_shift)
			return (true);
	}
	return (false);
}

static bool
nvkm_drm_vm_binding_needs_map_materialize(
    struct nvkm_drm_file *nfile, const struct nvkm_drm_vm_bind_segment *segments,
    uint32_t segment_count, const struct nvkm_drm_vm_binding *binding,
    uint64_t addr, uint64_t end, bool *pmaterialize_full_cover)
{
	uint64_t old_start, old_end, cut_start, cut_end;
	bool full_cover, lower_target;

	(void)nfile;
	if (binding->page_shift <= NVKM_GMMU_SPT_SHIFT)
		return (false);
	old_start = binding->addr;
	old_end = binding->addr + binding->size;
	cut_start = old_start > addr ? old_start : addr;
	cut_end = old_end < end ? old_end : end;
	if (cut_start >= cut_end)
		return (false);
	full_cover = (cut_start == old_start && cut_end == old_end);
	lower_target = nvkm_drm_vm_bind_segments_overlap_lower(segments,
	    segment_count, cut_start, cut_end, binding->page_shift);
	if (pmaterialize_full_cover != NULL)
		*pmaterialize_full_cover = lower_target;
	if (full_cover && !lower_target)
		return (false);
	return (true);
}

static int
nvkm_drm_vm_materialize_plan_prepare_map_conflicts(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_materialize_plan *plan,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    uint64_t addr, uint64_t size)
{
	struct nvkm_drm_vm_binding *binding;
	uint64_t end = addr + size;
	uint32_t count = 0;
	int err;

	(void)sc;
	nvkm_drm_vm_materialize_plan_init(plan);
	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL; binding = nvkm_drm_vm_binding_next_overlap(nfile,
	    binding, addr, size)) {
		if (nvkm_drm_vm_binding_needs_map_materialize(nfile, segments,
		    segment_count, binding, addr, end, NULL))
			count++;
	}
	err = nvkm_drm_vm_materialize_plan_alloc(plan, count);
	if (err != 0)
		return (err);
	if (count == 0)
		return (0);

	count = 0;
	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL; binding = nvkm_drm_vm_binding_next_overlap(nfile,
	    binding, addr, size)) {
		bool materialize_full_cover = false;

		if (!nvkm_drm_vm_binding_needs_map_materialize(nfile, segments,
		    segment_count, binding, addr, end,
		    &materialize_full_cover))
			continue;
		err = nvkm_drm_vm_materialize_entry_prepare(nfile,
		    &plan->entries[count], binding, addr, size,
		    materialize_full_cover);
		if (err != 0) {
			nvkm_drm_vm_materialize_plan_fini(nfile->vmm, plan);
			return (err);
		}
		count++;
	}
	return (0);
}

/*
 * nvkm_drm_vm_bindings_ensure_target_pts_except_2m()
 *
 * Ownership:
 *   Borrows nfile and the live binding tree.  It prepares lower LPT/SPT
 *   storage for a non-2M MAP segment; it does not acquire GEM ownership, write
 *   PTEs, or mutate mappings.
 *
 * Lifetime:
 *   Allocated PT pages become owned by the VMM.  Windows currently covered by
 *   a live 2 MiB leaf are skipped because the later materialize split plan
 *   supplies their child PTs before lower-page writers run.  Windows covered
 *   by a caller-owned sparse-clear plan are also skipped when that plan proves
 *   it will split the old sparse parent and provide child PT storage before
 *   the prepared writer runs.
 *
 * Threading:
 *   Requires nfile->vm_token.  May allocate page-table pages and therefore
 *   belongs to VM_BIND prepare, before any large-leaf materialize commit.
 */
static int
nvkm_drm_vm_bindings_ensure_target_pts_except_2m(
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size,
    uint8_t page_shift,
    const struct nvkm_gsp_vmm_sparse_unmap_plan *sparse_clear_plan)
{
	uint64_t page_2m = 1ULL << NVKM_GMMU_PD0_SHIFT;
	uint64_t end;
	uint64_t cur;

	if (size == 0)
		return (0);
	if (addr > UINT64_MAX - size)
		return (-EINVAL);
	end = addr + size;

	for (cur = addr; cur < end;) {
		struct nvkm_drm_vm_binding *binding;
		uint64_t chunk_end = (cur & ~(page_2m - 1)) + page_2m;
		uint64_t chunk_size;
		bool has_2m = false;

		if (chunk_end < cur || chunk_end > end)
			chunk_end = end;
		chunk_size = chunk_end - cur;
		for (binding = nvkm_drm_vm_binding_first_overlap(nfile, cur,
		    chunk_size); binding != NULL; binding =
		    nvkm_drm_vm_binding_next_overlap(nfile, binding, cur,
		    chunk_size)) {
			if (binding->page_shift == NVKM_GMMU_PD0_SHIFT) {
				has_2m = true;
				break;
			}
		}
		if (!has_2m) {
			int err;

			if (sparse_clear_plan != NULL) {
				err =
				    nvkm_gsp_vmm_check_unmap_sparse_range_prepared(
				    nfile->vmm, sparse_clear_plan, cur,
				    chunk_size, page_shift);
				if (err == 0) {
					cur = chunk_end;
					continue;
				}
				if (err != ENOENT)
					return (-err);
			}
			err = nvkm_gsp_vmm_ensure_pt_range(nfile->vmm, cur,
			    chunk_size);

			if (err != 0)
				return (-err);
		}
		cur = chunk_end;
	}
	return (0);
}

/*
 * nvkm_drm_vm_bindings_ensure_target_pd0_segments()
 *
 * Ownership:
 *   Borrows the MAP segment plan and prepares PD0 parent storage for direct
 *   2 MiB target segments.  It does not acquire GEM ownership, create child
 *   LPT/SPT tables, write PTEs, or mutate live bindings.
 *
 * Lifetime:
 *   Allocated PD0 pages become owned by the VMM.  The caller keeps VM_BIND
 *   serialization until the matching prepared writer consumes those slots.
 *
 * Threading:
 *   Requires nfile->vm_token.  This is prepare-stage work and may allocate
 *   page-table pages; it must run before the no-fail MAP commit section.
 */
static int
nvkm_drm_vm_bindings_ensure_target_pd0_segments(
    struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count)
{
	int err;

	for (uint32_t i = 0; i < segment_count; i++) {
		if (segments[i].page_shift != NVKM_GMMU_PD0_SHIFT)
			continue;
		err = nvkm_gsp_vmm_ensure_pd0_range(nfile->vmm,
		    segments[i].addr, segments[i].size);
		if (err != 0)
			return (-err);
	}
	return (0);
}

/*
 * nvkm_drm_vm_bindings_ensure_target_storage()
 *
 * Ownership:
 *   Borrows the MAP segment plan, nfile, and the live binding tree.  It only
 *   prepares page-table storage selected by each segment's target page class;
 *   it does not acquire GEM ownership, pin BOs, write PTEs, or mutate mappings.
 *
 * Lifetime:
 *   PT pages allocated for 4 KiB/64 KiB target segments and PD0 pages allocated
 *   for direct 2 MiB target segments become owned by the VMM.  Existing live
 *   2 MiB leaves are skipped for lower-page target segments because the later
 *   materialize split plan supplies their child PTs.  A non-NULL
 *   sparse_clear_plan is borrowed only for this prepare call; if it proves the
 *   sparse clear will materialize the child PTs, this helper does not allocate
 *   duplicate live storage.  Direct 2 MiB target segments deliberately do not
 *   allocate lower LPT/SPT storage.
 *
 * Threading:
 *   Requires nfile->vm_token.  This is prepare-stage work and may allocate
 *   page-table pages or take vmm->tok for read-only sparse-clear validation,
 *   so it must finish before the no-fail MAP commit section.
 */
static int
nvkm_drm_vm_bindings_ensure_target_storage(struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    const struct nvkm_gsp_vmm_sparse_unmap_plan *sparse_clear_plan)
{
	int err;

	for (uint32_t i = 0; i < segment_count; i++) {
		if (segments[i].page_shift == NVKM_GMMU_PD0_SHIFT) {
			err = nvkm_gsp_vmm_ensure_pd0_range(nfile->vmm,
			    segments[i].addr, segments[i].size);
		} else {
			err = nvkm_drm_vm_bindings_ensure_target_pts_except_2m(
			    nfile, segments[i].addr, segments[i].size,
			    segments[i].page_shift, sparse_clear_plan);
		}
		if (err != 0)
			return (err);
	}
	return (0);
}

/*
 * nvkm_drm_vm_bind_preflight_clear_pd0_target_segments()
 *
 * Ownership:
 *   Borrows the direct-MAP segment plan and VMM.  It does not allocate, write
 *   PTEs/PDEs, pin BOs, or mutate the software mapping tree.
 *
 * Lifetime:
 *   The result is valid while VM_BIND serialization keeps the segment plan,
 *   live binding tree, sparse-region tree, and VMM page-table state stable.
 *   A successful result proves every direct 2 MiB target segment is either an
 *   already prepared empty PD0 slot or can be cleared by the following commit
 *   pass without discovering a later segment conflict.  If
 *   allow_prepared_sparse_clear is true, the caller already owns a sparse-clear
 *   plan covering this MAP op and will commit it before the PD0 clear writer.
 *
 * Threading:
 *   Called before the no-fail direct-2M MAP clear commit.  It may acquire the
 *   VMM token through the backend check helpers but performs only read-only
 *   validation.
 */
static int
nvkm_drm_vm_bind_preflight_clear_pd0_target_segments(
    struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    bool allow_prepared_sparse_clear)
{
	int err;

	for (uint32_t i = 0; i < segment_count; i++) {
		if (segments[i].page_shift != NVKM_GMMU_PD0_SHIFT)
			continue;

		err = nvkm_gsp_vmm_check_prepared_pt_range(nfile->vmm,
		    segments[i].addr, segments[i].size,
		    NVKM_GMMU_PD0_SHIFT);
		if (err == 0)
			continue;
		if (err != EBUSY)
			return (-err);

		if (allow_prepared_sparse_clear) {
			err =
			    nvkm_gsp_vmm_check_clear_pd0_target_range_allow_sparse(
			    nfile->vmm, segments[i].addr, segments[i].size);
		} else {
			err = nvkm_gsp_vmm_check_clear_pd0_target_range(
			    nfile->vmm, segments[i].addr, segments[i].size);
		}
		if (err != 0)
			return (-err);
	}
	return (0);
}

/*
 * nvkm_drm_vm_bind_clear_pd0_target_segments_noflush()
 *
 * Ownership:
 *   Borrows the MAP segment plan and clears only direct 2 MiB target windows
 *   that still have old PD0 or lower-table ownership.  It does not mutate the
 *   software mapping tree; commit_replace_range() retires old mappings after
 *   all new target PTEs are installed.
 *
 * Lifetime:
 *   This helper runs inside the no-fail MAP commit section after
 *   nvkm_drm_vm_bind_preflight_clear_pd0_target_segments() has validated the
 *   whole segment set.  Cleared hardware ownership is published by the
 *   caller's final VMM flush/TLB invalidate.
 *
 * Threading:
 *   Requires VM_BIND serialization and the surrounding GSP/VMM mutation
 *   boundary.  It performs no allocation or BO lookup; any sparse-region or
 *   partial-window conflict must have been rejected by the earlier preflight.
 */
static int
nvkm_drm_vm_bind_clear_pd0_target_segments_noflush(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    struct nvkm_drm_vm_dirty_set *dirty_set)
{
	int err;

	err = nvkm_drm_vm_bind_preflight_clear_pd0_target_segments(nfile,
	    segments, segment_count, false);
	if (err != 0)
		return (err);

	for (uint32_t i = 0; i < segment_count; i++) {
		if (segments[i].page_shift != NVKM_GMMU_PD0_SHIFT)
			continue;

		err = nvkm_gsp_vmm_check_prepared_pt_range(nfile->vmm,
		    segments[i].addr, segments[i].size,
		    NVKM_GMMU_PD0_SHIFT);
		if (err == 0)
			continue;
		if (err != EBUSY)
			return (-err);

		err = nvkm_gsp_vmm_clear_pd0_target_noflush(nfile->vmm,
		    segments[i].addr, segments[i].size);
		if (err != 0)
			return (-err);
		nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set,
		    segments[i].addr, segments[i].size);
	}
	return (0);
}

static bool
nvkm_drm_vm_binding_range_has_2m_overlap(struct nvkm_drm_file *nfile,
    uint64_t addr, uint64_t size)
{
	struct nvkm_drm_vm_binding *binding;

	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL; binding = nvkm_drm_vm_binding_next_overlap(nfile,
	    binding, addr, size)) {
		if (binding->page_shift == NVKM_GMMU_PD0_SHIFT)
			return (true);
	}
	return (false);
}

static int
nvkm_drm_vm_bind_segment_check_writer_args(
    const struct nvkm_drm_vm_bind_segment *segment, const struct nvkm_bo *bo)
{
	uint64_t page_size;

	if (segment->size == 0)
		return (-EINVAL);
	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) == 0) {
		if (segment->page_shift == NVKM_GMMU_SPT_SHIFT)
			page_size = NVKM_GMMU_PT_PAGE_SIZE;
		else if (segment->page_shift == NVKM_GMMU_LPT_SHIFT)
			page_size = NVKM_GMMU_LPT_PAGE_SIZE;
		else if (segment->page_shift == NVKM_GMMU_PD0_SHIFT)
			page_size = NVKM_GMMU_PD0_PAGE_SIZE;
		else
			return (-EINVAL);
		if (segment->sysmem_paddrs == NULL ||
		    segment->sysmem_page_count !=
		    segment->size / NVKM_GMMU_PT_PAGE_SIZE ||
		    ((segment->addr | segment->size | segment->bo_offset) &
		    (page_size - 1)) != 0)
			return (-EINVAL);
		if (segment->page_shift != NVKM_GMMU_SPT_SHIFT) {
			uint32_t pages_per_leaf =
			    (uint32_t)(page_size / NVKM_GMMU_PT_PAGE_SIZE);

			if ((segment->paddr & (page_size - 1)) != 0 ||
			    (segment->sysmem_page_count % pages_per_leaf) != 0)
				return (-EINVAL);
			for (uint32_t i = 1;
			    i < segment->sysmem_page_count; i++) {
				if ((uint64_t)segment->sysmem_paddrs[i] !=
				    (uint64_t)segment->sysmem_paddrs[0] +
				    (uint64_t)i * NVKM_GMMU_PT_PAGE_SIZE)
					return (-EINVAL);
			}
		}
		return (0);
	}

	if (segment->page_shift == NVKM_GMMU_SPT_SHIFT)
		page_size = NVKM_GMMU_PT_PAGE_SIZE;
	else if (segment->page_shift == NVKM_GMMU_LPT_SHIFT)
		page_size = NVKM_GMMU_LPT_PAGE_SIZE;
	else if (segment->page_shift == NVKM_GMMU_PD0_SHIFT)
		page_size = NVKM_GMMU_PD0_PAGE_SIZE;
	else
		return (-EINVAL);
	if ((segment->addr | segment->paddr | segment->size) &
	    (page_size - 1))
		return (-EINVAL);
	return (0);
}

/*
 * nvkm_drm_vm_bindings_check_prepared_target_pts()
 *
 * Ownership:
 *   Borrows the target MAP plan and the VM binding tree.  It only validates
 *   that later prepared writers have page-table storage to consume; it does
 *   not allocate, write PTEs, pin BOs, or mutate mappings.
 *
 * Lifetime:
 *   The result is valid while VM_BIND serialization is held.  Chunks currently
 *   covered by a live 2 MiB leaf are intentionally skipped because the
 *   subsequent MAP-specific materialize step prepares and commits their child
 *   PTs before the writer runs.  Chunks covered by sparse_clear_plan may still
 *   look missing or busy in the live VMM lookup; the plan is the ownership
 *   proof that commit will split/clear the sparse parent before the MAP writer.
 *
 * Threading:
 *   Requires nfile->vm_token and may take vmm->tok for direct lookup checks.
 *   This is the prepare gate for prepared-only MAP writers.
 */
static int
nvkm_drm_vm_bindings_check_prepared_target_pts(struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    const struct nvkm_bo *bo,
    const struct nvkm_gsp_vmm_sparse_unmap_plan *sparse_clear_plan)
{
	uint64_t page_2m = 1ULL << NVKM_GMMU_PD0_SHIFT;
	int err;

	for (uint32_t i = 0; i < segment_count; i++) {
		uint64_t end;
		uint64_t cur;

		err = nvkm_drm_vm_bind_segment_check_writer_args(&segments[i],
		    bo);
		if (err != 0)
			return (err);
		if (segments[i].addr > UINT64_MAX - segments[i].size)
			return (-EINVAL);
		end = segments[i].addr + segments[i].size;
		for (cur = segments[i].addr; cur < end;) {
			uint64_t chunk_end = (cur & ~(page_2m - 1)) +
			    page_2m;
			uint64_t chunk_size;

			if (chunk_end < cur || chunk_end > end)
				chunk_end = end;
			chunk_size = chunk_end - cur;
			if (segments[i].page_shift == NVKM_GMMU_PD0_SHIFT ||
			    !nvkm_drm_vm_binding_range_has_2m_overlap(nfile,
			    cur, chunk_size)) {
				err = nvkm_gsp_vmm_check_prepared_pt_range(
				    nfile->vmm, cur, chunk_size,
				    segments[i].page_shift);
				if ((err == EBUSY || err == ENOENT) &&
				    sparse_clear_plan != NULL) {
					err =
					    nvkm_gsp_vmm_check_unmap_sparse_range_prepared(
					    nfile->vmm, sparse_clear_plan, cur,
					    chunk_size, segments[i].page_shift);
					if (err == 0) {
						cur = chunk_end;
						continue;
					}
				}
				if (err == EBUSY &&
				    segments[i].page_shift ==
				    NVKM_GMMU_PD0_SHIFT) {
					err =
					    nvkm_gsp_vmm_check_clear_pd0_target_range(
					    nfile->vmm, cur, chunk_size);
				}
				if (err != 0)
					return (-err);
			}
			cur = chunk_end;
		}
	}
	return (0);
}

/*
 * nvkm_drm_vm_bindings_materialize_map_conflicts()
 *
 * Ownership:
 *   Borrows the target segment plan and mutates only conflicting live
 *   mappings.  Replacement old mappings are prepared before any hardware
 *   ownership split and retired through retired_bindings after success.
 *
 * Lifetime:
 *   This is the MAP-specific materialize path.  Unlike unmap/null/sparse,
 *   a full old 2 MiB leaf still must be split when the new MAP will install
 *   64 KiB or 4 KiB target leaves over it.
 *
 * Threading:
 *   Requires nfile->vm_token and the surrounding VM_BIND/GSP serialization.
 *   All allocations are performed by the materialize helpers before their
 *   no-fail split commit starts.
 */
static int
nvkm_drm_vm_bindings_materialize_map_conflicts(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    uint64_t addr, uint64_t size, struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_materialize_plan plan;
	int err;

	if (addr >= nfile->vm_bindings_max_end)
		return (0);
	err = nvkm_drm_vm_materialize_plan_prepare_map_conflicts(sc, nfile,
	    &plan, segments, segment_count, addr, size);
	if (err != 0)
		return (err);
	err = nvkm_drm_vm_materialize_plan_commit(sc, nfile, &plan,
	    dirty_set, retired_bindings);
	nvkm_drm_vm_materialize_plan_fini(nfile->vmm, &plan);
	return (err);
}

static void nvkm_drm_vm_bindings_free_prepared(
    struct nvkm_drm_vm_binding_list *bindings);

/*
 * nvkm_drm_vm_binding_prepare_remove_tail_for_segment()
 *
 * Ownership:
 *   Borrows source and appends at most one cloned tail binding to tail_bindings.
 *   The cloned tail owns its GEM reference and VM_BIND pin on success.
 *
 * Lifetime:
 *   The source binding is not mutated.  The prepared tail is detached until the
 *   caller's remove_range commit inserts it or the abort path frees it.
 *
 * Threading:
 *   Runs under nfile->vm_token before hardware PTEs are changed.  It may sleep
 *   through allocation or BO pinning and therefore must not run in the no-fail
 *   commit section.
 */
static int
nvkm_drm_vm_binding_prepare_remove_tail_for_segment(
    struct nvkm_drm_file *nfile, const struct nvkm_drm_vm_binding *source,
    uint64_t segment_addr, uint64_t segment_size, uint64_t segment_bo_offset,
    uint8_t page_shift, uint64_t target_addr, uint64_t target_end,
    struct nvkm_drm_vm_binding_list *tail_bindings)
{
	struct nvkm_drm_vm_binding *tail;
	uint64_t segment_end, cut_start, cut_end;
	uint64_t head_size, tail_size, tail_bo_offset;
	int err;

	if (segment_size == 0 || segment_addr > UINT64_MAX - segment_size)
		return (-EINVAL);
	segment_end = segment_addr + segment_size;
	cut_start = segment_addr > target_addr ? segment_addr : target_addr;
	cut_end = segment_end < target_end ? segment_end : target_end;
	if (cut_start >= cut_end)
		return (0);

	head_size = cut_start - segment_addr;
	tail_size = segment_end - cut_end;
	if (head_size == 0 || tail_size == 0)
		return (0);

	tail_bo_offset = segment_bo_offset + (cut_end - segment_addr);
	err = nvkm_drm_vm_binding_prepare_clone(nfile, source, cut_end,
	    tail_size, tail_bo_offset, page_shift, &tail);
	if (err != 0)
		return (err);
	LIST_INSERT_HEAD(tail_bindings, tail, link);
	return (0);
}

/*
 * nvkm_drm_vm_binding_prepare_remove_tails_2m()
 *
 * Ownership:
 *   Borrows one live 2 MiB binding and prepares cloned tail bindings for the
 *   lower-level segments that materialize_large_partials() will expose.
 *
 * Lifetime:
 *   The source binding and hardware PD0 leaf remain unchanged here.  Prepared
 *   tails become valid only after the later materialize/remove commit consumes
 *   them.
 *
 * Threading:
 *   Runs in the prepare phase under nfile->vm_token.  It mirrors the 2 MiB
 *   materialization geometry but performs no BAR1 writes and no tracker splice.
 */
static int
nvkm_drm_vm_binding_prepare_remove_tails_2m(struct nvkm_drm_file *nfile,
    const struct nvkm_drm_vm_binding *binding, uint64_t target_addr,
    uint64_t target_end, struct nvkm_drm_vm_binding_list *tail_bindings)
{
	uint64_t page_2m = 1ULL << NVKM_GMMU_PD0_SHIFT;
	uint64_t page_64k = NVKM_GMMU_LPT_PAGE_SIZE;
	uint64_t old_start = binding->addr;
	uint64_t old_end;
	uint64_t window;
	int err;

	if (binding->size == 0 || old_start > UINT64_MAX - binding->size)
		return (-EINVAL);
	if ((binding->addr | binding->size | binding->bo_offset) &
	    (page_2m - 1))
		return (-EINVAL);

	old_end = binding->addr + binding->size;
	for (window = old_start; window < old_end; window += page_2m) {
		uint64_t window_end = window + page_2m;
		uint64_t cut_start = window > target_addr ? window :
		    target_addr;
		uint64_t cut_end = window_end < target_end ? window_end :
		    target_end;

		if (cut_start >= cut_end || (cut_start == window &&
		    cut_end == window_end))
			continue;

		for (uint64_t sub = window; sub < window_end;
		    sub += page_64k) {
			uint64_t sub_end = sub + page_64k;
			uint64_t sub_cut_start = sub > target_addr ? sub :
			    target_addr;
			uint64_t sub_cut_end = sub_end < target_end ? sub_end :
			    target_end;
			uint64_t sub_bo_offset = binding->bo_offset +
			    (sub - old_start);

			if (sub_cut_start >= sub_cut_end ||
			    (sub_cut_start == sub && sub_cut_end == sub_end))
				continue;

			err = nvkm_drm_vm_binding_prepare_remove_tail_for_segment(
			    nfile, binding, sub, page_64k, sub_bo_offset,
			    NVKM_GMMU_SPT_SHIFT, target_addr, target_end,
			    tail_bindings);
			if (err != 0)
				return (err);
		}
	}
	return (0);
}

/*
 * nvkm_drm_vm_bindings_prepare_remove_range()
 *
 * Ownership:
 *   Preallocates every tail binding that remove_range() may need after large
 *   partial mappings are materialized.  The returned tail_bindings list owns
 *   GEM references and VM_BIND pins until commit consumes it or abort frees it.
 *
 * Lifetime:
 *   Existing live bindings and hardware PTEs are not changed here.  The prepared
 *   tails are valid only while the caller keeps nfile->vm_token and then either
 *   calls remove_range() commit logic or aborts the list.
 *
 * Threading:
 *   Runs before the VM_BIND no-fail commit section mutates page tables.  It may
 *   sleep while allocating binding nodes and pinning BOs.
 */
static int
nvkm_drm_vm_bindings_prepare_remove_range(struct nvkm_drm_file *nfile,
    uint64_t addr, uint64_t size,
    struct nvkm_drm_vm_binding_list *tail_bindings)
{
	struct nvkm_drm_vm_binding *binding;
	uint64_t end;
	int err;

	LIST_INIT(tail_bindings);
	if (size == 0 || addr > UINT64_MAX - size)
		return (-EINVAL);
	end = addr + size;
	if (addr >= nfile->vm_bindings_max_end)
		return (0);

	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL;
	    binding = nvkm_drm_vm_binding_next_overlap(nfile, binding,
	    addr, size)) {
		uint64_t old_start, old_end, cut_start, cut_end;
		uint8_t tail_page_shift;

		old_start = binding->addr;
		if (binding->size == 0 ||
		    old_start > UINT64_MAX - binding->size) {
			err = -EINVAL;
			goto fail;
		}
		old_end = binding->addr + binding->size;
		cut_start = old_start > addr ? old_start : addr;
		cut_end = old_end < end ? old_end : end;
		if (cut_start >= cut_end ||
		    (cut_start == old_start && cut_end == old_end))
			continue;

		if (binding->page_shift == NVKM_GMMU_PD0_SHIFT) {
			err = nvkm_drm_vm_binding_prepare_remove_tails_2m(nfile,
			    binding, addr, end, tail_bindings);
			if (err != 0)
				goto fail;
			continue;
		}

		tail_page_shift = binding->page_shift;
		if (binding->page_shift > NVKM_GMMU_SPT_SHIFT)
			tail_page_shift = NVKM_GMMU_SPT_SHIFT;
		err = nvkm_drm_vm_binding_prepare_remove_tail_for_segment(nfile,
		    binding, old_start, binding->size, binding->bo_offset,
		    tail_page_shift, addr, end, tail_bindings);
		if (err != 0)
			goto fail;
	}
	return (0);

fail:
	nvkm_drm_vm_bindings_free_prepared(tail_bindings);
	return (err);
}

/*
 * nvkm_drm_vm_bindings_free_prepared()
 *
 * Ownership:
 *   Consumes every binding currently linked on bindings.  Each binding owns a
 *   GEM reference and may own a VM_BIND BO pin; both are released here.
 *
 * Lifetime:
 *   The list head remains owned by the caller and is empty on return.
 *
 * Threading:
 *   Called from VM_BIND prepare/abort paths while the caller owns the per-file
 *   VM token.  It does not touch hardware page tables.
 */
static void
nvkm_drm_vm_bindings_free_prepared(
    struct nvkm_drm_vm_binding_list *bindings)
{
	nvkm_drm_vm_bindings_release(bindings);
}

/*
 * nvkm_drm_vm_bindings_abort_replace_range()
 *
 * Ownership:
 *   Consumes prepared tail bindings that were not committed into the live
 *   tracker.  Each node owns its GEM reference and VM_BIND pin.
 *   Existing live bindings and hardware PTEs are untouched.
 *
 * Lifetime:
 *   The prepared list must not be used after this call except as an empty list.
 *
 * Threading:
 *   Requires the same VM_BIND serialization as prepare/commit.
 */
static void
nvkm_drm_vm_bindings_abort_replace_range(
    struct nvkm_drm_vm_binding_list *tail_bindings)
{
	nvkm_drm_vm_bindings_free_prepared(tail_bindings);
}

/*
 * nvkm_drm_vm_bindings_commit_replace_range()
 *
 * Ownership:
 *   Consumes prepared tail bindings and mutates nfile's binding tracker to
 *   describe the final VA state after a successful MAP replacement.
 *
 * Lifetime:
 *   The caller must have already installed the new PTEs for [addr, addr+size).
 *   Old bindings covering that exact range are removed from the software
 *   tracker without first writing invalid PTEs, because the new valid PTEs have
 *   already overwritten them and the caller will flush once before ioctl
 *   return.  Fully covered old bindings move to retired_bindings; the caller
 *   must release them only after the VM_BIND done fence is signaled.
 *
 * Threading:
 *   Requires nfile->vm_token.  This function does not acquire the VMM token and
 *   does not touch BAR1; it is a no-fail software state transition.
 */
static void
nvkm_drm_vm_bindings_commit_replace_range(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size,
    struct nvkm_drm_vm_binding_list *tail_bindings,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_binding *binding, *next;
	uint64_t end = addr + size;
	bool changed = false;

	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL; binding = next) {
		uint64_t old_start, old_end, cut_start, cut_end;
		uint64_t head_size, tail_size;

		next = nvkm_drm_vm_binding_next_overlap(nfile, binding,
		    addr, size);
		changed = true;
		old_start = binding->addr;
		old_end = binding->addr + binding->size;
		cut_start = old_start > addr ? old_start : addr;
		cut_end = old_end < end ? old_end : end;
		head_size = cut_start - old_start;
		tail_size = old_end - cut_end;
		nvkm_drm_vm_bind_note_replace_clear_skip(sc, cut_end - cut_start);

		if (head_size != 0 && tail_size != 0) {
			binding->size = head_size;
		} else if (head_size != 0) {
			binding->size = head_size;
		} else if (tail_size != 0) {
			nvkm_drm_vm_binding_rekey_tail(nfile, binding,
			    cut_end, tail_size,
			    binding->bo_offset + cut_end - old_start);
		} else {
			binding->pte_installed = false;
			nvkm_drm_vm_binding_unlink_retire(binding,
			    retired_bindings);
		}
	}

	while ((binding = LIST_FIRST(tail_bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_insert_sorted(nfile, binding);
	}
	if (changed)
		nvkm_drm_vm_bindings_recalc_max_end(nfile);
}

/*
 * nvkm_drm_vm_valid_map_plan_*()
 *
 * Ownership:
 *   The plan owns every fallible object prepared for one non-noop valid MAP:
 *   sparse-clear ownership transferred from the caller, replacement tails, new
 *   target bindings, and the large-leaf materialize plan.  Commit consumes those
 *   objects into the VMM page tables and live mapping tree.
 *
 * Lifetime:
 *   The caller must keep the segment plan, BO, and VM remap serialization alive
 *   from prepare through commit/fini.  On abort, fini releases all unpublished
 *   objects.  On successful commit, old live mappings move to retired_bindings
 *   and remain caller-owned until the VM_BIND done fence is visible.
 *
 * Threading:
 *   Prepare may allocate, pin, populate TT pages, and inspect VMM state.  Commit
 *   runs under the VM_BIND/GSP mutation boundary and should only consume prepared
 *   state, write PTE/PDEs, and splice the software mapping tree.
 */
static void
nvkm_drm_vm_valid_map_plan_init(struct nvkm_drm_vm_valid_map_plan *plan)
{
	LIST_INIT(&plan->new_bindings);
	LIST_INIT(&plan->replace_tails);
	nvkm_drm_vm_materialize_plan_init(&plan->materialize_plan);
	plan->sparse_clear_plan = NULL;
	plan->committed = false;
}

static void
nvkm_drm_vm_valid_map_plan_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_valid_map_plan *plan)
{
	if (!plan->committed) {
		nvkm_drm_vm_bindings_free_prepared(&plan->new_bindings);
		nvkm_drm_vm_bindings_abort_replace_range(&plan->replace_tails);
	}
	nvkm_drm_vm_bind_abort_sparse_clear(nfile, &plan->sparse_clear_plan);
	nvkm_drm_vm_materialize_plan_fini(nfile->vmm,
	    &plan->materialize_plan);
	nvkm_drm_vm_valid_map_plan_init(plan);
}

static int
nvkm_drm_vm_valid_map_plan_prepare(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_valid_map_plan *plan,
    struct drm_gem_object *obj, const struct nvkm_drm_vm_bind_segment *segments,
    uint32_t segment_count, uint64_t addr, uint64_t size, uint8_t pte_kind,
    struct nvkm_gsp_vmm_sparse_unmap_plan **psparse_clear_plan,
    const struct nvkm_bo *bo)
{
	int err;

	nvkm_drm_vm_valid_map_plan_init(plan);
	plan->sparse_clear_plan = *psparse_clear_plan;
	*psparse_clear_plan = NULL;

	err = nvkm_drm_vm_bindings_prepare_remove_range(nfile, addr, size,
	    &plan->replace_tails);
	if (err != 0)
		goto fail;

	err = nvkm_drm_vm_bind_prepare_segment_bindings(nfile, obj, segments,
	    segment_count, pte_kind, &plan->new_bindings);
	if (err != 0)
		goto fail;

	err = nvkm_drm_vm_bindings_ensure_target_storage(nfile, segments,
	    segment_count, plan->sparse_clear_plan);
	if (err != 0)
		goto fail;

	err = nvkm_drm_vm_bindings_check_prepared_target_pts(nfile, segments,
	    segment_count, bo, plan->sparse_clear_plan);
	if (err != 0)
		goto fail;

	err = nvkm_drm_vm_materialize_plan_prepare_map_conflicts(sc, nfile,
	    &plan->materialize_plan, segments, segment_count, addr, size);
	if (err != 0)
		goto fail;

	err = nvkm_drm_vm_materialize_plan_preflight(nfile,
	    &plan->materialize_plan);
	if (err != 0)
		goto fail;

	err = nvkm_drm_vm_bind_preflight_clear_pd0_target_segments(nfile,
	    segments, segment_count, plan->sparse_clear_plan != NULL);
	if (err != 0)
		goto fail;

	return (0);

fail:
	nvkm_drm_vm_valid_map_plan_fini(nfile, plan);
	return (err);
}

static int
nvkm_drm_vm_valid_map_plan_commit_preflight(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_valid_map_plan *plan,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    const struct nvkm_bo *bo)
{
	int err;

	err = nvkm_drm_vm_materialize_plan_preflight(nfile,
	    &plan->materialize_plan);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_bindings_check_prepared_target_pts(nfile, segments,
	    segment_count, bo, plan->sparse_clear_plan);
	if (err != 0)
		return (err);

	return (nvkm_drm_vm_bind_preflight_clear_pd0_target_segments(nfile,
	    segments, segment_count, plan->sparse_clear_plan != NULL));
}

static int
nvkm_drm_vm_valid_map_plan_commit(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_valid_map_plan *plan,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    struct nvkm_bo *bo, uint32_t op_flags, uint64_t addr, uint64_t size,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	int err;

	err = nvkm_drm_vm_valid_map_plan_commit_preflight(nfile, plan,
	    segments, segment_count, bo);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_materialize_plan_commit(sc, nfile,
	    &plan->materialize_plan, dirty_set, retired_bindings);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_bind_commit_sparse_clear_noflush(sc, nfile,
	    &plan->sparse_clear_plan, NVKM_DRM_VM_TRACE_MAP, addr, size,
	    dirty_set);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_bind_clear_pd0_target_segments_noflush(sc, nfile,
	    segments, segment_count, dirty_set);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_bind_map_segments_noflush(sc, nfile, segments,
	    segment_count, bo, op_flags & 0xff, dirty_set);
	if (err != 0)
		return (-err);

	nvkm_drm_vm_bind_note_map_segments(sc, op_flags, segments,
	    segment_count, bo);
	nvkm_drm_vm_bindings_commit_replace_range(sc, nfile, addr, size,
	    &plan->replace_tails, retired_bindings);
	nvkm_drm_vm_bindings_insert_prepared(nfile, &plan->new_bindings);
	plan->committed = true;
	return (0);
}

/*
 * nvkm_drm_vm_remove_plan_*()
 *
 * Ownership:
 *   The plan owns detached tail bindings and the large-leaf materialize plan
 *   prepared for one valid-range removal.  Commit consumes tail bindings into
 *   nfile's live tracker and moves fully removed live bindings to the caller's
 *   retired list.
 *
 * Lifetime:
 *   The plan is valid only while the caller keeps VM remap serialization.  Fini
 *   releases any prepared ownership not consumed by a successful commit.  The
 *   caller keeps retired bindings alive until the VM_BIND done fence is visible.
 *
 * Threading:
 *   Prepare may allocate and pin, so it runs before the no-fail commit section.
 *   Commit runs with the same VM_BIND/GSP serialization as the PTE writers and
 *   does not flush or publish fences; the caller consumes dirty_set at the
 *   enclosing invalidate boundary.
 */
static void
nvkm_drm_vm_remove_plan_init(struct nvkm_drm_vm_remove_plan *plan)
{
	LIST_INIT(&plan->tail_bindings);
	nvkm_drm_vm_materialize_plan_init(&plan->materialize_plan);
	plan->addr = 0;
	plan->size = 0;
	plan->clear_empty_range = false;
	plan->preserve_target_pts = false;
	plan->preserve_page_shift = NVKM_GMMU_SPT_SHIFT;
	plan->materialize_full_cover = false;
	plan->clear_action = NVKM_DRM_VM_TRACE_UNMAP;
}

static void
nvkm_drm_vm_remove_plan_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_remove_plan *plan)
{
	nvkm_drm_vm_bindings_free_prepared(&plan->tail_bindings);
	nvkm_drm_vm_materialize_plan_fini(nfile->vmm,
	    &plan->materialize_plan);
	nvkm_drm_vm_remove_plan_init(plan);
}

static int
nvkm_drm_vm_remove_plan_prepare(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_remove_plan *plan,
    uint64_t addr, uint64_t size, bool clear_empty_range,
    bool preserve_target_pts, uint8_t preserve_page_shift,
    bool materialize_full_cover, uint32_t clear_action)
{
	int err;

	nvkm_drm_vm_remove_plan_init(plan);
	if (size == 0 || addr > UINT64_MAX - size)
		return (-EINVAL);
	plan->addr = addr;
	plan->size = size;
	plan->clear_empty_range = clear_empty_range;
	plan->preserve_target_pts = preserve_target_pts;
	plan->preserve_page_shift = preserve_page_shift;
	plan->materialize_full_cover = materialize_full_cover;
	plan->clear_action = clear_action;

	if (addr >= nfile->vm_bindings_max_end)
		return (0);

	err = nvkm_drm_vm_bindings_prepare_remove_range(nfile, addr, size,
	    &plan->tail_bindings);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_materialize_plan_prepare_range(sc, nfile,
	    &plan->materialize_plan, addr, size, materialize_full_cover);
	if (err != 0) {
		nvkm_drm_vm_remove_plan_fini(nfile, plan);
		return (err);
	}
	err = nvkm_drm_vm_materialize_plan_preflight(nfile,
	    &plan->materialize_plan);
	if (err != 0) {
		nvkm_drm_vm_remove_plan_fini(nfile, plan);
		return (err);
	}
	return (0);
}

static int
nvkm_drm_vm_remove_plan_commit(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_remove_plan *plan,
    uint32_t *punmapped, struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_binding *binding, *next;
	uint64_t addr = plan->addr;
	uint64_t size = plan->size;
	uint64_t end = addr + size;
	int err;

	*punmapped = 0;
	if (addr >= nfile->vm_bindings_max_end) {
		if (plan->clear_empty_range)
			nvkm_drm_vm_bind_note_empty_clear_skip(sc, size);
		return (0);
	}

	err = nvkm_drm_vm_materialize_plan_commit(sc, nfile,
	    &plan->materialize_plan, dirty_set, retired_bindings);
	if (err != 0)
		return (err);

	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL;
	    binding = nvkm_drm_vm_binding_next_overlap(nfile, binding,
	    addr, size)) {
		(*punmapped)++;
	}

	if (*punmapped != 0) {
		for (binding = nvkm_drm_vm_binding_first_overlap(nfile,
		    addr, size); binding != NULL;
		    binding = nvkm_drm_vm_binding_next_overlap(nfile,
		    binding, addr, size)) {
			uint64_t old_start, old_end, cut_start, cut_end;
			uint8_t clear_page_shift;

			old_start = binding->addr;
			old_end = binding->addr + binding->size;
			cut_start = old_start > addr ? old_start : addr;
			cut_end = old_end < end ? old_end : end;
			clear_page_shift = plan->preserve_target_pts ?
			    plan->preserve_page_shift : binding->page_shift;
			err = nvkm_gsp_vmm_check_unmap_valid_range_page(
			    nfile->vmm, cut_start, cut_end - cut_start,
			    plan->preserve_target_pts, clear_page_shift);
			if (err != 0)
				return (-err);
		}
		for (binding = nvkm_drm_vm_binding_first_overlap(nfile,
		    addr, size); binding != NULL;
		    binding = nvkm_drm_vm_binding_next_overlap(nfile,
		    binding, addr, size)) {
			uint64_t old_start, old_end, cut_start, cut_end;
			uint8_t clear_page_shift;

			old_start = binding->addr;
			old_end = binding->addr + binding->size;
			cut_start = old_start > addr ? old_start : addr;
			cut_end = old_end < end ? old_end : end;
			clear_page_shift = plan->preserve_target_pts ?
			    plan->preserve_page_shift : binding->page_shift;
			nvkm_drm_vm_bind_note_clear_shape(sc,
			    to_nvkm_bo(binding->obj), binding->pte_kind,
			    binding->page_shift, cut_end - cut_start);
			nvkm_drm_vm_bind_note_clear(sc, plan->clear_action,
			    cut_end - cut_start);
			if (plan->preserve_target_pts) {
				if (nvkm_drm_vm_bind_clear_is_conflict(
				    plan->clear_action)) {
					err =
					    nvkm_gsp_vmm_clear_conflict_preserve_page_noflush(
					    nfile->vmm, cut_start,
					    cut_end - cut_start,
					    clear_page_shift);
				} else {
					err =
					    nvkm_gsp_vmm_unmap_valid_preserve_page_noflush(
					    nfile->vmm, cut_start,
					    cut_end - cut_start,
					    clear_page_shift);
				}
			} else {
				err = nvkm_gsp_vmm_unmap_valid_page_noflush(
				    nfile->vmm, cut_start, cut_end - cut_start,
				    clear_page_shift);
			}
			if (err != 0)
				return (-err);
			nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set,
			    cut_start, cut_end - cut_start);
		}
	} else if (plan->clear_empty_range) {
		nvkm_drm_vm_bind_note_empty_clear_skip(sc, size);
	}

	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, addr, size);
	    binding != NULL; binding = next) {
		uint64_t old_start, old_end, cut_start, cut_end;
		uint64_t head_size, tail_size;

		next = nvkm_drm_vm_binding_next_overlap(nfile, binding,
		    addr, size);
		old_start = binding->addr;
		old_end = binding->addr + binding->size;
		cut_start = old_start > addr ? old_start : addr;
		cut_end = old_end < end ? old_end : end;
		head_size = cut_start - old_start;
		tail_size = old_end - cut_end;

		if (head_size != 0 && tail_size != 0) {
			binding->size = head_size;
		} else if (head_size != 0) {
			binding->size = head_size;
		} else if (tail_size != 0) {
			nvkm_drm_vm_binding_rekey_tail(nfile, binding,
			    cut_end, tail_size,
			    binding->bo_offset + cut_end - old_start);
		} else {
			binding->pte_installed = false;
			nvkm_drm_vm_binding_unlink_retire(binding,
			    retired_bindings);
		}
	}

	while ((binding = LIST_FIRST(&plan->tail_bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_insert_sorted(nfile, binding);
	}
	if (*punmapped != 0)
		nvkm_drm_vm_bindings_recalc_max_end(nfile);
	return (0);
}

/*
 * nvkm_drm_vm_clear_plan_*()
 *
 * Ownership:
 *   The plan owns every prepared object needed by one clear-style op: the
 *   optional sparse clear plan plus the valid-range remove plan.  Commit moves
 *   fully removed valid bindings to retired_bindings and consumes sparse clear
 *   storage into the VMM sparse tree.
 *
 * Lifetime:
 *   The caller keeps VM_BIND serialization from prepare through commit/fini.
 *   On abort, fini releases unpublished sparse/removal ownership and leaves the
 *   live mapping tree unchanged.  On successful commit, old BO refs/pins remain
 *   owned by retired_bindings until the VM_BIND done fence is visible.
 *
 * Threading:
 *   Prepare may allocate sparse split records, clone tail mappings, and pin BOs.
 *   Commit runs inside the VM_BIND/GSP mutation boundary and does not flush; the
 *   caller publishes dirty PTE/PDE writes at the enclosing invalidate boundary.
 */
static void
nvkm_drm_vm_clear_plan_init(struct nvkm_drm_vm_clear_plan *plan)
{
	nvkm_drm_vm_remove_plan_init(&plan->remove_plan);
	plan->sparse_clear_plan = NULL;
	plan->addr = 0;
	plan->size = 0;
	plan->action = NVKM_DRM_VM_TRACE_UNMAP;
}

static void
nvkm_drm_vm_clear_plan_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_clear_plan *plan)
{
	nvkm_drm_vm_bind_abort_sparse_clear(nfile, &plan->sparse_clear_plan);
	nvkm_drm_vm_remove_plan_fini(nfile, &plan->remove_plan);
	nvkm_drm_vm_clear_plan_init(plan);
}

static int
nvkm_drm_vm_clear_plan_prepare(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_clear_plan *plan,
    uint64_t addr, uint64_t size, bool clear_sparse, bool clear_empty_range,
    bool preserve_target_pts, uint8_t preserve_page_shift,
    bool materialize_full_cover, uint32_t action)
{
	int err;

	nvkm_drm_vm_clear_plan_init(plan);
	plan->addr = addr;
	plan->size = size;
	plan->action = action;

	if (clear_sparse) {
		err = nvkm_drm_vm_bind_prepare_sparse_clear(nfile, addr, size,
		    NVKM_GMMU_PD0_SHIFT, NVKM_DRM_VM_SPARSE_CLEAR_FINAL,
		    &plan->sparse_clear_plan);
		if (err != 0)
			goto fail;
	}

	err = nvkm_drm_vm_remove_plan_prepare(sc, nfile, &plan->remove_plan,
	    addr, size, clear_empty_range, preserve_target_pts,
	    preserve_page_shift, materialize_full_cover, action);
	if (err != 0)
		goto fail;

	return (0);

fail:
	nvkm_drm_vm_clear_plan_fini(nfile, plan);
	return (err);
}

static int
nvkm_drm_vm_clear_plan_commit(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_clear_plan *plan,
    uint32_t *punmapped, struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	int err;

	err = nvkm_drm_vm_remove_plan_commit(sc, nfile, &plan->remove_plan,
	    punmapped, dirty_set, retired_bindings);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_bind_commit_sparse_clear_noflush(sc, nfile,
	    &plan->sparse_clear_plan, plan->action, plan->addr, plan->size,
	    dirty_set);
	if (err != 0)
		return (err);

	return (0);
}

/*
 * nvkm_drm_vm_segment_remove_plan_*()
 *
 * Ownership:
 *   The plan borrows a caller-owned, contiguous segment array and owns detached
 *   tail bindings plus a materialize plan for the whole covered range.  Commit
 *   clears valid PTE/PDE state per segment page_shift, then performs one mapping
 *   splice for the full range.
 *
 * Lifetime:
 *   The borrowed segments must outlive prepare, commit, and fini.  Prepared
 *   tail/materialize ownership is private to this plan until commit consumes it
 *   or fini aborts it.  Retired bindings remain caller-owned after commit.
 *
 * Threading:
 *   Prepare may allocate/pin and must run before the no-fail clear section.
 *   Commit requires VM_BIND/GSP serialization and does not flush; visibility is
 *   still published by the enclosing VM_BIND invalidate boundary.
 */
static void
nvkm_drm_vm_segment_remove_plan_init(
    struct nvkm_drm_vm_segment_remove_plan *plan)
{
	LIST_INIT(&plan->tail_bindings);
	nvkm_drm_vm_materialize_plan_init(&plan->materialize_plan);
	plan->segments = NULL;
	plan->segment_count = 0;
	plan->addr = 0;
	plan->size = 0;
	plan->clear_action = NVKM_DRM_VM_TRACE_MAP_SPARSE;
}

static void
nvkm_drm_vm_segment_remove_plan_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_segment_remove_plan *plan)
{
	nvkm_drm_vm_bindings_free_prepared(&plan->tail_bindings);
	nvkm_drm_vm_materialize_plan_fini(nfile->vmm,
	    &plan->materialize_plan);
	nvkm_drm_vm_segment_remove_plan_init(plan);
}

static int
nvkm_drm_vm_segment_remove_plan_check_coverage(
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    uint64_t addr, uint64_t size)
{
	uint64_t cur, end;

	if (segments == NULL || segment_count == 0 || size == 0 ||
	    addr > UINT64_MAX - size)
		return (-EINVAL);
	cur = addr;
	end = addr + size;
	for (uint32_t i = 0; i < segment_count; i++) {
		uint64_t seg_end;

		if (segments[i].size == 0 ||
		    segments[i].addr != cur ||
		    segments[i].addr > UINT64_MAX - segments[i].size)
			return (-EINVAL);
		seg_end = segments[i].addr + segments[i].size;
		if (seg_end > end)
			return (-EINVAL);
		cur = seg_end;
	}
	if (cur != end)
		return (-EINVAL);
	return (0);
}

static int
nvkm_drm_vm_segment_remove_plan_prepare(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_segment_remove_plan *plan,
    const struct nvkm_drm_vm_bind_segment *segments, uint32_t segment_count,
    uint64_t addr, uint64_t size, uint32_t clear_action)
{
	int err;

	nvkm_drm_vm_segment_remove_plan_init(plan);
	err = nvkm_drm_vm_segment_remove_plan_check_coverage(segments,
	    segment_count, addr, size);
	if (err != 0)
		return (err);
	plan->segments = segments;
	plan->segment_count = segment_count;
	plan->addr = addr;
	plan->size = size;
	plan->clear_action = clear_action;

	if (addr >= nfile->vm_bindings_max_end)
		return (0);

	err = nvkm_drm_vm_bindings_prepare_remove_range(nfile, addr, size,
	    &plan->tail_bindings);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_materialize_plan_prepare_map_conflicts(sc, nfile,
	    &plan->materialize_plan, segments, segment_count, addr, size);
	if (err != 0) {
		nvkm_drm_vm_segment_remove_plan_fini(nfile, plan);
		return (err);
	}
	err = nvkm_drm_vm_materialize_plan_preflight(nfile,
	    &plan->materialize_plan);
	if (err != 0) {
		nvkm_drm_vm_segment_remove_plan_fini(nfile, plan);
		return (err);
	}
	return (0);
}

static int
nvkm_drm_vm_segment_remove_plan_commit(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_segment_remove_plan *plan, uint32_t *punmapped,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_binding *binding, *next;
	uint64_t end = plan->addr + plan->size;
	int err;

	*punmapped = 0;
	if (plan->addr >= nfile->vm_bindings_max_end)
		return (0);

	err = nvkm_drm_vm_materialize_plan_commit(sc, nfile,
	    &plan->materialize_plan, dirty_set, retired_bindings);
	if (err != 0)
		return (err);

	for (uint32_t i = 0; i < plan->segment_count; i++) {
		const struct nvkm_drm_vm_bind_segment *seg = &plan->segments[i];

		for (binding = nvkm_drm_vm_binding_first_overlap(nfile,
		    seg->addr, seg->size); binding != NULL; binding =
		    nvkm_drm_vm_binding_next_overlap(nfile, binding,
		    seg->addr, seg->size)) {
			(*punmapped)++;
		}
	}

	for (uint32_t i = 0; i < plan->segment_count; i++) {
		const struct nvkm_drm_vm_bind_segment *seg = &plan->segments[i];

		for (binding = nvkm_drm_vm_binding_first_overlap(nfile,
		    seg->addr, seg->size); binding != NULL; binding =
		    nvkm_drm_vm_binding_next_overlap(nfile, binding,
		    seg->addr, seg->size)) {
			uint64_t old_start, old_end, cut_start, cut_end;

			old_start = binding->addr;
			old_end = binding->addr + binding->size;
			cut_start = old_start > seg->addr ? old_start : seg->addr;
			cut_end = old_end < seg->addr + seg->size ? old_end :
			    seg->addr + seg->size;
			err = nvkm_gsp_vmm_check_unmap_valid_range_page(
			    nfile->vmm, cut_start, cut_end - cut_start, true,
			    seg->page_shift);
			if (err != 0)
				return (-err);
		}
	}

	for (uint32_t i = 0; i < plan->segment_count; i++) {
		const struct nvkm_drm_vm_bind_segment *seg = &plan->segments[i];

		for (binding = nvkm_drm_vm_binding_first_overlap(nfile,
		    seg->addr, seg->size); binding != NULL; binding =
		    nvkm_drm_vm_binding_next_overlap(nfile, binding,
		    seg->addr, seg->size)) {
			uint64_t old_start, old_end, cut_start, cut_end;

			old_start = binding->addr;
			old_end = binding->addr + binding->size;
			cut_start = old_start > seg->addr ? old_start : seg->addr;
			cut_end = old_end < seg->addr + seg->size ? old_end :
			    seg->addr + seg->size;
			nvkm_drm_vm_bind_note_clear_shape(sc,
			    to_nvkm_bo(binding->obj), binding->pte_kind,
			    binding->page_shift, cut_end - cut_start);
			nvkm_drm_vm_bind_note_clear(sc, plan->clear_action,
			    cut_end - cut_start);
			if (nvkm_drm_vm_bind_clear_is_conflict(
			    plan->clear_action)) {
				err =
				    nvkm_gsp_vmm_clear_conflict_preserve_page_noflush(
				    nfile->vmm, cut_start, cut_end - cut_start,
				    seg->page_shift);
			} else {
				err =
				    nvkm_gsp_vmm_unmap_valid_preserve_page_noflush(
				    nfile->vmm, cut_start, cut_end - cut_start,
				    seg->page_shift);
			}
			if (err != 0)
				return (-err);
			nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set,
			    cut_start, cut_end - cut_start);
		}
	}

	for (binding = nvkm_drm_vm_binding_first_overlap(nfile, plan->addr,
	    plan->size); binding != NULL; binding = next) {
		uint64_t old_start, old_end, cut_start, cut_end;
		uint64_t head_size, tail_size;

		next = nvkm_drm_vm_binding_next_overlap(nfile, binding,
		    plan->addr, plan->size);
		old_start = binding->addr;
		old_end = binding->addr + binding->size;
		cut_start = old_start > plan->addr ? old_start : plan->addr;
		cut_end = old_end < end ? old_end : end;
		head_size = cut_start - old_start;
		tail_size = old_end - cut_end;

		if (head_size != 0 && tail_size != 0) {
			binding->size = head_size;
		} else if (head_size != 0) {
			binding->size = head_size;
		} else if (tail_size != 0) {
			nvkm_drm_vm_binding_rekey_tail(nfile, binding,
			    cut_end, tail_size,
			    binding->bo_offset + cut_end - old_start);
		} else {
			binding->pte_installed = false;
			nvkm_drm_vm_binding_unlink_retire(binding,
			    retired_bindings);
		}
	}

	while ((binding = LIST_FIRST(&plan->tail_bindings)) != NULL) {
		LIST_REMOVE(binding, link);
		nvkm_drm_vm_binding_insert_sorted(nfile, binding);
	}
	if (*punmapped != 0)
		nvkm_drm_vm_bindings_recalc_max_end(nfile);
	return (0);
}

/*
 * nvkm_drm_vm_sparse_map_plan_*()
 *
 * Ownership:
 *   The plan owns every fallible object prepared for one MAP|SPARSE op: the
 *   sparse segment array, unlinked sparse regions, the sparse-clear plan for
 *   old sparse reservations, and the valid target-clear plan.  Commit consumes
 *   those objects into the VMM sparse tree, PTE/PDE state, and live mapping
 *   tree.
 *
 * Lifetime:
 *   The caller keeps VM_BIND serialization from prepare through commit/fini.
 *   On abort, fini releases every unpublished sparse region and prepared clear
 *   object.  On successful commit, valid bindings removed by the target clear
 *   stay in retired_bindings until the VM_BIND done fence is visible.
 *
 * Threading:
 *   Prepare may allocate sparse regions, split old sparse records, clone/pin
 *   valid tail bindings, and inspect VMM state.  Commit runs under the
 *   VM_BIND/GSP mutation boundary and does not flush; the enclosing VM_BIND
 *   invalidate publishes the dirty PTE/PDE writes.
 */
static void
nvkm_drm_vm_sparse_map_plan_init(struct nvkm_drm_vm_sparse_map_plan *plan)
{
	nvkm_drm_vm_bind_segment_plan_init(&plan->segment_plan);
	nvkm_drm_vm_segment_remove_plan_init(&plan->target_clear_plan);
	plan->sparse_regions = NULL;
	plan->sparse_clear_plan = NULL;
	plan->addr = 0;
	plan->size = 0;
}

static void
nvkm_drm_vm_sparse_map_plan_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_sparse_map_plan *plan)
{
	if (plan->sparse_regions != NULL) {
		for (uint32_t i = 0; i < plan->segment_plan.count; i++) {
			if (plan->sparse_regions[i] != NULL)
				nvkm_gsp_vmm_abort_sparse_region(nfile->vmm,
				    plan->sparse_regions[i]);
		}
		kfree(plan->sparse_regions);
	}
	nvkm_drm_vm_bind_abort_sparse_clear(nfile, &plan->sparse_clear_plan);
	nvkm_drm_vm_segment_remove_plan_fini(nfile, &plan->target_clear_plan);
	nvkm_drm_vm_bind_segment_plan_fini(&plan->segment_plan);
	nvkm_drm_vm_sparse_map_plan_init(plan);
}

static int
nvkm_drm_vm_sparse_map_plan_prepare(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_sparse_map_plan *plan,
    uint64_t addr, uint64_t size)
{
	int err;

	nvkm_drm_vm_sparse_map_plan_init(plan);
	plan->addr = addr;
	plan->size = size;

	err = nvkm_drm_vm_bind_build_sparse_segments(sc, addr, size,
	    &plan->segment_plan);
	if (err != 0)
		goto fail;

	plan->sparse_regions = kmalloc_array(plan->segment_plan.count,
	    sizeof(*plan->sparse_regions), GFP_KERNEL);
	if (plan->sparse_regions == NULL) {
		err = -ENOMEM;
		goto fail;
	}
	for (uint32_t i = 0; i < plan->segment_plan.count; i++)
		plan->sparse_regions[i] = NULL;
	for (uint32_t i = 0; i < plan->segment_plan.count; i++) {
		const struct nvkm_drm_vm_bind_segment *seg =
		    &plan->segment_plan.segments[i];

		err = nvkm_gsp_vmm_prepare_sparse_region_page(nfile->vmm,
		    seg->addr, seg->size, seg->page_shift,
		    &plan->sparse_regions[i]);
		if (err != 0) {
			err = -err;
			goto fail;
		}
	}

		err = nvkm_drm_vm_bind_prepare_sparse_clear(nfile, addr, size,
		    nvkm_drm_vm_bind_segments_min_page_shift(
		    plan->segment_plan.segments, plan->segment_plan.count),
		    NVKM_DRM_VM_SPARSE_CLEAR_TARGET_OVERWRITE,
		    &plan->sparse_clear_plan);
	if (err != 0)
		goto fail;

	err = nvkm_drm_vm_segment_remove_plan_prepare(sc, nfile,
	    &plan->target_clear_plan, plan->segment_plan.segments,
	    plan->segment_plan.count, addr, size, NVKM_DRM_VM_TRACE_MAP_SPARSE);
	if (err != 0)
		goto fail;

	err = nvkm_gsp_vmm_check_sparse_regions_commit(nfile->vmm,
	    plan->sparse_regions, plan->segment_plan.count, addr, size);
	if (err != 0) {
		err = -err;
		goto fail;
	}

	return (0);

fail:
	nvkm_drm_vm_sparse_map_plan_fini(nfile, plan);
	return (err);
}

static int
nvkm_drm_vm_sparse_map_plan_commit(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_sparse_map_plan *plan,
    uint32_t op_flags, uint32_t *punmapped,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	int err;

	err = nvkm_drm_vm_segment_remove_plan_commit(sc, nfile,
	    &plan->target_clear_plan, punmapped, dirty_set, retired_bindings);
	if (err != 0)
		return (err);

	err = nvkm_drm_vm_bind_commit_sparse_clear_noflush(sc, nfile,
	    &plan->sparse_clear_plan, NVKM_DRM_VM_TRACE_MAP_SPARSE, plan->addr,
	    plan->size, dirty_set);
	if (err != 0)
		return (err);

	for (uint32_t i = 0; i < plan->segment_plan.count; i++) {
		err = nvkm_gsp_vmm_commit_sparse_noflush(nfile->vmm,
		    plan->sparse_regions[i]);
		if (err != 0) {
			nvkm_gsp_vmm_abort_sparse_region(nfile->vmm,
			    plan->sparse_regions[i]);
			plan->sparse_regions[i] = NULL;
			return (-err);
		}
		nvkm_drm_vm_bind_note_dirty_range(sc, dirty_set,
		    plan->segment_plan.segments[i].addr,
		    plan->segment_plan.segments[i].size);
		plan->sparse_regions[i] = NULL;
	}

	nvkm_drm_vm_bind_note_map_segments(sc, op_flags,
	    plan->segment_plan.segments, plan->segment_plan.count, NULL);
	return (0);
}

/*
 * nvkm_drm_vm_bindings_remove_range()
 *
 * Ownership:
 *   Compatibility wrapper for current VM_BIND callers.  It owns one temporary
 *   remove plan and releases any uncommitted prepared objects before return.
 *
 * Lifetime:
 *   The caller still owns retired_bindings until the VM_BIND done fence becomes
 *   visible.  Hardware writes are published by the caller's final VMM invalidate.
 *
 * Threading:
 *   Requires nfile->vm_token and the caller's GSP/VMM mutation boundary.
 */
static int
nvkm_drm_vm_bindings_remove_range(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size,
    bool clear_empty_range, bool preserve_target_pts,
    uint8_t preserve_page_shift, bool materialize_full_cover,
    uint32_t clear_action, uint32_t *punmapped,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_remove_plan plan;
	int err;

	err = nvkm_drm_vm_remove_plan_prepare(sc, nfile, &plan, addr, size,
	    clear_empty_range, preserve_target_pts, preserve_page_shift,
	    materialize_full_cover, clear_action);
	if (err != 0)
		return (err);
	err = nvkm_drm_vm_remove_plan_commit(sc, nfile, &plan, punmapped,
	    dirty_set, retired_bindings);
	nvkm_drm_vm_remove_plan_fini(nfile, &plan);
	return (err);
}

static int
nvkm_drm_vm_binding_add(struct nvkm_drm_file *nfile, uint64_t addr,
    uint64_t size, struct drm_gem_object *obj, uint64_t bo_offset,
    bool bo_pinned)
{
	struct nvkm_drm_vm_binding *binding;
	int err;

	binding = nvkm_drm_vm_binding_alloc(nfile, addr, size, obj, bo_offset,
	    0, NVKM_GMMU_SPT_SHIFT);
	if (binding == NULL)
		return (ENOMEM);

	binding->bo_pinned = bo_pinned;
	err = nvkm_drm_vm_binding_pin(binding);
	if (err != 0) {
		kfree(binding);
		return (err);
	}
	nvkm_drm_vm_binding_insert_sorted(nfile, binding);
	return (0);
}


static void
nvkm_drm_vm_bind_record_error(struct nvkm_softc *sc, uint32_t op,
    uint32_t flags, uint32_t handle, uint64_t addr, uint64_t range,
    uint64_t bo_offset, int err)
{
	sc->vm_bind_error_count++;
	sc->vm_bind_last_error = err;
	sc->vm_bind_last_op = op;
	sc->vm_bind_last_flags = flags;
	sc->vm_bind_last_handle = handle;
	sc->vm_bind_last_addr = addr;
	sc->vm_bind_last_range = range;
	sc->vm_bind_last_bo_offset = bo_offset;
}

static void
nvkm_drm_vm_trace_record(struct nvkm_softc *sc, uint32_t action,
    uint32_t flags, uint32_t handle, uint64_t addr, uint64_t range,
    uint64_t bo_offset, struct drm_gem_object *obj, int error)
{
	struct nvkm_drm_vm_trace *trace;

	if (sc->vm_trace_enable == 0 && nvkm_debug == 0)
		return;

	trace = &sc->vm_trace[sc->vm_trace_next % NVKM_DRM_VM_TRACE_COUNT];
	memset(trace, 0, sizeof(*trace));
	trace->seq = ++sc->vm_trace_seq;
	trace->action = action;
	trace->flags = flags;
	trace->handle = handle;
	trace->addr = addr;
	trace->range = range;
	trace->bo_offset = bo_offset;
	trace->error = error;
	trace->pte_kind = flags & 0xff;
	if (obj != NULL) {
		struct nvkm_bo *bo = to_nvkm_bo(obj);

		trace->obj = (uintptr_t)obj;
		trace->bo_size = obj->size;
		trace->bo_paddr = bo->paddr;
		trace->bo_domain = bo->domain;
		trace->bo_tile_mode = bo->tile_mode;
		trace->bo_tile_flags = bo->tile_flags;
		trace->cpu_mapped = nvkm_bo_cpu_mappable(bo);
	}
	sc->vm_trace_next++;
}

static void
nvkm_drm_dump_push_buffer(struct nvkm_softc *sc, struct nvkm_drm_file *nfile,
    uint64_t va, uint32_t va_len)
{
	struct nvkm_drm_vm_binding *binding;
	struct nvkm_bo *bo;
	uint64_t offset;
	uint32_t count;

	binding = nvkm_drm_vm_binding_first_overlap(nfile, va, va_len);
	if (binding == NULL)
		goto no_binding;
	if (va < binding->addr ||
	    va + va_len > binding->addr + binding->size)
		goto no_binding;

	bo = to_nvkm_bo(binding->obj);
	offset = binding->bo_offset + (va - binding->addr);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC dump push va=0x%016jx len=0x%08x binding=0x%016jx+0x%016jx obj=%p domain=0x%x paddr=0x%016jx offset=0x%jx cpu_map=%u\n",
	    (uintmax_t)va, va_len, (uintmax_t)binding->addr,
	    (uintmax_t)binding->size, binding->obj, bo->domain,
	    (uintmax_t)bo->paddr, (uintmax_t)offset,
	    nvkm_bo_has_sysmem(bo));
	if (!nvkm_bo_has_sysmem(bo))
		return;
	if (offset > bo->base.size ||
	    va_len > bo->base.size - offset)
		return;

	count = va_len / sizeof(uint32_t);
	if (count > 48)
		count = 48;
	for (uint32_t i = 0; i < count; i += 4) {
		uint32_t word[4] = {};

		for (uint32_t j = 0; j < 4 && i + j < count; j++)
			(void)nvkm_bo_read32(bo, offset +
			    (uint64_t)(i + j) * sizeof(uint32_t),
			    &word[j]);
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC push[%02u]=%08x %08x %08x %08x\n",
		    i, word[0], word[1], word[2], word[3]);
	}
	return;

no_binding:
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC dump push va=0x%016jx len=0x%08x no binding\n",
	    (uintmax_t)va, va_len);
}

static void
nvkm_drm_dump_large_push(struct nvkm_softc *sc, struct nvkm_drm_file *nfile,
    uint64_t va, uint32_t va_len)
{
	if (nvkm_debug != 0 && va_len >= 0xa0)
		nvkm_drm_dump_push_buffer(sc, nfile, va, va_len);
}

static void
nvkm_drm_exec_trace_record(struct nvkm_softc *sc, uint64_t seq,
    uint32_t channel, uint32_t chid, uint32_t post_slot,
    uint32_t gpf_index, uint32_t push_index, uint32_t push_count,
    const struct drm_nouveau_exec_push *push)
{
	struct nvkm_drm_exec_trace *trace;

	if (nvkm_debug == 0)
		return;

	trace = &sc->exec_trace[sc->exec_trace_next %
	    NVKM_DRM_EXEC_TRACE_COUNT];
	memset(trace, 0, sizeof(*trace));
	trace->seq = seq;
	trace->channel = channel;
	trace->chid = chid;
	trace->post_slot = post_slot;
	trace->gpf_index = gpf_index;
	trace->push_index = push_index;
	trace->push_count = push_count;
	trace->flags = push->flags;
	trace->va = push->va;
	trace->va_len = push->va_len;
	sc->exec_trace_next++;
}

static void
nvkm_drm_exec_trace_complete(struct nvkm_softc *sc,
    const struct nvkm_drm_exec_pending *pending, int error)
{
	for (uint32_t i = 0; i < pending->trace_count; i++) {
		struct nvkm_drm_exec_trace *trace;
		uint32_t idx = (pending->trace_first + i) %
		    NVKM_DRM_EXEC_TRACE_COUNT;

		trace = &sc->exec_trace[idx];
		if (trace->seq != pending->trace_seq ||
		    trace->post_slot != pending->post_slot)
			continue;
		trace->completed = 1;
		trace->error = error;
	}
}

int
nvkm_drm_register(struct nvkm_softc *sc)
{
	struct pci_dev *pdev = NULL;
	struct drm_device *ddev;
	int err;

	/* Wrap the BSD device_t into a Linux pci_dev shim that dfly drm
	 * core understands. drm_init_pdev mallocs *pdev for us. */
	drm_init_pdev(sc->dev, &pdev);
	if (pdev == NULL) {
		nvkm_debugf(sc->dev,
		    "drm: drm_init_pdev failed\n");
		return (ENOMEM);
	}
	sc->drm_pdev = pdev;

	ddev = drm_dev_alloc(&nvkm_drm_driver, &pdev->dev);
	if (IS_ERR(ddev)) {
		nvkm_debugf(sc->dev,
		    "drm: drm_dev_alloc failed (%ld)\n", PTR_ERR(ddev));
		return (ENOMEM);
	}

	ddev->dev_private = sc;
	ddev->pdev        = pdev;
	sc->drm_dev       = ddev;

	err = nvkm_ttm_init(sc, ddev);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "drm: nvkm_ttm_init failed (%d)\n", err);
		drm_dev_put(ddev);
		sc->drm_dev = NULL;
		return (err);
	}

	/* Prepare KMS mode_config/connectors before drm_dev_register(), which
	 * registers the objects created here.  The imported display engine owns
	 * GSP display discovery; this layer owns DragonFly DRM object setup. */
	(void)nvkm_drm_kms_init(ddev, sc);

	err = drm_dev_register(ddev, 0);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "drm: drm_dev_register failed (%d)\n", err);
		nvkm_drm_kms_fini(sc);
		nvkm_ttm_fini(sc);
		drm_dev_put(ddev);
		sc->drm_dev = NULL;
		return (err);
	}

	nvkm_debugf(sc->dev,
	    "drm: registered as %s; check /dev/dri/renderD* and card*\n",
	    NVKM_DRM_NAME);
	return (0);
}

void
nvkm_drm_unregister(struct nvkm_softc *sc)
{
	if (sc->drm_dev != NULL) {
		drm_dev_unregister(sc->drm_dev);
		nvkm_drm_kms_fini(sc);
		nvkm_dispnv50_fini(sc);
		nvkm_ttm_fini(sc);
		drm_dev_put(sc->drm_dev);
		sc->drm_dev = NULL;
	}
}

/* ============================================================
 * Nouveau-uAPI ioctl handlers (Phase 3 Step 2).
 * Mirrors Linux include/uapi/drm/nouveau_drm.h subset that Mesa
 * NVK uses during VkPhysicalDevice probe.
 * ============================================================ */

/* ---- nouveau_drm.h subset (from Linux uapi) ---- */
#define DRM_NOUVEAU_GETPARAM		0x00
#define DRM_NOUVEAU_CHANNEL_ALLOC	0x02
#define DRM_NOUVEAU_CHANNEL_FREE	0x03
#define DRM_NOUVEAU_NVIF		0x07
#define DRM_NOUVEAU_VM_INIT		0x10
#define DRM_NOUVEAU_VM_BIND		0x11
#define DRM_NOUVEAU_EXEC		0x12
#define DRM_NOUVEAU_GEM_NEW		0x40
#define DRM_NOUVEAU_GEM_PUSHBUF		0x41
#define DRM_NOUVEAU_GEM_CPU_PREP	0x42
#define DRM_NOUVEAU_GEM_CPU_FINI	0x43
#define DRM_NOUVEAU_GEM_INFO		0x44

#define NOUVEAU_GETPARAM_PCI_VENDOR	3
#define NOUVEAU_GETPARAM_PCI_DEVICE	4
#define NOUVEAU_GETPARAM_BUS_TYPE	5
#define NOUVEAU_GETPARAM_FB_SIZE	8
#define NOUVEAU_GETPARAM_AGP_SIZE	9
#define NOUVEAU_GETPARAM_CHIPSET_ID	11
#define NOUVEAU_GETPARAM_VM_VRAM_BASE	12
#define NOUVEAU_GETPARAM_GRAPH_UNITS	13
#define NOUVEAU_GETPARAM_PTIMER_TIME	14
#define NOUVEAU_GETPARAM_HAS_BO_USAGE	15
#define NOUVEAU_GETPARAM_HAS_PAGEFLIP	16
#define NOUVEAU_GETPARAM_EXEC_PUSH_MAX	17
#define NOUVEAU_GETPARAM_VRAM_BAR_SIZE	18
#define NOUVEAU_GETPARAM_VRAM_USED	19
#define NOUVEAU_GETPARAM_HAS_VMA_TILEMODE 20

struct drm_nouveau_getparam {
	uint64_t param;
	uint64_t value;
};

struct drm_nouveau_vm_init {
	uint64_t kernel_managed_addr;
	uint64_t kernel_managed_size;
};

/* ---- NVIF wire format ---- */
struct nvif_ioctl_v0 {
	uint8_t  version;
	uint8_t  type;
#define NVIF_IOCTL_V0_NEW	0x02
#define NVIF_IOCTL_V0_DEL	0x03
#define NVIF_IOCTL_V0_MTHD	0x04
	uint8_t  path_nr;
	uint8_t  pad03[3];
	uint8_t  owner;
	uint8_t  route;
	uint64_t token;
	uint64_t object;
	uint8_t  data[];
} __packed;

struct nvif_ioctl_new_v0 {
	uint8_t  version;
	uint8_t  pad01[2];
	uint8_t  route;
	uint32_t pad04;        /* explicit pad: linux uses NV_DECLARE_ALIGNED u64 */
	uint64_t token;
	uint64_t object;
	uint32_t handle;
#define NV_DEVICE	0x0080
	uint32_t oclass;
	uint8_t  data[];
} __packed;

struct nv_device_v0 {
	uint8_t  version;
	uint8_t  pad01[7];
	uint64_t device;
	uint32_t priv;
	uint32_t pad14;
} __packed;

struct nvif_ioctl_mthd_v0 {
	uint8_t  version;
	uint8_t  method;
#define NV_DEVICE_V0_INFO	0x00
	uint8_t  pad02[6];
	uint8_t  data[];
} __packed;

struct nv_device_info_v0 {
	uint8_t  version;
	uint8_t  platform;
#define NV_DEVICE_INFO_V0_PCIE	0x03
	uint16_t chipset;
	uint8_t  revision;
	uint8_t  family;
	uint8_t  pad06[2];
	uint64_t ram_size;
	uint64_t ram_user;
	char     chip[16];
	char     name[64];
} __packed;

struct drm_nouveau_channel_alloc {
	uint32_t fb_ctxdma_handle;
	uint32_t tt_ctxdma_handle;
	int32_t  channel;
	uint32_t pushbuf_domains;
	uint32_t notifier_handle;
	struct {
		uint32_t handle;
		uint32_t grclass;
	} subchan[8];
	uint32_t nr_subchan;
};

struct nvif_ioctl_sclass_oclass_v0 {
	int32_t  oclass;
	int16_t  minver;
	int16_t  maxver;
};
struct nvif_ioctl_sclass_v0 {
	uint8_t  version;
	uint8_t  count;
	uint8_t  pad02[6];
	struct nvif_ioctl_sclass_oclass_v0 oclass[];
};
#define NVIF_IOCTL_V0_SCLASS	0x01

static uint32_t
nvkm_drm_chip_classes(struct nvkm_softc *sc,
    uint32_t classes[NVKM_CHIP_CLASS_COUNT])
{
	const struct nvkm_chip_config *chip = sc->chip;
	uint32_t count = 0;

	if (chip == NULL)
		return (0);
	if (chip->class_3d != 0)
		classes[count++] = chip->class_3d;
	if (chip->class_compute != 0)
		classes[count++] = chip->class_compute;
	if (chip->class_copy != 0)
		classes[count++] = chip->class_copy;
	if (chip->class_twod != 0)
		classes[count++] = chip->class_twod;
	if (chip->class_m2mf != 0)
		classes[count++] = chip->class_m2mf;
	return (count);
}

static bool
nvkm_drm_chip_class_supported(struct nvkm_softc *sc, uint32_t oclass)
{
	uint32_t classes[NVKM_CHIP_CLASS_COUNT];
	uint32_t count;

	count = nvkm_drm_chip_classes(sc, classes);
	for (uint32_t i = 0; i < count; i++) {
		if (classes[i] == oclass)
			return (true);
	}
	return (false);
}

static bool
nvkm_drm_chip_class_needs_gr_ctx(struct nvkm_softc *sc, uint32_t oclass)
{
	const struct nvkm_chip_config *chip = sc->chip;

	if (chip == NULL)
		return (false);
	return (oclass == chip->class_3d ||
	    oclass == chip->class_compute ||
	    oclass == chip->class_twod ||
	    oclass == chip->class_m2mf);
}

#define NOUVEAU_FIFO_ENGINE_GR	0x01
#define NOUVEAU_FIFO_ENGINE_CE	0x30
static struct nvkm_drm_chan *
nvkm_drm_channel_find(struct nvkm_drm_file *nfile, uint32_t id)
{
	struct nvkm_drm_chan *dchan;

	LIST_FOREACH(dchan, &nfile->channels, link) {
		if (dchan->id == id)
			return (dchan);
	}
	return (NULL);
}

static struct nvkm_drm_chan_obj *
nvkm_drm_channel_obj_slot(struct nvkm_drm_chan *dchan)
{
	for (uint32_t i = 0; i < NVKM_DRM_MAX_CHAN_OBJS; i++) {
		if (dchan->obj[i].oclass == 0)
			return (&dchan->obj[i]);
	}
	return (NULL);
}

static void
nvkm_drm_channel_clear(struct nvkm_softc *sc, struct nvkm_drm_chan *dchan)
{
	if (dchan->chan != NULL) {
		lwkt_gettoken(&sc->gsp_tok);
		nvkm_drm_exec_pending_cancel_channel(sc, dchan->chan, -ENODEV);
		lwkt_reltoken(&sc->gsp_tok);
		for (uint32_t i = 0; i < NVKM_DRM_MAX_CHAN_OBJS; i++) {
			if (dchan->obj[i].oclass != 0 &&
			    dchan->obj[i].object.handle != 0)
				(void)nvkm_gsp_rm_free(&dchan->obj[i].object);
		}
		(void)nvkm_gsp_chan_dtor(dchan->chan);
		kfree(dchan->chan);
	}
	kfree(dchan);
}

static void
nvkm_drm_file_release(struct drm_device *ddev, struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct nvkm_drm_vm_binding *binding, *binding_next;
	struct nvkm_drm_chan *dchan, *dchan_next;
	struct nvkm_drm_vm_dirty_set dirty_set;
	struct nvkm_drm_vm_binding_list release_bindings;
	uint32_t binding_count = 0, channel_count = 0;

	if (nfile == NULL)
		return;

	sc->vm_bind_release_count++;
	nvkm_drm_jobs_close(nfile);

	LIST_FOREACH_MUTABLE(dchan, &nfile->channels, link, dchan_next) {
		LIST_REMOVE(dchan, link);
		nvkm_drm_channel_clear(sc, dchan);
		channel_count++;
	}
	sc->vm_bind_release_channel_count += channel_count;

	nvkm_drm_vm_dirty_set_init(&dirty_set);
	LIST_INIT(&release_bindings);
	lwkt_gettoken(&nfile->vm_token);
	lwkt_gettoken(&sc->gsp_tok);
	LIST_FOREACH_MUTABLE(binding, &nfile->vm_bindings, link,
	    binding_next) {
		int reclaim_err;

		reclaim_err = nvkm_drm_vm_binding_reclaim_noflush(sc,
		    binding, &dirty_set, &release_bindings);
		if (reclaim_err != 0)
			sc->vm_bind_release_reclaim_error_count++;
		binding_count++;
	}
	sc->vm_bind_release_binding_count += binding_count;
	nvkm_drm_vm_dirty_set_publish(sc, &dirty_set);
	if (dirty_set.dirty && nfile->vmm != NULL)
		nvkm_drm_vm_dirty_set_flush(nfile->vmm, &dirty_set);
	lwkt_reltoken(&sc->gsp_tok);
	lwkt_reltoken(&nfile->vm_token);

	(void)nvkm_drm_vm_bindings_release(&release_bindings);

	nvkm_debugf(sc->dev,
	    "nvkm_drm: postclose released bindings=%u channels=%u\n",
	    binding_count, channel_count);
	file_priv->driver_priv = NULL;
	nvkm_drm_file_put(nfile);
}

/* Lazily build this file's per-file VMM.  Device open no longer creates
 * it: libdrm probes the node (drmGetDevices2) with throwaway open/close
 * cycles that never touch the GPU, and paying the multi-RPC VMM ctor
 * (~0.4s) on each made enumeration cost tens of seconds.  The first
 * VM_BIND or channel alloc that actually uses the GPU builds it here. */
static int
nvkm_drm_file_ensure_vmm(struct nvkm_softc *sc, struct nvkm_drm_file *nfile)
{
	struct nvkm_gsp_vmm *vmm;
	uint32_t client_handle;
	int err;

	if (nfile->vmm != NULL)
		return (0);

	lwkt_gettoken(&nfile->vm_token);
	if (nfile->vmm != NULL) {
		lwkt_reltoken(&nfile->vm_token);
		return (0);
	}

	vmm = kzalloc(sizeof(*vmm), GFP_KERNEL);
	if (vmm == NULL) {
		lwkt_reltoken(&nfile->vm_token);
		return (-ENOMEM);
	}
	client_handle = atomic_fetchadd_int(&nvkm_drm_next_client_handle, 1);
	/* Hold gsp_tok across the whole multi-RPC ctor so the client/device/
	 * vaspace/PDE-copy sequence is atomic against other GSP users; the
	 * token auto-releases during each RPC reply wait. */
	lwkt_gettoken(&sc->gsp_tok);
	err = nvkm_gsp_vmm_ctor(sc, client_handle, vmm);
	lwkt_reltoken(&sc->gsp_tok);
	if (err != 0) {
		nvkm_infof(sc->dev,
		    "nvkm_drm: per-file VMM ctor failed handle=0x%x err=%d\n",
		    client_handle, err);
		kfree(vmm);
		lwkt_reltoken(&nfile->vm_token);
		return (-err);
	}
	/* Publish only after the VMM is fully built so the lock-free fast
	 * path above never observes a half-constructed vmm. */
	nfile->vmm = vmm;
	lwkt_reltoken(&nfile->vm_token);
	return (0);
}

static int
nvkm_drm_open(struct drm_device *ddev, struct drm_file *file_priv)
{
	struct nvkm_drm_file *nfile;

	nfile = kzalloc(sizeof(*nfile), GFP_KERNEL);
	if (nfile == NULL)
		return (-ENOMEM);
	kref_init(&nfile->refcount);
	nfile->sc = nvkm_drm_sc(ddev);
	LIST_INIT(&nfile->vm_bindings);
	LIST_INIT(&nfile->vm_validate_bindings);
	RB_INIT(&nfile->vm_binding_tree);
	LIST_INIT(&nfile->channels);
	TAILQ_INIT(&nfile->job_queue);
	lwkt_token_init(&nfile->vm_token, "nvkm-vm");
	lwkt_token_init(&nfile->job_token, "nvkm-job");
		lockinit(&nfile->job_submit_lock, "nvkjsb", 0, 0);
		reservation_object_init(&nfile->vm_resv);
		reservation_object_init(&nfile->vm_exec_resv);
		INIT_WORK(&nfile->job_work, nvkm_drm_job_work);

	/* nfile->vmm stays NULL; nvkm_drm_file_ensure_vmm builds it on the
	 * first GPU use so probe-only opens stay cheap (see that helper). */
	file_priv->driver_priv = nfile;
	return (0);
}

static void
nvkm_drm_postclose(struct drm_device *ddev, struct drm_file *file_priv)
{
	nvkm_drm_file_release(ddev, file_priv);
}

static void
nvkm_drm_restore_console(struct drm_device *ddev, const char *reason)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	unsigned int primary_clients;
	int err;

	if (sc == NULL)
		return;

	primary_clients = nvkm_drm_primary_client_count(ddev);
	if (primary_clients != 0) {
		sc->kms_restore_skip_primary_count++;
		sc->kms_restore_last_primary_count = primary_clients;
		sc->kms_restore_last_open_count = ddev->open_count;
		nvkm_debugf(sc->dev,
		    "drm: skip %s console restore: primary_clients=%u "
		    "open_count=%d\n",
		    reason, primary_clients, ddev->open_count);
		return;
	}

	err = nvkm_drm_kms_schedule(sc, reason);
	if (err != 0)
		nvkm_infof(sc->dev,
		    "drm: %s console restore schedule failed err=%d\n",
		    reason, err);
}

static void
nvkm_drm_lastclose(struct drm_device *ddev)
{
	nvkm_drm_restore_console(ddev, "lastclose");
}

static unsigned int
nvkm_drm_primary_client_count(struct drm_device *ddev)
{
	struct drm_file *file_priv;
	unsigned int count = 0;

	if (ddev == NULL)
		return (0);

	mutex_lock(&ddev->filelist_mutex);
	list_for_each_entry(file_priv, &ddev->filelist, lhead) {
		if (file_priv->minor != NULL &&
		    file_priv->minor->type == DRM_MINOR_PRIMARY)
			count++;
	}
	mutex_unlock(&ddev->filelist_mutex);

	return (count);
}

/* ---- Helper: locate nvkm_softc from drm_file ---- */
static struct nvkm_softc *
nvkm_drm_sc(struct drm_device *ddev)
{
	return (struct nvkm_softc *)ddev->dev_private;
}

/* ---- DRM_NOUVEAU_GETPARAM ---- */
static int
nvkm_drm_ioctl_getparam(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct drm_nouveau_getparam *gp = data;

	switch (gp->param) {
	case NOUVEAU_GETPARAM_PCI_VENDOR:
		gp->value = pci_get_vendor(sc->dev);
		break;
	case NOUVEAU_GETPARAM_PCI_DEVICE:
		gp->value = pci_get_device(sc->dev);
		break;
	case NOUVEAU_GETPARAM_CHIPSET_ID:
		gp->value = sc->chip->chipset;
		break;
	case NOUVEAU_GETPARAM_BUS_TYPE:
		/* Legacy GETPARAM bus enum (Linux nouveau_abi16): AGP=0, PCI=1,
		 * PCIE=2, SOC=3 — distinct from the NVIF NV_DEVICE_INFO_V0_PCIE
		 * (=3) enum. We are a discrete PCIe card. */
		gp->value = 2;
		break;
	case NOUVEAU_GETPARAM_FB_SIZE:
		gp->value = sc->fb_usable_size;
		break;
	case NOUVEAU_GETPARAM_VRAM_BAR_SIZE:
		gp->value = sc->bar_res[1] != NULL ?
		    rman_get_size(sc->bar_res[1]) : 0;
		break;
	case NOUVEAU_GETPARAM_EXEC_PUSH_MAX:
		/*
		 * Max pushes NVK may pack into one EXEC ioctl. Match nouveau's
		 * nouveau_exec_push_max_from_ib_max(): half the GPFIFO ring minus
		 * one, so a single submit cannot outrun the ring once the
		 * completion trailer, fetch-window NOPs, and the get != put
		 * headroom are accounted for, and two max submits still fit.
		 */
		gp->value = NVKM_DRM_GPFIFO_ENTRIES / 2 - 1;
		break;
	case NOUVEAU_GETPARAM_GRAPH_UNITS:
		/* NVK interprets low 8 bits as GPC count and bits 8..23
		 * as TPC count. Keep the per-chip value in the chip table
		 * instead of encoding a TU102-only topology here. */
		gp->value = sc->chip->graph_units;
		break;
	case NOUVEAU_GETPARAM_VRAM_USED:
		/* NVK asserts >0 on success; force fail so NVK uses 0 fallback. */
		return (-EINVAL);
	case NOUVEAU_GETPARAM_PTIMER_TIME: {
		/* 64-bit nanosecond counter at NV_PTIMER_TIME_0/_1 (BAR0 MMIO).
		 * Re-read the high word to guard against low-word rollover
		 * between the two reads. Mesa treats 0 as a valid timestamp, so
		 * a real value is required. */
		uint32_t hi, lo, hi2;

		do {
			hi = nvkm_rd32(sc, 0x009410);
			lo = nvkm_rd32(sc, 0x009400);
			hi2 = nvkm_rd32(sc, 0x009410);
		} while (hi != hi2);
		gp->value = ((uint64_t)hi << 32) | lo;
		break;
	}
	case NOUVEAU_GETPARAM_HAS_VMA_TILEMODE:
		/*
		 * VM_BIND carries the PTE kind in op->flags bits 7:0 and the
		 * map paths write it into GMMU PTE bits 63:56.  This is what
		 * NVK gates VK_EXT_image_drm_format_modifier on.  The sysctl
		 * is a kill switch while tiled-rendering wedges are debugged.
		 */
		gp->value = sc->vma_tilemode != 0 ? 1 : 0;
		break;
	default:
		nvkm_debugf(sc->dev,
		    "nvkm_drm: GETPARAM 0x%llx unhandled\n",
		    (unsigned long long)gp->param);
		return (-EINVAL);
	}
	return (0);
}

/* ---- DRM_NOUVEAU_VM_INIT ---- */
static int
nvkm_drm_ioctl_vm_init(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct drm_nouveau_vm_init *vminit = data;

	sc->vm_init_kernel_addr = vminit->kernel_managed_addr;
	sc->vm_init_kernel_size = vminit->kernel_managed_size;
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_INIT addr=0x%llx size=0x%llx (stub success)\n",
	    (unsigned long long)vminit->kernel_managed_addr,
	    (unsigned long long)vminit->kernel_managed_size);
	return (0);
}

/* ---- DRM_NOUVEAU_NVIF ---- */
static int
nvkm_drm_ioctl_nvif(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvif_ioctl_v0 *hdr = data;

	if (hdr->version != 0) {
		nvkm_debugf(sc->dev, "nvkm_drm: NVIF bad version %u\n",
		    hdr->version);
		return (-ENOSYS);
	}

	switch (hdr->type) {
	case NVIF_IOCTL_V0_NEW: {
		struct nvif_ioctl_new_v0 *new_ = (void *)hdr->data;
		struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
		struct nvkm_drm_chan *dchan;
		struct nvkm_drm_chan_obj *obj;
		int err;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: NVIF NEW oclass=0x%x handle=0x%x token=0x%llx\n",
		    new_->oclass, new_->handle,
		    (unsigned long long)new_->token);
		if (new_->oclass == NV_DEVICE) {
			/* stub: accept the NV_DEVICE allocation. */
			return (0);
		}
		if (!nvkm_drm_chip_class_supported(sc, new_->oclass))
			return (-EINVAL);
		if (nfile == NULL)
			return (-ENXIO);
		dchan = nvkm_drm_channel_find(nfile, (uint32_t)hdr->token);
		if (dchan == NULL || dchan->chan == NULL)
			return (-ENOENT);
		if (nvkm_drm_chip_class_needs_gr_ctx(sc, new_->oclass)) {
			err = nvkm_gsp_chan_promote_gr_ctx(nfile->vmm,
			    dchan->chan, 0);
			if (err != 0)
				return (-err);
		}
		obj = nvkm_drm_channel_obj_slot(dchan);
		if (obj == NULL)
			return (-ENOMEM);
		err = nvkm_gsp_chan_alloc_obj(dchan->chan, new_->handle,
		    new_->oclass, &obj->object);
		if (err != 0)
			return (-err);
		obj->handle = new_->handle;
		obj->oclass = new_->oclass;
		obj->nvif_object = new_->object;
		return (0);
	}
	case NVIF_IOCTL_V0_MTHD: {
		struct nvif_ioctl_mthd_v0 *mthd = (void *)hdr->data;
		if (mthd->method == NV_DEVICE_V0_INFO) {
			struct nv_device_info_v0 *info = (void *)mthd->data;
			memset(info, 0, sizeof(*info));
			info->version  = 0;
			info->platform = NV_DEVICE_INFO_V0_PCIE;
			info->chipset  = sc->chip->chipset;
			info->revision = pci_get_revid(sc->dev);
			info->family   = 0x0a;	/* TURING per nv_device.h family enum */
			info->ram_size = sc->fb_usable_size;
			info->ram_user = sc->fb_usable_size;
			strncpy(info->chip, sc->chip->chip, sizeof(info->chip));
			strncpy(info->name, sc->pci_device != NULL ?
			    sc->pci_device->name : sc->chip->fallback_name,
			    sizeof(info->name));
			nvkm_debugf(sc->dev,
			    "nvkm_drm: NVIF MTHD DEVICE_INFO -> %s ram=0x%llx user=0x%llx bar1=0x%llx\n",
			    info->chip,
			    (unsigned long long)info->ram_size,
			    (unsigned long long)info->ram_user,
			    (unsigned long long)(sc->bar_res[1] != NULL ?
			    rman_get_size(sc->bar_res[1]) : 0));
			return (0);
		}
		return (-EINVAL);
	}
	case NVIF_IOCTL_V0_SCLASS: {
		struct nvif_ioctl_sclass_v0 *sc_ = (void *)hdr->data;
		uint32_t classes[NVKM_CHIP_CLASS_COUNT];
		uint32_t count = nvkm_drm_chip_classes(sc, classes);
		uint32_t want = sc_->count;
		uint32_t fill = want < count ? want : count;

		nvkm_debugf(sc->dev,
		    "nvkm_drm: NVIF SCLASS want=%u fill=%u\n", want, fill);
		for (uint32_t i = 0; i < fill; i++) {
			sc_->oclass[i].oclass = classes[i];
			sc_->oclass[i].minver = 0;
			sc_->oclass[i].maxver = 0;
		}
		sc_->count = fill;
		return (0);
	}
	case NVIF_IOCTL_V0_DEL: {
		struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
		struct nvkm_drm_chan *dchan;

		/* Free the engine object created by a matching NVIF NEW and
		 * clear its slot (channel teardown then skips it). Untracked
		 * handles — e.g. the NV_DEVICE stub, which allocates nothing —
		 * succeed without action, as on Linux. */
		if (nfile != NULL) {
			LIST_FOREACH(dchan, &nfile->channels, link) {
				for (uint32_t i = 0; i < NVKM_DRM_MAX_CHAN_OBJS;
				    i++) {
					struct nvkm_drm_chan_obj *o =
					    &dchan->obj[i];

					if (o->oclass == 0 ||
					    o->nvif_object != hdr->object)
						continue;
					if (o->object.handle != 0)
						(void)nvkm_gsp_rm_free(
						    &o->object);
					o->oclass = 0;
					o->handle = 0;
					o->nvif_object = 0;
					return (0);
				}
			}
		}
		return (0);
	}
	default:
		nvkm_debugf(sc->dev,
		    "nvkm_drm: NVIF type %u unhandled\n", hdr->type);
		return (-EINVAL);
	}
}

/* ---- DRM_NOUVEAU_CHANNEL_ALLOC ---- */
static int
nvkm_drm_ioctl_channel_alloc(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_channel_alloc *req = data;
	struct nvkm_drm_chan *dchan = NULL;
	uint32_t engine_type;
	uint32_t channel_count = 0;
	int err;

	if (nfile == NULL)
		return (-ENXIO);
	err = nvkm_drm_file_ensure_vmm(sc, nfile);
	if (err != 0)
		return (err);
	LIST_FOREACH(dchan, &nfile->channels, link)
		channel_count++;
	if (channel_count >= NVKM_DRM_MAX_CHANNELS)
		return (-ENOMEM);

	/* Engine selector matches nouveau (Kepler+): tt_ctxdma_handle is an
	 * engine id only when fb_ctxdma_handle == ~0, and an unrecognised
	 * engine is rejected rather than silently mapped to graphics. We
	 * implement GR and CE; the video engines are absent. Determined before
	 * any allocation so the reject path leaks nothing. */
	engine_type = NV2080_ENGINE_TYPE_GRAPHICS;
	if (req->fb_ctxdma_handle == ~0u) {
		switch (req->tt_ctxdma_handle) {
		case NOUVEAU_FIFO_ENGINE_GR:
			engine_type = NV2080_ENGINE_TYPE_GRAPHICS;
			break;
		case NOUVEAU_FIFO_ENGINE_CE:
			engine_type = NV2080_ENGINE_TYPE_COPY0;
			break;
		default:
			nvkm_debugf(sc->dev,
			    "nvkm_drm: CHANNEL_ALLOC unsupported engine tt=0x%x\n",
			    req->tt_ctxdma_handle);
			return (-ENOSYS);
		}
	}

	dchan = kzalloc(sizeof(*dchan), GFP_KERNEL);
	if (dchan == NULL)
		return (-ENOMEM);
	dchan->chan = kzalloc(sizeof(*dchan->chan), GFP_KERNEL);
	if (dchan->chan == NULL) {
		kfree(dchan);
		return (-ENOMEM);
	}

	lwkt_gettoken(&sc->gsp_tok);
	if (engine_type == NV2080_ENGINE_TYPE_GRAPHICS) {
		err = nvkm_gsp_gr_oneinit(sc->gsp_vmm);
		if (err != 0) {
			lwkt_reltoken(&sc->gsp_tok);
			kfree(dchan->chan);
			kfree(dchan);
			return (-err);
		}
	}
	err = nvkm_gsp_chan_ctor(nfile->vmm, engine_type, dchan->chan);
	lwkt_reltoken(&sc->gsp_tok);
	if (err != 0) {
		kfree(dchan->chan);
		kfree(dchan);
		return (-err);
	}

	/* Atomic: concurrent drm_file opens must not collide on a channel id. */
	dchan->id = atomic_fetchadd_int(&nvkm_drm_next_channel, 1);
	dchan->engine_type = engine_type;
	LIST_INSERT_HEAD(&nfile->channels, dchan, link);
	req->channel = dchan->id;
	req->pushbuf_domains = 2;
	req->notifier_handle = 0;
	req->nr_subchan = 0;
	nvkm_debugf(sc->dev,
	    "nvkm_drm: CHANNEL_ALLOC -> channel=%d chid=%d engine=0x%x req_tt=0x%x\n",
	    req->channel, dchan->chan->chid, engine_type, req->tt_ctxdma_handle);
	return (0);
}

/* ---- DRM_NOUVEAU_CHANNEL_FREE ---- */
struct drm_nouveau_channel_free { int32_t channel; };
static int
nvkm_drm_ioctl_channel_free(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_channel_free *req = data;
	struct nvkm_drm_chan *dchan;

	if (nfile == NULL)
		return (-ENXIO);
	nvkm_drm_jobs_flush(nfile);
	dchan = nvkm_drm_channel_find(nfile, (uint32_t)req->channel);
	if (dchan == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: CHANNEL_FREE unknown channel=%d\n", req->channel);
		return (-ENOENT);
	}
	LIST_REMOVE(dchan, link);
	nvkm_drm_channel_clear(sc, dchan);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: CHANNEL_FREE channel=%d\n", req->channel);
	return (0);
}

#define DRM_NOUVEAU_SYNC_SYNCOBJ	0x0
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ	0x1
#define DRM_NOUVEAU_SYNC_TYPE_MASK	0xf

struct drm_nouveau_sync {
	uint32_t flags;
	uint32_t handle;
	uint64_t timeline_value;
};

#define DRM_NOUVEAU_VM_BIND_OP_MAP	0x0
#define DRM_NOUVEAU_VM_BIND_OP_UNMAP	0x1
#define DRM_NOUVEAU_VM_BIND_SPARSE	(1 << 8)
#define DRM_NOUVEAU_VM_BIND_RUN_ASYNC	0x1

struct drm_nouveau_vm_bind_op {
	uint32_t op;
	uint32_t flags;
	uint32_t handle;
	uint32_t pad;
	uint64_t addr;
	uint64_t bo_offset;
	uint64_t range;
};

struct drm_nouveau_vm_bind {
	uint32_t op_count;
	uint32_t flags;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t op_ptr;
};

enum nvkm_drm_vm_op_plan_kind {
	NVKM_DRM_VM_OP_PLAN_NONE,
	NVKM_DRM_VM_OP_PLAN_CLEAR,
	NVKM_DRM_VM_OP_PLAN_MAP_NULL,
	NVKM_DRM_VM_OP_PLAN_MAP_SPARSE,
	NVKM_DRM_VM_OP_PLAN_MAP_VALID,
	NVKM_DRM_VM_OP_PLAN_MAP_NOOP,
};

struct nvkm_drm_vm_valid_op_plan {
	struct nvkm_drm_vm_bind_segment_plan segment_plan;
	struct nvkm_drm_vm_valid_map_plan valid_map_plan;
	struct nvkm_gsp_vmm_sparse_unmap_plan *sparse_clear_plan;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;
	bool job_object;
	bool bo_temp_pinned;
	bool bo_temp_no_evict_pinned;
	bool had_sparse_clear;
	bool fast_noop;
	uint8_t pte_kind;
};

struct nvkm_drm_vm_op_plan {
	enum nvkm_drm_vm_op_plan_kind kind;
	uint32_t action;
	uint32_t op_flags;
	uint32_t handle;
	uint64_t addr;
	uint64_t size;
	uint64_t bo_offset;
	union {
		struct nvkm_drm_vm_clear_plan clear_plan;
		struct nvkm_drm_vm_sparse_map_plan sparse_map_plan;
		struct nvkm_drm_vm_valid_op_plan valid_map_plan;
	} u;
};

struct nvkm_drm_vm_batch_plan {
	struct drm_nouveau_vm_bind_op *ops;
	struct drm_gem_object **objects;
	struct drm_nouveau_vm_bind_op *failed_op;
	struct nvkm_drm_vm_op_plan current_op;
	struct nvkm_drm_vm_dirty_set dirty_set;
	uint32_t op_count;
	bool current_op_active;
};

/*
 * nvkm_drm_vm_op_plan_*()
 *
 * Ownership:
 *   The top-level op plan owns exactly one prepared VM_BIND operation.  It
 *   delegates storage ownership to the per-op clear, sparse-map, or valid-map
 *   plan and additionally owns transient GEM/BO/segment state for valid MAP.
 *
 * Lifetime:
 *   The caller keeps VM_BIND serialization from prepare through commit/fini.
 *   Fini is required on every path after prepare is attempted.  Published live
 *   bindings still move to retired_bindings and outlive the done fence, matching
 *   the existing nouveau-visible completion ordering.
 *
 * Threading:
 *   Prepare is the only phase that may allocate, lookup GEM handles, pin BOs, or
 *   populate TT pages.  Commit runs inside the VM_BIND/GSP mutation boundary and
 *   only consumes prepared state, writes PTE/PDEs, updates software mappings, and
 *   records the enclosing dirty set.
 */
static void
nvkm_drm_vm_op_plan_init(struct nvkm_drm_vm_op_plan *plan)
{
	memset(plan, 0, sizeof(*plan));
	plan->kind = NVKM_DRM_VM_OP_PLAN_NONE;
}

static void
nvkm_drm_vm_valid_op_plan_init(struct nvkm_drm_vm_valid_op_plan *plan)
{
	nvkm_drm_vm_bind_segment_plan_init(&plan->segment_plan);
	nvkm_drm_vm_valid_map_plan_init(&plan->valid_map_plan);
	plan->sparse_clear_plan = NULL;
	plan->obj = NULL;
	plan->bo = NULL;
		plan->job_object = false;
		plan->bo_temp_pinned = false;
		plan->bo_temp_no_evict_pinned = false;
		plan->had_sparse_clear = false;
		plan->fast_noop = false;
		plan->pte_kind = 0;
}

static void
nvkm_drm_vm_valid_op_plan_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_valid_op_plan *plan)
{
	if (plan->bo_temp_pinned && plan->bo != NULL)
		(void)nvkm_bo_vm_bind_unpin(plan->bo,
		    plan->bo_temp_no_evict_pinned);
	plan->bo_temp_pinned = false;
	plan->bo_temp_no_evict_pinned = false;
	nvkm_drm_vm_valid_map_plan_fini(nfile, &plan->valid_map_plan);
	nvkm_drm_vm_bind_abort_sparse_clear(nfile, &plan->sparse_clear_plan);
	nvkm_drm_vm_bind_segment_plan_fini(&plan->segment_plan);
	nvkm_drm_vm_bind_put_op_object(plan->obj, plan->job_object);
	nvkm_drm_vm_valid_op_plan_init(plan);
}

static void
nvkm_drm_vm_op_plan_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_op_plan *plan)
{
	switch (plan->kind) {
	case NVKM_DRM_VM_OP_PLAN_CLEAR:
	case NVKM_DRM_VM_OP_PLAN_MAP_NULL:
		nvkm_drm_vm_clear_plan_fini(nfile, &plan->u.clear_plan);
		break;
	case NVKM_DRM_VM_OP_PLAN_MAP_SPARSE:
		nvkm_drm_vm_sparse_map_plan_fini(nfile,
		    &plan->u.sparse_map_plan);
		break;
	case NVKM_DRM_VM_OP_PLAN_MAP_VALID:
	case NVKM_DRM_VM_OP_PLAN_MAP_NOOP:
		nvkm_drm_vm_valid_op_plan_fini(nfile,
		    &plan->u.valid_map_plan);
		break;
	case NVKM_DRM_VM_OP_PLAN_NONE:
		break;
	}
	nvkm_drm_vm_op_plan_init(plan);
}

static struct drm_gem_object *
nvkm_drm_vm_op_plan_trace_obj(struct nvkm_drm_vm_op_plan *plan)
{
	switch (plan->kind) {
	case NVKM_DRM_VM_OP_PLAN_MAP_VALID:
	case NVKM_DRM_VM_OP_PLAN_MAP_NOOP:
		return (plan->u.valid_map_plan.obj);
	default:
		return (NULL);
	}
}

static void
nvkm_drm_vm_op_plan_prepare_common(struct nvkm_drm_vm_op_plan *plan,
    enum nvkm_drm_vm_op_plan_kind kind, uint32_t action,
    const struct drm_nouveau_vm_bind_op *op)
{
	plan->kind = kind;
	plan->action = action;
	plan->op_flags = op->flags;
	plan->handle = op->handle;
	plan->addr = op->addr;
	plan->size = op->range;
	plan->bo_offset = op->bo_offset;
}

static int
nvkm_drm_vm_op_plan_prepare_clear(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_op_plan *plan,
    const struct drm_nouveau_vm_bind_op *op)
{
	uint32_t action =
	    (op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0 ?
	    NVKM_DRM_VM_TRACE_UNMAP_SPARSE : NVKM_DRM_VM_TRACE_UNMAP;
	bool clear_sparse = (op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0;

	nvkm_drm_vm_op_plan_prepare_common(plan, NVKM_DRM_VM_OP_PLAN_CLEAR,
	    action, op);
	nvkm_drm_vm_clear_plan_init(&plan->u.clear_plan);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND unmap op=%u flags=0x%08x handle=%u addr=0x%016jx range=0x%016jx\n",
	    op->op, op->flags, op->handle, (uintmax_t)op->addr,
	    (uintmax_t)op->range);
	nvkm_drm_vm_bind_note_op(sc, action, op->range);
	return (nvkm_drm_vm_clear_plan_prepare(sc, nfile, &plan->u.clear_plan,
	    op->addr, op->range, clear_sparse, !clear_sparse, false,
	    NVKM_GMMU_SPT_SHIFT, false, action));
}

static int
nvkm_drm_vm_op_plan_prepare_map_null(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_op_plan *plan,
    const struct drm_nouveau_vm_bind_op *op)
{
	nvkm_drm_vm_op_plan_prepare_common(plan, NVKM_DRM_VM_OP_PLAN_MAP_NULL,
	    NVKM_DRM_VM_TRACE_MAP_NULL, op);
	nvkm_drm_vm_clear_plan_init(&plan->u.clear_plan);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND map-null flags=0x%08x addr=0x%016jx range=0x%016jx\n",
	    op->flags, (uintmax_t)op->addr, (uintmax_t)op->range);
	nvkm_drm_vm_bind_note_op(sc, NVKM_DRM_VM_TRACE_MAP_NULL, op->range);
	return (nvkm_drm_vm_clear_plan_prepare(sc, nfile, &plan->u.clear_plan,
	    op->addr, op->range, true, true, false, NVKM_GMMU_SPT_SHIFT, false,
	    NVKM_DRM_VM_TRACE_MAP_NULL));
}

static int
nvkm_drm_vm_op_plan_prepare_map_sparse(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_op_plan *plan,
    const struct drm_nouveau_vm_bind_op *op)
{
	nvkm_drm_vm_op_plan_prepare_common(plan, NVKM_DRM_VM_OP_PLAN_MAP_SPARSE,
	    NVKM_DRM_VM_TRACE_MAP_SPARSE, op);
	nvkm_drm_vm_sparse_map_plan_init(&plan->u.sparse_map_plan);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND map sparse flags=0x%08x handle=%u addr=0x%016jx range=0x%016jx\n",
	    op->flags, op->handle, (uintmax_t)op->addr,
	    (uintmax_t)op->range);
	nvkm_drm_vm_bind_note_op(sc, NVKM_DRM_VM_TRACE_MAP_SPARSE, op->range);
	return (nvkm_drm_vm_sparse_map_plan_prepare(sc, nfile,
	    &plan->u.sparse_map_plan, op->addr, op->range));
}

static int
nvkm_drm_vm_op_plan_prepare_valid_map(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_op_plan *plan,
    const struct drm_nouveau_vm_bind_op *op, struct drm_gem_object *job_obj)
{
	struct nvkm_drm_vm_valid_op_plan *valid = &plan->u.valid_map_plan;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;
	int err;

	nvkm_drm_vm_op_plan_prepare_common(plan, NVKM_DRM_VM_OP_PLAN_MAP_VALID,
	    NVKM_DRM_VM_TRACE_MAP, op);
	nvkm_drm_vm_valid_op_plan_init(valid);
	valid->pte_kind = op->flags & 0xff;
	if (job_obj != NULL) {
		obj = job_obj;
		valid->job_object = true;
	} else {
		obj = drm_gem_object_lookup(file_priv, op->handle);
	}
	valid->obj = obj;
	if (obj == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND missing BO handle=%u\n", op->handle);
		return (-ENOENT);
	}
	bo = to_nvkm_bo(obj);
	valid->bo = bo;
	if (op->bo_offset > obj->size ||
	    op->range > obj->size - op->bo_offset) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND BO range invalid handle=%u bo_off=0x%016jx range=0x%016jx size=0x%016jx\n",
		    op->handle, (uintmax_t)op->bo_offset,
		    (uintmax_t)op->range, (uintmax_t)obj->size);
		return (-EINVAL);
	}
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND map flags=0x%08x handle=%u obj=%p domain=0x%x addr=0x%016jx bo_off=0x%016jx range=0x%016jx paddr=0x%016jx\n",
	    op->flags, op->handle, obj, bo->domain, (uintmax_t)op->addr,
	    (uintmax_t)op->bo_offset, (uintmax_t)op->range,
	    (uintmax_t)bo->paddr);
	nvkm_drm_vm_bind_note_op(sc, NVKM_DRM_VM_TRACE_MAP, op->range);

	if (!nvkm_gsp_vmm_has_sparse_region(nfile->vmm, op->addr,
	    op->range) &&
	    nvkm_drm_vm_bindings_match_exact_map_op(nfile, obj, op->addr,
	    op->range, op->bo_offset, valid->pte_kind)) {
		plan->kind = NVKM_DRM_VM_OP_PLAN_MAP_NOOP;
		valid->fast_noop = true;
		return (0);
	}

	err = nvkm_bo_vm_bind_pin(bo, &valid->bo_temp_no_evict_pinned);
	if (err != 0)
		return (-err);
	valid->bo_temp_pinned = true;

	if ((bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) == 0) {
		err = nvkm_bo_ensure_ttm_populated(bo);
		if (err != 0)
			return (-err);
	}
	err = nvkm_drm_vm_bind_build_map_segments(sc, bo, op->addr,
	    op->bo_offset, op->range, &valid->segment_plan);
	if (err != 0)
		return (err);

	if (nvkm_drm_vm_bindings_match_map_range(nfile, obj,
	    valid->segment_plan.segments, valid->segment_plan.count,
	    valid->pte_kind)) {
		err = nvkm_drm_vm_bind_prepare_sparse_clear(nfile, op->addr,
		    op->range, NVKM_GMMU_PD0_SHIFT,
		    NVKM_DRM_VM_SPARSE_CLEAR_METADATA_ONLY,
		    &valid->sparse_clear_plan);
		if (err != 0)
			return (err);
		plan->kind = NVKM_DRM_VM_OP_PLAN_MAP_NOOP;
		valid->had_sparse_clear = valid->sparse_clear_plan != NULL;
		return (0);
	}

	err = nvkm_drm_vm_bind_prepare_sparse_clear(nfile, op->addr,
	    op->range, nvkm_drm_vm_bind_segments_min_page_shift(
	    valid->segment_plan.segments, valid->segment_plan.count),
	    NVKM_DRM_VM_SPARSE_CLEAR_TARGET_OVERWRITE,
	    &valid->sparse_clear_plan);
	if (err != 0)
		return (err);

	nvkm_drm_vm_bind_debug_segments(sc, valid->segment_plan.segments,
	    valid->segment_plan.count);
	err = nvkm_drm_vm_valid_map_plan_prepare(sc, nfile,
	    &valid->valid_map_plan, obj, valid->segment_plan.segments,
	    valid->segment_plan.count, op->addr, op->range, valid->pte_kind,
	    &valid->sparse_clear_plan, bo);
	if (err != 0)
		return (err);
	(void)nvkm_bo_vm_bind_unpin(bo, valid->bo_temp_no_evict_pinned);
	valid->bo_temp_pinned = false;
	valid->bo_temp_no_evict_pinned = false;
	return (0);
}

static int
nvkm_drm_vm_op_plan_prepare(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_op_plan *plan,
    const struct drm_nouveau_vm_bind_op *op, struct drm_gem_object *job_obj)
{
	nvkm_drm_vm_op_plan_init(plan);
	switch (op->op) {
	case DRM_NOUVEAU_VM_BIND_OP_UNMAP:
		return (nvkm_drm_vm_op_plan_prepare_clear(sc, nfile, plan, op));
	case DRM_NOUVEAU_VM_BIND_OP_MAP:
		if ((op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0)
			return (nvkm_drm_vm_op_plan_prepare_map_sparse(sc, nfile,
			    plan, op));
		if (op->handle == 0)
			return (nvkm_drm_vm_op_plan_prepare_map_null(sc, nfile,
			    plan, op));
		return (nvkm_drm_vm_op_plan_prepare_valid_map(sc, file_priv,
		    nfile, plan, op, job_obj));
	default:
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND unknown op=%u flags=0x%08x\n",
		    op->op, op->flags);
		return (-EINVAL);
	}
}

static int
nvkm_drm_vm_op_plan_commit_preflight(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_op_plan *plan)
{
	struct nvkm_drm_vm_valid_op_plan *valid;

	switch (plan->kind) {
	case NVKM_DRM_VM_OP_PLAN_MAP_VALID:
		valid = &plan->u.valid_map_plan;
		return (nvkm_drm_vm_valid_map_plan_commit_preflight(nfile,
		    &valid->valid_map_plan, valid->segment_plan.segments,
		    valid->segment_plan.count, valid->bo));
	default:
		return (0);
	}
}

static int
nvkm_drm_vm_op_plan_commit(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct nvkm_drm_vm_op_plan *plan,
    struct nvkm_drm_vm_dirty_set *dirty_set,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_valid_op_plan *valid;
	uint32_t unmapped = 0;
	bool promoted;
	int err;

	switch (plan->kind) {
	case NVKM_DRM_VM_OP_PLAN_CLEAR:
	case NVKM_DRM_VM_OP_PLAN_MAP_NULL:
		err = nvkm_drm_vm_clear_plan_commit(sc, nfile,
		    &plan->u.clear_plan, &unmapped, dirty_set,
		    retired_bindings);
		if (err != 0)
			return (err);
		(void)nvkm_drm_vm_bindings_normalize(sc, nfile, plan->addr,
		    plan->size, dirty_set, retired_bindings);
		return (0);
	case NVKM_DRM_VM_OP_PLAN_MAP_SPARSE:
		err = nvkm_drm_vm_sparse_map_plan_commit(sc, nfile,
		    &plan->u.sparse_map_plan, plan->op_flags, &unmapped,
		    dirty_set, retired_bindings);
		if (err != 0)
			return (err);
		(void)nvkm_drm_vm_bindings_normalize(sc, nfile, plan->addr,
		    plan->size, dirty_set, retired_bindings);
		return (0);
	case NVKM_DRM_VM_OP_PLAN_MAP_NOOP:
		valid = &plan->u.valid_map_plan;
		err = nvkm_drm_vm_bind_commit_sparse_clear_noflush(sc, nfile,
		    &valid->sparse_clear_plan, NVKM_DRM_VM_TRACE_MAP, plan->addr,
		    plan->size, dirty_set);
		if (err != 0)
			return (err);
		promoted = nvkm_drm_vm_bindings_normalize(sc, nfile, plan->addr,
		    plan->size, dirty_set, retired_bindings);
		if (!valid->had_sparse_clear && !promoted) {
			sc->vm_bind_noop_count++;
			if (valid->fast_noop)
				sc->vm_bind_noop_fast_count++;
		}
		return (0);
	case NVKM_DRM_VM_OP_PLAN_MAP_VALID:
		valid = &plan->u.valid_map_plan;
		err = nvkm_drm_vm_valid_map_plan_commit(sc, nfile,
		    &valid->valid_map_plan, valid->segment_plan.segments,
		    valid->segment_plan.count, valid->bo, plan->op_flags,
		    plan->addr, plan->size, dirty_set, retired_bindings);
		if (err != 0)
			return (err);
		nvkm_drm_vm_bind_mark_bo_tiled(valid->bo, valid->pte_kind);
		nvkm_drm_vm_bindings_normalize(sc, nfile, plan->addr,
		    plan->size, dirty_set, retired_bindings);
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND track addr=0x%016jx size=0x%016jx obj=%p\n",
		    (uintmax_t)plan->addr, (uintmax_t)plan->size, valid->obj);
		return (0);
	case NVKM_DRM_VM_OP_PLAN_NONE:
		return (-EINVAL);
	}
	return (-EINVAL);
}

/*
 * nvkm_drm_vm_batch_plan_*()
 *
 * Ownership:
 *   The batch plan borrows the VM_BIND op array and the optional per-op GEM
 *   object array from the ioctl/job owner.  It owns at most one active op plan
 *   at a time.  Submitted live bindings and retired old bindings are still
 *   transferred through the op plan into the caller-provided retired list.
 *
 * Lifetime:
 *   The batch plan is a sequential cursor, not an all-or-nothing transaction.
 *   Each op is prepared, committed, traced, and finished before the next op is
 *   touched.  If a later op fails during prepare, already committed earlier ops
 *   remain visible exactly as nouveau VM_BIND ordering requires.
 *
 * Threading:
 *   The caller must hold the same VM_BIND serialization used by the per-op plan
 *   path.  apply() runs inside the VM/GSP mutation window and only delays the
 *   final GMMU invalidate through dirty_set; it does not wait on GPU fences.
 */
static void
nvkm_drm_vm_batch_plan_init(struct nvkm_drm_vm_batch_plan *plan,
    struct drm_nouveau_vm_bind_op *ops, uint32_t op_count,
    struct drm_gem_object **objects)
{
	memset(plan, 0, sizeof(*plan));
	plan->ops = ops;
	plan->objects = objects;
	plan->op_count = op_count;
	nvkm_drm_vm_dirty_set_init(&plan->dirty_set);
	nvkm_drm_vm_op_plan_init(&plan->current_op);
}

static void
nvkm_drm_vm_batch_plan_fini(struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_batch_plan *plan)
{
	if (plan->current_op_active) {
		nvkm_drm_vm_op_plan_fini(nfile, &plan->current_op);
		plan->current_op_active = false;
	}
}

/*
 * nvkm_drm_vm_bind_inject_prepare_failure()
 *
 * Ownership:
 *   Borrows sc and consumes the one-shot debug counter when it fires.
 *
 * Lifetime:
 *   The helper is called after an op has prepared all temporary state and
 *   before commit publishes any mapping/PTE changes.  On injection, the caller
 *   must still run op-plan fini so prepared refs, pins, sparse plans, and
 *   segment storage are released without touching live VM state.
 *
 * Threading:
 *   Called while the VM_BIND mutation path is serialized by vm_token/gsp_tok.
 *   The sysctl writer may update the integer concurrently; this is debug-only
 *   and one-shot, so races can only change whether the next prepared op is
 *   injected, never the VM_BIND ABI or normal default-off behavior.
 */
static bool
nvkm_drm_vm_bind_inject_prepare_failure(struct nvkm_softc *sc,
    uint32_t op_index)
{
	if (sc == NULL || sc->vm_bind_prepare_fail_after <= 0)
		return (false);
	if (sc->vm_bind_prepare_fail_after > 1) {
		sc->vm_bind_prepare_fail_after--;
		return (false);
	}
	sc->vm_bind_prepare_fail_after = 0;
	sc->vm_bind_prepare_fail_count++;
	sc->vm_bind_prepare_fail_last_index = op_index;
	return (true);
}

/*
 * nvkm_drm_vm_bind_inject_commit_pt_failure()
 *
 * Ownership:
 *   Borrows sc and the already prepared op plan.  It consumes the public
 *   one-shot counter and records the VA used for diagnostics.
 *
 * Lifetime:
 *   Called after the read-only commit preflight has proven prepared storage is
 *   present, but before materialize, sparse clear, PTE write, or mapping splice
 *   publishes any new state.  A non-zero return therefore leaves the old VM
 *   state unchanged while still exercising the commit-error path.
 *
 * Threading:
 *   Called under the VM_BIND mutation serialization.  The sysctl writer may
 *   race with the integer counter, but normal operation leaves it zero.
 */
static bool
nvkm_drm_vm_bind_inject_commit_pt_failure(struct nvkm_softc *sc,
    const struct nvkm_drm_vm_op_plan *plan, uint32_t op_index)
{
	if (sc == NULL || sc->vm_bind_commit_pt_fail_after <= 0)
		return (false);
	if (sc->vm_bind_commit_pt_fail_after > 1) {
		sc->vm_bind_commit_pt_fail_after--;
		return (false);
	}
	sc->vm_bind_commit_pt_fail_after = 0;
	sc->vm_bind_commit_pt_fail_count++;
	sc->vm_bind_commit_pt_fail_last_index = op_index;
	sc->vm_bind_commit_pt_fail_last_va = plan != NULL ? plan->addr : 0;
	return (true);
}

/*
 * nvkm_drm_vm_bind_inject_parent_child_failure()
 *
 * Ownership:
 *   Borrows sc and the already prepared op plan.  It consumes the one-shot
 *   debug counter and records the VA whose commit unit was rejected.
 *
 * Lifetime:
 *   Called after commit preflight has walked the prepared page-table state and
 *   before any materialize, sparse clear, PTE/PDE writer, or mapping splice is
 *   allowed to publish.  A non-zero return therefore simulates detection of a
 *   parent/child ownership invariant failure without corrupting live VMM state.
 *
 * Threading:
 *   Called under the serialized VM_BIND mutation path.  Concurrent sysctl
 *   writes are debug-only; the default-off path is unaffected and keeps the
 *   nouveau VM_BIND ABI unchanged.
 */
static bool
nvkm_drm_vm_bind_inject_parent_child_failure(struct nvkm_softc *sc,
    const struct nvkm_drm_vm_op_plan *plan, uint32_t op_index)
{
	if (sc == NULL || sc->vm_bind_parent_child_fail_after <= 0)
		return (false);
	if (sc->vm_bind_parent_child_fail_after > 1) {
		sc->vm_bind_parent_child_fail_after--;
		return (false);
	}
	sc->vm_bind_parent_child_fail_after = 0;
	sc->vm_bind_parent_child_fail_count++;
	sc->vm_bind_parent_child_fail_last_index = op_index;
	sc->vm_bind_parent_child_fail_last_va = plan != NULL ? plan->addr : 0;
	return (true);
}

static int
nvkm_drm_vm_batch_plan_apply(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    struct nvkm_drm_vm_batch_plan *plan,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	int err = 0;

	sc->vm_bind_batch_count++;
	if (plan->op_count > 1)
		sc->vm_bind_batch_multi_op_count++;

	for (uint32_t i = 0; i < plan->op_count; i++) {
		struct drm_nouveau_vm_bind_op *op = &plan->ops[i];
		struct drm_gem_object *job_obj = NULL;
		bool prepared = false;

		plan->failed_op = op;
		if (op->range == 0 || op->addr > UINT64_MAX - op->range ||
		    ((op->addr | op->bo_offset | op->range) &
		     (NVKM_GMMU_PT_PAGE_SIZE - 1))) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND invalid idx=%u op=%u flags=0x%08x handle=%u addr=0x%016jx bo_off=0x%016jx range=0x%016jx\n",
			    i, op->op, op->flags, op->handle,
			    (uintmax_t)op->addr, (uintmax_t)op->bo_offset,
			    (uintmax_t)op->range);
			sc->vm_bind_batch_prepare_error_count++;
			return (-EINVAL);
		}

		nvkm_drm_vm_op_plan_init(&plan->current_op);
		plan->current_op_active = true;
		if (plan->objects != NULL)
			job_obj = plan->objects[i];
		err = nvkm_drm_vm_op_plan_prepare(sc, file_priv, nfile,
		    &plan->current_op, op, job_obj);
		if (err == 0 &&
		    nvkm_drm_vm_bind_inject_prepare_failure(sc, i))
			err = -EIO;
		if (err == 0) {
			prepared = true;
			err = nvkm_drm_vm_op_plan_commit_preflight(nfile,
			    &plan->current_op);
			if (err == 0 &&
			    nvkm_drm_vm_bind_inject_commit_pt_failure(sc,
			    &plan->current_op, i))
				err = -ENOENT;
			if (err == 0 &&
			    nvkm_drm_vm_bind_inject_parent_child_failure(sc,
			    &plan->current_op, i))
				err = -EIO;
			if (err == 0) {
				err = nvkm_drm_vm_op_plan_commit(sc, nfile,
				    &plan->current_op, &plan->dirty_set,
				    retired_bindings);
			}
		}
		if (plan->current_op.action != 0) {
			nvkm_drm_vm_trace_record(sc, plan->current_op.action,
			    op->flags, op->handle, op->addr, op->range,
			    op->bo_offset,
			    nvkm_drm_vm_op_plan_trace_obj(&plan->current_op),
			    err);
		}
		if (err != 0) {
			if (prepared)
				sc->vm_bind_batch_commit_error_count++;
			else
				sc->vm_bind_batch_prepare_error_count++;
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND op failed idx=%u op=%u flags=0x%08x err=%d\n",
			    i, op->op, op->flags, err);
			return (err);
		}
		sc->vm_bind_batch_committed_op_count++;
		nvkm_drm_vm_op_plan_fini(nfile, &plan->current_op);
		plan->current_op_active = false;
	}
	plan->failed_op = NULL;
	return (0);
}

static void
nvkm_hotproc_record_vm_bind(struct nvkm_softc *sc,
    const struct drm_nouveau_vm_bind *req,
    const struct drm_nouveau_vm_bind_op *ops)
{
	struct nvkm_hotproc_slot *slot;
	char comm[MAXCOMLEN + 1];
	pid_t pid;
	uint64_t map_count = 0;
	uint64_t map_pages = 0;
	uint64_t unmap_count = 0;
	uint64_t unmap_pages = 0;
	uint64_t sparse_count = 0;
	uint64_t other_count = 0;
	uint64_t max_pages = 0;

	if (sc == NULL || req == NULL)
		return;

	for (uint32_t i = 0; ops != NULL && i < req->op_count; i++) {
		const struct drm_nouveau_vm_bind_op *op = &ops[i];
		uint64_t pages = op->range / NVKM_GMMU_PT_PAGE_SIZE;

		if (pages > max_pages)
			max_pages = pages;
		if ((op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0)
			sparse_count++;
		switch (op->op) {
		case DRM_NOUVEAU_VM_BIND_OP_MAP:
			map_count++;
			map_pages += pages;
			break;
		case DRM_NOUVEAU_VM_BIND_OP_UNMAP:
			unmap_count++;
			unmap_pages += pages;
			break;
		default:
			other_count++;
			break;
		}
	}

	nvkm_proc_snapshot(&pid, comm, sizeof(comm));

	spin_lock(&sc->hotproc_lock);
	slot = nvkm_hotproc_find_slot_locked(sc, pid, comm);
	slot->vm_bind_ioctl_count++;
	slot->vm_bind_op_count += req->op_count;
	if ((req->flags & DRM_NOUVEAU_VM_BIND_RUN_ASYNC) != 0)
		slot->vm_bind_async_count++;
	else
		slot->vm_bind_sync_count++;
	slot->vm_bind_wait_count += req->wait_count;
	slot->vm_bind_sig_count += req->sig_count;
	slot->vm_bind_map_count += map_count;
	slot->vm_bind_map_pages += map_pages;
	slot->vm_bind_unmap_count += unmap_count;
	slot->vm_bind_unmap_pages += unmap_pages;
	slot->vm_bind_sparse_count += sparse_count;
	slot->vm_bind_other_count += other_count;
	if (max_pages > slot->vm_bind_max_pages)
		slot->vm_bind_max_pages = max_pages;
	spin_unlock(&sc->hotproc_lock);
}

struct nvkm_drm_exec_signal;

static int nvkm_drm_queue_vm_bind_job(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    const struct drm_nouveau_vm_bind *req,
    struct drm_nouveau_vm_bind_op *ops, bool sync);

static int nvkm_drm_vm_bind_attach_resv_fence(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct drm_nouveau_vm_bind_op *ops,
    uint32_t op_count, struct drm_gem_object **objects,
    struct dma_fence *fence);

/*
 * nvkm_drm_vm_bind_apply()
 *
 * Ownership:
 *   Borrows ops and objects from the VM_BIND job.  When a MAP op succeeds,
 *   ownership of that op's GEM reference moves into a live binding.  Old live
 *   bindings removed or replaced by the operation move into retired_bindings.
 *
 * Lifetime:
 *   retired_bindings remains owned by the caller.  It must outlive this call
 *   and must not be released until after the VM_BIND done fence is signaled.
 *   That preserves nouveau's visible completion ordering while avoiding
 *   no_share BO free waits on the current job's own reservation fence.
 *
 * Threading:
 *   Takes nfile->vm_token and sc->gsp_tok for the PTE update window.  It does
 *   not wait for GPU execution fences; userspace-provided syncobjs express
 *   those dependencies.
 */
static int
nvkm_drm_vm_bind_apply(struct nvkm_softc *sc, struct drm_file *file_priv,
    struct nvkm_drm_file *nfile, struct drm_nouveau_vm_bind_op *ops,
    uint32_t op_count, struct drm_gem_object **objects,
    struct nvkm_drm_vm_binding_list *retired_bindings)
{
	struct nvkm_drm_vm_batch_plan batch_plan;
	struct drm_nouveau_vm_bind_op *current_op = NULL;
	int err = 0;
	bool gsp_tok_held = false;
	bool remap_started = false;
	uint64_t profile_start;

	/* vm_token (outer) serializes this remap against other remaps and
	 * prevents EXEC from doorbelling while the PTE update is half-written.
	 * It deliberately does not wait for already submitted GPU work; nouveau
	 * exposes that ordering through syncobjs and VM_BIND/EXEC fences. */
	nvkm_drm_vm_batch_plan_init(&batch_plan, ops, op_count, objects);
	profile_start = nvkm_drm_profile_now_us(sc);
	lwkt_gettoken(&nfile->vm_token);
	remap_started = true;
	lwkt_gettoken(&sc->gsp_tok);
	gsp_tok_held = true;
	nvkm_drm_profile_add_us(sc, &sc->vm_bind_profile_token_wait_us,
	    profile_start);
	profile_start = nvkm_drm_profile_now_us(sc);

	err = nvkm_drm_vm_batch_plan_apply(sc, file_priv, nfile, &batch_plan,
	    retired_bindings);
	current_op = batch_plan.failed_op;
	nvkm_drm_vm_batch_plan_fini(nfile, &batch_plan);
	nvkm_drm_vm_dirty_set_publish(sc, &batch_plan.dirty_set);

out_unlock:
	nvkm_drm_profile_add_us(sc, &sc->vm_bind_profile_apply_us,
	    profile_start);
	/* One TLB invalidate publishes every PTE the loop wrote,
	 * instead of one per op (the _noflush calls skip it). */
	if (gsp_tok_held && batch_plan.dirty_set.dirty) {
		profile_start = nvkm_drm_profile_now_us(sc);
		sc->vm_bind_flush_count++;
		nvkm_drm_vm_dirty_set_flush(nfile->vmm,
		    &batch_plan.dirty_set);
		nvkm_drm_profile_add_us(sc, &sc->vm_bind_profile_flush_us,
		    profile_start);
	} else if (gsp_tok_held) {
		sc->vm_bind_clean_batch_count++;
	}
	/* Release inner (gsp_tok) before outer (vm_token). */
	if (gsp_tok_held)
		lwkt_reltoken(&sc->gsp_tok);
	if (remap_started)
		lwkt_reltoken(&nfile->vm_token);
	if (err != 0 && current_op != NULL)
		nvkm_drm_vm_bind_record_error(sc, current_op->op,
		    current_op->flags, current_op->handle, current_op->addr,
		    current_op->range, current_op->bo_offset, err);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND complete ops=%u err=%d\n",
	    op_count, err);
	return (err);
}

static int
nvkm_drm_vm_bind_prepare_objects(struct drm_file *file_priv,
    struct drm_nouveau_vm_bind_op *ops, uint32_t op_count,
    struct drm_gem_object ***pobjects)
{
	struct drm_gem_object **objects;
	int err = 0;

	*pobjects = NULL;
	if (op_count == 0)
		return (0);

	objects = kcalloc(op_count, sizeof(*objects), GFP_KERNEL);
	if (objects == NULL)
		return (-ENOMEM);

	for (uint32_t i = 0; i < op_count; i++) {
		struct drm_nouveau_vm_bind_op *op = &ops[i];
		struct drm_gem_object *obj;

		if (op->range == 0 || op->addr > UINT64_MAX - op->range ||
		    ((op->addr | op->bo_offset | op->range) &
		     (NVKM_GMMU_PT_PAGE_SIZE - 1))) {
			err = -EINVAL;
			break;
		}
		switch (op->op) {
		case DRM_NOUVEAU_VM_BIND_OP_MAP:
			break;
		case DRM_NOUVEAU_VM_BIND_OP_UNMAP:
			continue;
		default:
			err = -EINVAL;
			goto out;
		}
		if ((op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0 ||
		    op->handle == 0)
			continue;

		obj = drm_gem_object_lookup(file_priv, op->handle);
		if (obj == NULL) {
			err = -ENOENT;
			break;
		}
		if (op->bo_offset > obj->size ||
		    op->range > obj->size - op->bo_offset) {
			drm_gem_object_put_unlocked(obj);
			err = -EINVAL;
			break;
		}
		objects[i] = obj;
	}

out:
	if (err != 0) {
		for (uint32_t i = 0; i < op_count; i++) {
			if (objects[i] != NULL)
				drm_gem_object_put_unlocked(objects[i]);
		}
		kfree(objects);
		return (err);
	}

	*pobjects = objects;
	return (0);
}

static void
nvkm_drm_vm_bind_objects_put(struct drm_gem_object **objects,
    uint32_t op_count)
{
	if (objects == NULL)
		return;
	for (uint32_t i = 0; i < op_count; i++) {
		if (objects[i] != NULL)
			drm_gem_object_put_unlocked(objects[i]);
	}
	kfree(objects);
}

static int
nvkm_drm_ioctl_vm_bind(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_vm_bind *req = data;
	struct drm_nouveau_vm_bind_op *ops = NULL;
	int err = 0;
	bool async_bind;
	uint64_t profile_start;
	uint64_t profile_total_start;

	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND begin ops=%u waits=%u sigs=%u flags=0x%08x\n",
	    req->op_count, req->wait_count, req->sig_count, req->flags);
	if (sc->gsp_vmm == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND no gsp_vmm\n");
		return (-ENXIO);
	}
	if (nfile == NULL)
		return (-ENXIO);
	if ((req->flags & ~DRM_NOUVEAU_VM_BIND_RUN_ASYNC) != 0)
		return (-EINVAL);
	async_bind = (req->flags & DRM_NOUVEAU_VM_BIND_RUN_ASYNC) != 0;
	if (!async_bind && (req->wait_count != 0 || req->sig_count != 0))
		return (-EINVAL);
	if (req->op_count > 1024) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND too many ops=%u\n", req->op_count);
		return (-EINVAL);
	}

	profile_total_start = nvkm_drm_profile_now_us(sc);
	err = nvkm_drm_file_ensure_vmm(sc, nfile);
	if (err != 0)
		return (err);
	sc->vm_bind_ioctl_count++;
	sc->vm_bind_op_count += req->op_count;
	if (req->op_count > sc->vm_bind_max_op_count)
		sc->vm_bind_max_op_count = req->op_count;
	if (async_bind)
		sc->vm_bind_async_count++;
	else
		sc->vm_bind_sync_count++;

	if (req->op_count != 0) {
		profile_start = nvkm_drm_profile_now_us(sc);
		ops = kmalloc_array(req->op_count, sizeof(*ops), GFP_KERNEL);
		if (ops == NULL)
			return (-ENOMEM);
		err = copyin((const void *)(uintptr_t)req->op_ptr, ops,
		    sizeof(*ops) * req->op_count);
		nvkm_drm_profile_add_us(sc, &sc->vm_bind_profile_copyin_us,
		    profile_start);
		if (err != 0) {
			kfree(ops);
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND copyin failed ops=%u err=%d\n",
			    req->op_count, err);
			return (-EFAULT);
		}
	}
	nvkm_hotproc_record_vm_bind(sc, req, ops);

	err = nvkm_drm_queue_vm_bind_job(sc, file_priv, nfile, req, ops,
	    !async_bind);
	ops = NULL;
	nvkm_drm_profile_add_us(sc, &sc->vm_bind_profile_total_us,
	    profile_total_start);
	return (err);
}

/* ---- DRM_NOUVEAU_EXEC ---- */
struct drm_nouveau_exec {
	uint32_t channel;
	uint32_t push_count;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t push_ptr;
};

#define NV_USERD_SLOT_SIZE	0x200
#define NV_USERD_GP_GET		0x88
#define NV_USERD_GP_PUT		0x8c
#define NV_USERMODE_BASE	0xbb0000u
#define NV_USERMODE_DOORBELL	(NV_USERMODE_BASE + 0x90)
#define NVC06F_GP_ENTRY1_LENGTH_SHIFT	10
#define NVC06F_GP_ENTRY1_NO_PREFETCH	(1u << 31)
#define DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH	0x1u
#define DRM_NOUVEAU_EXEC_PUSH_FLAGS_KNOWN	\
	DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH
#define NVC36F_DMA_SEC_OP_INC_METHOD	1u
#define NVC36F_MEM_OP_A_OFFSET		0x28
#define NVC36F_SEM_ADDR_LO_OFFSET	0x5c
#define NVC36F_SEM_EXECUTE_OFFSET	0x6c
#define NVC36F_NON_STALL_INTERRUPT_OFFSET	0x20
#define NVC36F_PUSH_HDR_MEM_OP_A_TO_D \
	((NVC36F_DMA_SEC_OP_INC_METHOD << 29) | (4u << 16) | \
	 (NVC36F_MEM_OP_A_OFFSET >> 2))
#define NVC36F_PUSH_HDR_SEM_ADDR_TRIPLET \
	((NVC36F_DMA_SEC_OP_INC_METHOD << 29) | (3u << 16) | \
	 (NVC36F_SEM_ADDR_LO_OFFSET >> 2))
#define NVC36F_PUSH_HDR_SEM_EXECUTE \
	((NVC36F_DMA_SEC_OP_INC_METHOD << 29) | (1u << 16) | \
	 (NVC36F_SEM_EXECUTE_OFFSET >> 2))
#define NVC36F_PUSH_HDR_NON_STALL_INTERRUPT \
	((NVC36F_DMA_SEC_OP_INC_METHOD << 29) | (1u << 16) | \
	 (NVC36F_NON_STALL_INTERRUPT_OFFSET >> 2))
#define NVC36F_SEM_EXECUTE_RELEASE	1u
#define NVC36F_SEM_EXECUTE_RELEASE_WFI	(1u << 20)
#define NVC36F_MEM_OP_C_MEMBAR_SYS_MEMBAR	0u
#define NVC36F_MEM_OP_D_OPERATION_MEMBAR	(0x5u << 27)
#define NVKM_DRM_SUBMIT_GVA_PUSHBUF	(NVKM_VMM_CLIENT_BASE + 0x0000ULL)
#define NVKM_DRM_SUBMIT_GVA_GPFIFO	(NVKM_VMM_CLIENT_BASE + 0x1000ULL)
#define NVKM_DRM_SUBMIT_GVA_SEMA	(NVKM_VMM_CLIENT_BASE + 0x2000ULL)
#define NVKM_DRM_POST_PUSH_DWORDS	13
#define NVKM_DRM_POST_RING_SLOTS	64
#define NVKM_DRM_GPFIFO_FETCH_WINDOW	0x40
#define NVKM_DRM_EXEC_POLL_US		5000000

struct nvkm_drm_exec_fence {
	struct dma_fence base;
	spinlock_t lock;
	struct nvkm_softc *sc;
	volatile uint32_t *sema;
	uint32_t payload;
	uint32_t producer_type;
	uint32_t producer_channel;
	int32_t producer_chid;
	uint32_t producer_post_slot;
	uint32_t producer_payload;
	uint64_t producer_submit_count;
	uint32_t signal_source;
	uint32_t signal_count;
	int signal_error;
};

#define NVKM_DRM_FENCE_PRODUCER_NONE		0
#define NVKM_DRM_FENCE_PRODUCER_EXEC_JOB	1
#define NVKM_DRM_FENCE_PRODUCER_VM_BIND_JOB	2
#define NVKM_DRM_FENCE_PRODUCER_EXEC_INTERNAL	3
#define NVKM_DRM_FENCE_PRODUCER_TTM_MOVE	4

#define NVKM_DRM_FENCE_SIGNAL_NONE		0
#define NVKM_DRM_FENCE_SIGNAL_JOB		1
#define NVKM_DRM_FENCE_SIGNAL_PENDING		2
#define NVKM_DRM_FENCE_SIGNAL_TTM_MOVE		3
#define NVKM_DRM_FENCE_SIGNAL_SUBMIT_CLEANUP	4
#define NVKM_DRM_FENCE_SIGNAL_EMPTY_EXEC	5

struct nvkm_drm_exec_signal {
	struct dma_fence *fence;
	struct drm_syncobj *syncobj;
	struct dma_fence_chain *chain;
	uint64_t timeline_value;
	uint32_t type;
};

static const char *
nvkm_drm_fence_name(struct dma_fence *fence __unused)
{
	return ("nvkm-drm");
}

static bool
nvkm_drm_fence_is_signaled(struct dma_fence *fence)
{
	struct nvkm_drm_exec_fence *f =
	    container_of(fence, struct nvkm_drm_exec_fence, base);
	volatile uint32_t *sema = f->sema;

	if (sema == NULL)
		return (false);
	cpu_lfence();
	return (*sema == f->payload);
}

/*
 * nvkm_drm_fence_wait()
 *
 * Ownership:
 *   Borrows fence for the duration of the wait.  The caller owns the wait
 *   reference exactly as with dma_fence_default_wait().
 *
 * Lifetime:
 *   Records scalar diagnostics before sleeping so a stuck waiter can be
 *   identified from dev.drm.0.state without retaining any fence pointer.
 *
 * Threading:
 *   May sleep in dma_fence_default_wait().  Diagnostic fields are best-effort
 *   telemetry and are not synchronization state.
 */
static signed long
nvkm_drm_fence_wait(struct dma_fence *fence, bool intr, signed long timeout)
{
	struct nvkm_drm_exec_fence *f =
	    container_of(fence, struct nvkm_drm_exec_fence, base);
	struct nvkm_softc *sc = f->sc;
	signed long ret;

	if (sc != NULL) {
		sc->fence_wait_count++;
		sc->fence_wait_active = 1;
		sc->fence_wait_last_intr = intr ? 1 : 0;
		sc->fence_wait_last_timeout = timeout;
		sc->fence_wait_last_context = fence->context;
		sc->fence_wait_last_seqno = fence->seqno;
		sc->fence_wait_last_flags = fence->flags;
		sc->fence_wait_last_error = fence->error;
		sc->fence_wait_last_producer_type = f->producer_type;
		sc->fence_wait_last_producer_channel = f->producer_channel;
		sc->fence_wait_last_producer_chid = f->producer_chid;
		sc->fence_wait_last_producer_post_slot = f->producer_post_slot;
		sc->fence_wait_last_producer_payload = f->producer_payload;
		sc->fence_wait_last_producer_submit_count =
		    f->producer_submit_count;
		sc->fence_wait_last_signal_source = f->signal_source;
		sc->fence_wait_last_signal_count = f->signal_count;
		sc->fence_wait_last_signal_error = f->signal_error;
	}

	ret = dma_fence_default_wait(fence, intr, timeout);

	if (sc != NULL) {
		sc->fence_wait_last_ret = ret;
		sc->fence_wait_last_flags = fence->flags;
		sc->fence_wait_last_error = fence->error;
		sc->fence_wait_last_signal_source = f->signal_source;
		sc->fence_wait_last_signal_count = f->signal_count;
		sc->fence_wait_last_signal_error = f->signal_error;
		sc->fence_wait_active = 0;
	}
	return (ret);
}

static const struct dma_fence_ops nvkm_drm_fence_ops = {
	.get_driver_name = nvkm_drm_fence_name,
	.get_timeline_name = nvkm_drm_fence_name,
	.signaled = nvkm_drm_fence_is_signaled,
	.wait = nvkm_drm_fence_wait,
};

/*
 * nvkm_drm_fence_flag_signaled()
 *
 * Ownership:
 *   Borrows fence.  It does not take, drop, or publish a dma_fence
 *   reference.
 *
 * Lifetime:
 *   The caller must hold a valid fence reference for the duration of the
 *   call.  This helper samples only the software completion flag and must
 *   not be used for functional wait decisions.
 *
 * Threading:
 *   Lockless diagnostic read.  The value is best-effort telemetry and may
 *   race with a concurrent signal path.
 */
static bool
nvkm_drm_fence_flag_signaled(struct dma_fence *fence)
{
	return (fence != NULL &&
	    test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags));
}

/*
 * nvkm_drm_exec_fence_hw_ready()
 *
 * Ownership:
 *   Borrows fence and reads the nvkm EXEC fence payload in place.  It never
 *   takes ownership and never calls dma_fence_signal().
 *
 * Lifetime:
 *   The caller must hold a valid fence reference.  For nvkm EXEC fences the
 *   semaphore pointer remains owned by the pending EXEC/job object; a NULL
 *   pointer means the fence has not been armed yet.
 *
 * Threading:
 *   Lockless diagnostic read paired with a CPU load fence before sampling the
 *   GPU-written semaphore value.  It is telemetry only; completion is still
 *   published by the normal dma_fence signal path.
 */
static bool
nvkm_drm_exec_fence_hw_ready(struct dma_fence *fence)
{
	struct nvkm_drm_exec_fence *f;
	volatile uint32_t *sema;

	if (fence == NULL || fence->ops != &nvkm_drm_fence_ops)
		return (false);
	f = container_of(fence, struct nvkm_drm_exec_fence, base);
	sema = f->sema;
	if (sema == NULL)
		return (false);
	cpu_lfence();
	return (*sema == f->payload);
}

static struct dma_fence *
nvkm_drm_exec_fence_create(struct nvkm_softc *sc, unsigned seqno)
{
	struct nvkm_drm_exec_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (f == NULL)
		return (NULL);

	lockinit(&f->lock, "nvdfen", 0, 0);
	f->sc = sc;
	dma_fence_init(&f->base, &nvkm_drm_fence_ops, &f->lock,
	    sc->fence_context, seqno);
	return (&f->base);
}

/*
 * nvkm_drm_exec_fence_set_producer()
 *
 * Ownership:
 *   Borrows fence and stores scalar producer metadata in the nvkm-private
 *   fence object.  It never takes ownership of channel, job, pending, or BO
 *   pointers.
 *
 * Lifetime:
 *   Metadata is a snapshot for diagnostics and remains valid for the fence
 *   lifetime.  A zero channel/chid/post_slot means the producer has not armed
 *   a hardware completion packet yet.
 *
 * Threading:
 *   Best-effort debug store before the fence is normally published to
 *   userspace or reservations.  It is not a synchronization primitive.
 */
static void
nvkm_drm_exec_fence_set_producer(struct dma_fence *fence, uint32_t type,
    uint32_t channel, int32_t chid, uint32_t post_slot, uint32_t payload,
    uint64_t submit_count)
{
	struct nvkm_drm_exec_fence *f;

	if (fence == NULL || fence->ops != &nvkm_drm_fence_ops)
		return;

	f = container_of(fence, struct nvkm_drm_exec_fence, base);
	f->producer_type = type;
	f->producer_channel = channel;
	f->producer_chid = chid;
	f->producer_post_slot = post_slot;
	f->producer_payload = payload;
	f->producer_submit_count = submit_count;
}

/*
 * nvkm_drm_exec_fence_note_signal()
 *
 * Ownership:
 *   Borrows fence and records scalar signal-path telemetry before the caller
 *   invokes dma_fence_signal().
 *
 * Lifetime:
 *   The counters describe attempted software completion of this fence.  They
 *   intentionally survive after dma_fence_signal() so dependency diagnostics
 *   can distinguish an unsignaled producer from a common fence-layer issue.
 *
 * Threading:
 *   Lockless diagnostic update.  Multiple racing signal paths may overwrite
 *   signal_source/error, but signal_count still shows that completion was
 *   attempted.
 */
static void
nvkm_drm_exec_fence_note_signal(struct dma_fence *fence, uint32_t source,
    int error)
{
	struct nvkm_drm_exec_fence *f;

	if (fence == NULL || fence->ops != &nvkm_drm_fence_ops)
		return;

	f = container_of(fence, struct nvkm_drm_exec_fence, base);
	f->signal_source = source;
	f->signal_error = error;
	f->signal_count++;
}

/*
 * nvkm_drm_bo_create_ttm_move_fence()
 *
 * Ownership:
 *   Borrows bo and returns one dma_fence reference in *pfence.  The caller owns
 *   that reference and must either publish it with
 *   nvkm_drm_bo_publish_ttm_move_fence() or drop it with dma_fence_put().
 *
 * Lifetime:
 *   Called during TTM move prepare before any live GPUVA PTEs are rewritten.
 *   Allocation failure is therefore still recoverable without rolling back a
 *   committed remap.
 *
 * Threading:
 *   Called from the TTM move path while the BO is reserved.  It does not take
 *   VM, GSP, or reservation locks.
 */
int
nvkm_drm_bo_create_ttm_move_fence(struct nvkm_bo *bo,
    struct dma_fence **pfence)
{
	struct nvkm_softc *sc;
	struct dma_fence *fence;

	if (pfence == NULL)
		return (-EINVAL);
	*pfence = NULL;
	if (bo == NULL || bo->base.dev == NULL)
		return (-EINVAL);
	sc = bo->base.dev->dev_private;
	if (sc == NULL)
		return (-ENODEV);

	fence = nvkm_drm_exec_fence_create(sc, ++sc->fence_seqno);
	if (fence == NULL) {
		sc->ttm_rebind_fence_error_count++;
		return (-ENOMEM);
	}
	nvkm_drm_exec_fence_set_producer(fence,
	    NVKM_DRM_FENCE_PRODUCER_TTM_MOVE, 0, -1, 0, 0, 0);

	*pfence = fence;
	return (0);
}

/*
 * nvkm_drm_bo_publish_ttm_move_fence()
 *
 * Ownership:
 *   Borrows bo and fence.  The BO reservation object takes its own reference
 *   when the fence is attached; the caller keeps ownership of its reference.
 *
 * Lifetime:
 *   Called after a synchronous TTM backing move and GPUVA rebind have both
 *   completed, but before the TTM move callback returns success.  The fence is
 *   signaled immediately because current nvkm TTM moves are CPU-synchronous;
 *   future async moves can reuse the same publication point and delay signal
 *   until their completion callback.
 *
 * Threading:
 *   Called from the TTM move path while the BO is reserved.  It only locks the
 *   BO/VM reservation object through nvkm_bo_resv_add_excl_fence(); callers
 *   must not hold nfile->vm_token or sc->gsp_tok.
 */
int
nvkm_drm_bo_publish_ttm_move_fence(struct nvkm_bo *bo,
    struct dma_fence *fence)
{
	struct nvkm_softc *sc;
	int err;

	if (bo == NULL || bo->base.dev == NULL || fence == NULL)
		return (-EINVAL);
	sc = bo->base.dev->dev_private;
	if (sc == NULL)
		return (-ENODEV);

	nvkm_bo_resv_add_excl_fence(bo, fence);
	sc->ttm_rebind_resv_attach_count++;
	nvkm_drm_exec_fence_note_signal(fence,
	    NVKM_DRM_FENCE_SIGNAL_TTM_MOVE, 0);
	err = dma_fence_signal(fence);
	if (err != 0 && !dma_fence_is_signaled(fence)) {
		sc->ttm_rebind_fence_error_count++;
		device_printf(sc->dev,
		    "nvkm_drm: failed to signal TTM move fence obj=%p err=%d\n",
		    &bo->base, err);
	}

	sc->ttm_rebind_fence_count++;
	return (0);
}

static void
nvkm_drm_wait_fences_put(struct dma_fence **fences, uint32_t count)
{
	if (fences == NULL)
		return;
	for (uint32_t i = 0; i < count; i++)
		dma_fence_put(fences[i]);
	kfree(fences);
}

static int
nvkm_drm_collect_wait_syncobjs(struct nvkm_softc *sc,
    struct drm_file *file_priv, uint32_t count, uint64_t wait_ptr,
    struct dma_fence ***pfences, uint32_t *pretained_count)
{
	struct drm_nouveau_sync *waits;
	struct dma_fence **fences;
	uint32_t retained = 0;
	int err = 0;

	*pfences = NULL;
	*pretained_count = 0;
	if (count == 0)
		return (0);
	sc->sync_wait_count += count;
	if (count > 64) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync wait invalid count=%u\n", count);
		return (-EINVAL);
	}

	waits = kmalloc_array(count, sizeof(*waits), GFP_KERNEL);
	fences = kcalloc(count, sizeof(*fences), GFP_KERNEL);
	if (waits == NULL) {
		return (-ENOMEM);
	}
	if (fences == NULL) {
		kfree(waits);
		return (-ENOMEM);
	}
	err = copyin((const void *)(uintptr_t)wait_ptr, waits,
	    sizeof(*waits) * count);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync wait copyin failed count=%u err=%d\n",
		    count, err);
		err = -EFAULT;
		goto out;
	}

	for (uint32_t i = 0; i < count; i++) {
		struct dma_fence *fence;
		uint32_t type = waits[i].flags & DRM_NOUVEAU_SYNC_TYPE_MASK;

		if (type != DRM_NOUVEAU_SYNC_SYNCOBJ &&
		    type != DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync wait unsupported flags idx=%u flags=0x%08x\n",
			    i, waits[i].flags);
			err = -EINVAL;
			break;
		}
		err = drm_syncobj_find_fence(file_priv, waits[i].handle,
		    type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ ?
		    waits[i].timeline_value : 0, &fence);
		if (err != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync wait missing fence idx=%u handle=%u err=%d\n",
			    i, waits[i].handle, err);
			break;
		}
		if (fence->ops == &nvkm_drm_fence_ops)
			sc->sync_wait_local_count++;
		else
			sc->sync_wait_external_count++;
		if (dma_fence_is_signaled(fence)) {
			sc->sync_wait_already_signaled_count++;
			dma_fence_put(fence);
			continue;
		}
		fences[retained++] = fence;
	}

out:
	kfree(waits);
	if (err != 0) {
		nvkm_drm_wait_fences_put(fences, retained);
		sc->sync_wait_error_count++;
		*pfences = NULL;
		*pretained_count = 0;
		return (err);
	}
	if (retained == 0) {
		kfree(fences);
		fences = NULL;
	}
	*pfences = fences;
	*pretained_count = retained;
	return (0);
}

static void
nvkm_drm_exec_signals_put(struct nvkm_drm_exec_signal *signals,
    uint32_t count)
{
	if (signals == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		dma_fence_put(signals[i].fence);
		if (signals[i].chain != NULL)
			dma_fence_chain_free(signals[i].chain);
		if (signals[i].syncobj != NULL)
			drm_syncobj_put(signals[i].syncobj);
	}
	kfree(signals);
}

/*
 * nvkm_drm_exec_signals_publish()
 *
 * Ownership:
 *   Consumes each prepared syncobj reference stored in signals.  For timeline
 *   syncobjs it also transfers ownership of the prepared dma_fence_chain node
 *   to drm_syncobj_add_point().  The per-signal fence reference remains owned
 *   by signals and is released by nvkm_drm_exec_signals_put().
 *
 * Lifetime:
 *   Must be called after the job done fence exists and before the job can be
 *   observed by the worker queue.  After this returns, userspace may wait on
 *   the published syncobj while the job object still owns the fence reference
 *   needed to signal completion.
 *
 * Threading:
 *   Called under nfile->job_submit_lock so out-sync publication is ordered with
 *   reservation-fence publication and per-file job enqueue.
 */
static void
nvkm_drm_exec_signals_publish(struct nvkm_drm_exec_signal *signals,
    uint32_t count)
{
	if (signals == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		if (signals[i].syncobj == NULL)
			continue;
		if (signals[i].type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			drm_syncobj_add_point(signals[i].syncobj,
			    signals[i].chain, dma_fence_get(signals[i].fence),
			    signals[i].timeline_value);
			signals[i].chain = NULL;
		} else {
			drm_syncobj_replace_fence(signals[i].syncobj, 0,
			    signals[i].fence);
		}
		drm_syncobj_put(signals[i].syncobj);
		signals[i].syncobj = NULL;
	}
}

static void
nvkm_drm_fence_set_error(struct dma_fence *fence, int error)
{
	if (fence == NULL || error == 0)
		return;
	fence->error = error;
}

static void
nvkm_drm_exec_signals_signal(struct nvkm_drm_exec_signal *signals,
    uint32_t count, int error)
{
	if (signals == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		if (error != 0)
			nvkm_drm_fence_set_error(signals[i].fence, error);
		nvkm_drm_exec_fence_note_signal(signals[i].fence,
		    NVKM_DRM_FENCE_SIGNAL_JOB, error);
		(void)dma_fence_signal(signals[i].fence);
	}
}

static void
nvkm_drm_probe_pending_signal(struct nvkm_softc *sc,
    const struct nvkm_drm_exec_pending *pending, int error)
{
	if (sc == NULL || pending == NULL)
		return;

	sc->exec_pending_signal_count++;
	if (error != 0)
		sc->exec_pending_signal_error_count++;
	sc->exec_pending_signal_fence_count += pending->fence_count;
	for (uint32_t i = 0; i < pending->fence_count; i++) {
		if (nvkm_drm_fence_flag_signaled(pending->fences[i]))
			sc->exec_pending_signal_already_signaled_count++;
		if (nvkm_drm_exec_fence_hw_ready(pending->fences[i]))
			sc->exec_pending_signal_hw_ready_count++;
	}
}

static void
nvkm_drm_exec_pending_signal(struct nvkm_drm_exec_pending *pending, int error)
{
	for (uint32_t i = 0; i < pending->fence_count; i++) {
		if (error != 0)
			nvkm_drm_fence_set_error(pending->fences[i], error);
		nvkm_drm_exec_fence_note_signal(pending->fences[i],
		    NVKM_DRM_FENCE_SIGNAL_PENDING, error);
		(void)dma_fence_signal(pending->fences[i]);
	}
}

static void
nvkm_drm_exec_fence_arm(struct dma_fence *fence, volatile uint32_t *sema,
    uint32_t payload, const struct nvkm_drm_exec_pending *pending)
{
	struct nvkm_drm_exec_fence *f;

	if (fence == NULL || fence->ops != &nvkm_drm_fence_ops)
		return;

	f = container_of(fence, struct nvkm_drm_exec_fence, base);
	if (pending != NULL && pending->chan != NULL) {
		nvkm_drm_exec_fence_set_producer(fence, f->producer_type,
		    f->producer_channel, pending->chan->chid,
		    pending->post_slot, pending->payload,
		    pending->trace_seq);
	}
	f->payload = payload;
	cpu_mfence();
	f->sema = sema;
}

static void
nvkm_drm_exec_pending_arm_fences(struct nvkm_drm_exec_pending *pending)
{
	for (uint32_t i = 0; i < pending->fence_count; i++)
		nvkm_drm_exec_fence_arm(pending->fences[i], pending->sema,
		    pending->payload, pending);
}

static int
nvkm_drm_submit_slot_alloc(struct nvkm_gsp_chan *chan, uint32_t *slot)
{
	for (uint32_t i = 0; i < NVKM_DRM_POST_RING_SLOTS; i++) {
		uint32_t candidate = (chan->submit_post_slot + i) %
		    NVKM_DRM_POST_RING_SLOTS;
		uint64_t bit = 1ULL << candidate;

		if ((chan->submit_post_slots_busy & bit) != 0)
			continue;

		chan->submit_post_slots_busy |= bit;
		chan->submit_post_slot = (candidate + 1) %
		    NVKM_DRM_POST_RING_SLOTS;
		*slot = candidate;
		return (0);
	}
	return (-EAGAIN);
}

static void
nvkm_drm_submit_slot_release(struct nvkm_gsp_chan *chan, uint32_t slot)
{
	chan->submit_post_slots_busy &= ~(1ULL << slot);
	wakeup(chan);
}

static uint32_t
nvkm_drm_gpfifo_required(uint32_t put, uint32_t push_count)
{
	uint32_t required = 0;

	for (uint32_t i = 0; i < push_count; i++) {
		if ((put & (NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)) ==
		    NVKM_DRM_GPFIFO_FETCH_WINDOW - 1) {
			required++;
			put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
		}
		required++;
		put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	}
	if ((put & (NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)) ==
	    NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)
		required++;
	required++;
	return (required);
}

static uint32_t
nvkm_drm_gpfifo_free(uint32_t get, uint32_t put)
{
	return ((get - put - 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1));
}

static int
nvkm_drm_gpfifo_wait_space(struct nvkm_softc *sc, struct nvkm_gsp_chan *chan,
    uint64_t slot_bar1, uint32_t push_count, uint32_t *put,
    uint32_t *required_out)
{
	uint32_t required;
	int timeout_ticks = 5 * hz;
	int wait_ticks = hz / 100;

	if (wait_ticks < 1)
		wait_ticks = 1;

	*put = chan->gpf_put & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	required = nvkm_drm_gpfifo_required(*put, push_count);
	if (required >= NVKM_DRM_GPFIFO_ENTRIES)
		return (-EINVAL);
	if (required_out != NULL)
		*required_out = required;
	if (chan->gpf_free >= required)
		return (0);

	bool mismatch_logged = false;

	while (timeout_ticks > 0) {
		uint32_t get;
		uint32_t free;
		uint32_t put_rb;

		/*
		 * Match Linux nvif_chan_gpfifo_wait(): cache the free count
		 * and only refresh GP_GET when the software count is exhausted.
		 * GP_PUT is written only by this driver; the shadow value is
		 * authoritative, and USERD GP_PUT readback is debug-only because
		 * it can be stale during channel bring-up.
		 */
		if (nvkm_debug != 0) {
			put_rb = nvkm_gsp_bar1_rd32(sc,
			    slot_bar1 + NV_USERD_GP_PUT) &
			    (NVKM_DRM_GPFIFO_ENTRIES - 1);
			if (put_rb != *put && !mismatch_logged) {
				mismatch_logged = true;
				nvkm_infof(sc->dev,
				    "EXEC GP_PUT readback mismatch chid=%d readback=%u shadow=%u\n",
				    chan->chid, put_rb, *put);
			}
		}
		get = nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET) &
		    (NVKM_DRM_GPFIFO_ENTRIES - 1);
		free = nvkm_drm_gpfifo_free(get, *put);
		chan->gpf_free = free;
		if (free >= required)
			return (0);

		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC waits for GPFIFO space chid=%d get=%u put=%u free=%u required=%u\n",
		    chan->chid, get, *put, free, required);
		lwkt_reltoken(&sc->gsp_tok);
		(void)tsleep(chan, 0, "nvkgpf", wait_ticks);
		lwkt_gettoken(&sc->gsp_tok);
		timeout_ticks -= wait_ticks;
	}

	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC GPFIFO space timeout chid=%d put=%u required=%u\n",
	    chan->chid, *put, nvkm_drm_gpfifo_required(*put, push_count));
	return (-ETIME);
}

static struct nvkm_drm_exec_pending *
nvkm_drm_exec_pending_create(struct nvkm_gsp_chan *chan,
    volatile uint32_t *sema, uint32_t payload, uint32_t post_slot,
    uint64_t trace_seq, uint32_t trace_first, uint32_t trace_count,
    struct nvkm_drm_exec_signal *signals, uint32_t sig_count,
    struct dma_fence *exec_fence)
{
	struct nvkm_drm_exec_pending *pending;

	pending = kzalloc(sizeof(*pending), GFP_KERNEL);
	if (pending == NULL)
		return (NULL);

	pending->chan = chan;
	pending->sema = sema;
	pending->payload = payload;
	pending->post_slot = post_slot;
	pending->trace_seq = trace_seq;
	pending->trace_first = trace_first;
	pending->trace_count = trace_count;
	(void)signals;
	(void)sig_count;
	if (exec_fence == NULL) {
		kfree(pending);
		return (NULL);
	}
	pending->fence_count = 1;
	pending->fences[0] = dma_fence_get(exec_fence);
	return (pending);
}

static void
nvkm_drm_exec_pending_put(struct nvkm_softc *sc,
    struct nvkm_drm_exec_pending *pending)
{
	if (pending == NULL)
		return;
	for (uint32_t i = 0; i < pending->fence_count; i++)
		dma_fence_put(pending->fences[i]);
	kfree(pending);
}

static void
nvkm_drm_exec_pending_cancel_channel(struct nvkm_softc *sc,
    struct nvkm_gsp_chan *chan, int error)
{
	struct nvkm_drm_exec_pending *pending, *next;

	for (pending = LIST_FIRST(&sc->exec_pending); pending != NULL;
	    pending = next) {
		next = LIST_NEXT(pending, link);
		if (pending->chan != chan)
			continue;

		LIST_REMOVE(pending, link);
		nvkm_drm_exec_trace_complete(sc, pending, error);
		nvkm_drm_probe_pending_signal(sc, pending, error);
		nvkm_drm_exec_pending_signal(pending, error);
		atomic_store_rel_int(&pending->done, 1);
		wakeup(pending);
		nvkm_drm_submit_slot_release(chan, pending->post_slot);
		nvkm_drm_exec_pending_put(sc, pending);
	}
}

#define NVKM_DRM_FAULT_DATA_SCAN_MAX_SIZE	(16ULL * 1024ULL * 1024ULL)

static void
nvkm_drm_fault_scan_pushes(struct nvkm_softc *sc,
    struct nvkm_drm_exec_pending *pending, uint64_t fault_addr)
{
	uint32_t fault_lo = (uint32_t)fault_addr;
	uint32_t fault_hi = (uint32_t)(fault_addr >> 32);

	if (pending->nfile == NULL)
		return;

	for (uint32_t n = 0; n < pending->trace_count; n++) {
		struct nvkm_drm_exec_trace *trace;
		struct nvkm_drm_vm_binding *binding = NULL;
		struct nvkm_bo *bo;
		uint64_t binding_offset = 0;
		uint64_t offset;
		uint32_t count;

		trace = &sc->exec_trace[(pending->trace_first + n) %
		    NVKM_DRM_EXEC_TRACE_COUNT];
		if (trace->seq == 0)
			continue;
		LIST_FOREACH(binding, &pending->nfile->vm_bindings, link) {
			if (!nvkm_drm_gpu_va_fits_binding(binding, trace->va,
			    trace->va_len, &binding_offset))
				continue;
			break;
		}
		if (binding == NULL)
			continue;

		bo = to_nvkm_bo(binding->obj);
		if (!nvkm_bo_has_sysmem(bo))
			continue;
		if (binding->bo_offset > bo->base.size ||
		    binding_offset > bo->base.size - binding->bo_offset ||
		    trace->va_len >
		    bo->base.size - binding->bo_offset - binding_offset)
			continue;

		offset = binding->bo_offset + binding_offset;
		count = trace->va_len / sizeof(uint32_t);
		sc->rc_fault_push_scan_count++;
		for (uint32_t i = 0; i + 1 < count; i++) {
			uint32_t lo;
			uint32_t hi;
			uint64_t value;

			if (nvkm_bo_read32(bo, offset +
			    (uint64_t)i * sizeof(uint32_t), &lo) != 0 ||
			    nvkm_bo_read32(bo, offset +
			    (uint64_t)(i + 1) * sizeof(uint32_t), &hi) != 0)
				break;
			value = (uint64_t)lo | ((uint64_t)hi << 32);

			if (value != fault_addr &&
			    !(lo == fault_lo && (hi & 0xffu) == fault_hi))
				continue;
			sc->rc_fault_push_hit_count++;
			if (sc->rc_fault_push_hit_seq == 0) {
				sc->rc_fault_push_hit_seq = trace->seq;
				sc->rc_fault_push_hit_va = trace->va;
				sc->rc_fault_push_hit_dword = i;
			}
		}
	}
}

void
nvkm_drm_exec_fault_channel_locked(struct nvkm_softc *sc, uint32_t chid,
    int error, uint64_t fault_addr)
{
	struct nvkm_drm_exec_pending *pending, *next;

	sc->rc_fault_pending_count = 0;
	sc->rc_fault_binding_count = 0;
	sc->rc_fault_binding_addr = 0;
	sc->rc_fault_binding_size = 0;
	sc->rc_fault_binding_grefcnt = 0;
	sc->rc_fault_nearest_lo_addr = 0;
	sc->rc_fault_nearest_lo_size = 0;
	sc->rc_fault_nearest_hi_addr = 0;
	sc->rc_fault_nearest_hi_size = 0;
	sc->rc_fault_push_scan_count = 0;
	sc->rc_fault_push_hit_count = 0;
	sc->rc_fault_push_hit_seq = 0;
	sc->rc_fault_push_hit_va = 0;
	sc->rc_fault_push_hit_dword = 0;
	sc->rc_fault_data_scan_count = 0;
	sc->rc_fault_data_hit_count = 0;
	sc->rc_fault_data_hit_addr = 0;
	sc->rc_fault_data_hit_size = 0;
	sc->rc_fault_data_hit_offset = 0;
	sc->rc_fault_data_hit_value = 0;

	for (pending = LIST_FIRST(&sc->exec_pending); pending != NULL;
	    pending = next) {
		next = LIST_NEXT(pending, link);
		if (pending->chan == NULL ||
		    (uint32_t)pending->chan->chid != chid)
			continue;

		sc->rc_fault_pending_count++;
		nvkm_drm_fault_scan_pushes(sc, pending, fault_addr);

		pending->chan->faulted = 1;
		pending->chan->fault_error = error;
		LIST_REMOVE(pending, link);
		nvkm_drm_exec_trace_complete(sc, pending, error);
		nvkm_drm_probe_pending_signal(sc, pending, error);
		nvkm_drm_exec_pending_signal(pending, error);
		sc->exec_async_wait_error_count++;
		atomic_store_rel_int(&pending->done, 1);
		wakeup(pending);
		nvkm_drm_submit_slot_release(pending->chan, pending->post_slot);
		nvkm_drm_exec_pending_put(sc, pending);
	}
}

void
nvkm_drm_exec_complete_intr(struct nvkm_softc *sc)
{
	struct nvkm_drm_exec_pending *pending, *next;

	lwkt_gettoken(&sc->gsp_tok);
	for (pending = LIST_FIRST(&sc->exec_pending); pending != NULL;
	    pending = next) {
		next = LIST_NEXT(pending, link);
		if (*(pending->sema) != pending->payload)
			continue;

		LIST_REMOVE(pending, link);
		nvkm_drm_exec_trace_complete(sc, pending, 0);
		nvkm_drm_probe_pending_signal(sc, pending, 0);
		nvkm_drm_exec_pending_signal(pending, 0);
		sc->exec_async_complete_count++;
		atomic_store_rel_int(&pending->done, 1);
		wakeup(pending);
		nvkm_drm_submit_slot_release(pending->chan, pending->post_slot);
		nvkm_drm_exec_pending_put(sc, pending);
	}
	lwkt_reltoken(&sc->gsp_tok);
}

static int
nvkm_drm_prepare_signal_syncobjs(struct nvkm_softc *sc,
    struct drm_file *file_priv, uint32_t count, uint64_t sig_ptr,
    struct dma_fence *done_fence, struct nvkm_drm_exec_signal **psignals)
{
	struct drm_nouveau_sync *sigs;
	struct nvkm_drm_exec_signal *signals;
	int err = 0;

	*psignals = NULL;
	if (count == 0)
		return (0);
	if (done_fence == NULL)
		return (-EINVAL);
	sc->sync_signal_count += count;
	if (count > 64) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal invalid count=%u\n", count);
		return (-EINVAL);
	}

	sigs = kmalloc_array(count, sizeof(*sigs), GFP_KERNEL);
	signals = kcalloc(count, sizeof(*signals), GFP_KERNEL);
	if (sigs == NULL || signals == NULL) {
		err = -ENOMEM;
		goto out_free_arrays;
	}

	err = copyin((const void *)(uintptr_t)sig_ptr, sigs,
	    sizeof(*sigs) * count);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal copyin failed count=%u err=%d\n",
		    count, err);
		err = -EFAULT;
		goto out_free_arrays;
	}

	for (uint32_t i = 0; i < count; i++) {
		uint32_t type = sigs[i].flags & DRM_NOUVEAU_SYNC_TYPE_MASK;

		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal prepare idx=%u flags=0x%08x handle=%u timeline=0x%016jx\n",
		    i, sigs[i].flags, sigs[i].handle,
		    (uintmax_t)sigs[i].timeline_value);
		if (type != DRM_NOUVEAU_SYNC_SYNCOBJ &&
		    type != DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync signal unsupported flags idx=%u flags=0x%08x\n",
			    i, sigs[i].flags);
			err = -EINVAL;
			goto out_free_arrays;
		}
		signals[i].syncobj = drm_syncobj_find(file_priv,
		    sigs[i].handle);
		if (signals[i].syncobj == NULL) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync signal missing syncobj idx=%u handle=%u\n",
			    i, sigs[i].handle);
			err = -ENOENT;
			goto out_free_arrays;
		}
		signals[i].type = type;
		signals[i].timeline_value = sigs[i].timeline_value;
		signals[i].fence = dma_fence_get(done_fence);
		if (type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			signals[i].chain = dma_fence_chain_alloc();
			if (signals[i].chain == NULL) {
				err = -ENOMEM;
				goto out_free_arrays;
			}
		}
	}

	*psignals = signals;
	signals = NULL;

out_free_arrays:
	nvkm_drm_exec_signals_put(signals, count);
	kfree(sigs);
	if (err != 0) {
		sc->sync_signal_error_count++;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal prepare failed count=%u err=%d\n",
		    count, err);
	}
	return (err);
}

enum nvkm_drm_job_type {
	NVKM_DRM_JOB_EXEC,
	NVKM_DRM_JOB_VM_BIND,
};

struct nvkm_drm_job_dep {
	struct dma_fence_cb cb;
	struct nvkm_drm_job *job;
	struct dma_fence *fence;
	bool armed;
};

struct nvkm_drm_job {
	TAILQ_ENTRY(nvkm_drm_job) link;
	struct kref refcount;
	enum nvkm_drm_job_type type;
	struct nvkm_softc *sc;
	struct drm_file *file_priv;
	struct nvkm_drm_file *nfile;
	struct dma_fence *done_fence;
	struct dma_fence **wait_fences;
	struct nvkm_drm_job_dep *wait_deps;
	int result;
	uint32_t wait_count;
	uint32_t dep_pending;
	bool completed;
	bool deps_armed;
	bool queued;
	bool running;
	union {
		struct {
			uint32_t channel;
			uint32_t push_count;
			uint32_t sig_count;
			struct drm_nouveau_exec_push *pushes;
			struct nvkm_drm_exec_signal *signals;
		} exec;
		struct {
			uint32_t op_count;
			uint32_t sig_count;
			struct drm_nouveau_vm_bind_op *ops;
			struct drm_gem_object **objects;
			struct nvkm_drm_exec_signal *signals;
			struct nvkm_drm_vm_bind_retire *retire;
		} vm_bind;
	};
};

static int nvkm_drm_exec_submit(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    uint32_t channel_id, struct drm_nouveau_exec_push *pushes,
    uint32_t push_count, struct dma_fence *done_fence,
    struct nvkm_drm_exec_signal *signals, uint32_t sig_count);

#define NVKM_DRM_JOB_DIAG_ARM_DEPS	1
#define NVKM_DRM_JOB_DIAG_WAIT_DEPS	2
#define NVKM_DRM_JOB_DIAG_RUN		3
#define NVKM_DRM_JOB_DIAG_COMPLETE	4

#define NVKM_DRM_JOB_DEP_NONE		0
#define NVKM_DRM_JOB_DEP_NOUVEAU	1
#define NVKM_DRM_JOB_DEP_CHAIN		2
#define NVKM_DRM_JOB_DEP_ARRAY		3
#define NVKM_DRM_JOB_DEP_SYNCOBJ_STUB	4
#define NVKM_DRM_JOB_DEP_OTHER		5

#define NVKM_DRM_EXEC_DIAG_VALIDATE	1
#define NVKM_DRM_EXEC_DIAG_VM_TOKEN	2
#define NVKM_DRM_EXEC_DIAG_GSP_TOKEN	3
#define NVKM_DRM_EXEC_DIAG_SLOT_ALLOC	4
#define NVKM_DRM_EXEC_DIAG_GPFIFO_WAIT	5
#define NVKM_DRM_EXEC_DIAG_BUILD_PUSH	6
#define NVKM_DRM_EXEC_DIAG_FENCE	7
#define NVKM_DRM_EXEC_DIAG_QUEUE	8
#define NVKM_DRM_EXEC_DIAG_DOORBELL	9
#define NVKM_DRM_EXEC_DIAG_CLEANUP	10

/*
 * Ownership:
 *   The diagnostic snapshot is owned by nvkm_softc.  Job and EXEC callers lend
 *   metadata by value only; the helpers do not retain job, channel, BO, or
 *   fence references.
 *
 * Lifetime:
 *   Fields live with the device softc and remain readable after a userspace
 *   client wedges or exits.  They do not point into userspace memory.
 *
 * Threading:
 *   Updates are lockless best-effort telemetry.  The helpers intentionally do
 *   not take job_token, vm_token, gsp_tok, or reservation locks, so they cannot
 *   add a new lock dependency to the sync path being diagnosed.
 */
static uint64_t
nvkm_drm_job_diag_begin(struct nvkm_softc *sc,
    const struct nvkm_drm_job *job, uint32_t stage, uint64_t *start_us)
{
	uint64_t seq;

	if (sc == NULL || sc->sync_diag_enable == 0) {
		*start_us = 0;
		return (0);
	}

	*start_us = nvkm_drm_profile_now_us(sc);
	seq = ++sc->job_diag_enter_count;
	sc->job_diag_active_seq = seq;
	sc->job_diag_active_start_us = *start_us;
	sc->job_diag_active_stage = stage;
	sc->job_diag_active_type = job != NULL ? job->type : 0;
	sc->job_diag_active_channel =
	    job != NULL && job->type == NVKM_DRM_JOB_EXEC ?
	    job->exec.channel : 0;
	sc->job_diag_active_wait_count = job != NULL ? job->wait_count : 0;
	sc->job_diag_active_dep_pending =
	    job != NULL ? job->dep_pending : 0;
	sc->job_diag_active_sig_count =
	    job != NULL && job->type == NVKM_DRM_JOB_EXEC ?
	    job->exec.sig_count :
	    (job != NULL && job->type == NVKM_DRM_JOB_VM_BIND ?
	     job->vm_bind.sig_count : 0);
	sc->job_diag_active_dep_index = 0;
	sc->job_diag_active_dep_type = NVKM_DRM_JOB_DEP_NONE;
	sc->job_diag_active_dep_signaled = 0;
	sc->job_diag_active_dep_hw_ready = 0;
	sc->job_diag_active_dep_context = 0;
	sc->job_diag_active_dep_seqno = 0;
	sc->job_diag_active_dep_flags = 0;
	sc->job_diag_active_dep_error = 0;
	sc->job_diag_active_dep_chain_point = 0;
	sc->job_diag_active_dep_chain_prev_seqno = 0;
	sc->job_diag_active_dep_array_count = 0;
	sc->job_diag_active_dep_array_pending = 0;
	sc->job_diag_active_dep_producer_type = 0;
	sc->job_diag_active_dep_producer_channel = 0;
	sc->job_diag_active_dep_producer_chid = -1;
	sc->job_diag_active_dep_producer_post_slot = 0;
	sc->job_diag_active_dep_producer_payload = 0;
	sc->job_diag_active_dep_producer_submit_count = 0;
	sc->job_diag_active_dep_signal_source = 0;
	sc->job_diag_active_dep_signal_count = 0;
	sc->job_diag_active_dep_signal_error = 0;
	sc->job_diag_last_stage = stage;
	return (seq);
}

static uint32_t
nvkm_drm_job_diag_fence_type(const struct dma_fence *fence)
{
	const char *driver_name;

	if (fence == NULL)
		return (NVKM_DRM_JOB_DEP_NONE);
	if (fence->ops == &nvkm_drm_fence_ops)
		return (NVKM_DRM_JOB_DEP_NOUVEAU);
	if (fence->ops == &dma_fence_chain_ops)
		return (NVKM_DRM_JOB_DEP_CHAIN);
	if (fence->ops == &dma_fence_array_ops)
		return (NVKM_DRM_JOB_DEP_ARRAY);
	if (fence->ops != NULL && fence->ops->get_driver_name != NULL) {
		driver_name = fence->ops->get_driver_name((struct dma_fence *)fence);
		if (driver_name != NULL && strcmp(driver_name, "syncobjstub") == 0)
			return (NVKM_DRM_JOB_DEP_SYNCOBJ_STUB);
	}
	return (NVKM_DRM_JOB_DEP_OTHER);
}

/*
 * nvkm_drm_job_diag_sample_dep()
 *
 * Ownership:
 *   Borrows fence and job while the caller already owns the wait-dependency
 *   reference.  The diagnostic state stores only scalar snapshots and never
 *   keeps a fence pointer or reference.
 *
 * Lifetime:
 *   The sampled values remain readable after the job exits, but they are not a
 *   synchronization contract.  A later signal can make the sampled flags stale.
 *
 * Threading:
 *   Best-effort telemetry.  This helper intentionally avoids
 *   dma_fence_is_signaled(), because that helper may call the fence signal path
 *   through ops->signaled; diagnostics must not alter fence state.
 */
static void
nvkm_drm_job_diag_sample_dep(struct nvkm_softc *sc,
    const struct nvkm_drm_job *job, uint32_t index, struct dma_fence *fence)
{
	struct dma_fence_chain *chain;
	struct dma_fence_array *array;
	struct nvkm_drm_exec_fence *nfence;
	uint32_t type;

	if (sc == NULL || sc->sync_diag_enable == 0 || job == NULL ||
	    fence == NULL)
		return;

	type = nvkm_drm_job_diag_fence_type(fence);
	sc->job_diag_active_dep_index = index;
	sc->job_diag_active_dep_type = type;
	sc->job_diag_active_dep_signaled =
	    nvkm_drm_fence_flag_signaled(fence) ? 1 : 0;
	sc->job_diag_active_dep_hw_ready =
	    nvkm_drm_exec_fence_hw_ready(fence) ? 1 : 0;
	sc->job_diag_active_dep_context = fence->context;
	sc->job_diag_active_dep_seqno = fence->seqno;
	sc->job_diag_active_dep_flags = fence->flags;
	sc->job_diag_active_dep_error = fence->error;
	sc->job_diag_active_dep_chain_point = 0;
	sc->job_diag_active_dep_chain_prev_seqno = 0;
	sc->job_diag_active_dep_array_count = 0;
	sc->job_diag_active_dep_array_pending = 0;
	sc->job_diag_active_dep_producer_type = 0;
	sc->job_diag_active_dep_producer_channel = 0;
	sc->job_diag_active_dep_producer_chid = -1;
	sc->job_diag_active_dep_producer_post_slot = 0;
	sc->job_diag_active_dep_producer_payload = 0;
	sc->job_diag_active_dep_producer_submit_count = 0;
	sc->job_diag_active_dep_signal_source = 0;
	sc->job_diag_active_dep_signal_count = 0;
	sc->job_diag_active_dep_signal_error = 0;

	if (type == NVKM_DRM_JOB_DEP_CHAIN) {
		chain = to_dma_fence_chain(fence);
		if (chain != NULL) {
			sc->job_diag_active_dep_chain_point = chain->point;
			sc->job_diag_active_dep_chain_prev_seqno =
			    chain->prev_seqno;
		}
	} else if (type == NVKM_DRM_JOB_DEP_ARRAY) {
		array = to_dma_fence_array(fence);
		if (array != NULL) {
			sc->job_diag_active_dep_array_count =
			    array->num_fences;
			sc->job_diag_active_dep_array_pending =
			    atomic_read(&array->num_pending);
		}
	} else if (type == NVKM_DRM_JOB_DEP_NOUVEAU) {
		nfence = container_of(fence, struct nvkm_drm_exec_fence,
		    base);
		sc->job_diag_active_dep_producer_type =
		    nfence->producer_type;
		sc->job_diag_active_dep_producer_channel =
		    nfence->producer_channel;
		sc->job_diag_active_dep_producer_chid =
		    nfence->producer_chid;
		sc->job_diag_active_dep_producer_post_slot =
		    nfence->producer_post_slot;
		sc->job_diag_active_dep_producer_payload =
		    nfence->producer_payload;
		sc->job_diag_active_dep_producer_submit_count =
		    nfence->producer_submit_count;
		sc->job_diag_active_dep_signal_source =
		    nfence->signal_source;
		sc->job_diag_active_dep_signal_count =
		    nfence->signal_count;
		sc->job_diag_active_dep_signal_error =
		    nfence->signal_error;
	}
}

static void
nvkm_drm_job_diag_stage(struct nvkm_softc *sc, uint64_t seq,
    const struct nvkm_drm_job *job, uint32_t stage)
{
	if (sc == NULL || sc->sync_diag_enable == 0 || seq == 0)
		return;
	if (sc->job_diag_active_seq != seq)
		return;
	sc->job_diag_active_stage = stage;
	sc->job_diag_last_stage = stage;
	sc->job_diag_active_dep_pending =
	    job != NULL ? job->dep_pending : 0;
	if (stage == NVKM_DRM_JOB_DIAG_WAIT_DEPS && job != NULL) {
		for (uint32_t i = 0; i < job->wait_count; i++) {
			if (!job->wait_deps[i].armed)
				continue;
			nvkm_drm_job_diag_sample_dep(sc, job, i,
			    job->wait_deps[i].fence);
			break;
		}
	}
}

static void
nvkm_drm_job_diag_finish(struct nvkm_softc *sc, uint64_t seq, int ret,
    uint64_t start_us)
{
	uint64_t end_us, elapsed_us;

	if (sc == NULL || seq == 0)
		return;

	end_us = nvkm_drm_profile_now_us(sc);
	elapsed_us = end_us >= start_us ? end_us - start_us : 0;
	sc->job_diag_leave_count++;
	sc->job_diag_last_seq = seq;
	sc->job_diag_last_us = elapsed_us;
	sc->job_diag_last_ret = ret;
	if (elapsed_us > sc->job_diag_slow_us_max)
		sc->job_diag_slow_us_max = elapsed_us;
	if (elapsed_us >= 10000)
		sc->job_diag_slow_count++;
	if (sc->job_diag_active_seq == seq)
		sc->job_diag_active_stage = 0;
}

/*
 * nvkm_drm_vm_bind_debug_delay()
 *
 * Ownership:
 *   Borrows sc only.  The sysctl value remains owned by the device softc and is
 *   sampled once at the start of the helper.
 *
 * Lifetime:
 *   Used only by debug probes to widen the window after an async VM_BIND done
 *   fence has been published but before the job can complete.  The knob
 *   defaults to zero and is not part of the nouveau UAPI.
 *
 * Threading:
 *   Called from the per-file job worker without holding job_token, vm_token, or
 *   gsp_tok.  It must not sleep while page-table or reservation locks are held.
 */
static void
nvkm_drm_vm_bind_debug_delay(struct nvkm_softc *sc)
{
	int delay_ms, ticks;

	if (sc == NULL)
		return;
	delay_ms = sc->vm_bind_job_delay_ms;
	if (delay_ms <= 0)
		return;
	if (delay_ms > 5000)
		delay_ms = 5000;
	ticks = (delay_ms * hz + 999) / 1000;
	if (ticks < 1)
		ticks = 1;
	(void)tsleep(&sc->vm_bind_job_delay_ms, 0, "nvkvbd", ticks);
}

static uint64_t
nvkm_drm_exec_diag_begin(struct nvkm_softc *sc, uint32_t channel,
    uint32_t push_count, uint32_t sig_count, uint64_t *start_us)
{
	uint64_t seq;

	if (sc == NULL || sc->sync_diag_enable == 0) {
		*start_us = 0;
		return (0);
	}

	*start_us = nvkm_drm_profile_now_us(sc);
	seq = ++sc->exec_diag_enter_count;
	sc->exec_diag_active_seq = seq;
	sc->exec_diag_active_start_us = *start_us;
	sc->exec_diag_active_stage = NVKM_DRM_EXEC_DIAG_VALIDATE;
	sc->exec_diag_active_channel = channel;
	sc->exec_diag_active_push_count = push_count;
	sc->exec_diag_active_sig_count = sig_count;
	sc->exec_diag_active_put = 0;
	sc->exec_diag_active_slot = 0;
	return (seq);
}

static void
nvkm_drm_exec_diag_stage(struct nvkm_softc *sc, uint64_t seq,
    uint32_t stage, uint32_t put, uint32_t slot)
{
	if (sc == NULL || sc->sync_diag_enable == 0 || seq == 0)
		return;
	if (sc->exec_diag_active_seq != seq)
		return;
	sc->exec_diag_active_stage = stage;
	sc->exec_diag_active_put = put;
	sc->exec_diag_active_slot = slot;
}

static void
nvkm_drm_exec_diag_finish(struct nvkm_softc *sc, uint64_t seq, int ret,
    uint64_t start_us)
{
	uint64_t end_us, elapsed_us;

	if (sc == NULL || seq == 0)
		return;

	end_us = nvkm_drm_profile_now_us(sc);
	elapsed_us = end_us >= start_us ? end_us - start_us : 0;
	sc->exec_diag_leave_count++;
	sc->exec_diag_last_seq = seq;
	sc->exec_diag_last_us = elapsed_us;
	sc->exec_diag_last_ret = ret;
	sc->exec_diag_last_stage = sc->exec_diag_active_stage;
	sc->exec_diag_last_channel = sc->exec_diag_active_channel;
	sc->exec_diag_last_push_count = sc->exec_diag_active_push_count;
	sc->exec_diag_last_sig_count = sc->exec_diag_active_sig_count;
	sc->exec_diag_last_put = sc->exec_diag_active_put;
	sc->exec_diag_last_slot = sc->exec_diag_active_slot;
	if (elapsed_us > sc->exec_diag_slow_us_max)
		sc->exec_diag_slow_us_max = elapsed_us;
	if (elapsed_us >= 10000)
		sc->exec_diag_slow_count++;
	if (sc->exec_diag_active_seq == seq)
		sc->exec_diag_active_stage = 0;
}

static int
nvkm_drm_job_queue_work_locked(struct nvkm_drm_file *nfile)
{
	if (nfile->job_work_queued)
		return (0);
	nfile->job_work_queued = true;
	if (!queue_work(system_wq, &nfile->job_work)) {
		nfile->job_work_queued = false;
		return (-EIO);
	}
	return (0);
}

static void
nvkm_drm_job_wakeup_locked(struct nvkm_drm_file *nfile)
{
	nfile->job_epoch++;
	wakeup(&nfile->job_epoch);
}

static void
nvkm_drm_job_signal(struct nvkm_drm_job *job, int error)
{
	if (job->done_fence != NULL) {
		job->sc->job_done_signal_count++;
		if (error != 0)
			job->sc->job_done_signal_error_count++;
		if (nvkm_drm_fence_flag_signaled(job->done_fence))
			job->sc->job_done_signal_already_signaled_count++;
		if (nvkm_drm_exec_fence_hw_ready(job->done_fence))
			job->sc->job_done_signal_hw_ready_count++;
		if (error != 0)
			nvkm_drm_fence_set_error(job->done_fence, error);
		nvkm_drm_exec_fence_note_signal(job->done_fence,
		    NVKM_DRM_FENCE_SIGNAL_JOB, error);
		(void)dma_fence_signal(job->done_fence);
		return;
	}

	switch (job->type) {
	case NVKM_DRM_JOB_EXEC:
		nvkm_drm_exec_signals_signal(job->exec.signals,
		    job->exec.sig_count, error);
		break;
	case NVKM_DRM_JOB_VM_BIND:
		nvkm_drm_exec_signals_signal(job->vm_bind.signals,
		    job->vm_bind.sig_count, error);
		break;
	}
}

static void
nvkm_drm_job_release(struct kref *kref)
{
	struct nvkm_drm_job *job =
	    container_of(kref, struct nvkm_drm_job, refcount);

	if (job == NULL)
		return;
	dma_fence_put(job->done_fence);
	nvkm_drm_wait_fences_put(job->wait_fences, job->wait_count);
	kfree(job->wait_deps);
	switch (job->type) {
	case NVKM_DRM_JOB_EXEC:
		nvkm_drm_exec_signals_put(job->exec.signals,
		    job->exec.sig_count);
		kfree(job->exec.pushes);
		break;
	case NVKM_DRM_JOB_VM_BIND:
		nvkm_drm_exec_signals_put(job->vm_bind.signals,
		    job->vm_bind.sig_count);
		nvkm_drm_vm_bind_retire_free(job->vm_bind.retire);
		nvkm_drm_vm_bind_objects_put(job->vm_bind.objects,
		    job->vm_bind.op_count);
		kfree(job->vm_bind.ops);
		break;
	}
	kfree(job);
}

static void
nvkm_drm_job_get(struct nvkm_drm_job *job)
{
	kref_get(&job->refcount);
}

static void
nvkm_drm_job_put(struct nvkm_drm_job *job)
{
	if (job != NULL)
		kref_put(&job->refcount, nvkm_drm_job_release);
}

static int
nvkm_drm_job_prepare_deps(struct nvkm_drm_job *job)
{
	if (job->wait_count == 0)
		return (0);
	job->wait_deps = kcalloc(job->wait_count, sizeof(*job->wait_deps),
	    GFP_KERNEL);
	if (job->wait_deps == NULL)
		return (-ENOMEM);
	for (uint32_t i = 0; i < job->wait_count; i++) {
		job->wait_deps[i].job = job;
		job->wait_deps[i].fence = job->wait_fences[i];
	}
	return (0);
}

static void
nvkm_drm_job_dep_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct nvkm_drm_job_dep *dep =
	    container_of(cb, struct nvkm_drm_job_dep, cb);
	struct nvkm_drm_job *job = dep->job;
	struct nvkm_drm_file *nfile = job->nfile;
	struct nvkm_softc *sc = job->sc;
	int err;

	(void)fence;
	sc->sync_job_dep_cb_count++;
	lwkt_gettoken(&nfile->job_token);
	if (dep->armed) {
		dep->armed = false;
		if (job->dep_pending != 0)
			job->dep_pending--;
		if (job->dep_pending == 0 && job->queued &&
		    !nfile->job_closing) {
			err = nvkm_drm_job_queue_work_locked(nfile);
			if (err == 0)
				sc->sync_job_dep_queue_count++;
			else
				sc->sync_job_dep_queue_error_count++;
		}
		nvkm_drm_job_wakeup_locked(nfile);
		wakeup(job);
	}
	lwkt_reltoken(&nfile->job_token);

	nvkm_drm_job_put(job);
}

static int
nvkm_drm_job_arm_deps_locked(struct nvkm_drm_job *job)
{
	struct nvkm_softc *sc = job->sc;
	int err;

	if (job->deps_armed)
		return (0);
	job->deps_armed = true;

	for (uint32_t i = 0; i < job->wait_count; i++) {
		struct nvkm_drm_job_dep *dep = &job->wait_deps[i];
		struct dma_fence *fence = dep->fence;

		if (fence == NULL || dma_fence_is_signaled(fence))
			continue;
		sc->sync_wait_blocking_count++;
		dep->armed = true;
		job->dep_pending++;
		nvkm_drm_job_get(job);
		err = dma_fence_add_callback(fence, &dep->cb,
		    nvkm_drm_job_dep_cb);
		if (err == 0) {
			sc->sync_job_wait_armed_count++;
			continue;
		}

		dep->armed = false;
		if (job->dep_pending != 0)
			job->dep_pending--;
		nvkm_drm_job_put(job);
		if (err == -ENOENT)
			continue;
		sc->sync_wait_error_count++;
		return (err);
	}

	return (0);
}

static void
nvkm_drm_job_disarm_deps_locked(struct nvkm_drm_job *job)
{
	for (uint32_t i = 0; i < job->wait_count; i++) {
		struct nvkm_drm_job_dep *dep = &job->wait_deps[i];

		if (!dep->armed)
			continue;
		if (dma_fence_remove_callback(dep->fence, &dep->cb)) {
			dep->armed = false;
			if (job->dep_pending != 0)
				job->dep_pending--;
			nvkm_drm_job_put(job);
		}
	}
}

static bool
nvkm_drm_job_has_armed_deps_locked(struct nvkm_drm_job *job)
{
	for (uint32_t i = 0; i < job->wait_count; i++) {
		if (job->wait_deps[i].armed)
			return (true);
	}
	return (false);
}

static void
nvkm_drm_job_wait_deps_disarmed(struct nvkm_drm_job *job)
{
	struct nvkm_drm_file *nfile = job->nfile;

	for (;;) {
		lwkt_gettoken(&nfile->job_token);
		if (!nvkm_drm_job_has_armed_deps_locked(job)) {
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		lwkt_reltoken(&nfile->job_token);
		(void)tsleep(job, 0, "nvkjcb", hz / 10);
	}
}

static int
nvkm_drm_job_enqueue(struct nvkm_drm_job *job)
{
	struct nvkm_drm_file *nfile = job->nfile;
	int err = 0;

	lwkt_gettoken(&nfile->job_token);
	if (nfile->job_closing) {
		err = -ENODEV;
	} else {
		TAILQ_INSERT_TAIL(&nfile->job_queue, job, link);
		job->queued = true;
		err = nvkm_drm_job_queue_work_locked(nfile);
		if (err != 0) {
			job->queued = false;
			TAILQ_REMOVE(&nfile->job_queue, job, link);
		}
	}
	lwkt_reltoken(&nfile->job_token);
	return (err);
}

/*
 * nvkm_drm_job_wait_complete()
 *
 * Ownership:
 *   Borrows job and the caller's synchronous-submit reference.  The queued job
 *   keeps its own reference and may continue running after this helper returns
 *   -EINTR.
 *
 * Lifetime:
 *   Used by synchronous VM_BIND ioctls after their job has been published.  If a
 *   signal interrupts the wait, userspace is allowed to leave the ioctl while
 *   postclose or the normal worker path later completes, cancels, and releases
 *   the job.
 *
 * Threading:
 *   Sleeps without holding job_token.  The sleep is interruptible so Xorg or a
 *   compositor cannot become an unkillable D-state process while a dependency
 *   fence is delayed or lost.
 */
static int
nvkm_drm_job_wait_complete(struct nvkm_drm_job *job)
{
	struct nvkm_drm_file *nfile = job->nfile;
	int result;
	int sleep_error;

	for (;;) {
		lwkt_gettoken(&nfile->job_token);
		if (job->completed) {
			result = job->result;
			lwkt_reltoken(&nfile->job_token);
			return (result);
		}
		if (!job->running && !nfile->job_work_queued &&
			    (!job->queued || job->dep_pending == 0))
				(void)nvkm_drm_job_queue_work_locked(nfile);
		lwkt_reltoken(&nfile->job_token);
		sleep_error = tsleep(job, PCATCH, "nvkjsy", hz / 10);
		if (sleep_error == EINTR || sleep_error == ERESTART)
			return (-EINTR);
	}
}

static int
nvkm_drm_job_run(struct nvkm_drm_job *job)
{
	int err;

	switch (job->type) {
	case NVKM_DRM_JOB_EXEC:
		err = nvkm_drm_exec_submit(job->sc, job->file_priv,
		    job->nfile, job->exec.channel, job->exec.pushes,
		    job->exec.push_count, job->done_fence, job->exec.signals,
		    job->exec.sig_count);
		break;
	case NVKM_DRM_JOB_VM_BIND:
		if (job->vm_bind.op_count != 0) {
			err = nvkm_drm_vm_bind_apply(job->sc, job->file_priv,
			    job->nfile, job->vm_bind.ops,
			    job->vm_bind.op_count, job->vm_bind.objects,
			    &job->vm_bind.retire->bindings);
		} else {
			err = 0;
		}
		nvkm_drm_job_signal(job, err);
		nvkm_drm_vm_bind_retire_schedule(&job->vm_bind.retire);
		break;
	}
	return (err);
}

static void
nvkm_drm_job_work(struct work_struct *work)
{
	struct nvkm_drm_file *nfile =
	    container_of(work, struct nvkm_drm_file, job_work);
	struct nvkm_drm_job *job;
	uint64_t diag_seq, diag_start;
	int err;

	for (;;) {
		lwkt_gettoken(&nfile->job_token);
		if (nfile->job_closing) {
			nfile->job_work_queued = false;
			nvkm_drm_job_wakeup_locked(nfile);
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		job = TAILQ_FIRST(&nfile->job_queue);
		if (job == NULL) {
			nfile->job_work_queued = false;
			nvkm_drm_job_wakeup_locked(nfile);
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		diag_seq = nvkm_drm_job_diag_begin(job->sc, job,
		    NVKM_DRM_JOB_DIAG_ARM_DEPS, &diag_start);
		err = nvkm_drm_job_arm_deps_locked(job);
		if (err != 0) {
			nvkm_drm_job_diag_finish(job->sc, diag_seq, err,
			    diag_start);
			TAILQ_REMOVE(&nfile->job_queue, job, link);
			job->queued = false;
			job->result = err;
			job->completed = true;
			nvkm_drm_job_disarm_deps_locked(job);
			nvkm_drm_job_wakeup_locked(nfile);
			wakeup(job);
			lwkt_reltoken(&nfile->job_token);

			nvkm_drm_job_signal(job, err);
			nvkm_drm_job_put(job);
			continue;
		}
		if (job->dep_pending != 0) {
			nvkm_drm_job_diag_stage(job->sc, diag_seq, job,
			    NVKM_DRM_JOB_DIAG_WAIT_DEPS);
			nvkm_drm_job_diag_finish(job->sc, diag_seq, 0,
			    diag_start);
			nfile->job_work_queued = false;
			nvkm_drm_job_wakeup_locked(nfile);
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		TAILQ_REMOVE(&nfile->job_queue, job, link);
		job->queued = false;
		job->running = true;
		job->sc->sync_job_ready_count++;
		nvkm_drm_job_diag_stage(job->sc, diag_seq, job,
		    NVKM_DRM_JOB_DIAG_RUN);
		lwkt_reltoken(&nfile->job_token);

		err = nvkm_drm_job_run(job);
		lwkt_gettoken(&nfile->job_token);
		job->running = false;
		job->result = err;
		job->completed = true;
		nvkm_drm_job_diag_stage(job->sc, diag_seq, job,
		    NVKM_DRM_JOB_DIAG_COMPLETE);
		nvkm_drm_job_diag_finish(job->sc, diag_seq, err,
		    diag_start);
		nvkm_drm_job_wakeup_locked(nfile);
		wakeup(job);
		lwkt_reltoken(&nfile->job_token);
		nvkm_drm_job_put(job);
	}
}

static void
nvkm_drm_jobs_flush(struct nvkm_drm_file *nfile)
{
	struct nvkm_drm_job *job;

	if (nfile == NULL)
		return;
	for (;;) {
		flush_work(&nfile->job_work);
		lwkt_gettoken(&nfile->job_token);
		if (TAILQ_EMPTY(&nfile->job_queue)) {
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		job = TAILQ_FIRST(&nfile->job_queue);
		if (job != NULL &&
		    (!job->deps_armed || job->dep_pending == 0))
			(void)nvkm_drm_job_queue_work_locked(nfile);
		lwkt_reltoken(&nfile->job_token);
		(void)tsleep(&nfile->job_epoch, 0, "nvkjfl", hz / 10);
	}
}

static void
nvkm_drm_jobs_close(struct nvkm_drm_file *nfile)
{
	struct nvkm_drm_job *job;

	if (nfile == NULL)
		return;

	for (;;) {
		lwkt_gettoken(&nfile->job_token);
		nfile->job_closing = true;
		job = TAILQ_FIRST(&nfile->job_queue);
		if (job == NULL) {
			nvkm_drm_job_wakeup_locked(nfile);
			lwkt_reltoken(&nfile->job_token);
			break;
		}
		TAILQ_REMOVE(&nfile->job_queue, job, link);
		job->queued = false;
		job->result = -ECANCELED;
		job->completed = true;
		nvkm_drm_job_disarm_deps_locked(job);
		nvkm_drm_job_wakeup_locked(nfile);
		wakeup(job);
		lwkt_reltoken(&nfile->job_token);

		nvkm_drm_job_wait_deps_disarmed(job);
		job->sc->sync_job_cancel_count++;
		nvkm_drm_job_signal(job, -ECANCELED);
		nvkm_drm_job_put(job);
	}
	flush_work(&nfile->job_work);
}

/*
 * nvkm_drm_try_vm_bind_sync_fast()
 *
 * Ownership:
 *   Borrows req, file_priv, nfile, and sc. ops remains owned by the caller
 *   when this function returns -EAGAIN. Any other return value consumes ops,
 *   either by applying the bind or by failing before submission can be useful.
 *
 * Lifetime:
 *   This path is only valid for synchronous VM_BIND calls with no explicit
 *   wait or signal syncobjs. It runs only while the per-file job queue is idle,
 *   so it cannot overtake an EXEC or async VM_BIND already published to the
 *   queue. On success, the ioctl returns after PTE updates are applied and the
 *   VMM flush/TLB invalidate has completed, which is the same completion point
 *   that a signaled synchronous VM_BIND job would expose.
 *
 * Threading:
 *   The first job_submit_lock check is only a cheap filter that avoids GEM
 *   lookup work when the queue is already busy.  The second check, held from
 *   the idle decision through the direct apply, is the ordering boundary that
 *   prevents a later submit from entering the queue ahead of this bind. The
 *   apply helper still owns vm_token and gsp_tok for the actual PTE mutation.
 */
static int
nvkm_drm_try_vm_bind_sync_fast(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    const struct drm_nouveau_vm_bind *req,
    struct drm_nouveau_vm_bind_op *ops, bool sync)
{
	struct drm_gem_object **objects = NULL;
	struct nvkm_drm_vm_bind_retire *retire = NULL;
	bool idle;
	int err;

	if (!sync || req->wait_count != 0 || req->sig_count != 0)
		return (-EAGAIN);

	lockmgr(&nfile->job_submit_lock, LK_EXCLUSIVE);
	lwkt_gettoken(&nfile->job_token);
	idle = !nfile->job_closing && TAILQ_EMPTY(&nfile->job_queue) &&
	    !nfile->job_work_queued;
	lwkt_reltoken(&nfile->job_token);
	lockmgr(&nfile->job_submit_lock, LK_RELEASE);
	if (!idle)
		return (-EAGAIN);

	if (req->op_count != 0) {
		retire = kzalloc(sizeof(*retire), GFP_KERNEL);
		if (retire == NULL) {
			kfree(ops);
			sc->vm_bind_fast_count++;
			sc->vm_bind_fast_error_count++;
			return (-ENOMEM);
		}
		INIT_WORK(&retire->work, nvkm_drm_vm_bind_retire_work);
		LIST_INIT(&retire->bindings);
		retire->sc = sc;

		err = nvkm_drm_vm_bind_prepare_objects(file_priv, ops,
		    req->op_count, &objects);
		if (err != 0) {
			nvkm_drm_vm_bind_retire_free(retire);
			kfree(ops);
			sc->vm_bind_fast_count++;
			sc->vm_bind_fast_error_count++;
			return (err);
		}
	}

	lockmgr(&nfile->job_submit_lock, LK_EXCLUSIVE);
	lwkt_gettoken(&nfile->job_token);
	idle = !nfile->job_closing && TAILQ_EMPTY(&nfile->job_queue) &&
	    !nfile->job_work_queued;
	lwkt_reltoken(&nfile->job_token);
	if (!idle) {
		lockmgr(&nfile->job_submit_lock, LK_RELEASE);
		nvkm_drm_vm_bind_retire_free(retire);
		nvkm_drm_vm_bind_objects_put(objects, req->op_count);
		return (-EAGAIN);
	}

	sc->vm_bind_fast_count++;
	if (req->op_count != 0) {
		err = nvkm_drm_vm_bind_apply(sc, file_priv, nfile, ops,
		    req->op_count, objects, &retire->bindings);
		if (err != 0)
			sc->vm_bind_fast_error_count++;
		nvkm_drm_vm_bind_retire_schedule(&retire);
	} else {
		err = 0;
	}
	lockmgr(&nfile->job_submit_lock, LK_RELEASE);

	nvkm_drm_vm_bind_objects_put(objects, req->op_count);
	kfree(ops);
	return (err);
}

static int
nvkm_drm_queue_vm_bind_job(struct nvkm_softc *sc,
    struct drm_file *file_priv, struct nvkm_drm_file *nfile,
    const struct drm_nouveau_vm_bind *req,
    struct drm_nouveau_vm_bind_op *ops, bool sync)
{
	struct nvkm_drm_job *job;
	bool published = false;
	bool sync_ref_taken = false;
	int err;

	err = nvkm_drm_try_vm_bind_sync_fast(sc, file_priv, nfile, req,
	    ops, sync);
	if (err != -EAGAIN)
		return (err);

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (job == NULL) {
		kfree(ops);
		return (-ENOMEM);
	}
	kref_init(&job->refcount);
	job->type = NVKM_DRM_JOB_VM_BIND;
	job->sc = sc;
	job->file_priv = file_priv;
	job->nfile = nfile;
	job->vm_bind.op_count = req->op_count;
	job->vm_bind.sig_count = req->sig_count;
	job->vm_bind.ops = ops;
	if (req->op_count != 0) {
		job->vm_bind.retire = kzalloc(sizeof(*job->vm_bind.retire),
		    GFP_KERNEL);
		if (job->vm_bind.retire == NULL) {
			err = -ENOMEM;
			goto fail;
		}
		INIT_WORK(&job->vm_bind.retire->work,
		    nvkm_drm_vm_bind_retire_work);
		LIST_INIT(&job->vm_bind.retire->bindings);
		job->vm_bind.retire->sc = sc;
	}
	job->done_fence = nvkm_drm_exec_fence_create(sc, ++sc->fence_seqno);
	if (job->done_fence == NULL) {
		err = -ENOMEM;
		goto fail;
	}
	nvkm_drm_exec_fence_set_producer(job->done_fence,
	    NVKM_DRM_FENCE_PRODUCER_VM_BIND_JOB, 0, -1, 0, 0, 0);

	err = nvkm_drm_vm_bind_prepare_objects(file_priv, ops, req->op_count,
	    &job->vm_bind.objects);
	if (err != 0)
		goto fail;

	err = nvkm_drm_collect_wait_syncobjs(sc, file_priv, req->wait_count,
	    req->wait_ptr, &job->wait_fences, &job->wait_count);
	if (err != 0)
		goto fail;
	err = nvkm_drm_job_prepare_deps(job);
	if (err != 0)
		goto fail;

	err = nvkm_drm_prepare_signal_syncobjs(sc, file_priv, req->sig_count,
	    req->sig_ptr, job->done_fence, &job->vm_bind.signals);
	if (err != 0)
		goto fail;

	lockmgr(&nfile->job_submit_lock, LK_EXCLUSIVE);
	err = nvkm_drm_vm_bind_attach_resv_fence(sc, nfile, ops,
	    req->op_count, job->vm_bind.objects, job->done_fence);
	if (err == 0) {
		nvkm_drm_exec_signals_publish(job->vm_bind.signals,
		    job->vm_bind.sig_count);
		published = true;
		nvkm_drm_vm_bind_debug_delay(sc);
		if (sync) {
			nvkm_drm_job_get(job);
			sync_ref_taken = true;
		}
		err = nvkm_drm_job_enqueue(job);
	}
	lockmgr(&nfile->job_submit_lock, LK_RELEASE);
	if (err != 0) {
		if (published)
			goto fail_signal;
		goto fail;
	}
	if (sync) {
		err = nvkm_drm_job_wait_complete(job);
		nvkm_drm_job_put(job);
	}
	return (err);

fail_signal:
	nvkm_drm_job_signal(job, err);
	if (sync_ref_taken)
		nvkm_drm_job_put(job);
fail:
	nvkm_drm_job_put(job);
	return (err);
}

static struct nvkm_drm_vm_binding *
nvkm_drm_vm_binding_find_overlap(struct nvkm_drm_file *nfile, uint64_t addr,
    uint64_t size)
{
	return (nvkm_drm_vm_binding_first_overlap(nfile, addr, size));
}

static bool
nvkm_drm_gem_object_array_has(struct drm_gem_object **objects,
    uint32_t count, struct drm_gem_object *obj)
{
	for (uint32_t i = 0; i < count; i++) {
		if (objects[i] == obj)
			return (true);
	}
	return (false);
}

static void
nvkm_drm_gem_object_array_put(struct drm_gem_object **objects,
    uint32_t count)
{
	if (objects == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		if (objects[i] != NULL)
			drm_gem_object_put_unlocked(objects[i]);
	}
	kfree(objects);
}

static int
nvkm_drm_gem_object_array_append(struct drm_gem_object ***pobjects,
    uint32_t *pcount, uint32_t *pcapacity, struct drm_gem_object *obj)
{
	struct drm_gem_object **objects = *pobjects;
	uint32_t count = *pcount;
	uint32_t capacity = *pcapacity;

	if (obj == NULL)
		return (0);
	if (nvkm_drm_gem_object_array_has(objects, count, obj))
		return (0);
	if (count == capacity) {
		uint32_t new_capacity = capacity != 0 ? capacity * 2 : 8;
		struct drm_gem_object **new_objects;

		new_objects = krealloc(objects,
		    sizeof(*new_objects) * new_capacity, M_DRM, GFP_KERNEL);
		if (new_objects == NULL)
			return (-ENOMEM);
		objects = new_objects;
		capacity = new_capacity;
	}
	drm_gem_object_get(obj);
	objects[count++] = obj;
	*pobjects = objects;
	*pcount = count;
	*pcapacity = capacity;
	return (0);
}

/*
 * nvkm_drm_vm_validate_snapshot_objects()
 *
 * Ownership:
 *   On success, returns a caller-owned array of GEM object references for the
 *   unique BOs currently present on nfile's validate list.  The caller must
 *   release it with nvkm_drm_gem_object_array_put().
 *
 * Lifetime:
 *   The snapshot is only a candidate set.  Each GEM reference keeps the BO
 *   alive after vm_token is released; live binding state is rechecked by the
 *   TTM move callback and by the clear helper after validation.
 *
 * Threading:
 *   Takes nfile->vm_token while walking the VM validate list.  It never locks
 *   BO reservation objects while holding vm_token.
 */
static int
nvkm_drm_vm_validate_snapshot_objects(struct nvkm_drm_file *nfile,
    struct drm_gem_object ***pobjects, uint32_t *pobject_count)
{
	struct drm_gem_object **objects = NULL;
	struct nvkm_drm_vm_binding *binding;
	uint32_t count = 0;
	uint32_t capacity = 0;
	int err = 0;

	*pobjects = NULL;
	*pobject_count = 0;

	lwkt_gettoken(&nfile->vm_token);
	LIST_FOREACH(binding, &nfile->vm_validate_bindings, validate_link) {
		err = nvkm_drm_gem_object_array_append(&objects, &count,
		    &capacity, binding->obj);
		if (err != 0)
			break;
	}
	lwkt_reltoken(&nfile->vm_token);
	if (err != 0) {
		nvkm_drm_gem_object_array_put(objects, count);
		return (err);
	}

	*pobjects = objects;
	*pobject_count = count;
	return (0);
}

/*
 * nvkm_drm_vm_validate_clear_object()
 *
 * Ownership:
 *   Borrows nfile and obj.  It clears validate-list membership for every live
 *   binding of obj in this VM but does not release GEM references or VM_BIND
 *   pins.
 *
 * Lifetime:
 *   Called after DragonFly TTM reports the BO is valid for its preferred
 *   placement.  Bindings that were unmapped concurrently are already detached
 *   and no longer appear on the live validate list.
 *
 * Threading:
 *   Takes nfile->vm_token only for VM list mutation.  It does not sleep and
 *   does not acquire TTM reservation locks.
 */
static void
nvkm_drm_vm_validate_clear_object(struct nvkm_drm_file *nfile,
    struct drm_gem_object *obj)
{
	struct nvkm_drm_vm_binding *binding, *next;

	lwkt_gettoken(&nfile->vm_token);
	LIST_FOREACH_MUTABLE(binding, &nfile->vm_validate_bindings,
	    validate_link, next) {
		if (binding->obj == obj)
			nvkm_drm_vm_binding_validate_clear(binding);
	}
	lwkt_reltoken(&nfile->vm_token);
}

/*
 * nvkm_drm_vm_validate_rebind()
 *
 * Ownership:
 *   Borrows sc and nfile.  Candidate BOs are held by temporary GEM references
 *   while this helper calls DragonFly TTM validate; nvkm does not own or
 *   rewrite TTM placement directly.
 *
 * Lifetime:
 *   Runs immediately before an EXEC doorbell.  Any live GPUVA binding marked
 *   by a previous TTM move is validated back through TTM, which in turn calls
 *   nvkm's move/rebind callback to refresh page tables before the EXEC can
 *   observe them.
 *
 * Threading:
 *   Snapshots candidates under nfile->vm_token, then releases it before
 *   calling ttm_bo_validate().  This avoids deadlocking with the TTM move
 *   callback, which takes vm_token and gsp_tok to rebind GPUVA PTEs.
 */
static int
nvkm_drm_vm_validate_rebind(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile)
{
	struct drm_gem_object **objects = NULL;
	uint32_t object_count = 0;
	int err;

	sc->ttm_vm_validate_exec_count++;
	err = nvkm_drm_vm_validate_snapshot_objects(nfile, &objects,
	    &object_count);
	if (err != 0) {
		sc->ttm_vm_validate_error_count++;
		sc->ttm_vm_validate_last_error = err;
		return (err);
	}
	if (object_count == 0) {
		sc->ttm_vm_validate_empty_count++;
		return (0);
	}
	sc->ttm_vm_validate_candidate_count += object_count;

	for (uint32_t i = 0; i < object_count; i++) {
		struct nvkm_bo *bo = to_nvkm_bo(objects[i]);

		if (!nvkm_bo_ttm_prefers_vram(bo)) {
			nvkm_drm_vm_validate_clear_object(nfile, objects[i]);
			continue;
		}
		sc->ttm_vm_validate_bo_count++;
		err = nvkm_ttm_validate_bo_preferred(bo);
		if (err != 0) {
			sc->ttm_vm_validate_error_count++;
			sc->ttm_vm_validate_last_error = err;
			break;
		}
		if (!bo->ttm_backed || bo->tbo.mem.mem_type != TTM_PL_VRAM) {
			err = -EIO;
			sc->ttm_vm_validate_error_count++;
			sc->ttm_vm_validate_last_error = err;
			break;
		}
		nvkm_drm_vm_validate_clear_object(nfile, objects[i]);
	}

	nvkm_drm_gem_object_array_put(objects, object_count);
	return (err);
}

/*
 * nvkm_drm_vm_bind_snapshot_resv_objects()
 *
 * Ownership:
 *   On success, returns a newly allocated array whose entries each hold one
 *   GEM reference.  The caller owns the array and every entry reference and
 *   must release them with nvkm_drm_gem_object_array_put().  The objects
 *   argument is borrowed; this helper takes independent references for entries
 *   it returns.
 *
 * Lifetime:
 *   The snapshot covers the BOs whose GPUVA bookkeeping can be changed by the
 *   VM_BIND job: explicit MAP objects plus currently active bindings
 *   overlapped by any MAP or UNMAP op.  The references keep those BOs alive
 *   after nfile->vm_token is released.
 *
 * Threading:
 *   Takes nfile->vm_token only while walking the binding list.  It never locks
 *   BO reservation objects while holding vm_token; reservation fences are
 *   attached only after the snapshot owns stable GEM references.
 */
static int
nvkm_drm_vm_bind_snapshot_resv_objects(struct nvkm_drm_file *nfile,
    struct drm_nouveau_vm_bind_op *ops, uint32_t op_count,
    struct drm_gem_object **objects, struct drm_gem_object ***pobjects,
    uint32_t *pobject_count)
{
	*pobjects = NULL;
	*pobject_count = 0;

	for (;;) {
		struct drm_gem_object **resv_objects = NULL;
		struct nvkm_drm_vm_binding *binding;
		uint32_t binding_count = 0;
		uint32_t object_count = 0;
		uint32_t object_capacity;
		uint32_t seen_count = 0;
		bool retry = false;
		int err = 0;

		lwkt_gettoken(&nfile->vm_token);
		LIST_FOREACH(binding, &nfile->vm_bindings, link)
			binding_count++;
		lwkt_reltoken(&nfile->vm_token);

		object_capacity = op_count + binding_count;
		if (object_capacity != 0) {
			resv_objects = kcalloc(object_capacity,
			    sizeof(*resv_objects), GFP_KERNEL);
			if (resv_objects == NULL)
				return (-ENOMEM);
		}

		for (uint32_t i = 0; i < op_count; i++) {
			if (objects != NULL && objects[i] != NULL) {
				err = nvkm_drm_gem_object_array_append(&resv_objects,
				    &object_count, &object_capacity, objects[i]);
				if (err != 0)
					goto fail;
			}
		}

		lwkt_gettoken(&nfile->vm_token);
		LIST_FOREACH(binding, &nfile->vm_bindings, link) {
			if (seen_count == binding_count) {
				retry = true;
				break;
			}
			seen_count++;
			for (uint32_t i = 0; i < op_count; i++) {
				struct drm_nouveau_vm_bind_op *op = &ops[i];

				if (!nvkm_drm_vm_ranges_overlap(op->addr, op->range,
				    binding->addr, binding->size))
					continue;
				err = nvkm_drm_gem_object_array_append(&resv_objects,
				    &object_count, &object_capacity, binding->obj);
				break;
			}
			if (err != 0)
				break;
		}
		lwkt_reltoken(&nfile->vm_token);
		if (err != 0)
			goto fail;
		if (retry) {
			nvkm_drm_gem_object_array_put(resv_objects, object_count);
			continue;
		}

		*pobjects = resv_objects;
		*pobject_count = object_count;
		return (0);

fail:
		nvkm_drm_gem_object_array_put(resv_objects, object_count);
		return (err);
	}
}

/*
 * nvkm_drm_exec_attach_public_resv_fence()
 *
 * Ownership:
 *   Borrows fence and nfile. nfile->vm_resv takes its own fence reference
 *   through reservation_object_add_excl_fence(); the caller keeps ownership of
 *   its existing reference.
 *
 * Lifetime:
 *   Called after the EXEC done fence exists and before the ioctl returns.  This
 *   preserves the existing public/no-share wait behavior for CPU_PREP, GEM
 *   free, and file teardown even while the job is only queued.
 *
 * Threading:
 *   May sleep while locking the per-file reservation object.  It does not hold
 *   nfile->vm_token or sc->gsp_tok, and it never locks BO reservation objects.
 */
static int
nvkm_drm_exec_attach_public_resv_fence(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct dma_fence *fence)
{
	uint64_t profile_start;

	if (fence == NULL)
		return (-EINVAL);

	if (nvkm_drm_fence_flag_signaled(fence))
		sc->exec_resv_attach_signaled_count++;
	else
		sc->exec_resv_attach_pending_count++;
	profile_start = nvkm_drm_profile_now_us(sc);
	reservation_object_lock(&nfile->vm_resv, NULL);
	reservation_object_add_excl_fence(&nfile->vm_resv, fence);
	reservation_object_unlock(&nfile->vm_resv);
	sc->exec_resv_attach_calls++;
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_attach_resv_us,
	    profile_start);
	return (0);
}

/*
 * nvkm_drm_exec_attach_runtime_resv_fence()
 *
 * Ownership:
 *   Borrows fence and nfile. nfile->vm_exec_resv takes its own fence reference
 *   through reservation_object_add_excl_fence(); the caller keeps ownership of
 *   its existing reference.
 *
 * Lifetime:
 *   Called from the ordered job worker after TTM validate/rebind has completed
 *   and after the completion trailer/pending fence has a real signal path, but
 *   before the doorbell can make the EXEC visible to hardware.  This keeps TTM
 *   live-bound rebind waiting on already-running EXEC work without letting an
 *   EXEC validate path or concurrent eviction wait on an unarmed done fence.
 *
 * Threading:
 *   May sleep while locking nfile->vm_exec_resv.  It must run without holding
 *   nfile->vm_token, sc->gsp_tok, or any TTM BO reservation lock.
 */
static int
nvkm_drm_exec_attach_runtime_resv_fence(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct dma_fence *fence)
{
	uint64_t profile_start;

	if (fence == NULL)
		return (-EINVAL);

	if (nvkm_drm_fence_flag_signaled(fence))
		sc->exec_runtime_resv_attach_signaled_count++;
	else
		sc->exec_runtime_resv_attach_pending_count++;
	profile_start = nvkm_drm_profile_now_us(sc);
	reservation_object_lock(&nfile->vm_exec_resv, NULL);
	reservation_object_add_excl_fence(&nfile->vm_exec_resv, fence);
	reservation_object_unlock(&nfile->vm_exec_resv);
	sc->exec_runtime_resv_attach_calls++;
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_attach_resv_us,
	    profile_start);
	return (0);
}

/*
 * nvkm_drm_vm_bind_attach_resv_fence()
 *
 * Ownership:
 *   Borrows ops, objects, nfile, and fence.  Each BO reservation object takes
 *   its own fence reference through nvkm_bo_resv_add_shared_fence().
 *
 * Lifetime:
 *   Must be called after the VM_BIND done fence is visible to out syncobjs and
 *   before the job can run.  This mirrors nouveau BOOKKEEP reservation usage
 *   for VM_BIND: the fence protects GPUVA bookkeeping for the specific BOs the
 *   bind operation maps, unmaps, or replaces.
 *
 * Threading:
 *   May sleep while snapshotting affected VM_BIND objects and while locking BO
 *   reservation objects.  It does not hold nfile->vm_token across reservation
 *   locks.
 */
static int
nvkm_drm_vm_bind_attach_resv_fence(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct drm_nouveau_vm_bind_op *ops,
    uint32_t op_count, struct drm_gem_object **objects,
    struct dma_fence *fence)
{
	struct drm_gem_object **resv_objects;
	uint32_t object_count;
	uint64_t profile_start;
	int err;

	if (fence == NULL)
		return (-EINVAL);

	if (nvkm_drm_fence_flag_signaled(fence))
		sc->vm_bind_resv_attach_signaled_count++;
	else
		sc->vm_bind_resv_attach_pending_count++;
	profile_start = nvkm_drm_profile_now_us(sc);
	err = nvkm_drm_vm_bind_snapshot_resv_objects(nfile, ops, op_count,
	    objects, &resv_objects, &object_count);
	if (err != 0)
		return (err);

	for (uint32_t i = 0; i < object_count; i++) {
		err = nvkm_bo_resv_add_shared_fence(to_nvkm_bo(resv_objects[i]),
		    fence);
		if (err != 0)
			break;
	}
	nvkm_drm_gem_object_array_put(resv_objects, object_count);
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_attach_resv_us,
	    profile_start);
	return (err);
}

static int
nvkm_drm_exec_submit(struct nvkm_softc *sc, struct drm_file *file_priv,
    struct nvkm_drm_file *nfile, uint32_t channel_id,
    struct drm_nouveau_exec_push *pushes, uint32_t push_count,
    struct dma_fence *done_fence, struct nvkm_drm_exec_signal *signals,
    uint32_t sig_count)
{
	struct drm_nouveau_exec req_storage;
	struct drm_nouveau_exec *req = &req_storage;
	struct nvkm_drm_chan *dchan;
	struct nvkm_gsp_chan *chan;
	struct nvkm_drm_exec_pending *pending = NULL;
	struct dma_fence *exec_fence = NULL;
	uint32_t *gpf, *post, *sema;
	uint64_t slot_bar1, post_gva, sema_gva;
	uint32_t put = 0, payload, post_slot, post_offset, sema_offset;
	uint32_t gpf_required = 0;
	uint64_t trace_seq;
	uint32_t trace_first = 0;
	uint32_t trace_count = 0;
	uint64_t profile_start;
	uint64_t profile_push_start = 0;
	uint64_t diag_seq = 0;
	uint64_t diag_start = 0;
	int err = 0;
	bool gsp_tok_held = false;
	bool exec_completion_queued = false;
	bool submit_slot_allocated = false;
	uint64_t push_va_lo = ~0ULL;
	uint64_t push_va_hi = 0;

	(void)file_priv;
	memset(&req_storage, 0, sizeof(req_storage));
	req->channel = channel_id;
	req->push_count = push_count;
	req->sig_count = sig_count;

	if (nfile == NULL)
		return (-ENXIO);
	diag_seq = nvkm_drm_exec_diag_begin(sc, channel_id, push_count,
	    sig_count, &diag_start);
	sc->exec_submit_count++;
	dchan = nvkm_drm_channel_find(nfile, req->channel);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC begin channel=%u pushes=%u waits=%u sigs=%u\n",
	    req->channel, req->push_count, req->wait_count, req->sig_count);
	if (dchan == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC missing channel=%u\n", req->channel);
		err = -ENOENT;
		goto out_diag;
	}
	chan = dchan->chan;
	if (chan == NULL || chan->submit_gpf.kva == NULL ||
	    chan->submit_push.kva == NULL || chan->submit_sema.kva == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC channel=%u missing submit buffers chan=%p\n",
		    req->channel, chan);
		err = -ENXIO;
		goto out_diag;
	}
	/*
	 * Empty EXEC job:
	 *
	 * Ownership:
	 *   Borrows done_fence.  The job and VM reservation object own their fence
	 *   references; this helper only completes the fence state.
	 *
	 * Lifetime:
	 *   Dependency waits have already completed before the worker calls this
	 *   helper.  With no GPU push there will be no pending interrupt record, so
	 *   the done fence must complete here instead of waiting for GPU completion.
	 *
	 * Threading:
	 *   Runs from the ordered per-file job worker, never from the ioctl fast
	 *   path.  Signal-only EXECs fall through to the normal completion trailer
	 *   so their fences stay ordered behind in-flight channel work.
	 */
	if (req->push_count == 0 && req->sig_count == 0) {
		sc->exec_signal_only_count++;
		if (chan->faulted)
			err = chan->fault_error != 0 ? chan->fault_error : -EIO;
		else
			err = 0;
		if (done_fence != NULL) {
			if (err != 0)
				nvkm_drm_fence_set_error(done_fence, err);
			nvkm_drm_exec_fence_note_signal(done_fence,
			    NVKM_DRM_FENCE_SIGNAL_EMPTY_EXEC, err);
			(void)dma_fence_signal(done_fence);
		}
		goto out_diag;
	}
	if (req->push_count > NVKM_DRM_GPFIFO_ENTRIES - 2) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC too many pushes channel=%u pushes=%u\n",
		    req->channel, req->push_count);
		err = -EINVAL;
		goto out_diag;
	}

	/* push_count may be 0 here for a signal-only submit; pushes stays NULL
	 * and the push loop below runs zero times, leaving only the completion
	 * trailer (which orders the signals behind in-flight channel work). */
	err = nvkm_drm_vm_validate_rebind(sc, nfile);
	if (err != 0)
		goto out_diag;
	/* Block only against an in-progress page-table mutation. Once the
	 * doorbell is written, VM_BIND no longer waits for this submit; userspace
	 * syncobjs describe the GPU ordering, matching nouveau's VM_BIND UAPI.
	 */
	nvkm_drm_exec_diag_stage(sc, diag_seq, NVKM_DRM_EXEC_DIAG_VM_TOKEN,
	    0, 0);
	lwkt_gettoken(&nfile->vm_token);
	lwkt_reltoken(&nfile->vm_token);

	profile_start = nvkm_drm_profile_now_us(sc);
	nvkm_drm_exec_diag_stage(sc, diag_seq, NVKM_DRM_EXEC_DIAG_GSP_TOKEN,
	    0, 0);
	lwkt_gettoken(&sc->gsp_tok);
	gsp_tok_held = true;
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_token_wait_us,
	    profile_start);
	profile_push_start = nvkm_drm_profile_now_us(sc);
	gpf = (uint32_t *)chan->submit_gpf.kva;
	if (chan->faulted) {
		err = chan->fault_error != 0 ? chan->fault_error : -EIO;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC rejected faulted channel=%u chid=%d err=%d\n",
		    req->channel, chan->chid, err);
		goto out_unlock;
	}
	nvkm_drm_exec_diag_stage(sc, diag_seq,
	    NVKM_DRM_EXEC_DIAG_SLOT_ALLOC, put, 0);
	err = nvkm_drm_submit_slot_alloc(chan, &post_slot);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC no free post slots channel=%u busy=0x%016jx\n",
		    req->channel, (uintmax_t)chan->submit_post_slots_busy);
		goto out_unlock;
	}
	submit_slot_allocated = true;
	post_offset = post_slot * NVKM_DRM_POST_PUSH_DWORDS;
	sema_offset = post_slot;
	post = (uint32_t *)chan->submit_push.kva + post_offset;
	sema = (uint32_t *)chan->submit_sema.kva + sema_offset;
	post_gva = chan->submit_gva_push + (uint64_t)post_offset * 4;
	sema_gva = chan->submit_gva_sema + (uint64_t)sema_offset * 4;
	slot_bar1 = chan->userd_bar2_gva +
	    (uint64_t)((uint32_t)chan->chid % 8u) *
	    NV_USERD_SLOT_SIZE;

	nvkm_drm_exec_diag_stage(sc, diag_seq,
	    NVKM_DRM_EXEC_DIAG_GPFIFO_WAIT, put, post_slot);
	err = nvkm_drm_gpfifo_wait_space(sc, chan, slot_bar1, req->push_count,
	    &put, &gpf_required);
	if (err != 0)
		goto out_unlock;
	nvkm_drm_exec_diag_stage(sc, diag_seq,
	    NVKM_DRM_EXEC_DIAG_BUILD_PUSH, put, post_slot);
	payload = (uint32_t)(req->channel << 16) ^ put ^ req->push_count ^
	    0x51a70000u;
	sema[0] = 0;

	for (uint32_t i = 0; i < req->push_count; i++) {
		uint32_t entry0, entry1;

		if ((pushes[i].flags & ~DRM_NOUVEAU_EXEC_PUSH_FLAGS_KNOWN) != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC unknown push flags channel=%u push=%u flags=0x%08x\n",
			    req->channel, i, pushes[i].flags);
			err = -EINVAL;
			goto out_unlock;
		}
		if ((pushes[i].va | pushes[i].va_len) & 3 ||
		    pushes[i].va_len == 0 || pushes[i].va_len >= (1U << 23)) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC invalid push channel=%u idx=%u va=0x%016jx len=0x%08x flags=0x%08x\n",
			    req->channel, i, (uintmax_t)pushes[i].va,
			    pushes[i].va_len, pushes[i].flags);
			err = -EINVAL;
			goto out_unlock;
		}
		if (pushes[i].va < push_va_lo)
			push_va_lo = pushes[i].va;
		if (pushes[i].va + pushes[i].va_len > push_va_hi)
			push_va_hi = pushes[i].va + pushes[i].va_len;
		entry0 = (uint32_t)(pushes[i].va & 0xffffffffu);
		entry1 = (uint32_t)((pushes[i].va >> 32) & 0xffu) |
		    ((pushes[i].va_len / 4) << NVC06F_GP_ENTRY1_LENGTH_SHIFT);
		if ((pushes[i].flags & DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH) != 0)
			entry1 |= NVC06F_GP_ENTRY1_NO_PREFETCH;

		if ((put & (NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)) ==
		    NVKM_DRM_GPFIFO_FETCH_WINDOW - 1) {
			gpf[put * 2 + 0] = 0;
			gpf[put * 2 + 1] = 0;
			put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
		}
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC push channel=%u idx=%u va=0x%016jx len=0x%08x flags=0x%08x gpf[%u]=0x%08x:0x%08x\n",
		    req->channel, i, (uintmax_t)pushes[i].va, pushes[i].va_len,
		    pushes[i].flags, put, entry0, entry1);
		nvkm_drm_dump_large_push(sc, nfile, pushes[i].va,
		    pushes[i].va_len);
		if (nvkm_debug != 0) {
			if (trace_count == 0)
				trace_first = sc->exec_trace_next;
			trace_seq = sc->exec_submit_count;
			nvkm_drm_exec_trace_record(sc, trace_seq, req->channel,
			    chan->chid, post_slot, put, i, req->push_count,
			    &pushes[i]);
			trace_count++;
		}
		gpf[put * 2 + 0] = entry0;
		gpf[put * 2 + 1] = entry1;
		put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	}
	sc->exec_profile_pushes += req->push_count;

	post[0] = NVC36F_PUSH_HDR_SEM_ADDR_TRIPLET;
	post[1] = (uint32_t)(sema_gva & 0xffffffffu);
	post[2] = (uint32_t)((sema_gva >> 32) & 0xffu);
	post[3] = payload;
	post[4] = NVC36F_PUSH_HDR_SEM_EXECUTE;
	post[5] = NVC36F_SEM_EXECUTE_RELEASE |
	    NVC36F_SEM_EXECUTE_RELEASE_WFI;
	post[6] = NVC36F_PUSH_HDR_MEM_OP_A_TO_D;
	post[7] = 0;
	post[8] = 0;
	post[9] = NVC36F_MEM_OP_C_MEMBAR_SYS_MEMBAR;
	post[10] = NVC36F_MEM_OP_D_OPERATION_MEMBAR;
	post[11] = NVC36F_PUSH_HDR_NON_STALL_INTERRUPT;
	post[12] = 0;

	if ((put & (NVKM_DRM_GPFIFO_FETCH_WINDOW - 1)) ==
	    NVKM_DRM_GPFIFO_FETCH_WINDOW - 1) {
		gpf[put * 2 + 0] = 0;
		gpf[put * 2 + 1] = 0;
		put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	}
	gpf[put * 2 + 0] = (uint32_t)(post_gva & 0xffffffffu);
	gpf[put * 2 + 1] = (uint32_t)((post_gva >> 32) & 0xffu) |
	    (NVKM_DRM_POST_PUSH_DWORDS << NVC06F_GP_ENTRY1_LENGTH_SHIFT);
	put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_push_build_us,
	    profile_push_start);

	nvkm_drm_exec_diag_stage(sc, diag_seq, NVKM_DRM_EXEC_DIAG_FENCE,
	    put, post_slot);
	if (done_fence != NULL) {
		exec_fence = done_fence;
		if (signals != NULL && req->sig_count != 0)
			sc->exec_signal_fence_count++;
		else
			sc->exec_internal_fence_count++;
	} else {
		exec_fence = nvkm_drm_exec_fence_create(sc, ++sc->fence_seqno);
		if (exec_fence == NULL) {
			err = -ENOMEM;
			goto out_unlock;
		}
		nvkm_drm_exec_fence_set_producer(exec_fence,
		    NVKM_DRM_FENCE_PRODUCER_EXEC_INTERNAL, req->channel,
		    chan->chid, 0, 0, sc->exec_submit_count);
		sc->exec_internal_fence_count++;
	}
	pending = nvkm_drm_exec_pending_create(chan, (volatile uint32_t *)sema,
	    payload, post_slot, sc->exec_submit_count, trace_first,
	    trace_count, signals, req->sig_count, exec_fence);
	if (pending == NULL) {
		err = -ENOMEM;
		goto out_unlock;
	}
	nvkm_drm_exec_pending_arm_fences(pending);
	/*
	 * TTM live-bound rebind waits on vm_exec_resv before rewriting PTEs.
	 * The fence there must represent every EXEC, including submits where
	 * userspace did not request an out-fence and nvkm created an internal
	 * completion fence only for kernel ordering.
	 */
	err = nvkm_drm_exec_attach_runtime_resv_fence(sc, nfile, exec_fence);
	if (err != 0)
		goto out_unlock;

	profile_start = nvkm_drm_profile_now_us(sc);
	/*
	 * NVK command BOs and the fixed submit GPFIFO/post/semaphore pages are
	 * coherent sysmem on the x86 desktop targets supported by this driver.
	 * DragonFly pmap_invalidate_cache_range() is for cache-domain changes
	 * and broadcasts WBINVD on CPUs without CPUID_SS, which turns each EXEC
	 * into several global cache flushes. The only ordering needed here is
	 * the Linux nouveau kick sequence below: GP_PUT, wmb/sfence, USERD read
	 * flush, then doorbell.
	 */
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_cache_flush_us,
	    profile_start);
	/* Debug canary: detect command-pool chunk reuse while the GPU is still
	 * reading the previous submit. The scan is diagnostic only and is kept
	 * off the production submit path unless verbose DRM debug is enabled. */
	pending->push_va_lo = push_va_lo;
	pending->push_va_hi = push_va_hi;
	if (nvkm_debug != 0 && push_va_hi > push_va_lo) {
		struct nvkm_drm_exec_pending *op;

		LIST_FOREACH(op, &sc->exec_pending, link) {
			if (op->chan != chan)
				continue;
			if (push_va_lo < op->push_va_hi &&
			    op->push_va_lo < push_va_hi) {
				sc->exec_push_reuse_count++;
				sc->exec_push_reuse_va = push_va_lo;
				sc->exec_push_reuse_prev_payload = op->payload;
				if (sc->exec_push_reuse_count <= 3)
					nvkm_infof(sc->dev,
					    "EXEC push REUSE-IN-FLIGHT: new=[0x%llx,0x%llx) overlaps in-flight=[0x%llx,0x%llx) prev_payload=0x%08x (count=%llu)\n",
					    (unsigned long long)push_va_lo,
					    (unsigned long long)push_va_hi,
					    (unsigned long long)op->push_va_lo,
					    (unsigned long long)op->push_va_hi,
					    op->payload,
					    (unsigned long long)sc->exec_push_reuse_count);
				break;
			}
		}
	}
	/* Keep the file pointer only for fault-time binding diagnostics. The
	 * drm_file owns the channel and cancels pending records before the VMM
	 * is destroyed, so this borrowed pointer stays valid while pending.
	 */
	pending->nfile = nfile;
	nvkm_drm_exec_diag_stage(sc, diag_seq, NVKM_DRM_EXEC_DIAG_QUEUE,
	    put, post_slot);
	LIST_INSERT_HEAD(&sc->exec_pending, pending, link);
	exec_completion_queued = true;
	sc->exec_async_pending_count++;
	profile_start = nvkm_drm_profile_now_us(sc);
	nvkm_drm_exec_diag_stage(sc, diag_seq, NVKM_DRM_EXEC_DIAG_DOORBELL,
	    put, post_slot);
	nvkm_gsp_bar1_wr32(sc, slot_bar1 + NV_USERD_GP_PUT, put);
	cpu_sfence();
	(void)nvkm_gsp_bar1_rd32(sc, slot_bar1 + 0);
	nvkm_wr32(sc, NV_USERMODE_DOORBELL, chan->gsp_token);
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_doorbell_us,
	    profile_start);
	chan->gpf_put = put;
	if (chan->gpf_free >= gpf_required)
		chan->gpf_free -= gpf_required;
	else
		chan->gpf_free = 0;
	pending = NULL;
	submit_slot_allocated = false;
	lwkt_reltoken(&sc->gsp_tok);
	gsp_tok_held = false;
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC submitted channel=%u pushes=%u sigs=%u err=%d put=%u sema=0x%08x payload=0x%08x slot=%u\n",
	    req->channel, req->push_count, req->sig_count, err, put, sema[0],
	    payload, post_slot);

out_unlock:
	nvkm_drm_exec_diag_stage(sc, diag_seq, NVKM_DRM_EXEC_DIAG_CLEANUP,
	    put, submit_slot_allocated ? post_slot : 0);
	profile_start = nvkm_drm_profile_now_us(sc);
	if (exec_fence != NULL) {
		if (!exec_completion_queued || err != 0) {
			if (err != 0)
				nvkm_drm_fence_set_error(exec_fence, err);
			nvkm_drm_exec_fence_note_signal(exec_fence,
			    NVKM_DRM_FENCE_SIGNAL_SUBMIT_CLEANUP, err);
			(void)dma_fence_signal(exec_fence);
		}
		if (done_fence == NULL)
			dma_fence_put(exec_fence);
	}
	if (exec_fence == NULL && err != 0 && done_fence != NULL &&
	    !nvkm_drm_fence_flag_signaled(done_fence)) {
		nvkm_drm_fence_set_error(done_fence, err);
		nvkm_drm_exec_fence_note_signal(done_fence,
		    NVKM_DRM_FENCE_SIGNAL_SUBMIT_CLEANUP, err);
		(void)dma_fence_signal(done_fence);
	}
	if (submit_slot_allocated)
		nvkm_drm_submit_slot_release(chan, post_slot);
	nvkm_drm_exec_pending_put(sc, pending);
	if (gsp_tok_held)
		lwkt_reltoken(&sc->gsp_tok);
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_cleanup_us, profile_start);
	if (err != 0)
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC return channel=%u err=%d\n",
		    req->channel, err);
	nvkm_drm_exec_diag_finish(sc, diag_seq, err, diag_start);
	return (err);

out_diag:
	if (err != 0 && done_fence != NULL &&
	    !nvkm_drm_fence_flag_signaled(done_fence)) {
		nvkm_drm_fence_set_error(done_fence, err);
		nvkm_drm_exec_fence_note_signal(done_fence,
		    NVKM_DRM_FENCE_SIGNAL_SUBMIT_CLEANUP, err);
		(void)dma_fence_signal(done_fence);
	}
	nvkm_drm_exec_diag_finish(sc, diag_seq, err, diag_start);
	return (err);
}

static int
nvkm_drm_ioctl_exec(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_exec *req = data;
	struct nvkm_drm_chan *dchan;
	struct nvkm_gsp_chan *chan;
	struct nvkm_drm_job *job;
	uint64_t profile_start;
	int err;

	if (nfile == NULL)
		return (-ENXIO);
	dchan = nvkm_drm_channel_find(nfile, req->channel);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC queue channel=%u pushes=%u waits=%u sigs=%u\n",
	    req->channel, req->push_count, req->wait_count, req->sig_count);
	if (dchan == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC missing channel=%u\n", req->channel);
		return (-ENOENT);
	}
	chan = dchan->chan;
	if (chan == NULL || chan->submit_gpf.kva == NULL ||
	    chan->submit_push.kva == NULL || chan->submit_sema.kva == NULL) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC channel=%u missing submit buffers chan=%p\n",
		    req->channel, chan);
		return (-ENXIO);
	}
	if (req->push_count > NVKM_DRM_GPFIFO_ENTRIES - 2) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC too many pushes channel=%u pushes=%u\n",
		    req->channel, req->push_count);
		return (-EINVAL);
	}

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (job == NULL)
		return (-ENOMEM);
	kref_init(&job->refcount);
	job->type = NVKM_DRM_JOB_EXEC;
	job->sc = sc;
	job->file_priv = file_priv;
	job->nfile = nfile;
	job->exec.channel = req->channel;
	job->exec.push_count = req->push_count;
	job->exec.sig_count = req->sig_count;
	job->done_fence = nvkm_drm_exec_fence_create(sc, ++sc->fence_seqno);
	if (job->done_fence == NULL) {
		err = -ENOMEM;
		goto fail;
	}
	nvkm_drm_exec_fence_set_producer(job->done_fence,
	    NVKM_DRM_FENCE_PRODUCER_EXEC_JOB, req->channel, chan->chid,
	    0, 0, 0);

	if (req->push_count != 0) {
		job->exec.pushes = kmalloc_array(req->push_count,
		    sizeof(*job->exec.pushes), GFP_KERNEL);
		if (job->exec.pushes == NULL) {
			err = -ENOMEM;
			goto fail;
		}
		err = copyin((const void *)(uintptr_t)req->push_ptr,
		    job->exec.pushes,
		    sizeof(*job->exec.pushes) * req->push_count);
		if (err != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC push copyin failed channel=%u pushes=%u err=%d\n",
			    req->channel, req->push_count, err);
			err = -EFAULT;
			goto fail;
		}
	}

	profile_start = nvkm_drm_profile_now_us(sc);
	err = nvkm_drm_collect_wait_syncobjs(sc, file_priv, req->wait_count,
	    req->wait_ptr, &job->wait_fences, &job->wait_count);
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_wait_sync_us,
	    profile_start);
	if (err != 0)
		goto fail;
	err = nvkm_drm_job_prepare_deps(job);
	if (err != 0)
		goto fail;

	profile_start = nvkm_drm_profile_now_us(sc);
	err = nvkm_drm_prepare_signal_syncobjs(sc, file_priv, req->sig_count,
	    req->sig_ptr, job->done_fence, &job->exec.signals);
	nvkm_drm_profile_add_us(sc, &sc->exec_profile_prepare_signal_us,
	    profile_start);
	if (err != 0)
		goto fail;

	lockmgr(&nfile->job_submit_lock, LK_EXCLUSIVE);
	err = nvkm_drm_exec_attach_public_resv_fence(sc, nfile,
	    job->done_fence);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC attach reservation fence failed err=%d\n",
		    err);
	} else {
		nvkm_drm_exec_signals_publish(job->exec.signals,
		    job->exec.sig_count);
		err = nvkm_drm_job_enqueue(job);
	}
	lockmgr(&nfile->job_submit_lock, LK_RELEASE);
	if (err != 0)
		goto fail_signal;
	return (0);

fail_signal:
	nvkm_drm_job_signal(job, err);
fail:
	nvkm_drm_job_put(job);
	return (err);
}

/* ---- ioctl table ---- */
#define DRM_IOCTL_NOUVEAU_GETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GETPARAM, struct drm_nouveau_getparam)
#define DRM_IOCTL_NOUVEAU_VM_INIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_INIT, struct drm_nouveau_vm_init)
/* NVIF is variable-size; use DRM_IOWR with void marker */

static const struct drm_ioctl_desc nvkm_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(NOUVEAU_GETPARAM, nvkm_drm_ioctl_getparam,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_VM_INIT,  nvkm_drm_ioctl_vm_init,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_NVIF,     nvkm_drm_ioctl_nvif,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_CHANNEL_ALLOC, nvkm_drm_ioctl_channel_alloc,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_CHANNEL_FREE, nvkm_drm_ioctl_channel_free,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_NEW, nvkm_drm_ioctl_gem_new,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_INFO, nvkm_drm_ioctl_gem_info,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_CPU_PREP, nvkm_drm_ioctl_gem_cpu_prep,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_GEM_CPU_FINI, nvkm_drm_ioctl_gem_cpu_fini,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_VM_BIND, nvkm_drm_ioctl_vm_bind,
	    DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NOUVEAU_EXEC, nvkm_drm_ioctl_exec,
	    DRM_AUTH | DRM_RENDER_ALLOW),
};
