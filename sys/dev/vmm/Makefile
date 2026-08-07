KMOD=	vmm
# vmm_* is the runtime core.  It has no VFS/KOBJ dependency and is reused by
# vmmfs.ko and future native monitor frontends.
SRCS=	vmm_parse.c vmm_pcie_abi.c vmm_pcie_bar.c vmm_pcie_event.c vmm_pcie_config.c
SRCS+=	vmm_pcie_ecam.c vmm_pcie_root.c vmm_pcie.c
SRCS+=	vmm_pcie_user.c vmm_domain.c
SRCS+=	vmm_vcpu.c vmm_mem.c vmm_dma.c vmm_loader.c
SRCS+=	vmm_loader_x86.c vmm_console.c
SRCS+=	vmm_platform_x86.c
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
.elif ${MACHINE_PLATFORM} == "vkernel64"
# vkernel64 is a no-hardware control-plane target.  It can exercise vmmfs,
# taskqueue, loader, and memory handoff paths when tests stop before vCPU
# execution; real SVM/VMX backends remain pc64-only.
SRCS+=	vmm_vkernel.c
.else
.error "vmm.ko currently supports only MACHINE_PLATFORM=pc64 or vkernel64"
.endif

CLEANFILES+=	vmm_svm.o vmm_svm_entry.o vmm_vkernel.o assym.o assym.s
CLEANFILES+=	bus_if.h device_if.h opt_nfs.h


# Suppress .eh_frame: the kernel ELF object loader panics ("lost base for
# relatab") on the unwind-table sections.  Must be COPTS (kmod.mk hard-assigns
# CFLAGS, wiping any CFLAGS+= set earlier).
COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables
.include <bsd.kmod.mk>
