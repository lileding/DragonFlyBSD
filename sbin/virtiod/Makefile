PROG= virtiod
SRCS= virtiod.c virtiod_virtqueue.c virtiod_block.c
CFLAGS+= -I${.CURDIR}/../../sys
MAN=

test:
	${CC} ${CFLAGS} -Wall -Wextra -Werror -std=c11 \
	    ${.CURDIR}/virtiod_virtqueue_test.c \
	    ${.CURDIR}/virtiod_virtqueue.c -o /var/tmp/virtiod_virtqueue_test
	/var/tmp/virtiod_virtqueue_test
	${CC} ${CFLAGS} -Wall -Wextra -Werror -std=c11 \
	    ${.CURDIR}/virtiod_block_test.c \
	    ${.CURDIR}/virtiod_block.c -o /var/tmp/virtiod_block_test
	/var/tmp/virtiod_block_test

.include <bsd.prog.mk>
