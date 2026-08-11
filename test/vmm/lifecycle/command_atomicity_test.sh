#!/bin/sh
# pc64 lifecycle harness for declarative command commit and rmdir stop.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-command-atomicity-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-command-atomicity-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_command_atomicity_smoke_loader}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-command-atomicity-$$-mount_vmmfs}
MEM=${VMM_COMMAND_MEM:-2M}
TIMEOUT=${VMM_TIMEOUT:-20}
LOADED=0
MOUNTED=0

say()
{
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"
}

fail()
{
	say "FAIL: $*"
	exit 1
}

run()
{
	say "+ $*"
	"$@" >>"$LOG" 2>&1 || fail "$*"
}

mach()
{
	printf '%s/%s\n' "$MNT" "$1"
}

wait_event()
{
	file=$1
	pattern=$2
	i=0
	seen=

	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$file" 2>>"$LOG")
		[ -n "$out" ] && seen="$seen
$out"
		printf '%s\n' "$seen" | grep -q "$pattern" && return 0
		sleep 1
		i=$((i + 1))
	done
	printf '%s\n' "$seen" >>"$LOG"
	return 1
}

wait_machine_absent()
{
	vm=$1
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		[ ! -d "$(mach "$vm")" ] && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

unmount_cleanly()
{
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		umount "$MNT" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup_machine()
{
	vm=$1
	dir=$(mach "$vm")
	i=0

	[ -d "$dir" ] || return 0
	[ -e "$dir/stopped" ] || touch "$dir/stopped" >>"$LOG" 2>&1 || true
	touch "$dir/stopped" 2>>"$LOG" || true
	while [ "$i" -lt "$TIMEOUT" ] && [ -d "$dir" ]; do
		rmdir "$dir" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	cleanup_machine force_delete
	cleanup_machine incomplete_start
	[ "$MOUNTED" -eq 1 ] && unmount_cleanly && MOUNTED=0
	[ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] && kldunload vmm >>"$LOG" 2>&1
	rm -f /var/tmp/vmmld_command_atomicity_force_delete "$MOUNT_HELPER" "$LOADER"
}

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

start_loop()
{
	vm=force_delete
	w=/var/tmp/vmmld_command_atomicity_$vm

	printf '#!/bin/sh\nexec %s loop\n' "$LOADER" >"$w" || fail "write $w"
	chmod +x "$w" || fail "chmod $w"
	run mkdir "$(mach "$vm")"
	printf '1\n' >"$(mach "$vm")/vcpu" || fail "$vm vcpu"
	printf '%s\n' "$MEM" >"$(mach "$vm")/mem" || fail "$vm mem"
	printf '%s\n' "$w" >"$(mach "$vm")/loader" || fail "$vm loader"
	cat "$(mach "$vm")/events" >>"$LOG"
	run rm "$(mach "$vm")/stopped"
	wait_event "$(mach "$vm")/events" 'state running' || fail "$vm did not run"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be an absolute path" ;; esac
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
case "$MOUNT_HELPER" in *_vmm) ;; *) fail "VMM_MOUNT_HELPER must end in _vmm" ;; esac
check_module_image
run cc -Wall -Wextra -Werror -std=c11 -O2 "$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
run kldload "$VMM_KO"; LOADED=1
[ "${VMM_TRACE:-0}" = 1 ] && run sysctl debug.vmm.trace=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1

run mkdir "$(mach incomplete_start)"
cat "$(mach incomplete_start)/events" >>"$LOG"
run rm "$(mach incomplete_start)/stopped"
[ ! -e "$(mach incomplete_start)/stopped" ] ||
    fail "incomplete start kept stopped visible"
wait_event "$(mach incomplete_start)/events" 'reason=incomplete_config' ||
    fail "incomplete start did not record preparation failure"
wait_event "$(mach incomplete_start)/events" 'state stopped reason=start_failed error=22' ||
    fail "incomplete start did not converge current state"
printf 'reset\n' >"$(mach incomplete_start)/events" 2>>"$LOG" ||
    fail "incomplete reset command failed"
wait_event "$(mach incomplete_start)/events" 'reset done' ||
    fail "incomplete reset did not finish"
[ ! -e "$(mach incomplete_start)/stopped" ] ||
    fail "incomplete reset restored stopped despite desired running"
printf '1\n' >"$(mach incomplete_start)/vcpu" 2>>"$LOG" ||
    fail "desired running configuration write failed"
[ ! -s "$(mach incomplete_start)/vcpu" ] ||
    fail "desired running committed configuration after failed start"
run touch "$(mach incomplete_start)/stopped"
[ -e "$(mach incomplete_start)/stopped" ] ||
    fail "touch stopped did not restore desired stopped"
run rmdir "$(mach incomplete_start)"
wait_machine_absent incomplete_start || fail "incomplete start was not deleted"
say "PASS: failed start kept desired running and emitted lifecycle evidence"

start_loop
run touch "$(mach force_delete)/stopped"
[ -e "$(mach force_delete)/stopped" ] || fail "touch stopped failed"
run rmdir "$(mach force_delete)"
wait_machine_absent force_delete || fail "rmdir did not stop loop guest"
say "PASS: rmdir queued stop before deletion"

unmount_cleanly || fail "umount $MNT"
MOUNTED=0
run kldunload vmm; LOADED=0
say "PASS"
