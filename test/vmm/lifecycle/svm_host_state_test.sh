#!/bin/sh
# pc64 SVM host-state lifecycle harness.
#
# vmmfs vfs_init() claims SVM on every active pCPU.  This harness runs the
# root-interrupt stress guest, then verifies that module unload restored every
# CPU claimed during the final SVM lifetime recorded in dmesg.  The kernel
# message buffer is circular, so its line count cannot delimit a new run.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
LOG=${VMM_LOG:-/var/tmp/dfvmm-svm-host-state.log}
DMESG_AFTER=/var/tmp/dfvmm-svm-host-state-after-$$.log
DMESG_SESSION=/var/tmp/dfvmm-svm-host-state-session-$$.log

say() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }

cleanup()
{
	rm -f "$DMESG_AFTER" "$DMESG_SESSION"
}

capture_dmesg()
{
	dmesg >"$1" || fail "dmesg $1"
}

extract_cpus()
{
	awk -v state="$1" '$1 == "vmm:" && $2 == "svm" &&
	    $3 ~ /^cpu[0-9]+$/ && $4 == state {
		cpu = $3
		sub(/^cpu/, "", cpu)
		print cpu
	}' "$DMESG_SESSION" | sort -n -u
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
[ "$(uname -m)" = x86_64 ] || fail "run on a pc64 host"
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"

say "+ root interrupt stress"
VMM_KO="$VMM_KO" VMM_LOG="$LOG.root-intr" \
	"$REPO/test/vmm/root_intr/root_interrupt_test.sh" >>"$LOG" 2>&1 ||
	fail "root interrupt stress"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm remains loaded"
capture_dmesg "$DMESG_AFTER"

expected=$(sysctl -n hw.ncpu) || fail "read hw.ncpu"
initialized=$(grep -n "^vmm: svm initialized cpus=$expected$" "$DMESG_AFTER" |
	tail -n 1 | cut -d: -f1) || fail "find SVM initialization record"
end=$(grep -n "^vmm: svm uninitialized cpus=$expected$" "$DMESG_AFTER" |
	tail -n 1 | cut -d: -f1) || fail "find SVM teardown record"
start=$(grep -n "^vmm: svm cpu0 enabled " "$DMESG_AFTER" |
	awk -F: -v max="$initialized" '$1 < max { line = $1 } END { print line }') ||
	fail "find CPU0 enable record"
[ -n "$start" ] && [ -n "$initialized" ] && [ -n "$end" ] &&
	[ "$start" -le "$initialized" ] && [ "$initialized" -le "$end" ] ||
	fail "invalid final SVM lifetime"
sed -n "${start},${end}p" "$DMESG_AFTER" >"$DMESG_SESSION" ||
	fail "extract final SVM lifetime"

enabled=$(extract_cpus enabled)
restored=$(extract_cpus restored)
enabled_count=$(printf '%s\n' "$enabled" | sed '/^$/d' | wc -l)
restored_count=$(printf '%s\n' "$restored" | sed '/^$/d' | wc -l)
[ "$enabled_count" -eq "$expected" ] ||
	fail "enabled cpu count=$enabled_count expected=$expected"
[ "$restored_count" -eq "$expected" ] ||
	fail "restored cpu count=$restored_count expected=$expected"
[ "$enabled" = "$restored" ] || fail "enabled/restored CPU sets differ"
say "PASS: SVM host state restored on $expected CPUs"
