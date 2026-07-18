#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

MALLOC_DEFINE(M_ZSTD, "zstd", "ZSTD decompressor for tarfs");
