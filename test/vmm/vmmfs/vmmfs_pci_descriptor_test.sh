#!/bin/sh
# Verify static descriptor configuration and powered resource revocation.
set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
MOUNT=${VMMFS_MOUNT:-/var/tmp/vmmfs-pci-descriptor}
MACHINE=${VMMFS_MACHINE:-descriptor0}
SLOT=$MOUNT/$MACHINE/pci/0000:00:01.0
CLIENT=/var/tmp/vmmfs_pci_descriptor_client
LOADER=/var/tmp/vmmfs_halt_loader
BASE=/var/tmp/vmmfs-pci-descriptor-base-$$
BAD=/var/tmp/vmmfs-pci-descriptor-bad-$$
LOG=/var/tmp/vmmfs-pci-descriptor-client-$$

cleanup() {
	set +e
	[ -n "${CLIENT_PID:-}" ] && kill "$CLIENT_PID" 2>/dev/null
	[ -n "${CLIENT_PID:-}" ] && wait "$CLIENT_PID" 2>/dev/null
	[ -e "$SLOT/descriptor" ] && : | "$CLIENT" replace "$SLOT/descriptor"
	[ -d "$SLOT" ] && rmdir "$SLOT"
	[ -d "$MOUNT/$MACHINE" ] && rmdir "$MOUNT/$MACHINE"
	mount | grep -q " on $MOUNT " && umount "$MOUNT"
	rmdir "$MOUNT"
	kldstat -n vmmfs >/dev/null 2>&1 && kldunload vmmfs
	kldstat -n vmm >/dev/null 2>&1 && kldunload vmm
	rm -f "$CLIENT" "$LOADER" "$BASE" "$BAD" "$LOG"
}

wait_for() {
	line=$1
	for attempt in $(jot 100); do
		grep -qx "$line" "$LOG" && return 0
		sleep 0.1
	done
	cat "$LOG" >&2
	return 1
}

trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ]
! kldstat -n vmm >/dev/null 2>&1
! kldstat -n vmmfs >/dev/null 2>&1
make -C "$REPO/sys/dev/virtual/vmm"
make -C "$REPO/sys/vfs/vmmfs"
cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys" \
	"$REPO/test/vmm/vmmfs/vmmfs_pci_descriptor_client.c" -o "$CLIENT"
cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys" \
	"$REPO/test/vmm/vmmfs/vmmfs_halt_loader.c" -o "$LOADER"
kldload "$REPO/sys/dev/virtual/vmm/vmm.ko"
kldload "$REPO/sys/vfs/vmmfs/vmmfs.ko"
mkdir "$MOUNT"
mount -t vmmfs vmmfs "$MOUNT"
mkdir "$MOUNT/$MACHINE" "$MOUNT/$MACHINE/pci/0000:00:01.0"
printf '%s\n' 2097152 >"$MOUNT/$MACHINE/mem"
printf '%s\n' 1 >"$MOUNT/$MACHINE/vcpu"
printf '%s\n' "$LOADER --check-pci-config-loop" >"$MOUNT/$MACHINE/loader"
cat >"$BASE" <<'EOF'
version=1
header.type=endpoint
vendor_id=0x1af4
device_id=0x1042
subsystem_vendor_id=0x1af4
subsystem_device_id=0x0002
class=0x010000
revision=0x01
intx.pin=none
bar0.type=mem32
bar0.size=0x4000
bar0.prefetchable=0
doorbell0.bar=0
doorbell0.offset=0x0000
doorbell0.size=0x0004
doorbell0.width=4
doorbell0.space=mmio
config0.bar=0
config0.offset=0x0100
config0.width=4
config0.space=mmio
cap0.kind=pcie
cap1.kind=msix
cap1.vectors=4
cap1.table.bar=0
cap1.table.offset=0x1000
cap1.pba.bar=0
cap1.pba.offset=0x2000
cap2.kind=blob
cap2.id=0x09
cap2.access=static
cap2.data=0000
EOF
"$CLIENT" create "$SLOT/descriptor" <"$BASE"
cmp "$BASE" "$SLOT/descriptor"
test -e "$SLOT/events"
test -e "$SLOT/config"
sed 's/cap2.access=static/cap2.access=proxy/' "$BASE" >"$BAD"
if "$CLIENT" replace "$SLOT/descriptor" <"$BAD" 2>/dev/null; then
	echo "proxy capability unexpectedly accepted" >&2
	exit 1
fi
cmp "$BASE" "$SLOT/descriptor"
"$CLIENT" hold "$SLOT/descriptor" "$SLOT/kick0" "$SLOT/config" <"$BASE" >"$LOG" 2>&1 &
CLIENT_PID=$!
wait_for committed
rm "$MOUNT/$MACHINE/stopped"
wait_for ready
wait_for kick
wait_for config
if sh -c 'exec 3<"$1"' sh "$SLOT/kick0" 2>/dev/null; then
	echo "unauthorized kick open unexpectedly succeeded" >&2
	exit 1
fi
if sh -c 'exec 3<"$1"' sh "$SLOT/events" 2>/dev/null; then
	echo "unauthorized events open unexpectedly succeeded" >&2
	exit 1
fi
if sh -c 'exec 3<>"$1"' sh "$SLOT/config" 2>/dev/null; then
	echo "unauthorized config open unexpectedly succeeded" >&2
	exit 1
fi
for name in bar0 dma kick0 msix0 msix1 msix2 msix3; do
	test -e "$SLOT/$name"
done
touch "$MOUNT/$MACHINE/stopped"
wait "$CLIENT_PID"
unset CLIENT_PID
grep -qx revoked "$LOG"
test -e "$MOUNT/$MACHINE/stopped"
test -e "$SLOT/config"
for name in bar0 dma kick0 msix0 msix1 msix2 msix3; do
	test ! -e "$SLOT/$name"
done
: | "$CLIENT" replace "$SLOT/descriptor"
test ! -e "$SLOT/events"
test ! -e "$SLOT/config"
printf '%s\n' 'PASS: VMMFS PCI descriptor static configuration and revoke'
