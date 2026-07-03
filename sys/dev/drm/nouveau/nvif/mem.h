/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal nvif memory shim for imported MIT nouveau push
 * helpers.  The upstream nvif/mem.h does not carry a file-level license in
 * linux-v7.0, so keep only the fields required by nvif/push.h here.
 */
#ifndef _DFLY_NVIF_MEM_H_
#define _DFLY_NVIF_MEM_H_

#include <nvif/object.h>

struct nvif_mem {
	struct nvif_object object;
	u8 type;
	u8 page;
	u64 addr;
	u64 size;
};

#endif
