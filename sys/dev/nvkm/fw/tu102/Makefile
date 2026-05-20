# Firmware loader module for TU102 GSP-boot blobs at GSP-RM 570.144.
#
# Each entry maps a source file to a firmware imagename and version.
# Image names follow the linux-firmware naming convention so the same
# blobs can be sourced from /lib/firmware/nvidia/tu102/gsp/ unchanged.

KMOD=	nvkm_fw_tu102_570

FIRMWS=	booter_load.bin:nvidia/tu102/gsp/booter_load-570.144:570144

.include <bsd.kmod.mk>
