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

#include <drm/drmP.h>
#include <drm/drm_drv.h>

/* Identify ourselves as "nouveau" so unmodified Mesa NVK accepts us.
 * Version >= 1.0.3.1 is what NVK requires (winsys/nouveau_device.c). */
#define NVKM_DRM_NAME		"nouveau"
#define NVKM_DRM_DESC		"nVidia Riva/TNT/GeForce (dfly GSP-RM)"
#define NVKM_DRM_DATE		"20260522"
#define NVKM_DRM_MAJOR		1
#define NVKM_DRM_MINOR		3
#define NVKM_DRM_PATCH		1

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

/* NVIF ioctl is variable-size; encode with size=0 since dispatch
 * matches by NR only and the actual copy size comes from userspace. */
#define DRM_IOCTL_NOUVEAU_NVIF \
    _IOC(IOC_INOUT, DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_NOUVEAU_NVIF, 0)

static struct drm_driver nvkm_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ |
	    DRIVER_PRIME,
	.fops    = &nvkm_drm_fops,
	.ioctls  = nvkm_drm_ioctls,
	.num_ioctls = 10 /* sparse: max index DRM_NOUVEAU_VM_INIT(0x9)+1 */,
	.name    = NVKM_DRM_NAME,
	.desc    = NVKM_DRM_DESC,
	.date    = NVKM_DRM_DATE,
	.major   = NVKM_DRM_MAJOR,
	.minor   = NVKM_DRM_MINOR,
	.patchlevel = NVKM_DRM_PATCH,
};

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
		device_printf(sc->dev,
		    "drm: drm_init_pdev failed\n");
		return (ENOMEM);
	}
	sc->drm_pdev = pdev;

	ddev = drm_dev_alloc(&nvkm_drm_driver, &pdev->dev);
	if (IS_ERR(ddev)) {
		device_printf(sc->dev,
		    "drm: drm_dev_alloc failed (%ld)\n", PTR_ERR(ddev));
		return (ENOMEM);
	}

	ddev->dev_private = sc;
	ddev->pdev        = pdev;
	sc->drm_dev       = ddev;

	err = drm_dev_register(ddev, 0);
	if (err != 0) {
		device_printf(sc->dev,
		    "drm: drm_dev_register failed (%d)\n", err);
		drm_dev_put(ddev);
		sc->drm_dev = NULL;
		return (err);
	}

	device_printf(sc->dev,
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
#define DRM_NOUVEAU_NVIF		0x07
#define DRM_NOUVEAU_VM_INIT		0x09
#define DRM_NOUVEAU_VM_BIND		0x0a
#define DRM_NOUVEAU_EXEC		0x0b

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
	case NOUVEAU_GETPARAM_VRAM_BAR_SIZE:
		gp->value = sc->bar_res[1] != NULL ?
		    rman_get_size(sc->bar_res[1]) : 0;
		break;
	case NOUVEAU_GETPARAM_EXEC_PUSH_MAX:
		gp->value = 512;	/* default per NVK winsys */
		break;
	case NOUVEAU_GETPARAM_GRAPH_UNITS:
		/* low 16 = GPC mask, high 16 = TPC count -- placeholder. */
		gp->value = 0;
		break;
	case NOUVEAU_GETPARAM_VRAM_USED:
		gp->value = 0;
		break;
	case NOUVEAU_GETPARAM_PTIMER_TIME:
		/* TODO: read GPU PTIMER. */
		gp->value = 0;
		break;
	case NOUVEAU_GETPARAM_HAS_VMA_TILEMODE:
		gp->value = 0;
		break;
	default:
		device_printf(sc->dev,
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

	device_printf(sc->dev,
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
		device_printf(sc->dev,
		    "nvkm_drm: NVIF NEW oclass=0x%x handle=0x%x token=0x%llx\n",
		    new_->oclass, new_->handle,
		    (unsigned long long)new_->token);
		if (new_->oclass == NV_DEVICE) {
			/* stub: accept the NV_DEVICE allocation. */
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
			info->ram_size = sc->bar_res[1] ?
			    rman_get_size(sc->bar_res[1]) : 0;
			info->ram_user = info->ram_size;
			strncpy(info->chip, "TU102", sizeof(info->chip));
			strncpy(info->name, "NVIDIA GeForce RTX 2080 Ti",
			    sizeof(info->name));
			device_printf(sc->dev,
			    "nvkm_drm: NVIF MTHD DEVICE_INFO -> TU102\n");
			return (0);
		}
		return (-EINVAL);
	}
	case NVIF_IOCTL_V0_DEL:
		return (0);
	default:
		device_printf(sc->dev,
		    "nvkm_drm: NVIF type %u unhandled\n", hdr->type);
		return (-EINVAL);
	}
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
};
