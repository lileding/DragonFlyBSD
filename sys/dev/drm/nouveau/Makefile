KMOD=	nvkm
SRCS=	nvkm.c nvkm_pci.c nvkm_bios.c nvkm_falcon.c nvkm_fw.c nvkm_sec2.c nvkm_gsp.c nvkm_gsp_meta.c nvkm_gsp_boot.c nvkm_gsp_libos.c nvkm_gsp_rpc.c nvkm_gsp_static.c nvkm_gsp_seq.c nvkm_drm.c nvkm_gsp_debug.c \
	nvkm_mem.c nvkm_booter.c nvkm_fwsec.c nvkm_bo.c nvkm_gsp_rm.c nvkm_gsp_vmm.c nvkm_gsp_bar1.c nvkm_gsp_bar2.c nvkm_gsp_disp.c nvkm_drm_kms.c
SRCS+=	device_if.h bus_if.h pci_if.h opt_ddb.h opt_acpi.h opt_drm.h

NVKM_COMMON_HEADERS= \
	nvkm_priv.h \
	nvkm_bo.h \
	nvkm_gsp_rm.h \
	nvkm_gsp_vmm.h

.for src in ${SRCS:M*.c}
${src:R}.o: ${NVKM_COMMON_HEADERS}
.endfor

# DRM headers (Phase 3: register as drm driver)
KCFLAGS+= -I${.CURDIR}/../include
KCFLAGS+= -I${.CURDIR}/../include/drm
KCFLAGS+= -I${.CURDIR}/../include/uapi
KCFLAGS+= -I${.CURDIR}/../include/uapi/drm
CWARNFLAGS+= -Wno-error=cast-qual

.include <bsd.kmod.mk>
