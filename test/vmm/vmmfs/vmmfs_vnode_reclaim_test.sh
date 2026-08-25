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

make -C "$REPO/sys/vfs/vmmfs"
kldload "$VMM_KO"
kldload "$REPO/sys/vfs/vmmfs/vmmfs.ko"

for index in $(jot "$COUNT"); do
	machine="reclaim$index"
	mkdir "$MOUNT"
	mount -t vmmfs vmmfs "$MOUNT"
	mkdir "$MOUNT/$machine"
	printf '1\n' >"$MOUNT/$machine/vcpu"
	printf '%s\n' "$MEMORY_BYTES" >"$MOUNT/$machine/mem"
	exec 3<>"$MOUNT/$machine/boot"
	exec 3>&-
	wait_stopped "$machine" || fail "boot close did not stop $machine"
	rmdir "$MOUNT/$machine"
	umount "$MOUNT"
	rmdir "$MOUNT"
done

kldunload vmmfs
kldunload vmm
echo "PASS: VMMFS vnode reclaim lifecycle"
