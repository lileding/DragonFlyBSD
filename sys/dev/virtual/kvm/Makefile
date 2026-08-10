KMOD=	kvm
SRCS=	kvm.c kvm_eventfd.c kvm_vm.c

.if !defined(KERNBUILDDIR)
SRCS+=	bus_if.h device_if.h opt_nfs.h
.endif

CLEANFILES+=	bus_if.h device_if.h opt_nfs.h

COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables
.include <bsd.kmod.mk>
