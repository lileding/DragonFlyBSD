#!/bin/sh
# Verify both boot transports return to STOPPED when fd3 closes uncommitted.
set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
MOUNT=${VMMFS_MOUNT:-/var/tmp/vmmfs-boot-close}
MEMORY_BYTES=${VMMFS_MEMORY_BYTES:-2097152}
DIRECT=bootclose0
LOADER=loaderclose0

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
	for machine in "$DIRECT" "$LOADER"; do
		if [ -d "$MOUNT/$machine" ]; then
			[ -e "$MOUNT/$machine/stopped" ] || touch "$MOUNT/$machine/stopped"
			rmdir "$MOUNT/$machine"
		fi
	done
	mount | grep -q " on $MOUNT " && umount "$MOUNT"
	rmdir "$MOUNT"
	kldstat -n vmmfs >/dev/null 2>&1 && kldunload vmmfs
	kldstat -n vmm >/dev/null 2>&1 && kldunload vmm
}

configure()
{
	machine=$1
	mkdir "$MOUNT/$machine"
	printf '1\n' >"$MOUNT/$machine/vcpu"
	printf '%s\n' "$MEMORY_BYTES" >"$MOUNT/$machine/mem"
}

trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "must run as root"
! kldstat -n vmm >/dev/null 2>&1 || fail "vmm already loaded"
! kldstat -n vmmfs >/dev/null 2>&1 || fail "vmmfs already loaded"
make -C "$REPO/sys/dev/virtual/vmm"
make -C "$REPO/sys/vfs/vmmfs"
kldload "$REPO/sys/dev/virtual/vmm/vmm.ko"
kldload "$REPO/sys/vfs/vmmfs/vmmfs.ko"
mkdir "$MOUNT"
mount -t vmmfs vmmfs "$MOUNT"

configure "$DIRECT"
exec 3<>"$MOUNT/$DIRECT/boot"
exec 3>&- || true
wait_stopped "$DIRECT" || fail "direct boot close did not restore stopped"
rmdir "$MOUNT/$DIRECT"

configure "$LOADER"
printf '%s\n' 'exit 0' >"$MOUNT/$LOADER/loader"
if rm "$MOUNT/$LOADER/stopped"; then
	fail "loader without fd3 submission unexpectedly succeeded"
fi
wait_stopped "$LOADER" || fail "loader fd3 close did not restore stopped"
rmdir "$MOUNT/$LOADER"

echo "PASS: VMMFS boot fd close lifecycle"
