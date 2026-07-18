/* Kernel glue so contrib/zstd can compile inside tarfs.ko (DragonFly). */
#ifndef _TARFS_ZSTD_KDFLY_H_
#define _TARFS_ZSTD_KDFLY_H_

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/libkern.h>
/* zstd_preSplit may define abs64; avoid clash if ever pulled in */
#define abs64 ZSTD_abs64
#endif

#endif
