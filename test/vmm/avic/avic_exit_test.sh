#!/bin/sh
# pc64 AMD AVIC unsupported-exit harness.  These exits must be observable and
# decoded in per-machine events, but dfvmm must not emulate LAPIC/IPI fallback.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-avic-exit-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-avic-exit-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_avic_exit_smoke_loader}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-avic-exit-$$-mount_vmm}
WRAPPER=${VMM_LOADER_WRAPPER:-/var/tmp/vmmld_avic_exit}
MEM=${VMM_AVIC_MEM:-2M}
TIMEOUT=${VMM_TIMEOUT:-20}
LOADED=0
MOUNTED=0
MACHINES=

say() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; dump_runtime_state; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }
mach() { printf '%s/%s\n' "$MNT" "$1"; }

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

append_file()
{
	label=$1
	file=$2
	if [ -e "$file" ]; then
		{
			printf -- '--- %s: %s ---\n' "$label" "$file"
			cat "$file" 2>&1
			printf -- '--- end %s ---\n' "$label"
		} >>"$LOG"
	else
		printf -- '--- %s: %s missing ---\n' "$label" "$file" >>"$LOG"
	fi
}

dump_runtime_state()
{
	say "runtime state snapshot"
	kldstat -n vmm >>"$LOG" 2>&1 || true
	mount >>"$LOG" 2>&1 || true
	if [ "$MOUNTED" -eq 1 ]; then
		for vm in $MACHINES; do
			if [ -d "$(mach "$vm")" ]; then
				append_file "$vm-events" "$(mach "$vm")/events"
				append_file "$vm-console" "$(mach "$vm")/console"
			fi
		done
	fi
}

wait_event()
{
	file=$1
	first=$2
	second=${3:-}
	i=0
	seen=
	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$file" 2>>"$LOG")
		if [ -n "$out" ]; then
			seen="$seen
$out"
			{
				printf -- '--- poll events: %s ---\n' "$file"
				printf '%s\n' "$out"
				printf -- '--- end poll events ---\n'
			} >>"$LOG"
		fi
		if printf '%s\n' "$seen" | grep -q "$first"; then
			if [ -z "$second" ] ||
			    printf '%s\n' "$seen" | grep -q "$second"; then
				return 0
			fi
		fi
		sleep 1
		i=$((i + 1))
	done
	printf '%s\n' "$seen" >>"$LOG"
	return 1
}

cleanup_machine()
{
	vm=$1
	dir=$(mach "$vm")
	i=0

	[ -d "$dir" ] || return 0
	touch "$dir/stopped" 2>>"$LOG" || true
	while [ "$i" -lt "$TIMEOUT" ] && [ -d "$dir" ]; do
		rmdir "$dir" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

unmount_vmm()
{
	i=0
	[ "$MOUNTED" -eq 1 ] || return 0
	while [ "$i" -lt "$TIMEOUT" ]; do
		if umount "$MNT" >>"$LOG" 2>&1; then
			MOUNTED=0
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	for vm in $MACHINES; do
		cleanup_machine "$vm"
	done
	[ "$MOUNTED" -eq 1 ] && unmount_vmm
	[ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] &&
	    kldunload vmm >>"$LOG" 2>&1
	rm -f "$MOUNT_HELPER" "$WRAPPER" "$LOADER"
}

run_case()
{
	mode=$1
	first=$2
	second=$3
	vm=$mode
	dir=$(mach "$vm")

	printf '#!/bin/sh\nexec %s %s\n' "$LOADER" "$mode" >"$WRAPPER" ||
	    fail "write $WRAPPER"
	chmod +x "$WRAPPER" || fail "chmod $WRAPPER"
	run mkdir "$dir"
	MACHINES="$MACHINES $vm"
	printf '1\n' >"$dir/vcpu" || fail "write $vm/vcpu"
	printf '%s\n' "$MEM" >"$dir/mem" || fail "write $vm/mem"
	printf '%s\n' "$WRAPPER" >"$dir/loader" || fail "write $vm/loader"
	cat "$dir/events" >>"$LOG"
	run rm "$dir/stopped"
	wait_event "$dir/events" 'svm avic enabled' ||
	    fail "$mode did not enable AVIC"
	wait_event "$dir/events" 'svm avic bound' ||
	    fail "$mode did not bind AVIC"
	wait_event "$dir/events" "$first" "$second" ||
	    fail "$mode did not emit decoded AVIC exit"
	touch "$dir/stopped" || fail "stop $mode"
	wait_event "$dir/events" 'state stopped' ||
	    fail "$mode did not stop"
	run rmdir "$dir"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
say "AMD AVIC unsupported exit decode test"
say "repo=$REPO vmm_ko=$VMM_KO mount=$MNT mem=$MEM"
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be an absolute path" ;; esac
case "$MOUNT_HELPER" in *_vmm) ;; *) fail "VMM_MOUNT_HELPER path must end in _vmm" ;; esac
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
check_module_image
run cc -Wall -Wextra -Werror -std=c11 -O2 \
    "$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"

kldstat -n vmm >/dev/null 2>&1 &&
    fail "vmm already loaded; unload it before running this harness"
run kldload "$VMM_KO"; LOADED=1
run rm -f "$MOUNT_HELPER"; run ln -s /sbin/mount_std "$MOUNT_HELPER"
run mkdir -p "$MNT"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1

run_case avicipi 'svm vcpu0 avic incomplete_ipi' 'icrl='
run_case avicnoaccel 'svm vcpu0 avic noaccel' 'reg=ldr'

unmount_vmm || fail "umount $MNT"
run kldunload vmm; LOADED=0
say "PASS: AMD AVIC unsupported exit decode"
