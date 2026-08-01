/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <err.h>
#include <errno.h>
#include <string.h>

#include "virtiod.h"

int
main(int argc, char **argv)
{

	if (argc < 2)
		errno = EINVAL, err(1,
		    "usage: virtiod blk DEVICE_DIR RAW_IMAGE | "
		    "virtiod net DEVICE_DIR TAP MAC");
	if (strcmp(argv[1], "blk") == 0)
		return virtiod_blk_main(argc - 2, argv + 2);
	if (strcmp(argv[1], "net") == 0)
		return virtiod_net_main(argc - 2, argv + 2);
	errno = EINVAL;
	err(1, "unknown provider %s", argv[1]);
}
