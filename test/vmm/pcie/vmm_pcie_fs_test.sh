#!/bin/sh
# pc64 harness for the P1 vPCIe fabric and vmmfs presentation only.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-pcie-fs-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-pcie-fs-test.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-pcie-fs-$$-mount_vmmfs}
TIMEOUT=${VMM_TIMEOUT:-20}
LOADED=0
MOUNTED=0

say() { echo "$@" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
		fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
		fail "$VMM_KO contains .eh_frame"
}

unmount_when_quiesced()
{
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		umount "$MNT" >>"$LOG" 2>&1 && {
			MOUNTED=0
			return 0
		}
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	if [ "$MOUNTED" -eq 1 ]; then
		rmdir "$MNT/pcie1/devices/testp1" >>"$LOG" 2>&1
		rmdir "$MNT/pcie0/devices/testp1" >>"$LOG" 2>&1
		rmdir "$MNT/pcie1" >>"$LOG" 2>&1
		rmdir "$MNT/pcie0" >>"$LOG" 2>&1
		umount "$MNT" >>"$LOG" 2>&1 && MOUNTED=0
	fi
	[ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] &&
		kldunload vmm >>"$LOG" 2>&1
	rm -f "$MOUNT_HELPER"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be an absolute path" ;; esac
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
check_module_image
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"

run kldload "$VMM_KO"; LOADED=1
case "$MOUNT_HELPER" in *_vmm) ;; *) fail "invalid mount helper path" ;; esac
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run mkdir -p "$MNT"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1

# Root names are ordinary machine names; no VFS compatibility directories remain.
run mkdir "$MNT/machines"
run mkdir "$MNT/host"
run mkdir "$MNT/devices"
run rmdir "$MNT/machines"
run rmdir "$MNT/host"
run rmdir "$MNT/devices"

run mkdir "$MNT/pcie0"
run mkdir "$MNT/pcie1"
run mkdir "$MNT/pcie0/devices/testp1"
[ -d "$MNT/pcie0/devices/testp1" ] || fail "missing direct function"

run mv "$MNT/pcie0/devices/testp1" \
	"$MNT/pcie1/devices/testp1"
[ -d "$MNT/pcie1/devices/testp1" ] || fail "missing moved function"

run rmdir "$MNT/pcie1/devices/testp1"
run rmdir "$MNT/pcie0"
run rmdir "$MNT/pcie1"
unmount_when_quiesced || fail "machine teardown did not complete"
run kldunload vmm; LOADED=0
say "PASS: vPCIe P1 filesystem relation"
