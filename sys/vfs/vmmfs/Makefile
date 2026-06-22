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
RUSTFLAGS+=	-C llvm-args=-disable-loop-idiom-all

vmmfs_rust.o: vmmfs.rs kernel.rs parse.rs machine.rs intrinsics.rs
	${RUSTC} ${RUSTFLAGS} -o ${.TARGET} vmmfs.rs
	objcopy -R .eh_frame -R .rela.eh_frame ${.TARGET}

.include <bsd.kmod.mk>
