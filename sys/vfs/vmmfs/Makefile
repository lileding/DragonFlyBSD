KMOD=	vmmfs

# vmmfs is the VFS declaration and control-plane frontend.  Runtime machine,
# vCPU, memory, loader and PCIe execution live in sys/dev/virtual/vmm.
SRCS=	vmmfs.c vmmfs_vnode.c vmmfs_machine.c vmmfs_devices.c
SRCS+=	vmmfs_vcpu.c vmmfs_mem.c vmmfs_loader.c vmmfs_console.c
SRCS+=	vmmfs_device.c
SRCS+=	vmmfs_node_if.c vmmfs_node_if.h

COPTS+=	-I${.CURDIR}/../../dev/virtual/vmm

CLEANFILES+=	vmmfs_node_if.c vmmfs_node_if.h

vmmfs_node_if.c: vmmfs_node_if.m dragonfly
	awk -f dragonfly/tools/makeobjops.awk -- -c vmmfs_node_if.m
vmmfs_node_if.h: vmmfs_node_if.m dragonfly
	awk -f dragonfly/tools/makeobjops.awk -- -h vmmfs_node_if.m

COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables
.include <bsd.kmod.mk>
