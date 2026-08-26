KMOD=   virtio
MFILES= kern/bus_if.m kern/device_if.m
MFILES+= dev/virtual/virtio/virtio/virtio_bus_if.m
MFILES+= dev/virtual/virtio/virtio/virtio_if.m
MFILES+= dev/virtual/virtio/virtio/virtio_v1_bus_if.m
MFILES+= dev/virtual/virtio/virtio/virtio_v1_if.m

SRCS=   virtio.c virtqueue.c virtio_v1_core.c virtqueue_v1.c
SRCS+= virtio_bus_if.h virtio_bus_if.c
SRCS+= virtio_v1_bus_if.h virtio_v1_bus_if.c virtio_if.h virtio_if.c
SRCS+= device_if.h bus_if.h

.include <bsd.kmod.mk>
