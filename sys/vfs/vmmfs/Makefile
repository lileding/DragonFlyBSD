KMOD=	vmmfs
SRCS=	vmmfs.c
OBJS+=	vmmfs_rust.o

COPTS+=	-fno-asynchronous-unwind-tables
COPTS+=	-fno-unwind-tables

RUSTC?=	rustc
RUSTFLAGS+=	--edition=2021
RUSTFLAGS+=	--crate-name vmmfs
RUSTFLAGS+=	--crate-type lib
RUSTFLAGS+=	--emit=obj
RUSTFLAGS+=	--target x86_64-unknown-dragonfly
RUSTFLAGS+=	-C panic=abort
RUSTFLAGS+=	-C opt-level=2
RUSTFLAGS+=	-C code-model=kernel
RUSTFLAGS+=	-C no-redzone=yes
RUSTFLAGS+=	-C relocation-model=static
RUSTFLAGS+=	-C force-unwind-tables=no

vmmfs_rust.o: vmmfs.rs
	${RUSTC} ${RUSTFLAGS} -o ${.TARGET} vmmfs.rs

.include <bsd.kmod.mk>
