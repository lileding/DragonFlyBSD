KMOD=	vmm
# The active core starts with the machine object and its initial allocation
# path.  Legacy implementations stay under legacy/ and are excluded from the build.
SRCS=	vmm.c vmm_machine.c

.if ${MACHINE_PLATFORM} == "pc64"
.if !defined(KERNBUILDDIR)
SRCS+=	bus_if.h device_if.h opt_nfs.h
.endif
.else
.error "vmm.ko currently supports only MACHINE_PLATFORM=pc64"
.endif

CLEANFILES+=	bus_if.h device_if.h opt_nfs.h


# Suppress .eh_frame: the kernel ELF object loader panics ("lost base for
# relatab") on the unwind-table sections.  Must be COPTS (kmod.mk hard-assigns
# CFLAGS, wiping any CFLAGS+= set earlier).
COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables
.include <bsd.kmod.mk>
