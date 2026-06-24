KMOD=	vmm
# vmm_* = kernel hypervisor core (no VFS/KOBJ, kvm.ko-reusable).
# vmmfs_* = the filesystem/path layer (VFS nodes + KOBJ classes), vmm.ko only.
SRCS=	vmm_parse.c vmm_vcpu.c vmm_mem.c vmm_loader.c vmm_loader_x86.c vmm_console.c
SRCS+=	vmm_machine.c vmm_device.c vmm_host.c
.if ${MACHINE_PLATFORM} == "pc64"
# x86 VM execution backends are selected by runtime CPU probing, not by
# compile-time SVM/VMX feature switches.
SRCS+=	vmm_svm.c vmm_svm_entry.S
.if !defined(KERNBUILDDIR)
SRCS+=	bus_if.h device_if.h opt_nfs.h
SRCS+=	assym.s
vmm_svm_entry.o: assym.s
.endif
vmm_svm_entry.o:
	${CC} -c -x assembler-with-cpp -DLOCORE ${CFLAGS:N-flto} ${.IMPSRC}
.endif
SRCS+=	vmmfs.c vmmfs_vnode.c vmmfs_machines.c vmmfs_machine.c vmmfs_devices.c
SRCS+=	vmmfs_vcpu.c vmmfs_mem.c vmmfs_loader.c vmmfs_console.c
SRCS+=	vmmfs_device.c vmmfs_host.c
SRCS+=	vmmfs_node_if.c vmmfs_node_if.h

CLEANFILES+=	vmm_svm.o vmm_svm_entry.o assym.o assym.s
CLEANFILES+=	bus_if.h device_if.h opt_nfs.h

# The node-operation vtable is a KOBJ interface.  makeobjops turns the local
# vmmfs_node_if.m into the dispatch glue (vmmfs_node_if.{c,h}); the MFILES machinery
# only resolves interface files under the kernel source tree, so drive it here.
CLEANFILES+=	vmmfs_node_if.c vmmfs_node_if.h

vmmfs_node_if.c: vmmfs_node_if.m dragonfly
	awk -f dragonfly/tools/makeobjops.awk -- -c vmmfs_node_if.m
vmmfs_node_if.h: vmmfs_node_if.m dragonfly
	awk -f dragonfly/tools/makeobjops.awk -- -h vmmfs_node_if.m

# Suppress .eh_frame: the kernel ELF object loader panics ("lost base for
# relatab") on the unwind-table sections.  Must be COPTS (kmod.mk hard-assigns
# CFLAGS, wiping any CFLAGS+= set earlier).
COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables

.include <bsd.kmod.mk>
