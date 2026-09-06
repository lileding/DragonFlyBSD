KMOD= vmmfs
SRCS= vmmfs.c vmmfs_node.c vmmfs_node_vops.c vmmfs_root.c vmmfs_machine.c vmmfs_machine_id.c vmmfs_vcpu.c vmmfs_memory.c vmmfs_loader.c vmmfs_boot.c vmmfs_launch.c vmmfs_stopped.c vmmfs_events.c vmmfs_pciroot.c vmmfs_pcislot.c vmmfs_pcislot_descriptor.c vmmfs_pcislot_auth.c vmmfs_pcislot_config.c vmmfs_pcislot_resource.c vmmfs_pcislot_events.c vmmfs_serialroot.c vmmfs_serialport.c vmmfs_rtc.c vmmfs_platform_x64.c

# Keep every private layout in the module's build inputs.
SRCS+= vmmfs.h vmmfs_node.h vmmfs_parent.h vmmfs_root.h \
	vmmfs_machine.h vmmfs_machine_id.h vmmfs_vcpu.h vmmfs_memory.h \
	vmmfs_loader.h vmmfs_boot.h vmmfs_launch.h vmmfs_stopped.h \
	vmmfs_events.h vmmfs_pciroot.h vmmfs_pcislot.h \
	vmmfs_pcislot_descriptor.h vmmfs_pcislot_auth.h vmmfs_pcislot_config.h \
	vmmfs_pcislot_resource.h vmmfs_pcislot_events.h vmmfs_serialroot.h \
	vmmfs_serialport.h vmmfs_rtc.h vmmfs_platform_x64.h

COPTS+= -fno-asynchronous-unwind-tables
COPTS+= -fno-unwind-tables
.include <bsd.kmod.mk>

# Never fall back to installed headers before the source links exist.
CFLAGS:= ${CFLAGS:N-I${DESTDIR}/usr/include}

# kmod.mk supplies this only when .depend is absent.
${OBJS}: ${SRCS:M*.h}
