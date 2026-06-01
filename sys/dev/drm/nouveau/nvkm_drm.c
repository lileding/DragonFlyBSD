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

#include <drm/drmP.h>
#include <drm/drm_drv.h>
#include <drm/drm_syncobj.h>
#include <linux/dma-fence.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <machine/pmap.h>

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
static struct nvkm_softc *nvkm_drm_sc(struct drm_device *ddev);
static int nvkm_drm_open(struct drm_device *ddev, struct drm_file *file_priv);
static void nvkm_drm_postclose(struct drm_device *ddev,
    struct drm_file *file_priv);

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

static struct drm_driver nvkm_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ |
	    DRIVER_PRIME,
	.fops    = &nvkm_drm_fops,
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
	.gem_vm_ops = &nvkm_gem_pager_ops,
	.gem_free_object_unlocked = nvkm_bo_gem_free,
};

static uint64_t
nvkm_drm_profile_now_us(void)
{
	return ((uint64_t)ktime_to_us(ktime_get()));
}

static void
nvkm_drm_profile_add_us(uint64_t *total, uint64_t start_us)
{
	uint64_t end_us = nvkm_drm_profile_now_us();

	if (end_us >= start_us)
		*total += end_us - start_us;
}

struct nvkm_drm_vm_binding {
	LIST_ENTRY(nvkm_drm_vm_binding) link;
	uint64_t addr;
	uint64_t size;
	/* Borrowed GEM BO backing. The binding owns only the GPU VA mapping
	 * metadata/PTE state. The held GEM reference bounds this borrow lifetime;
	 * unmap drops the ref and must not release the BO backing allocation.
	 */
	struct drm_gem_object *obj;
};
LIST_HEAD(nvkm_drm_vm_binding_list, nvkm_drm_vm_binding);

struct nvkm_drm_chan_obj {
	uint32_t handle;
	uint32_t oclass;
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
	struct nvkm_drm_vm_binding_list vm_bindings;
	struct nvkm_drm_chan_list channels;
};

struct drm_nouveau_exec_push {
	uint64_t va;
	uint32_t va_len;
	uint32_t flags;
};

static uint32_t nvkm_drm_next_channel = 1;

static struct nvkm_drm_file *
nvkm_drm_file_priv(struct drm_file *file_priv)
{
	return (file_priv->driver_priv);
}

static bool
nvkm_drm_vm_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs)
{
	return (a < b + bs && b < a + as);
}

static int
nvkm_drm_vm_binding_wait(struct nvkm_softc *sc,
    struct nvkm_drm_vm_binding *binding, bool intr)
{
	struct nvkm_bo *bo = to_nvkm_bo(binding->obj);
	int err;

	sc->vm_bind_wait_count++;
	err = nvkm_bo_resv_wait(bo, intr);
	if (err != 0) {
		sc->vm_bind_wait_error_count++;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND wait failed addr=0x%016jx size=0x%016jx obj=%p err=%d\n",
		    (uintmax_t)binding->addr, (uintmax_t)binding->size,
		    binding->obj, err);
	}
	return (err);
}

static int
nvkm_drm_vm_bindings_remove(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, uint64_t addr, uint64_t size)
{
	struct nvkm_drm_vm_binding *binding, *next;
	int err = 0;

	LIST_FOREACH_MUTABLE(binding, &nfile->vm_bindings, link, next) {
		if (!nvkm_drm_vm_ranges_overlap(addr, size,
		    binding->addr, binding->size))
			continue;

		err = nvkm_drm_vm_binding_wait(sc, binding, true);
		if (err != 0)
			return (err);

		LIST_REMOVE(binding, link);
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND drop addr=0x%016jx size=0x%016jx obj=%p\n",
		    (uintmax_t)binding->addr, (uintmax_t)binding->size,
		    binding->obj);
		(void)nvkm_gsp_vmm_unmap(sc->gsp_vmm, binding->addr,
		    binding->size);
		drm_gem_object_put_unlocked(binding->obj);
		kfree(binding);
	}
	return (0);
}

static int
nvkm_drm_vm_binding_add(struct nvkm_drm_file *nfile, uint64_t addr,
    uint64_t size, struct drm_gem_object *obj)
{
	struct nvkm_drm_vm_binding *binding;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (binding == NULL)
		return (ENOMEM);

	binding->addr = addr;
	binding->size = size;
	binding->obj = obj;
	LIST_INSERT_HEAD(&nfile->vm_bindings, binding, link);
	return (0);
}

static int
nvkm_drm_flush_exec_pushes(struct nvkm_softc *sc, struct nvkm_drm_file *nfile,
    const struct drm_nouveau_exec_push *pushes, uint32_t push_count)
{
	struct nvkm_drm_vm_binding *binding;
	uint64_t flush_seq;
	uint32_t scanned = 0;
	uint32_t flushed = 0;
	int err = 0;

	flush_seq = ++sc->exec_cpu_flush_seq;
	for (uint32_t i = 0; i < push_count; i++) {
		bool found = false;

		LIST_FOREACH(binding, &nfile->vm_bindings, link) {
			struct nvkm_bo *bo = to_nvkm_bo(binding->obj);

			scanned++;
			if (!nvkm_drm_vm_ranges_overlap(pushes[i].va,
			    pushes[i].va_len, binding->addr, binding->size))
				continue;
			found = true;
			if (bo->kva == NULL)
				break;
			if (bo->cpu_flush_seq == flush_seq)
				break;
			pmap_invalidate_cache_range((vm_offset_t)bo->kva,
			    (vm_offset_t)bo->kva + binding->obj->size);
			bo->cpu_flush_seq = flush_seq;
			flushed++;
			break;
		}
		if (!found) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC push has no VM binding idx=%u va=0x%016jx len=0x%08x\n",
			    i, (uintmax_t)pushes[i].va, pushes[i].va_len);
			err = -EINVAL;
			break;
		}
	}
	sc->exec_profile_cpu_bind_scanned += scanned;
	sc->exec_profile_cpu_bind_flushed += flushed;
	if (flushed != 0)
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC flushed %u push BOs\n", flushed);
	return (err);
}

static void
nvkm_drm_dump_push_buffer(struct nvkm_softc *sc, struct nvkm_drm_file *nfile,
    uint64_t va, uint32_t va_len)
{
	struct nvkm_drm_vm_binding *binding;

	LIST_FOREACH(binding, &nfile->vm_bindings, link) {
		struct nvkm_bo *bo;
		uint64_t offset;
		uint32_t *dw;
		uint32_t count;

		if (va < binding->addr ||
		    va + va_len > binding->addr + binding->size)
			continue;

		bo = to_nvkm_bo(binding->obj);
		offset = va - binding->addr;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC dump push va=0x%016jx len=0x%08x binding=0x%016jx+0x%016jx obj=%p domain=0x%x paddr=0x%016jx offset=0x%jx cpu_map=%u\n",
		    (uintmax_t)va, va_len, (uintmax_t)binding->addr,
		    (uintmax_t)binding->size, binding->obj, bo->domain,
		    (uintmax_t)bo->paddr, (uintmax_t)offset, bo->kva != NULL);
		if (bo->kva == NULL)
			return;

		dw = (uint32_t *)((uint8_t *)bo->kva + offset);
		count = va_len / sizeof(uint32_t);
		if (count > 48)
			count = 48;
		for (uint32_t i = 0; i < count; i += 4) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: EXEC push[%02u]=%08x %08x %08x %08x\n",
			    i, dw[i + 0],
			    (i + 1 < count) ? dw[i + 1] : 0,
			    (i + 2 < count) ? dw[i + 2] : 0,
			    (i + 3 < count) ? dw[i + 3] : 0);
		}
		return;
	}

	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC dump push va=0x%016jx len=0x%08x no binding\n",
	    (uintmax_t)va, va_len);
}

static void
nvkm_drm_dump_large_push(struct nvkm_softc *sc, struct nvkm_drm_file *nfile,
    uint64_t va, uint32_t va_len)
{
	if (va_len >= 0xa0)
		nvkm_drm_dump_push_buffer(sc, nfile, va, va_len);
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

	err = drm_dev_register(ddev, 0);
	if (err != 0) {
		nvkm_debugf(sc->dev,
		    "drm: drm_dev_register failed (%d)\n", err);
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

/* TU102 supported object classes (low byte = engine type). */
static const struct nvif_ioctl_sclass_oclass_v0 nvkm_tu102_classes[] = {
	{ .oclass = 0xc597 },    /* TURING_A          — 3D */
	{ .oclass = 0xc5c0 },    /* TURING_COMPUTE_A  — compute */
	{ .oclass = 0xc5b5 },    /* TURING_DMA_COPY_A — copy */
	{ .oclass = 0x902d },    /* FERMI_TWOD_A      — 2D */
	{ .oclass = 0xa140 },    /* KEPLER_INLINE_TO_MEMORY_B — m2mf; Turing reports this, not 0x9039 (NVK queue init asserts if M2MF<=FERMI) */
};
#define NVKM_TU102_NUM_CLASSES \
	(sizeof(nvkm_tu102_classes) / sizeof(nvkm_tu102_classes[0]))

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
nvkm_drm_channel_clear(struct nvkm_drm_chan *dchan)
{
	if (dchan->chan != NULL) {
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
	uint32_t binding_count = 0, channel_count = 0;

	if (nfile == NULL)
		return;

	LIST_FOREACH_MUTABLE(binding, &nfile->vm_bindings, link,
	    binding_next) {
		LIST_REMOVE(binding, link);
		(void)nvkm_gsp_vmm_unmap(sc->gsp_vmm, binding->addr,
		    binding->size);
		drm_gem_object_put_unlocked(binding->obj);
		kfree(binding);
		binding_count++;
	}

	LIST_FOREACH_MUTABLE(dchan, &nfile->channels, link, dchan_next) {
		LIST_REMOVE(dchan, link);
		nvkm_drm_channel_clear(dchan);
		channel_count++;
	}

	nvkm_debugf(sc->dev,
	    "nvkm_drm: postclose released bindings=%u channels=%u\n",
	    binding_count, channel_count);
	kfree(nfile);
	file_priv->driver_priv = NULL;
}

static int
nvkm_drm_open(struct drm_device *ddev __unused, struct drm_file *file_priv)
{
	struct nvkm_drm_file *nfile;

	nfile = kzalloc(sizeof(*nfile), GFP_KERNEL);
	if (nfile == NULL)
		return (-ENOMEM);
	LIST_INIT(&nfile->vm_bindings);
	LIST_INIT(&nfile->channels);
	file_priv->driver_priv = nfile;
	return (0);
}

static void
nvkm_drm_postclose(struct drm_device *ddev, struct drm_file *file_priv)
{
	nvkm_drm_file_release(ddev, file_priv);
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
		/* TU102 chipset id 0x162. We could query GSP-RM but it's
		 * a known value for our hardware. */
		gp->value = 0x162;
		break;
	case NOUVEAU_GETPARAM_BUS_TYPE:
		gp->value = 3;	/* NV_DEVICE_INFO_V0_PCIE */
		break;
	case NOUVEAU_GETPARAM_FB_SIZE:
		gp->value = sc->fb_usable_size;
		break;
	case NOUVEAU_GETPARAM_VRAM_BAR_SIZE:
		gp->value = sc->bar_res[1] != NULL ?
		    rman_get_size(sc->bar_res[1]) : 0;
		break;
	case NOUVEAU_GETPARAM_EXEC_PUSH_MAX:
		gp->value = 512;	/* default per NVK winsys */
		break;
	case NOUVEAU_GETPARAM_GRAPH_UNITS:
		/* NVK interprets low 8 bits as GPC count and bits 8..23 as TPC count.
		 * TU102 / RTX 2080 Ti has 6 GPCs and 34 TPCs (68 SMs / 2 MPs per TPC). */
		gp->value = (34ULL << 8) | 6ULL;
		break;
	case NOUVEAU_GETPARAM_VRAM_USED:
		/* NVK asserts >0 on success; force fail so NVK uses 0 fallback. */
		return (-EINVAL);
	case NOUVEAU_GETPARAM_PTIMER_TIME:
		/* TODO: read GPU PTIMER. */
		gp->value = 0;
		break;
	case NOUVEAU_GETPARAM_HAS_VMA_TILEMODE:
		gp->value = 0;
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
		switch (new_->oclass) {
		case 0xc597: case 0xc5c0: case 0xc5b5:
		case 0x902d: case 0xa140:
			if (nfile == NULL)
				return (-ENXIO);
			dchan = nvkm_drm_channel_find(nfile,
			    (uint32_t)hdr->token);
			if (dchan == NULL || dchan->chan == NULL)
				return (-ENOENT);
			if (new_->oclass == 0xc597 || new_->oclass == 0xc5c0 ||
			    new_->oclass == 0x902d || new_->oclass == 0xa140) {
				err = nvkm_gsp_chan_promote_gr_ctx(sc->gsp_vmm,
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
			return (0);
		}
		return (-EINVAL);
	}
	case NVIF_IOCTL_V0_MTHD: {
		struct nvif_ioctl_mthd_v0 *mthd = (void *)hdr->data;
		if (mthd->method == NV_DEVICE_V0_INFO) {
			struct nv_device_info_v0 *info = (void *)mthd->data;
			memset(info, 0, sizeof(*info));
			info->version  = 0;
			info->platform = NV_DEVICE_INFO_V0_PCIE;
			info->chipset  = 0x162;	/* TU102 */
			info->revision = pci_get_revid(sc->dev);
			info->family   = 0x0a;	/* TURING per nv_device.h family enum */
			info->ram_size = sc->fb_usable_size;
			info->ram_user = sc->fb_usable_size;
			strncpy(info->chip, "TU102", sizeof(info->chip));
			strncpy(info->name, "NVIDIA GeForce RTX 2080 Ti",
			    sizeof(info->name));
			nvkm_debugf(sc->dev,
			    "nvkm_drm: NVIF MTHD DEVICE_INFO -> TU102 ram=0x%llx user=0x%llx bar1=0x%llx\n",
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
		uint32_t want = sc_->count;
		uint32_t fill = want < NVKM_TU102_NUM_CLASSES ?
		    want : NVKM_TU102_NUM_CLASSES;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: NVIF SCLASS want=%u fill=%u\n", want, fill);
		for (uint32_t i = 0; i < fill; i++)
			sc_->oclass[i] = nvkm_tu102_classes[i];
		sc_->count = fill;
		return (0);
	}
	case NVIF_IOCTL_V0_DEL:
		return (0);
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
	LIST_FOREACH(dchan, &nfile->channels, link)
		channel_count++;
	if (channel_count >= NVKM_DRM_MAX_CHANNELS)
		return (-ENOMEM);

	dchan = kzalloc(sizeof(*dchan), GFP_KERNEL);
	if (dchan == NULL)
		return (-ENOMEM);
	engine_type = (req->tt_ctxdma_handle == NOUVEAU_FIFO_ENGINE_CE) ?
	    NV2080_ENGINE_TYPE_COPY0 : NV2080_ENGINE_TYPE_GRAPHICS;
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
	err = nvkm_gsp_chan_ctor(sc->gsp_vmm, engine_type, dchan->chan);
	lwkt_reltoken(&sc->gsp_tok);
	if (err != 0) {
		kfree(dchan->chan);
		kfree(dchan);
		return (-err);
	}

	dchan->id = nvkm_drm_next_channel++;
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
	dchan = nvkm_drm_channel_find(nfile, (uint32_t)req->channel);
	if (dchan != NULL) {
		LIST_REMOVE(dchan, link);
		nvkm_drm_channel_clear(dchan);
	}
	nvkm_debugf(sc->dev,
	    "nvkm_drm: CHANNEL_FREE channel=%d\n", req->channel);
	return (0);
}

/* ---- DRM_NOUVEAU_VM_BIND ---- */
#define DRM_NOUVEAU_VM_BIND_OP_MAP	0x0
#define DRM_NOUVEAU_VM_BIND_OP_UNMAP	0x1
#define DRM_NOUVEAU_VM_BIND_SPARSE	(1 << 8)

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

static int
nvkm_drm_ioctl_vm_bind(struct drm_device *ddev, void *data,
    struct drm_file *file_priv)
{
	struct nvkm_softc *sc = nvkm_drm_sc(ddev);
	struct nvkm_drm_file *nfile = nvkm_drm_file_priv(file_priv);
	struct drm_nouveau_vm_bind *req = data;
	struct drm_nouveau_vm_bind_op *ops;
	int err = 0;

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
	if (req->op_count == 0)
		return (0);
	if (req->op_count > 1024) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND too many ops=%u\n", req->op_count);
		return (-EINVAL);
	}
	if (req->wait_count != 0 || req->sig_count != 0) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND unsupported sync waits=%u sigs=%u\n",
		    req->wait_count, req->sig_count);
		return (-EINVAL);
	}

	ops = kmalloc(sizeof(*ops) * req->op_count, M_TEMP, M_WAITOK);
	err = copyin((const void *)(uintptr_t)req->op_ptr, ops,
	    sizeof(*ops) * req->op_count);
	if (err != 0) {
		kfree(ops);
		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND copyin failed ops=%u err=%d\n",
		    req->op_count, err);
		return (-EFAULT);
	}

	for (uint32_t i = 0; i < req->op_count; i++) {
		struct drm_nouveau_vm_bind_op *op = &ops[i];

		if (op->range == 0 ||
		    ((op->addr | op->bo_offset | op->range) &
		     (NVKM_GMMU_PT_PAGE_SIZE - 1))) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND invalid idx=%u op=%u flags=0x%08x handle=%u addr=0x%016jx bo_off=0x%016jx range=0x%016jx\n",
			    i, op->op, op->flags, op->handle,
			    (uintmax_t)op->addr, (uintmax_t)op->bo_offset,
			    (uintmax_t)op->range);
			err = -EINVAL;
			break;
		}

		if (op->op == DRM_NOUVEAU_VM_BIND_OP_UNMAP) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND unmap idx=%u op=%u flags=0x%08x handle=%u addr=0x%016jx range=0x%016jx\n",
			    i, op->op, op->flags, op->handle, (uintmax_t)op->addr,
			    (uintmax_t)op->range);
			err = nvkm_drm_vm_bindings_remove(sc, nfile, op->addr,
			    op->range);
			if (err != 0)
				break;
			if ((op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0) {
				err = nvkm_gsp_vmm_unmap_sparse(sc->gsp_vmm,
				    op->addr, op->range);
			} else {
				err = nvkm_gsp_vmm_unmap(sc->gsp_vmm,
				    op->addr, op->range);
			}
			if (err != 0) {
				err = -err;
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND unmap failed idx=%u err=%d\n",
				    i, err);
			}
			continue;
		}

		if (op->op == DRM_NOUVEAU_VM_BIND_OP_MAP) {
			struct drm_gem_object *obj;
			struct nvkm_bo *bo;

			if ((op->flags & DRM_NOUVEAU_VM_BIND_SPARSE) != 0) {
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND map sparse idx=%u flags=0x%08x handle=%u addr=0x%016jx range=0x%016jx\n",
				    i, op->flags, op->handle,
				    (uintmax_t)op->addr, (uintmax_t)op->range);
				err = nvkm_gsp_vmm_map_sparse(sc->gsp_vmm,
				    op->addr, op->range);
				if (err != 0) {
					err = -err;
					nvkm_debugf(sc->dev,
					    "nvkm_drm: VM_BIND map sparse failed idx=%u err=%d\n",
					    i, err);
					break;
				}
				continue;
			}

			if (op->handle == 0) {
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND map-null idx=%u flags=0x%08x addr=0x%016jx range=0x%016jx\n",
				    i, op->flags, (uintmax_t)op->addr,
				    (uintmax_t)op->range);
				err = nvkm_drm_vm_bindings_remove(sc, nfile,
				    op->addr, op->range);
				if (err != 0)
					break;
				err = nvkm_gsp_vmm_unmap(sc->gsp_vmm,
				    op->addr, op->range);
				if (err != 0) {
					err = -err;
					nvkm_debugf(sc->dev,
					    "nvkm_drm: VM_BIND map-null failed idx=%u err=%d\n",
					    i, err);
					break;
				}
				continue;
			}

			obj = drm_gem_object_lookup(file_priv, op->handle);
			if (obj == NULL) {
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND missing BO idx=%u handle=%u\n",
				    i, op->handle);
				err = -ENOENT;
				break;
			}
			bo = to_nvkm_bo(obj);
			if (op->bo_offset > obj->size ||
			    op->range > obj->size - op->bo_offset) {
				drm_gem_object_put_unlocked(obj);
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND BO range invalid idx=%u handle=%u bo_off=0x%016jx range=0x%016jx size=0x%016jx\n",
				    i, op->handle, (uintmax_t)op->bo_offset,
				    (uintmax_t)op->range, (uintmax_t)obj->size);
				err = -EINVAL;
				break;
			}
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND map idx=%u flags=0x%08x handle=%u obj=%p domain=0x%x addr=0x%016jx bo_off=0x%016jx range=0x%016jx paddr=0x%016jx\n",
			    i, op->flags, op->handle, obj, bo->domain,
			    (uintmax_t)op->addr, (uintmax_t)op->bo_offset,
			    (uintmax_t)op->range,
			    (uintmax_t)(bo->paddr + (vm_paddr_t)op->bo_offset));
			sc->vm_bind_wait_count++;
			err = nvkm_bo_resv_wait(bo, true);
			if (err != 0) {
				sc->vm_bind_wait_error_count++;
				drm_gem_object_put_unlocked(obj);
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND BO wait failed idx=%u handle=%u err=%d\n",
				    i, op->handle, err);
				break;
			}
			err = nvkm_drm_vm_bindings_remove(sc, nfile, op->addr,
			    op->range);
			if (err != 0) {
				drm_gem_object_put_unlocked(obj);
				break;
			}
			if (bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) {
				err = nvkm_gsp_vmm_map_vram(sc->gsp_vmm,
				    op->addr, bo->paddr + op->bo_offset,
				    op->range);
			} else {
				err = nvkm_gsp_vmm_map_sysmem_kva(sc->gsp_vmm,
				    op->addr, (uint8_t *)bo->kva + op->bo_offset,
				    op->range);
			}
			if (err != 0) {
				drm_gem_object_put_unlocked(obj);
				err = -err;
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND map failed idx=%u err=%d\n",
				    i, err);
				break;
			}
			err = nvkm_drm_vm_binding_add(nfile, op->addr,
			    op->range, obj);
			if (err != 0) {
				(void)nvkm_gsp_vmm_unmap(sc->gsp_vmm,
				    op->addr, op->range);
				drm_gem_object_put_unlocked(obj);
				err = -err;
				nvkm_debugf(sc->dev,
				    "nvkm_drm: VM_BIND track failed idx=%u err=%d\n",
				    i, err);
				break;
			}
			nvkm_debugf(sc->dev,
			    "nvkm_drm: VM_BIND track addr=0x%016jx size=0x%016jx obj=%p\n",
			    (uintmax_t)op->addr, (uintmax_t)op->range, obj);
			continue;
		}

		nvkm_debugf(sc->dev,
		    "nvkm_drm: VM_BIND unknown op idx=%u op=%u flags=0x%08x\n",
		    i, op->op, op->flags);
		err = -EINVAL;
		break;
	}

	kfree(ops);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: VM_BIND complete ops=%u err=%d\n",
	    req->op_count, err);
	return (err);
}

/* ---- DRM_NOUVEAU_EXEC ---- */
#define DRM_NOUVEAU_SYNC_SYNCOBJ	0x0
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ	0x1
#define DRM_NOUVEAU_SYNC_TYPE_MASK	0xf

struct drm_nouveau_sync {
	uint32_t flags;
	uint32_t handle;
	uint64_t timeline_value;
};

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
#define NVKM_DRM_GPFIFO_ENTRIES		512
#define NVKM_DRM_GPFIFO_FETCH_WINDOW	0x40
#define NVKM_DRM_EXEC_POLL_US		5000000

struct nvkm_drm_exec_fence {
	struct dma_fence base;
	spinlock_t lock;
};

struct nvkm_drm_exec_signal {
	struct dma_fence *fence;
};

static const char *
nvkm_drm_fence_name(struct dma_fence *fence __unused)
{
	return ("nvkm-drm");
}

static const struct dma_fence_ops nvkm_drm_fence_ops = {
	.get_driver_name = nvkm_drm_fence_name,
	.get_timeline_name = nvkm_drm_fence_name,
	.wait = dma_fence_default_wait,
};

static struct dma_fence *
nvkm_drm_exec_fence_create(struct nvkm_softc *sc, unsigned seqno)
{
	struct nvkm_drm_exec_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (f == NULL)
		return (NULL);

	lockinit(&f->lock, "nvdfen", 0, 0);
	dma_fence_init(&f->base, &nvkm_drm_fence_ops, &f->lock,
	    sc->fence_context, seqno);
	return (&f->base);
}

static int
nvkm_drm_wait_syncobjs(struct nvkm_softc *sc, struct drm_file *file_priv,
    uint32_t count, uint64_t wait_ptr)
{
	struct drm_nouveau_sync *waits;
	int err = 0;

	if (count == 0)
		return (0);
	sc->sync_wait_count += count;
	if (count > 64) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync wait invalid count=%u\n", count);
		return (-EINVAL);
	}

	waits = kmalloc(sizeof(*waits) * count, M_TEMP, M_WAITOK);
	err = copyin((const void *)(uintptr_t)wait_ptr, waits,
	    sizeof(*waits) * count);
	if (err != 0) {
		kfree(waits);
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync wait copyin failed count=%u err=%d\n",
		    count, err);
		return (-EFAULT);
	}

	for (uint32_t i = 0; i < count; i++) {
		struct dma_fence *fence;
		uint32_t type = waits[i].flags & DRM_NOUVEAU_SYNC_TYPE_MASK;
		int ret;

		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync wait idx=%u flags=0x%08x handle=%u timeline=0x%016jx\n",
		    i, waits[i].flags, waits[i].handle,
		    (uintmax_t)waits[i].timeline_value);
		if (type != DRM_NOUVEAU_SYNC_SYNCOBJ &&
		    type != DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync wait unsupported flags idx=%u flags=0x%08x\n",
			    i, waits[i].flags);
			err = -EINVAL;
			break;
		}

		ret = drm_syncobj_find_fence(file_priv, waits[i].handle,
		    type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ ?
		    waits[i].timeline_value : 0, &fence);
		if (ret != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync wait missing fence idx=%u handle=%u err=%d\n",
			    i, waits[i].handle, ret);
			err = ret;
			break;
		}
		if (type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ &&
		    fence->seqno < waits[i].timeline_value) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync wait point not ready idx=%u handle=%u have=0x%08x want=0x%016jx\n",
			    i, waits[i].handle, fence->seqno,
			    (uintmax_t)waits[i].timeline_value);
			dma_fence_put(fence);
			err = -ETIME;
			break;
		}

		ret = dma_fence_wait(fence, true);
		dma_fence_put(fence);
		if (ret != 0) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync wait failed idx=%u handle=%u err=%d\n",
			    i, waits[i].handle, ret);
			err = ret;
			break;
		}
	}

	kfree(waits);
	if (err != 0) {
		sc->sync_wait_error_count++;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync wait failed count=%u err=%d\n",
		    count, err);
	}
	return (err);
}

static void
nvkm_drm_exec_signals_put(struct nvkm_drm_exec_signal *signals,
    uint32_t count)
{
	if (signals == NULL)
		return;
	for (uint32_t i = 0; i < count; i++)
		dma_fence_put(signals[i].fence);
	kfree(signals);
}

static void
nvkm_drm_exec_signals_signal(struct nvkm_drm_exec_signal *signals,
    uint32_t count, int error)
{
	if (signals == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		if (error != 0)
			dma_fence_set_error(signals[i].fence, error);
		(void)dma_fence_signal(signals[i].fence);
	}
}

static void
nvkm_drm_exec_pending_signal(struct nvkm_drm_exec_pending *pending, int error)
{
	for (uint32_t i = 0; i < pending->fence_count; i++) {
		if (error != 0)
			dma_fence_set_error(pending->fences[i], error);
		(void)dma_fence_signal(pending->fences[i]);
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
		nvkm_drm_exec_pending_signal(pending, 0);
		sc->exec_async_complete_count++;
		atomic_store_rel_int(&pending->done, 1);
		wakeup(pending);
	}
	lwkt_reltoken(&sc->gsp_tok);
}

static int
nvkm_drm_prepare_signal_syncobjs(struct nvkm_softc *sc,
    struct drm_file *file_priv, uint32_t count, uint64_t sig_ptr,
    struct nvkm_drm_exec_signal **psignals)
{
	struct drm_nouveau_sync *sigs;
	struct drm_syncobj **syncobjs;
	struct nvkm_drm_exec_signal *signals;
	int err = 0;

	*psignals = NULL;
	if (count == 0)
		return (0);
	sc->sync_signal_count += count;
	if (count > 64) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: sync signal invalid count=%u\n", count);
		return (-EINVAL);
	}

	sigs = kmalloc(sizeof(*sigs) * count, M_TEMP, M_WAITOK);
	syncobjs = kcalloc(count, sizeof(*syncobjs), GFP_KERNEL);
	signals = kcalloc(count, sizeof(*signals), GFP_KERNEL);
	if (syncobjs == NULL || signals == NULL) {
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
		unsigned seqno;

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
		syncobjs[i] = drm_syncobj_find(file_priv, sigs[i].handle);
		if (syncobjs[i] == NULL) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync signal missing syncobj idx=%u handle=%u\n",
			    i, sigs[i].handle);
			err = -ENOENT;
			goto out_free_arrays;
		}

		if (type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ)
			seqno = (unsigned)sigs[i].timeline_value;
		else
			seqno = ++sc->fence_seqno;
		signals[i].fence = nvkm_drm_exec_fence_create(sc, seqno);
		if (signals[i].fence == NULL) {
			nvkm_debugf(sc->dev,
			    "nvkm_drm: sync signal fence alloc failed idx=%u handle=%u\n",
			    i, sigs[i].handle);
			err = -ENOMEM;
			goto out_free_arrays;
		}
	}

	for (uint32_t i = 0; i < count; i++) {
		uint32_t type = sigs[i].flags & DRM_NOUVEAU_SYNC_TYPE_MASK;
		u64 point = type == DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ ?
		    sigs[i].timeline_value : 0;

		drm_syncobj_replace_fence(syncobjs[i], point, signals[i].fence);
	}

	*psignals = signals;
	signals = NULL;

out_free_arrays:
	if (syncobjs != NULL) {
		for (uint32_t i = 0; i < count; i++) {
			if (syncobjs[i] != NULL)
				drm_syncobj_put(syncobjs[i]);
		}
		kfree(syncobjs);
	}
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

static void
nvkm_drm_exec_attach_reservations(struct nvkm_softc *sc,
    struct nvkm_drm_file *nfile, struct dma_fence *fence)
{
	struct nvkm_drm_vm_binding *binding;
	uint32_t count = 0;

	if (fence == NULL)
		return;

	sc->exec_resv_attach_calls++;
	LIST_FOREACH(binding, &nfile->vm_bindings, link) {
		struct nvkm_bo *bo = to_nvkm_bo(binding->obj);

		nvkm_bo_resv_add_excl_fence(bo, fence);
		count++;
	}
	sc->exec_resv_attach_bos += count;
}

static void
nvkm_drm_dump_ctxctl_unit(struct nvkm_softc *sc, uint32_t base)
{
	nvkm_debugf(sc->dev,
	    "nvkm_drm: ctxctl[%06x] done=%08x stat=%08x %08x %08x %08x stat2=%08x %08x %08x %08x\n",
	    base, nvkm_rd32(sc, base + 0x400),
	    nvkm_rd32(sc, base + 0x800), nvkm_rd32(sc, base + 0x804),
	    nvkm_rd32(sc, base + 0x808), nvkm_rd32(sc, base + 0x80c),
	    nvkm_rd32(sc, base + 0x810), nvkm_rd32(sc, base + 0x814),
	    nvkm_rd32(sc, base + 0x818), nvkm_rd32(sc, base + 0x81c));
}

static void
nvkm_drm_dump_exec_timeout(struct nvkm_softc *sc)
{
	uint32_t gpcnr;

	nvkm_debugf(sc->dev,
	    "nvkm_drm: GR intr=%08x exc=%08x exc1=%08x cls_err=%08x trap_addr=%08x trap_data=%08x trap_hi=%08x status=%08x status1=%08x status2=%08x engine=%08x\n",
	    nvkm_rd32(sc, 0x400100), nvkm_rd32(sc, 0x400108),
	    nvkm_rd32(sc, 0x400118), nvkm_rd32(sc, 0x400110),
	    nvkm_rd32(sc, 0x400704), nvkm_rd32(sc, 0x400708),
	    nvkm_rd32(sc, 0x40070c), nvkm_rd32(sc, 0x400700),
	    nvkm_rd32(sc, 0x400604), nvkm_rd32(sc, 0x400608),
	    nvkm_rd32(sc, 0x40060c));
	nvkm_debugf(sc->dev,
	    "nvkm_drm: GR activity0=%08x activity1=%08x sked_activity=%08x grfifo_ctl=%08x grfifo_status=%08x\n",
	    nvkm_rd32(sc, 0x400380), nvkm_rd32(sc, 0x400384),
	    nvkm_rd32(sc, 0x407054), nvkm_rd32(sc, 0x400500),
	    nvkm_rd32(sc, 0x400504));
	nvkm_debugf(sc->dev,
	    "nvkm_drm: FECS inst=%08x cfg=%08x intr=%08x code=%08x class=%08x addr=%08x data=%08x\n",
	    nvkm_rd32(sc, 0x409b00), nvkm_rd32(sc, 0x409604),
	    nvkm_rd32(sc, 0x409c18), nvkm_rd32(sc, 0x409814),
	    nvkm_rd32(sc, 0x409808), nvkm_rd32(sc, 0x40980c),
	    nvkm_rd32(sc, 0x409810));

	gpcnr = nvkm_rd32(sc, 0x409604) & 0xffff;
	if (gpcnr > 8)
		gpcnr = 8;
	nvkm_drm_dump_ctxctl_unit(sc, 0x409000);
	for (uint32_t gpc = 0; gpc < gpcnr; gpc++)
		nvkm_drm_dump_ctxctl_unit(sc, 0x502000 + gpc * 0x8000);
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
	struct drm_nouveau_exec_push *pushes;
	struct nvkm_drm_exec_signal *signals = NULL;
	struct nvkm_drm_exec_pending pending;
	struct dma_fence *exec_fence = NULL;
	uint32_t *gpf, *post, *sema;
	uint64_t slot_bar1;
	uint32_t put, payload;
	uint64_t profile_start;
	uint64_t profile_push_start = 0;
	long wait_ret;
	int err = 0;
	bool gsp_tok_held = false;
	bool exec_completion_queued = false;

	if (nfile == NULL)
		return (-ENXIO);
	sc->exec_submit_count++;
	dchan = nvkm_drm_channel_find(nfile, req->channel);
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC begin channel=%u pushes=%u waits=%u sigs=%u\n",
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
	profile_start = nvkm_drm_profile_now_us();
	err = nvkm_drm_wait_syncobjs(sc, file_priv, req->wait_count,
	    req->wait_ptr);
	nvkm_drm_profile_add_us(&sc->exec_profile_wait_sync_us,
	    profile_start);
	if (err != 0)
		return (err);
	if (req->push_count == 0) {
		sc->exec_signal_only_count++;
		profile_start = nvkm_drm_profile_now_us();
		lwkt_gettoken(&sc->gsp_tok);
		nvkm_drm_profile_add_us(&sc->exec_profile_token_wait_us,
		    profile_start);
		profile_start = nvkm_drm_profile_now_us();
		err = nvkm_drm_prepare_signal_syncobjs(sc, file_priv,
		    req->sig_count, req->sig_ptr, &signals);
		nvkm_drm_profile_add_us(&sc->exec_profile_prepare_signal_us,
		    profile_start);
		if (err == 0)
			nvkm_drm_exec_signals_signal(signals, req->sig_count, 0);
		profile_start = nvkm_drm_profile_now_us();
		nvkm_drm_exec_signals_put(signals, req->sig_count);
		nvkm_drm_profile_add_us(&sc->exec_profile_cleanup_us,
		    profile_start);
		lwkt_reltoken(&sc->gsp_tok);
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC signal-only channel=%u sigs=%u err=%d\n",
		    req->channel, req->sig_count, err);
		return (err);
	}
	if (req->push_count > NVKM_DRM_GPFIFO_ENTRIES - 2) {
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC too many pushes channel=%u pushes=%u\n",
		    req->channel, req->push_count);
		return (-EINVAL);
	}

	pushes = kmalloc(sizeof(*pushes) * req->push_count, M_TEMP, M_WAITOK);
	err = copyin((const void *)(uintptr_t)req->push_ptr, pushes,
	    sizeof(*pushes) * req->push_count);
	if (err != 0) {
		kfree(pushes);
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC push copyin failed channel=%u pushes=%u err=%d\n",
		    req->channel, req->push_count, err);
		return (-EFAULT);
	}

	profile_start = nvkm_drm_profile_now_us();
	lwkt_gettoken(&sc->gsp_tok);
	gsp_tok_held = true;
	nvkm_drm_profile_add_us(&sc->exec_profile_token_wait_us,
	    profile_start);
	profile_push_start = nvkm_drm_profile_now_us();
	gpf = (uint32_t *)chan->submit_gpf.kva;
	post = (uint32_t *)chan->submit_push.kva;
	sema = (uint32_t *)chan->submit_sema.kva;
	slot_bar1 = chan->userd_bar2_gva +
	    (uint64_t)((uint32_t)chan->chid % 8u) *
	    NV_USERD_SLOT_SIZE;

	put = nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_PUT) &
	    (NVKM_DRM_GPFIFO_ENTRIES - 1);
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
		gpf[put * 2 + 0] = entry0;
		gpf[put * 2 + 1] = entry1;
		put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	}
	sc->exec_profile_pushes += req->push_count;

	post[0] = NVC36F_PUSH_HDR_SEM_ADDR_TRIPLET;
	post[1] = (uint32_t)(chan->submit_gva_sema & 0xffffffffu);
	post[2] = (uint32_t)((chan->submit_gva_sema >> 32) & 0xffu);
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
	gpf[put * 2 + 0] = (uint32_t)(chan->submit_gva_push & 0xffffffffu);
	gpf[put * 2 + 1] = (uint32_t)((chan->submit_gva_push >> 32) & 0xffu) |
	    (NVKM_DRM_POST_PUSH_DWORDS << NVC06F_GP_ENTRY1_LENGTH_SHIFT);
	put = (put + 1) & (NVKM_DRM_GPFIFO_ENTRIES - 1);
	nvkm_drm_profile_add_us(&sc->exec_profile_push_build_us,
	    profile_push_start);

	profile_start = nvkm_drm_profile_now_us();
	err = nvkm_drm_prepare_signal_syncobjs(sc, file_priv, req->sig_count,
	    req->sig_ptr, &signals);
	nvkm_drm_profile_add_us(&sc->exec_profile_prepare_signal_us,
	    profile_start);
	if (err != 0)
		goto out_unlock;
	if (signals != NULL && req->sig_count != 0) {
		exec_fence = signals[0].fence;
		sc->exec_signal_fence_count++;
	} else {
		exec_fence = nvkm_drm_exec_fence_create(sc, ++sc->fence_seqno);
		if (exec_fence == NULL) {
			err = -ENOMEM;
			goto out_unlock;
		}
		sc->exec_internal_fence_count++;
	}
	profile_start = nvkm_drm_profile_now_us();
	nvkm_drm_exec_attach_reservations(sc, nfile, exec_fence);
	nvkm_drm_profile_add_us(&sc->exec_profile_attach_resv_us,
	    profile_start);

	memset(&pending, 0, sizeof(pending));
	pending.sema = (volatile uint32_t *)sema;
	pending.payload = payload;
	if (signals != NULL && req->sig_count != 0) {
		pending.fence_count = req->sig_count;
		for (uint32_t i = 0; i < req->sig_count; i++)
			pending.fences[i] = signals[i].fence;
	} else {
		pending.fence_count = 1;
		pending.fences[0] = exec_fence;
	}

	profile_start = nvkm_drm_profile_now_us();
	err = nvkm_drm_flush_exec_pushes(sc, nfile, pushes, req->push_count);
	nvkm_drm_profile_add_us(&sc->exec_profile_flush_cpu_us,
	    profile_start);
	if (err != 0)
		goto out_unlock;
	profile_start = nvkm_drm_profile_now_us();
	pmap_invalidate_cache_range((vm_offset_t)chan->submit_gpf.kva,
	    (vm_offset_t)chan->submit_gpf.kva + 0x1000);
	pmap_invalidate_cache_range((vm_offset_t)chan->submit_push.kva,
	    (vm_offset_t)chan->submit_push.kva + 0x1000);
	pmap_invalidate_cache_range((vm_offset_t)chan->submit_sema.kva,
	    (vm_offset_t)chan->submit_sema.kva + 0x1000);
	cpu_sfence();
	nvkm_drm_profile_add_us(&sc->exec_profile_cache_flush_us,
	    profile_start);
	LIST_INSERT_HEAD(&sc->exec_pending, &pending, link);
	exec_completion_queued = true;
	sc->exec_async_pending_count++;
	profile_start = nvkm_drm_profile_now_us();
	nvkm_gsp_bar1_wr32(sc, slot_bar1 + NV_USERD_GP_PUT, put);
	cpu_sfence();
	(void)nvkm_gsp_bar1_rd32(sc, slot_bar1 + 0);
	nvkm_wr32(sc, NV_USERMODE_DOORBELL, chan->gsp_token);
	cpu_sfence();
	nvkm_drm_profile_add_us(&sc->exec_profile_doorbell_us,
	    profile_start);

	profile_start = nvkm_drm_profile_now_us();
	lwkt_reltoken(&sc->gsp_tok);
	gsp_tok_held = false;
	sc->exec_async_wait_count++;
	wait_ret = dma_fence_wait_timeout(exec_fence, false, 5 * hz);
	nvkm_drm_profile_add_us(&sc->exec_profile_poll_us, profile_start);
	if (wait_ret <= 0) {
		bool completed;

		lwkt_gettoken(&sc->gsp_tok);
		completed = atomic_load_acq_int(&pending.done) != 0;
		if (!completed)
			LIST_REMOVE(&pending, link);
		lwkt_reltoken(&sc->gsp_tok);
		if (completed)
			goto exec_complete;
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC timeout channel=%u pushes=%u put=%u sema=0x%08x want=0x%08x get=0x%08x\n",
		    req->channel, req->push_count, put, sema[0], payload,
		    nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET));
		nvkm_gsp_vmm_debug_dump_pte(sc->gsp_vmm, 0x0000003ffdf5c000ULL);
		nvkm_gsp_vmm_debug_dump_pte(sc->gsp_vmm, 0x0000003ffdf41000ULL);
		nvkm_drm_dump_exec_timeout(sc);
		sc->exec_timeout_count++;
		sc->exec_async_wait_error_count++;
		err = -ETIME;
		goto out_unlock;
	}

exec_complete:
	chan->gpf_put = put;
	nvkm_debugf(sc->dev,
	    "nvkm_drm: EXEC complete channel=%u pushes=%u sigs=%u err=%d put=%u sema=0x%08x payload=0x%08x\n",
	    req->channel, req->push_count, req->sig_count, err, put, sema[0],
	    payload);

out_unlock:
	profile_start = nvkm_drm_profile_now_us();
	if (signals != NULL) {
		if (!exec_completion_queued || err != 0)
			nvkm_drm_exec_signals_signal(signals, req->sig_count,
			    err);
		nvkm_drm_exec_signals_put(signals, req->sig_count);
	} else if (exec_fence != NULL) {
		if (!exec_completion_queued || err != 0) {
			if (err != 0)
				dma_fence_set_error(exec_fence, err);
			(void)dma_fence_signal(exec_fence);
		}
		dma_fence_put(exec_fence);
	}
	if (gsp_tok_held)
		lwkt_reltoken(&sc->gsp_tok);
	nvkm_drm_profile_add_us(&sc->exec_profile_cleanup_us, profile_start);
	if (err != 0)
		nvkm_debugf(sc->dev,
		    "nvkm_drm: EXEC return channel=%u err=%d\n",
		    req->channel, err);
	kfree(pushes);
	return (err);
}

/* ---- ioctl table ---- */
#define DRM_IOCTL_NOUVEAU_GETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GETPARAM, struct drm_nouveau_getparam)
#define DRM_IOCTL_NOUVEAU_VM_INIT \
    DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_INIT, struct drm_nouveau_vm_init)
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
