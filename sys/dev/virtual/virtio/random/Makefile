KMOD= virtio_random

MFILES= kern/bus_if.m kern/device_if.m
MFILES+= dev/virtual/virtio/virtio/virtio_bus_if.m
MFILES+= dev/virtual/virtio/v1/virtio_v1_bus_if.m
MFILES+= dev/virtual/virtio/v1/virtio_if.m

SRCS= virtio_random.c virtio_random_v1.c
SRCS+= virtio_bus_if.h virtio_v1_bus_if.h virtio_if.h
SRCS+= bus_if.h device_if.h

.include <bsd.kmod.mk>
