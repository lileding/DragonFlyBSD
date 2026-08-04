PROG= virtiod
SRCS= virtiod_main.c virtiod_config.c virtiod_blk.c virtiod_net.c virtiod_vsock.c virtiod_virtqueue.c \
	virtiod_block.c virtiod_tap.c
CFLAGS+= -I${.CURDIR}/../../sys
LDFLAGS+= -pthread
MAN=

.include <bsd.prog.mk>
