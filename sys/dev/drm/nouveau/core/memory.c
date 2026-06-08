/*
 * Copyright 2015 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs <bskeggs@redhat.com>
 */
#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
#include "nvkm_priv.h"
#include <core/memory.h>
#include <subdev/gsp.h>

/*
 * DragonFly display-only port:
 *
 * r535 display oneinit needs a VRAM-backed nvkm_memory object for display
 * instance RAM.  The full nouveau memory.c also owns fb comptags and instmem
 * backends, which would pull in unrelated subdevices.  Keep that upstream
 * implementation below, but compile this narrow native backend until those
 * subdevices are imported deliberately.
 */
struct nvkm_dfly_memory {
	struct nvkm_memory base;
	struct nvkm_softc *sc;
	uint64_t paddr;
	uint64_t size;
	enum nvkm_vram_kind kind;
};

static struct nvkm_dfly_memory *
nvkm_dfly_memory(struct nvkm_memory *memory)
{
	return container_of(memory, struct nvkm_dfly_memory, base);
}

static void *
nvkm_dfly_memory_dtor(struct nvkm_memory *memory)
{
	struct nvkm_dfly_memory *mem = nvkm_dfly_memory(memory);

	nvkm_gsp_vram_free_kind(mem->sc, mem->paddr, mem->kind, mem);
	return mem;
}

static enum nvkm_memory_target
nvkm_dfly_memory_target(struct nvkm_memory *memory)
{
	(void)memory;
	return NVKM_MEM_TARGET_VRAM;
}

static u8
nvkm_dfly_memory_page(struct nvkm_memory *memory)
{
	(void)memory;
	return 12;
}

static u64
nvkm_dfly_memory_bar2(struct nvkm_memory *memory)
{
	return nvkm_dfly_memory(memory)->paddr;
}

static u64
nvkm_dfly_memory_addr(struct nvkm_memory *memory)
{
	return nvkm_dfly_memory(memory)->paddr;
}

static u64
nvkm_dfly_memory_size(struct nvkm_memory *memory)
{
	return nvkm_dfly_memory(memory)->size;
}

static void
nvkm_dfly_memory_boot(struct nvkm_memory *memory, struct nvkm_vmm *vmm)
{
	(void)memory;
	(void)vmm;
}

static void __iomem *
nvkm_dfly_memory_acquire(struct nvkm_memory *memory)
{
	(void)memory;
	return NULL;
}

static void
nvkm_dfly_memory_release(struct nvkm_memory *memory)
{
	(void)memory;
}

static int
nvkm_dfly_memory_map(struct nvkm_memory *memory, u64 offset,
    struct nvkm_vmm *vmm, struct nvkm_vma *vma, void *argv, u32 argc)
{
	(void)memory;
	(void)offset;
	(void)vmm;
	(void)vma;
	(void)argv;
	(void)argc;
	return -ENOSYS;
}

static const struct nvkm_memory_func nvkm_dfly_memory_func = {
	.dtor = nvkm_dfly_memory_dtor,
	.target = nvkm_dfly_memory_target,
	.page = nvkm_dfly_memory_page,
	.bar2 = nvkm_dfly_memory_bar2,
	.addr = nvkm_dfly_memory_addr,
	.size = nvkm_dfly_memory_size,
	.boot = nvkm_dfly_memory_boot,
	.acquire = nvkm_dfly_memory_acquire,
	.release = nvkm_dfly_memory_release,
	.map = nvkm_dfly_memory_map,
};

static u32
nvkm_dfly_memory_rd32(struct nvkm_memory *memory, u64 offset)
{
	struct nvkm_dfly_memory *mem = nvkm_dfly_memory(memory);
	uint64_t page = (mem->paddr + offset) & ~0xfffULL;
	uint64_t page_off = (mem->paddr + offset) & 0xfffULL;
	uint64_t gva;
	u32 data = 0;

	if (nvkm_gsp_bar1_map_existing(mem->sc, page, &gva) == 0) {
		data = nvkm_gsp_bar1_rd32(mem->sc, gva + page_off);
		nvkm_gsp_bar1_unmap_existing(mem->sc, gva);
	}
	return data;
}

static void
nvkm_dfly_memory_wr32(struct nvkm_memory *memory, u64 offset, u32 data)
{
	struct nvkm_dfly_memory *mem = nvkm_dfly_memory(memory);
	uint64_t page = (mem->paddr + offset) & ~0xfffULL;
	uint64_t page_off = (mem->paddr + offset) & 0xfffULL;
	uint64_t gva;

	if (nvkm_gsp_bar1_map_existing(mem->sc, page, &gva) == 0) {
		nvkm_gsp_bar1_wr32(mem->sc, gva + page_off, data);
		nvkm_gsp_bar1_unmap_existing(mem->sc, gva);
	}
}

static const struct nvkm_memory_ptrs nvkm_dfly_memory_ptrs = {
	.rd32 = nvkm_dfly_memory_rd32,
	.wr32 = nvkm_dfly_memory_wr32,
};

void
nvkm_memory_ctor(const struct nvkm_memory_func *func, struct nvkm_memory *memory)
{
	memory->func = func;
	kref_init(&memory->kref);
}

static void
nvkm_memory_del(struct kref *kref)
{
	struct nvkm_memory *memory = container_of(kref, typeof(*memory), kref);

	if (memory->func && memory->func->dtor)
		memory = memory->func->dtor(memory);
	kfree(memory);
}

void
nvkm_memory_unref(struct nvkm_memory **pmemory)
{
	struct nvkm_memory *memory = *pmemory;

	if (memory) {
		kref_put(&memory->kref, nvkm_memory_del);
		*pmemory = NULL;
	}
}

struct nvkm_memory *
nvkm_memory_ref(struct nvkm_memory *memory)
{
	if (memory)
		kref_get(&memory->kref);
	return memory;
}

int
nvkm_memory_new(struct nvkm_device *device, enum nvkm_memory_target target,
    u64 size, u32 align, bool zero, struct nvkm_memory **pmemory)
{
	struct nvkm_dfly_memory *mem;

	(void)target;
	if (device == NULL || device->gsp == NULL || device->gsp->sc == NULL)
		return -ENODEV;

	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (mem == NULL)
		return -ENOMEM;

	nvkm_memory_ctor(&nvkm_dfly_memory_func, &mem->base);
	mem->base.ptrs = &nvkm_dfly_memory_ptrs;
	mem->sc = device->gsp->sc;
	mem->size = roundup2(size, PAGE_SIZE);
	mem->kind = NVKM_VRAM_BAR1_PAGE;
	mem->paddr = nvkm_gsp_vram_alloc_kind(mem->sc, mem->size,
	    MAX((uint64_t)align, (uint64_t)PAGE_SIZE), mem->kind, mem);
	if (mem->paddr == 0) {
		kfree(mem);
		return -ENOMEM;
	}
	if (zero) {
		for (u64 off = 0; off < mem->size; off += 4)
			nvkm_dfly_memory_wr32(&mem->base, off, 0);
	}
	*pmemory = &mem->base;
	return 0;
}

int
nvkm_memory_tags_get(struct nvkm_memory *memory, struct nvkm_device *device,
    u32 nr, void (*clr)(struct nvkm_device *, u32, u32),
    struct nvkm_tags **ptags)
{
	(void)memory;
	(void)device;
	(void)nr;
	(void)clr;
	*ptags = NULL;
	return -ENOSYS;
}

void
nvkm_memory_tags_put(struct nvkm_memory *memory, struct nvkm_device *device,
    struct nvkm_tags **ptags)
{
	(void)memory;
	(void)device;
	*ptags = NULL;
}
#else
#include <core/memory.h>
#include <core/mm.h>
#include <subdev/fb.h>
#include <subdev/instmem.h>

void
nvkm_memory_tags_put(struct nvkm_memory *memory, struct nvkm_device *device,
		     struct nvkm_tags **ptags)
{
	struct nvkm_fb *fb = device->fb;
	struct nvkm_tags *tags = *ptags;
	if (tags) {
		mutex_lock(&fb->tags.mutex);
		if (refcount_dec_and_test(&tags->refcount)) {
			nvkm_mm_free(&fb->tags.mm, &tags->mn);
			kfree(memory->tags);
			memory->tags = NULL;
		}
		mutex_unlock(&fb->tags.mutex);
		*ptags = NULL;
	}
}

int
nvkm_memory_tags_get(struct nvkm_memory *memory, struct nvkm_device *device,
		     u32 nr, void (*clr)(struct nvkm_device *, u32, u32),
		     struct nvkm_tags **ptags)
{
	struct nvkm_fb *fb = device->fb;
	struct nvkm_tags *tags;

	mutex_lock(&fb->tags.mutex);
	if ((tags = memory->tags)) {
		/* If comptags exist for the memory, but a different amount
		 * than requested, the buffer is being mapped with settings
		 * that are incompatible with existing mappings.
		 */
		if (tags->mn && tags->mn->length != nr) {
			mutex_unlock(&fb->tags.mutex);
			return -EINVAL;
		}

		refcount_inc(&tags->refcount);
		mutex_unlock(&fb->tags.mutex);
		*ptags = tags;
		return 0;
	}

	if (!(tags = kmalloc_obj(*tags))) {
		mutex_unlock(&fb->tags.mutex);
		return -ENOMEM;
	}

	if (!nvkm_mm_head(&fb->tags.mm, 0, 1, nr, nr, 1, &tags->mn)) {
		if (clr)
			clr(device, tags->mn->offset, tags->mn->length);
	} else {
		/* Failure to allocate HW comptags is not an error, the
		 * caller should fall back to an uncompressed map.
		 *
		 * As memory can be mapped in multiple places, we still
		 * need to track the allocation failure and ensure that
		 * any additional mappings remain uncompressed.
		 *
		 * This is handled by returning an empty nvkm_tags.
		 */
		tags->mn = NULL;
	}

	refcount_set(&tags->refcount, 1);
	*ptags = memory->tags = tags;
	mutex_unlock(&fb->tags.mutex);
	return 0;
}

void
nvkm_memory_ctor(const struct nvkm_memory_func *func,
		 struct nvkm_memory *memory)
{
	memory->func = func;
	kref_init(&memory->kref);
}

static void
nvkm_memory_del(struct kref *kref)
{
	struct nvkm_memory *memory = container_of(kref, typeof(*memory), kref);
	if (!WARN_ON(!memory->func)) {
		if (memory->func->dtor)
			memory = memory->func->dtor(memory);
		kfree(memory);
	}
}

void
nvkm_memory_unref(struct nvkm_memory **pmemory)
{
	struct nvkm_memory *memory = *pmemory;
	if (memory) {
		kref_put(&memory->kref, nvkm_memory_del);
		*pmemory = NULL;
	}
}

struct nvkm_memory *
nvkm_memory_ref(struct nvkm_memory *memory)
{
	if (memory)
		kref_get(&memory->kref);
	return memory;
}

int
nvkm_memory_new(struct nvkm_device *device, enum nvkm_memory_target target,
		u64 size, u32 align, bool zero,
		struct nvkm_memory **pmemory)
{
	struct nvkm_instmem *imem = device->imem;
	struct nvkm_memory *memory;
	bool preserve = true;
	int ret;

	if (unlikely(!imem))
		return -ENOSYS;

	switch (target) {
	case NVKM_MEM_TARGET_INST_SR_LOST:
		preserve = false;
		break;
	case NVKM_MEM_TARGET_INST:
		break;
	default:
		return -ENOSYS;
	}

	ret = nvkm_instobj_new(imem, size, align, zero, preserve, &memory);
	if (ret)
		return ret;

	*pmemory = memory;
	return 0;
}
#endif
