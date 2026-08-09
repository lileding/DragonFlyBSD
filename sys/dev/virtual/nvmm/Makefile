KMOD=	nvmm
SRCS=	nvmm.c nvmm_syscall.c nvmm_dragonfly.c

.if ${MACHINE_ARCH} == "x86_64"
.PATH:	${.CURDIR}/x86
SRCS+=	nvmm_x86.c
.endif

# Help quickly build and test this module alone.
.if !defined(KERNBUILDDIR)
KCFLAGS+=-g -DINVARIANTS
.endif

# Header files are installed by 'include/Makefile'.

.include <bsd.kmod.mk>
