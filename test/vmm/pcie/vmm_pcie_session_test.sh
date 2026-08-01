#!/bin/sh
# pc64 P2 vPCIe provider/consumer session harness.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BASE_SYS=$(cd "$REPO/../nvkm/sys" && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-pcie-session-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-pcie-session-test.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-pcie-session-$$-mount_vmm}
BIN=${VMM_PCIE_SESSION_TEST:-/var/tmp/vmm_pcie_session_test}
LOADED=0
MOUNTED=0

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }

cleanup()
{
	set +e
	if [ "$MOUNTED" -eq 1 ]; then
		rmdir "$MNT/session0/devices/session0" >>"$LOG" 2>&1
		rmdir "$MNT/session0" >>"$LOG" 2>&1
		umount "$MNT" >>"$LOG" 2>&1 && MOUNTED=0
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1
	fi
	rm -f "$MOUNT_HELPER" "$BIN"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be absolute" ;; esac
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"

run cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys/vmm" -I "$BASE_SYS" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$REPO/test/vmm/pcie/vmm_pcie_session_test.c" -o "$BIN"
run kldload "$VMM_KO"; LOADED=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1
run mkdir "$MNT/session0"
run mkdir "$MNT/session0/devices/session0"
run "$BIN" "$MNT/session0/devices/session0"
run rmdir "$MNT/session0/devices/session0"
run rmdir "$MNT/session0"
run umount "$MNT"; MOUNTED=0
run kldunload vmm; LOADED=0
say 'PASS: vPCIe provider/consumer session'
