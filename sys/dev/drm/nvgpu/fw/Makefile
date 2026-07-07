# Aggregate firmware module for NVIDIA GSP-RM 570.144 boot blobs.
#
# Each entry maps a source file to a firmware imagename and version.
# Image names follow the Linux nouveau convention:
#   nvidia/<chip>/gsp/<blob>-<version>
#
# The module is intentionally aggregate: a single nvgsp_570.ko registers
# all currently supported TU10x image names.  The payloads are shared for
# now, but the firmware_get() namespace matches Linux nouveau per chip.

KMOD=	nvgsp_570

FW_SRC?=	/lib/firmware/nvidia/tu102
NVGSP_570_VERSION=	570144
NVGSP_570_CHIPS=	tu102 tu104 tu106

.for chip in ${NVGSP_570_CHIPS}
FIRMWS+=	${chip}_booter_load.bin:nvidia/${chip}/gsp/booter_load-570.144:${NVGSP_570_VERSION} \
		${chip}_booter_unload.bin:nvidia/${chip}/gsp/booter_unload-570.144:${NVGSP_570_VERSION} \
		${chip}_acr_bl.bin:nvidia/${chip}/acr/bl:1 \
		${chip}_bootloader.bin:nvidia/${chip}/gsp/bootloader-570.144:${NVGSP_570_VERSION} \
		${chip}_gsp.bin:nvidia/${chip}/gsp/gsp-570.144:${NVGSP_570_VERSION}
NVGSP_570_ALIAS_BINS+=	${chip}_booter_load.bin ${chip}_booter_unload.bin \
		${chip}_acr_bl.bin ${chip}_bootloader.bin ${chip}_gsp.bin
.endfor

.for chip in ${NVGSP_570_CHIPS}
${chip}_booter_load.bin: booter_load.bin
	ln -sf ${.ALLSRC} ${.TARGET}

${chip}_booter_unload.bin: booter_unload.bin
	ln -sf ${.ALLSRC} ${.TARGET}

${chip}_acr_bl.bin: acr_bl.bin
	ln -sf ${.ALLSRC} ${.TARGET}

${chip}_bootloader.bin: bootloader.bin
	ln -sf ${.ALLSRC} ${.TARGET}

${chip}_gsp.bin: gsp.bin
	ln -sf ${.ALLSRC} ${.TARGET}
.endfor

booter_unload.bin: ${FW_SRC}/gsp/booter_unload-570.144.bin.xz
	unxz -k -c ${.ALLSRC} > ${.TARGET}

bootloader.bin: ${FW_SRC}/gsp/bootloader-570.144.bin.xz
	unxz -k -c ${.ALLSRC} > ${.TARGET}

gsp.bin: ${FW_SRC}/gsp/gsp-570.144.bin.xz
	unxz -k -c ${.ALLSRC} > ${.TARGET}

CLEANFILES+=	booter_unload.bin bootloader.bin gsp.bin ${NVGSP_570_ALIAS_BINS}

.include <bsd.kmod.mk>
