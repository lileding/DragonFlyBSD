#!/bin/sh
set -u
ROOT=$(dirname "$0"); REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}; MNT=${VMM_MOUNT:-/var/tmp/dfvmm-smoke-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-smoke-test.log}; LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_smoke_loader}
MEM=${VMM_SMOKE_MEM:-2M}; TIMEOUT=${VMM_TIMEOUT:-20}; LOADED=0; MOUNTED=0
say() { echo "$@" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }
mach() { echo "$MNT/machines/$1"; }
wait_event() { f=$1; p=$2; q=${3:-}; i=0; seen=; while [ "$i" -lt "$TIMEOUT" ]; do
	out=$(cat "$f" 2>>"$LOG"); [ -n "$out" ] && seen="$seen
$out"; printf "%s\n" "$seen" | grep -qx "$p" && { [ -z "$q" ] || printf "%s\n" "$seen" | grep -qx "$q"; } && return 0
	sleep 1; i=$((i + 1)); done; printf "%s\n" "$seen" >>"$LOG"; return 1; }
wait_console() { f=$1; p=$2; i=0; out=; while [ "$i" -lt "$TIMEOUT" ]; do
	out=$(cat "$f" 2>>"$LOG"); printf "%s\n" "$out" | grep -q "$p" && return 0
	sleep 1; i=$((i + 1)); done; printf "%s\n" "$out" >>"$LOG"; return 1; }
cleanup_machine()
{
	vm=$1; [ -d "$(mach "$vm")" ] || return 0
	echo force >"$(mach "$vm")/stopped" 2>>"$LOG"
	i=0; while [ "$i" -lt "$TIMEOUT" ] && [ -d "$(mach "$vm")" ]; do
		rmdir "$(mach "$vm")" >>"$LOG" 2>&1 && return 0; sleep 1; i=$((i + 1))
	done; return 1
}
cleanup()
{
	set +e; cleanup_machine vmmcall; cleanup_machine cpuid; cleanup_machine serial; cleanup_machine time; cleanup_machine xsetbv; cleanup_machine apicmsr; cleanup_machine timerint; cleanup_machine ud; cleanup_machine pic; cleanup_machine ioapic; cleanup_machine pm64; cleanup_machine hlt; cleanup_machine loop
	[ "$MOUNTED" -eq 1 ] && umount "$MNT" >>"$LOG" 2>&1 && MOUNTED=0
	[ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] && kldunload vmm >>"$LOG" 2>&1
	rm -f /var/tmp/vmmld_smoke_vmmcall /var/tmp/vmmld_smoke_cpuid /var/tmp/vmmld_smoke_serial /var/tmp/vmmld_smoke_time /var/tmp/vmmld_smoke_xsetbv /var/tmp/vmmld_smoke_apicmsr /var/tmp/vmmld_smoke_timerint /var/tmp/vmmld_smoke_ud /var/tmp/vmmld_smoke_pic /var/tmp/vmmld_smoke_ioapic /var/tmp/vmmld_smoke_pm64 /var/tmp/vmmld_smoke_hlt /var/tmp/vmmld_smoke_loop
}
wrapper() { w=/var/tmp/vmmld_smoke_$1; printf '#!/bin/sh\nexec %s %s\n' "$LOADER" "$1" >"$w"; chmod +x "$w"; echo "$w"; }
run_case()
{
	mode=$1; vm=$1; w=$(wrapper "$mode"); run mkdir "$(mach "$vm")"
	printf '1\n' >"$(mach "$vm")/vcpu"; printf '%s\n' "$MEM" >"$(mach "$vm")/mem"; printf '%s\n' "$w" >"$(mach "$vm")/loader"
	cat "$(mach "$vm")/events" >>"$LOG"; run rm "$(mach "$vm")/stopped"
	if [ "$mode" = loop ] || [ "$mode" = hlt ]; then wait_event "$(mach "$vm")/events" '^started$' || fail "$vm started"; echo force >"$(mach "$vm")/stopped"; wait_event "$(mach "$vm")/events" '^stopped$' || fail "$vm stopped"; [ -e "$(mach "$vm")/stopped" ] || fail "$vm stopped file"
	else wait_event "$(mach "$vm")/events" '^started$' '^stopped$' || fail "$vm self exit"; [ ! -e "$(mach "$vm")/stopped" ] || fail "$vm desired changed"; echo force >"$(mach "$vm")/stopped"; fi
	[ "$mode" != serial ] || wait_console "$(mach "$vm")/console" 'dfvmm-serial-ok' || fail "$vm console output"
	[ "$mode" != ud ] || wait_console "$(mach "$vm")/console" 'dfvmm-ud-ok' || fail "$vm console output"
	[ "$mode" != ud ] || wait_console "$(mach "$vm")/console" 'dfvmm-iret-ok' || fail "$vm iret output"
	[ "$mode" != pic ] || wait_console "$(mach "$vm")/console" 'dfvmm-pic-ok' || fail "$vm console output"
	[ "$mode" != ioapic ] || wait_console "$(mach "$vm")/console" 'dfvmm-ioapic-ok' || fail "$vm console output"
	[ "$mode" != pm64 ] || wait_console "$(mach "$vm")/console" 'dfvmm-pm64-ok' || fail "$vm console output"
	cleanup_machine "$vm" || fail "$vm cleanup"; say "PASS: $vm"
}
: >"$LOG" || exit 1; trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"; [ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
run cc -Wall -Wextra -Werror -std=c11 -O2 "$ROOT/smoke_loader.c" -o "$LOADER"
kldstat -n vmm >/dev/null 2>&1 || { run kldload "$VMM_KO"; LOADED=1; }
[ -x /sbin/mount_vmm ] || run ln -sf /sbin/mount_std /sbin/mount_vmm
run mkdir -p "$MNT"; run mount -t vmm vmm "$MNT"; MOUNTED=1
run_case vmmcall; run_case cpuid; run_case serial; run_case time; run_case xsetbv; run_case apicmsr; run_case timerint; run_case ud; run_case pic; run_case ioapic; run_case pm64; run_case hlt; run_case loop; say "PASS"
