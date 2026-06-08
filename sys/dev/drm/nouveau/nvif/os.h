/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local OS shim for imported linux-v7.0 nouveau MIT headers.
 * It may use compatibility headers already present in this dfly/nvkm tree,
 * but must not add new external kernel-compatibility or non-permissive source.
 */
#ifndef _DFLY_NVIF_OS_H_
#define _DFLY_NVIF_OS_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/bus.h>
#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/kref.h>
#include <linux/irqreturn.h>
#include <linux/err.h>
#include <linux/bug.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

#ifndef static_assert
#define static_assert _Static_assert
#endif

#ifndef LIST_HEAD
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
#endif

#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
/*
 * DragonFly GSP display bring-up imports the MIT nouveau core/display files,
 * but not the full Linux allocation API.  These helpers keep the imported
 * files source-shaped while routing allocation/locking to DragonFly's native
 * DRM compatibility headers already present in this tree.
 */
#ifdef kmalloc_obj
#undef kmalloc_obj
#endif
#ifdef kzalloc_obj
#undef kzalloc_obj
#endif
#ifdef kzalloc_objs
#undef kzalloc_objs
#endif
#define kmalloc_obj(obj)	kmalloc(sizeof(obj), M_DRM, GFP_KERNEL)
	#define kzalloc_obj(obj)	kzalloc(sizeof(obj), GFP_KERNEL)
	#define kzalloc_objs(obj, n)	kcalloc((n), sizeof(obj), GFP_KERNEL)

	/*
	 * DragonFly's kernel likely()/unlikely() macros feed the expression
	 * directly to __builtin_expect(), which warns on function pointers under
	 * -Werror.  Linux's form normalizes through !!(x), and the imported nouveau
	 * core uses likely() on optional function hooks.
	 */
	#ifdef likely
	#undef likely
	#endif
	#ifdef unlikely
	#undef unlikely
	#endif
	#define likely(x)	__builtin_expect(!!(x), 1)
	#define unlikely(x)	__builtin_expect(!!(x), 0)

	#ifndef spin_lock_init
	#define spin_lock_init(lock)	lockinit((lock), "nvspin", 0, LK_CANRECURSE)
	#endif
	#ifndef mutex_init
	#define mutex_init(lock)	lockinit((lock), "nvmutex", 0, LK_CANRECURSE)
	#endif

	#ifndef struct_size
	#define struct_size(ptr, member, count) \
		(sizeof(*(ptr)) + sizeof(*(ptr)->member) * (count))
#endif

#ifndef array3_size
#define array3_size(a, b, c)	((size_t)(a) * (size_t)(b) * (size_t)(c))
#endif

#ifndef read_lock_irqsave
#define read_lock_irqsave(lock, flags)	spin_lock_irqsave((lock), (flags))
#define read_unlock_irqrestore(lock, flags) \
	spin_unlock_irqrestore((lock), (flags))
#define write_lock_irq(lock)		spin_lock_irq((lock))
#define write_unlock_irq(lock)		spin_unlock_irq((lock))
#endif
#endif

static __inline u8 nvkm_ioread8(const volatile void __iomem *addr)
{
	return (*(volatile const u8 *)addr);
}
static __inline u16 nvkm_ioread16(const volatile void __iomem *addr)
{
	return (*(volatile const u16 *)addr);
}
static __inline u32 nvkm_ioread32(const volatile void __iomem *addr)
{
	return (*(volatile const u32 *)addr);
}
static __inline void nvkm_iowrite8(u8 data, volatile void __iomem *addr)
{
	*(volatile u8 *)addr = data;
}
static __inline void nvkm_iowrite16(u16 data, volatile void __iomem *addr)
{
	*(volatile u16 *)addr = data;
}
static __inline void nvkm_iowrite32(u32 data, volatile void __iomem *addr)
{
	*(volatile u32 *)addr = data;
}
static __inline void nvkm_iowrite64(u64 data, volatile void __iomem *addr)
{
	*(volatile u64 *)addr = data;
}

static __inline void *nvkm_kmalloc(size_t size, int flags)
{
	return kmalloc(size, M_TEMP, flags | M_NULLOK);
}
static __inline void *nvkm_kzalloc(size_t size, int flags)
{
	return kmalloc(size, M_TEMP, flags | M_ZERO | M_NULLOK);
}
static __inline void nvkm_kfree(const void *ptr)
{
	if (ptr != NULL)
		_kfree(__DECONST(void *, ptr), M_TEMP);
}

static __inline void nvkm_msleep(unsigned int msecs)
{
	static int sleep_token;
	int ticks = MAX((int)(msecs * hz / 1000), 1);
	tsleep(&sleep_token, 0, "nvkmslp", ticks);
}

#ifndef dev_WARN
#define dev_WARN(dev, fmt, ...) dev_warn((dev), fmt, ##__VA_ARGS__)
#endif

#ifndef CONFIG_NOUVEAU_DEBUG
#define CONFIG_NOUVEAU_DEBUG 4
#endif

typedef void *acpi_handle;

#endif
