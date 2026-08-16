KMOD= vmmfs
SRCS= vmmfs.c vmmfs_root.c vmmfs_machine.c vmmfs_vcpu.c vmmfs_memory.c vmmfs_loader.c vmmfs_stopped.c vmmfs_events.c vmmfs_pciroot.c vmmfs_pcislot.c

COPTS+= -fno-asynchronous-unwind-tables
COPTS+= -fno-unwind-tables
.include <bsd.kmod.mk>
