# Userspace regression tools for nvgpu DRM ABI paths.

CC?= cc
CFLAGS?= -O2 -pipe
CFLAGS+= -Wall -I/usr/local/include
LDFLAGS+= -pthread

TOOLS= \
	nvgpu_bo_lifetime_test \
	nvgpu_exec_sync_test \
	nvgpu_vm_bind_test

all: ${TOOLS}

.for tool in ${TOOLS}
${tool}: ${tool}.c
	${CC} ${CFLAGS} -o ${.TARGET} ${.ALLSRC} ${LDFLAGS}
.endfor

clean:
	rm -f ${TOOLS} *.core

.PHONY: all clean
