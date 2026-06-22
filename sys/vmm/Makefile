KMOD=	vmm
SRCS=	vmmfs.c vmmfs_vnode.c vmm_machine.c

# Suppress .eh_frame: the kernel ELF object loader panics ("lost base for
# relatab") on the unwind-table sections.  Must be COPTS (kmod.mk hard-assigns
# CFLAGS, wiping any CFLAGS+= set earlier).
COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables

.include <bsd.kmod.mk>
