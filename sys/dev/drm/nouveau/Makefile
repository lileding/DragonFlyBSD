KMOD=	nvgpu
SRCS=	nvgpu_device.c nvgpu_debug.c nvgpu_chip.c nvgpu_proc.c nvdrm_drv.c nvdrm_file.c
SRCS+=	device_if.h bus_if.h pci_if.h opt_ddb.h opt_acpi.h opt_drm.h

KCFLAGS+= -I../include
KCFLAGS+= -I../include/drm
KCFLAGS+= -I../include/uapi
KCFLAGS+= -I../include/uapi/drm
CWARNFLAGS+= -Wno-error=cast-qual

.include <bsd.kmod.mk>
