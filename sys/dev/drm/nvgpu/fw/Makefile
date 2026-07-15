# Aggregate firmware module for NVIDIA GSP-RM 570.144 boot blobs.
#
# Each entry maps a source file to a firmware imagename and version.
# Image names follow the Linux nouveau convention:
#   nvidia/<chip>/gsp/<blob>-<version>
#
# The module is intentionally aggregate: a single nvgsp570.ko registers
# all currently supported TU10x image names.  The payloads live in this
# directory so official builds do not depend on /lib/firmware.

KMOD=	nvgsp570

NVGSP_570_VERSION=	570144
NVGSP_570_CHIPS=	tu102 tu104 tu106

.for chip in ${NVGSP_570_CHIPS}
FIRMWS+=	${chip}_booter_load.bin:nvidia/${chip}/gsp/booter_load-570.144:${NVGSP_570_VERSION} \
		${chip}_booter_unload.bin:nvidia/${chip}/gsp/booter_unload-570.144:${NVGSP_570_VERSION} \
		${chip}_acr_bl.bin:nvidia/${chip}/acr/bl:1 \
		${chip}_bootloader.bin:nvidia/${chip}/gsp/bootloader-570.144:${NVGSP_570_VERSION} \
		${chip}_gsp.bin:nvidia/${chip}/gsp/gsp-570.144:${NVGSP_570_VERSION}
.endfor

.include <bsd.kmod.mk>
