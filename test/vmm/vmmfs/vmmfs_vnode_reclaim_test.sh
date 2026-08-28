#!/bin/sh
# Exercise deleted VMMFS boot vnodes across mount teardown.
set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
MOUNT=${VMMFS_MOUNT:-/var/tmp/vmmfs-vnode-reclaim}
COUNT=${VMMFS_RECLAIM_COUNT:-32}
MEMORY_BYTES=${VMMFS_MEMORY_BYTES:-2097152}
VMM_KO=${VMM_KO:-$REPO/sys/dev/virtual/vmm/vmm.ko}

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

wait_stopped()
{
	machine=$1
	for _ in $(jot 50); do
		[ -e "$MOUNT/$machine/stopped" ] && return 0
		sleep 1
	done
	return 1
}

cleanup()
{
	set +e
	mount | grep -q " on $MOUNT " && umount "$MOUNT"
	[ -d "$MOUNT" ] && rmdir "$MOUNT"
	kldstat -n vmmfs >/dev/null 2>&1 && kldunload vmmfs
	kldstat -n vmm >/dev/null 2>&1 && kldunload vmm
}

trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "must run as root"
! kldstat -n vmm >/dev/null 2>&1 || fail "vmm already loaded"
! kldstat -n vmmfs >/dev/null 2>&1 || fail "vmmfs already loaded"
[ -f "$VMM_KO" ] || fail "missing vmm module: $VMM_KO"

make -s -C "$REPO/sys/vfs/vmmfs" clean all
kldload "$VMM_KO"
kldload "$REPO/sys/vfs/vmmfs/vmmfs.ko"

for index in $(jot "$COUNT"); do
	machine="reclaim$index"
	mkdir "$MOUNT"
	mount -t vmmfs vmmfs "$MOUNT"
	race="race$index"
	for worker in $(jot 8); do
		(mkdir "$MOUNT/$race" 2>/dev/null || :) &
	done
	wait
	[ -d "$MOUNT/$race" ] || fail "concurrent create did not publish $race"
	rmdir "$MOUNT/$race" || fail "concurrent create did not reclaim $race"
	mkdir "$MOUNT/$machine"
	printf '1\n' >"$MOUNT/$machine/vcpu"
	printf '%s\n' "$MEMORY_BYTES" >"$MOUNT/$machine/mem"
	stopped_inode=$(stat -f %i "$MOUNT/$machine/stopped")
	mkdir "$MOUNT/$machine/pci/0000:00:01.0"
	exec 4<"$MOUNT/$machine/pci/0000:00:01.0/descriptor"
	touch "$MOUNT/$machine/serial/com1"
	exec 5<>"$MOUNT/$machine/serial/com1"
	rmdir "$MOUNT/$machine"
	exec 5>&-
	exec 4>&-
	machine="descriptor-race$index"
	mkdir "$MOUNT/$machine"
	mkdir "$MOUNT/$machine/pci/0000:00:01.0"
	descriptor='version=1
header.type=endpoint
vendor_id=0x1af4
device_id=0x1042
subsystem_vendor_id=0x1af4
subsystem_device_id=0x1042
class=0x010000
revision=0
intx.pin=none
'
	(
		for attempt in $(jot 100); do
			printf '%s' "$descriptor" > \
			    "$MOUNT/$machine/pci/0000:00:01.0/descriptor" 2>/dev/null || exit 0
		done
	) 2>/dev/null &
	writer=$!
	rmdir "$MOUNT/$machine" || fail "descriptor writer blocked rmdir"
	wait "$writer" || fail "descriptor writer exited unexpectedly"
	[ ! -e "$MOUNT/$machine" ] ||
		fail "descriptor writer left a deleted machine reachable"
	machine="reclaim$index"
	mkdir "$MOUNT/$machine"
	printf '1\n' >"$MOUNT/$machine/vcpu"
	printf '%s\n' "$MEMORY_BYTES" >"$MOUNT/$machine/mem"
	stopped_inode=$(stat -f %i "$MOUNT/$machine/stopped")
	mkdir "$MOUNT/$machine/pci/0000:00:01.0"
	printf '%s' 'version=1
header.type=endpoint
vendor_id=0x1af4
device_id=0x1042
subsystem_vendor_id=0x1af4
subsystem_device_id=0x1042
class=0x010000
revision=0
intx.pin=none
' >"$MOUNT/$machine/pci/0000:00:01.0/descriptor"
	exec 3<>"$MOUNT/$machine/boot"
	[ ! -e "$MOUNT/$machine/stopped" ] ||
		fail "stopped remained published while $machine was running"
	[ -e "$MOUNT/$machine/pci/0000:00:01.0/dma" ] ||
		fail "PCI resource generation was not published at power-on"
	exec 3>&-
	wait_stopped "$machine" || fail "boot close did not stop $machine"
	[ ! -e "$MOUNT/$machine/pci/0000:00:01.0/dma" ] ||
		fail "PCI resource generation remained published after power-off"
	[ "$stopped_inode" != "$(stat -f %i "$MOUNT/$machine/stopped")" ] ||
		fail "stopped reused its vnode across $machine restart"
	rmdir "$MOUNT/$machine"
	umount "$MOUNT"
	rmdir "$MOUNT"
done

kldunload vmmfs
kldunload vmm
echo "PASS: VMMFS vnode reclaim lifecycle"
