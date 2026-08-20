# SPDX-License-Identifier: MIT
#
# liburing compatibility layer: io_uring queue API on top of the native
# ioport()/ioevent() completion-port syscalls.  Static library for now (no
# symbol versioning yet).

LIB=	uring

SRCS=	liburing.c
INCS=	liburing.h io_uring.h

MAN=	liburing.3

# Static library only for now (no shared lib / symbol versioning).
NOPIC=	true

WARNS?=	2

.include <bsd.lib.mk>
