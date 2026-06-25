#!/bin/sh
# pc64 lifecycle harness for destructive paths that must stop real vCPUs.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-lifecycle-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-lifecycle-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_lifecycle_smoke_loader}
MEM=${VMM_LIFECYCLE_MEM:-2M}
TIMEOUT=${VMM_TIMEOUT:-20}
LOADED=0
MOUNTED=0

say() { echo "$@" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }
mach() { echo "$MNT/machines/$1"; }

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
		printf "%s\n" "$seen" | grep -qx "$pattern" && return 0
		sleep 1
		i=$((i + 1))
	done
	printf "%s\n" "$seen" >>"$LOG"
	return 1
}

wait_machine_absent()
{
	vm=$1
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		ls "$MNT/machines" 2>>"$LOG" | grep -qx "$vm" || return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

wrapper()
{
	vm=$1
	w=/var/tmp/vmmld_lifecycle_$vm
	printf '#!/bin/sh\nexec %s loop\n' "$LOADER" >"$w" || fail "write $w"
	chmod +x "$w" || fail "chmod $w"
	echo "$w"
}

ensure_mount()
{
	[ "$MOUNTED" -eq 1 ] && return 0
	run mkdir -p "$MNT"
	run mount -t vmm vmm "$MNT"
	MOUNTED=1
}

cleanup_machine()
{
	vm=$1
	[ "$MOUNTED" -eq 1 ] || return 0
	[ -d "$(mach "$vm")" ] || return 0
	echo force >"$(mach "$vm")/stopped" 2>>"$LOG"
	i=0
	while [ "$i" -lt "$TIMEOUT" ] && [ -d "$(mach "$vm")" ]; do
		rmdir "$(mach "$vm")" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	cleanup_machine force_loop
	cleanup_machine lease_loop
	[ "$MOUNTED" -eq 1 ] && umount -f "$MNT" >>"$LOG" 2>&1 && MOUNTED=0
	[ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] && kldunload vmm >>"$LOG" 2>&1
	rm -f /var/tmp/vmmld_lifecycle_force_loop /var/tmp/vmmld_lifecycle_lease_loop
}

start_loop()
{
	vm=$1
	w=$(wrapper "$vm")
	run mkdir "$(mach "$vm")"
	printf '1\n' >"$(mach "$vm")/vcpu" || fail "$vm vcpu"
	printf '%s\n' "$MEM" >"$(mach "$vm")/mem" || fail "$vm mem"
	printf '%s\n' "$w" >"$(mach "$vm")/loader" || fail "$vm loader"
	cat "$(mach "$vm")/events" >>"$LOG"
	run rm "$(mach "$vm")/stopped"
	wait_event "$(mach "$vm")/events" '^started$' || fail "$vm started"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
run cc -Wall -Wextra -Werror -std=c11 -O2 "$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"
kldstat -n vmm >/dev/null 2>&1 || { run kldload "$VMM_KO"; LOADED=1; }
[ -x /sbin/mount_vmm ] || run ln -sf /sbin/mount_std /sbin/mount_vmm

ensure_mount
start_loop force_loop
if umount "$MNT" >>"$LOG" 2>&1; then
	MOUNTED=0
	fail "plain umount succeeded while vCPU was running"
fi
run umount -f "$MNT"
MOUNTED=0
say "PASS: force unmount"

ensure_mount
start_loop lease_loop
exec 7<"$(mach lease_loop)/lease" || fail "lease open"
exec 7<&-
wait_machine_absent lease_loop || fail "lease close did not delete machine"
say "PASS: lease delete"
say "PASS"
