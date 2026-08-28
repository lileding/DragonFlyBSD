#!/bin/sh
# Verify PCI root setup and BDF topology without inserting a device.
set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
MOUNT=${VMMFS_MOUNT:-/var/tmp/vmmfs-pci-topology}
MACHINE=${VMMFS_MACHINE:-ecam0}
MEMORY_BYTES=${VMMFS_MEMORY_BYTES:-2097152}
EVENTS=/var/tmp/vmmfs-pci-topology-events-$$
LOADER=/var/tmp/vmmfs_halt_loader

cleanup()
{
	set +e
	if [ -d "$MOUNT/$MACHINE" ]; then
		touch "$MOUNT/$MACHINE/stopped"
		rmdir "$MOUNT/$MACHINE/pci/0000:7f:1f.7"
		rmdir "$MOUNT/$MACHINE/pci/0000:00:01.0"
		rmdir "$MOUNT/$MACHINE"
	fi
	mount | grep -q " on $MOUNT " && umount "$MOUNT"
	rmdir "$MOUNT"
	kldstat -n vmmfs >/dev/null 2>&1 && kldunload vmmfs
	kldstat -n vmm >/dev/null 2>&1 && kldunload vmm
	rm -f "$EVENTS" "$LOADER"
}

trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ]
! kldstat -n vmm >/dev/null 2>&1
! kldstat -n vmmfs >/dev/null 2>&1
make -C "$REPO/sys/dev/virtual/vmm"
make -C "$REPO/sys/vfs/vmmfs"
cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys" \
	"$REPO/test/vmm/vmmfs/vmmfs_halt_loader.c" -o "$LOADER"
kldload "$REPO/sys/dev/virtual/vmm/vmm.ko"
kldload "$REPO/sys/vfs/vmmfs/vmmfs.ko"
mkdir "$MOUNT"
mount -t vmmfs vmmfs "$MOUNT"
mkdir "$MOUNT/$MACHINE"
printf '1\n' >"$MOUNT/$MACHINE/vcpu"
printf '%s\n' "$MEMORY_BYTES" >"$MOUNT/$MACHINE/mem"
printf '%s\n' "$LOADER" >"$MOUNT/$MACHINE/loader"
mkdir "$MOUNT/$MACHINE/pci/0000:00:01.0"
mkdir "$MOUNT/$MACHINE/pci/0000:7f:1f.7"
if mkdir "$MOUNT/$MACHINE/pci/rootfs" 2>/dev/null; then exit 1; fi
if mkdir "$MOUNT/$MACHINE/pci/0000:00:00.0" 2>/dev/null; then exit 1; fi
rm "$MOUNT/$MACHINE/stopped"
sleep 2
/usr/bin/timeout 1 cat "$MOUNT/$MACHINE/events" >"$EVENTS" || [ $? -eq 124 ]
grep -q 'machine start completed' "$EVENTS"
grep -q 'machine vcpu halted index=0' "$EVENTS"
printf '%s\n' 'PASS: VMMFS PCI root topology'
