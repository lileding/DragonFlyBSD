# DragonFly kmod for tarfs (ported from FreeBSD sys/fs/tarfs)
# Embeds zstd decompress-only from contrib/zstd (FreeBSD tarfs ZSTDIO model).

KMOD=	tarfs
SRCS=	tarfs_vfsops.c tarfs_vnops.c tarfs_subr.c tarfs_io.c \
	zstd_kmalloc.c \
	entropy_common.c error_private.c fse_decompress.c xxhash.c \
	zstd_common.c huf_decompress.c zstd_ddict.c zstd_decompress.c \
	zstd_decompress_block.c debug.c
NOMAN=

ZSTDDIR=	${.CURDIR}/../../../contrib/zstd
ZSTDSHIM=	${.CURDIR}/zstd_shim

.PATH:	${ZSTDDIR}/lib/common ${ZSTDDIR}/lib/decompress ${ZSTDSHIM}

# kmod.mk resets CFLAGS from KCFLAGS — put flags there.
KCFLAGS+=	-DZSTD_HEAPMODE=1 \
		-DZSTD_NO_INTRINSICS \
		-U__BMI__ \
		-DXXH_NAMESPACE=ZSTD_ \
		-I${ZSTDSHIM} \
		-I${ZSTDDIR}/lib \
		-I${ZSTDDIR}/lib/common \
		-Wno-missing-prototypes \
		-Wno-cast-qual \
		-Wno-unused-parameter

.include <bsd.kmod.mk>

# After kmod.mk: per-file flags
.if ${CCVER:Mgcc*}
CFLAGS.zstd_decompress_block.c+=	-fno-tree-vectorize
.endif
