# Firmware loader module for TU102 GSP-boot blobs at GSP-RM 570.144.
#
# Each entry maps a source file to a firmware imagename and version.
# Image names follow the linux-firmware naming convention so the same
# blobs can be sourced from /lib/firmware/nvidia/tu102/ unchanged.
#
# The bigger blobs (gsp, bootloader) are auto-extracted from the
# system linux-firmware install rather than checked into git; the
# small ones (acr/bl, booter_load) live in this directory because
# they are needed at every build and round-trip cleanly.

KMOD=	nvkm_fw_tu102_570

FW_SRC?=	/lib/firmware/nvidia/tu102

FIRMWS=	booter_load.bin:nvidia/tu102/gsp/booter_load-570.144:570144 \
	acr_bl.bin:nvidia/tu102/acr/bl:1 \
	bootloader.bin:nvidia/tu102/gsp/bootloader-570.144:570144 \
	gsp.bin:nvidia/tu102/gsp/gsp-570.144:570144

bootloader.bin: ${FW_SRC}/gsp/bootloader-570.144.bin.xz
	unxz -k -c ${.ALLSRC} > ${.TARGET}

gsp.bin: ${FW_SRC}/gsp/gsp-570.144.bin.xz
	unxz -k -c ${.ALLSRC} > ${.TARGET}

CLEANFILES+=	bootloader.bin gsp.bin

.include <bsd.kmod.mk>
