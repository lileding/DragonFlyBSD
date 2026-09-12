/*
 * Copyright (c) 2026 The DragonFly Project. All rights reserved.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <unistd.h>

int
memfd_create(const char *name, unsigned int flags)
{
	return ((int)__syscall((quad_t)SYS_memfd_create, name, flags));
}
