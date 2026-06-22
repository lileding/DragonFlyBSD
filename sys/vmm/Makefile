KMOD=	vmm
SRCS=	vmmfs.c vmmfs_vnode.c vmm_machine.c
SRCS+=	vmm_device.c vmm_host.c vmmfs_devices.c vmmfs_machines.c
SRCS+=	vmm_vcpu.c vmm_mem.c vmm_loader.c
SRCS+=	vmm_node_if.c vmm_node_if.h

# The node-operation vtable is a KOBJ interface.  makeobjops turns the local
# vmm_node_if.m into the dispatch glue (vmm_node_if.{c,h}); the MFILES machinery
# only resolves interface files under the kernel source tree, so drive it here.
CLEANFILES+=	vmm_node_if.c vmm_node_if.h

vmm_node_if.c: vmm_node_if.m dragonfly
	awk -f dragonfly/tools/makeobjops.awk -- -c vmm_node_if.m
vmm_node_if.h: vmm_node_if.m dragonfly
	awk -f dragonfly/tools/makeobjops.awk -- -h vmm_node_if.m

# Suppress .eh_frame: the kernel ELF object loader panics ("lost base for
# relatab") on the unwind-table sections.  Must be COPTS (kmod.mk hard-assigns
# CFLAGS, wiping any CFLAGS+= set earlier).
COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables

.include <bsd.kmod.mk>
