KMOD=	nvgpu
SRCS=	nvgpu_device.c nvgpu_debug.c nvgpu_chip.c nvgpu_proc.c
SRCS+=	nvgpu_unload.c nvgpu_intr.c nvgpu_exec.c nvgpu_sched.c
SRCS+=	nvgpu_display.c nvdrm_drv.c nvdrm_file.c
SRCS+=	nvgsp_state.c nvgsp_boot.c nvgsp_rpc.c nvgsp_event.c
SRCS+=	nvgsp_vram.c nvgsp_bar.c nvgsp_vmm.c nvgsp_rm.c
SRCS+=	nvgsp_channel.c nvgsp_disp.c
SRCS+=	device_if.h bus_if.h pci_if.h opt_ddb.h opt_acpi.h opt_drm.h

KCFLAGS+= -I../include
KCFLAGS+= -I../include/drm
KCFLAGS+= -I../include/uapi
KCFLAGS+= -I../include/uapi/drm
CWARNFLAGS+= -Wno-error=cast-qual

.include <bsd.kmod.mk>
