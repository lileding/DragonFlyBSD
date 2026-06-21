/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Loader glue for NVIDIA-distributed GSP-boot firmware blobs.
 *
 * Firmware blobs (booter, GSP image, etc.) are packaged as separate
 * loadable kernel modules built via firmware(9)'s FIRMWS Makefile
 * mechanism. We retrieve them through firmware_get() and hold the
 * references for the lifetime of the device.
 *
 * Naming convention: linux-firmware style paths such as
 *   "nvidia/tu102/gsp/booter_load-570.144"
 * so the same blob shipped in /lib/firmware/ can be embedded unchanged.
 */

#include "nvkm_priv.h"

#include <sys/firmware.h>

int
nvkm_fw_init(struct nvkm_softc *sc)
{
	const struct firmware *fw;

	fw = firmware_get(sc->chip->fw_booter_load);
	if (fw == NULL) {
		nvkm_debugf(sc->dev,
		    "fw: cannot load \"%s\" (module nvgsp_570 absent?)\n",
		    sc->chip->fw_booter_load);
		return (ENOENT);
	}

	sc->fw_booter_load = fw;
	nvkm_debugf(sc->dev,
	    "fw: %s loaded, %zu bytes, version %u, first 8: "
	    "%02x %02x %02x %02x %02x %02x %02x %02x\n",
	    sc->chip->fw_booter_load,
	    fw->datasize, fw->version,
	    fw->data[0], fw->data[1], fw->data[2], fw->data[3],
	    fw->data[4], fw->data[5], fw->data[6], fw->data[7]);

	return (0);
}

void
nvkm_fw_fini(struct nvkm_softc *sc)
{
	if (sc->fw_booter_load != NULL) {
		firmware_put(sc->fw_booter_load, FIRMWARE_UNLOAD);
		sc->fw_booter_load = NULL;
	}
}
