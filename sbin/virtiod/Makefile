PROG= virtiod
SRCS= virtiod_main.c virtiod_config.c virtiod_blk.c virtiod_net.c virtiod_virtqueue.c \
	virtiod_block.c virtiod_tap.c
CFLAGS+= -I${.CURDIR}/../../sys
LDFLAGS+= -pthread
MAN=

test:
	${CC} ${CFLAGS} -Wall -Wextra -Werror -std=c11 \
	    ${.CURDIR}/virtiod_config_test.c ${.CURDIR}/virtiod_config.c \
	    -o ${.OBJDIR}/virtiod_config_test
	${.OBJDIR}/virtiod_config_test
	${CC} ${CFLAGS} -Wall -Wextra -Werror -std=c11 \
	    ${.CURDIR}/virtiod_virtqueue_test.c \
	    ${.CURDIR}/virtiod_virtqueue.c -o ${.OBJDIR}/virtiod_virtqueue_test
	${.OBJDIR}/virtiod_virtqueue_test
	${CC} ${CFLAGS} -Wall -Wextra -Werror -std=c11 \
	    ${.CURDIR}/virtiod_block_test.c \
	    ${.CURDIR}/virtiod_block.c -o ${.OBJDIR}/virtiod_block_test
	${.OBJDIR}/virtiod_block_test

.include <bsd.prog.mk>
