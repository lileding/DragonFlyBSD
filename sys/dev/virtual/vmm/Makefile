KMOD=	vmm
# The active core starts with the machine object and its initial allocation
# path.  Legacy implementations stay under legacy/ and are excluded from the build.
SRCS=	vmm.c vmm_machine.c vmm_vcpu.c vmm_io.c

.if ${MACHINE_ARCH} == "x86_64"
.PATH:	${.CURDIR}/x64 ${.CURDIR}/x64/svm
SRCS+=	vmm_x64_pic.c vmm_x64_pit.c vmm_svm.c vmm_svm_avic.c vmm_svm_backend.c \
	vmm_svm_entry.S

.if !defined(KERNBUILDDIR)
SRCS+=	bus_if.h device_if.h opt_nfs.h
.endif
.endif

CLEANFILES+=	bus_if.h device_if.h opt_nfs.h


# Suppress .eh_frame: the kernel ELF object loader panics ("lost base for
# relatab") on the unwind-table sections.  Must be COPTS (kmod.mk hard-assigns
# CFLAGS, wiping any CFLAGS+= set earlier).
COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables
.include <bsd.kmod.mk>
