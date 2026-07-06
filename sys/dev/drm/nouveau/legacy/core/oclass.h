/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Local DragonFly shim for linux-v7.0 nouveau display object classes.
 * The linux-v7.0 core/oclass.h file has no explicit per-file license marker,
 * so keep only the declarations required by the imported MIT display code.
 */
#ifndef _DFLY_NVKM_CORE_OCLASS_H_
#define _DFLY_NVKM_CORE_OCLASS_H_

#include <core/os.h>
#include <core/debug.h>

struct nvkm_client;
struct nvkm_engine;
struct nvkm_object;
struct nvkm_object_func;
struct nvkm_oclass;

struct nvkm_sclass {
	int minver;
	int maxver;
	s32 oclass;
	const struct nvkm_object_func *func;
	int (*ctor)(const struct nvkm_oclass *, void *data, u32 size,
	    struct nvkm_object **);
};

struct nvkm_oclass {
	int (*ctor)(const struct nvkm_oclass *, void *data, u32 size,
	    struct nvkm_object **);
	struct nvkm_sclass base;
	const void *priv;
	const void *engn;
	u32 handle;
	u64 object;
	struct nvkm_client *client;
	struct nvkm_object *parent;
	struct nvkm_engine *engine;
};

#endif
