#!/bin/sh
set -u
ROOT=$(dirname "$0"); REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}; MNT=${VMM_MOUNT:-/var/tmp/dfvmm-revoke-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-revoke-test.log}; LOADER=${VMM_REVOKE_LOADER:-/var/tmp/vmm_revoke_loader}
MEM=${VMM_REVOKE_MEM:-2M}; TIMEOUT=${VMM_TIMEOUT:-20}; DELAY=${VMM_REVOKE_DELAY:-5}; LOADED=0; MOUNTED=0
say() { echo "$@" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }
mach() { echo "$MNT/machines/$1"; }
wait_path() { p=$1; i=0; while [ "$i" -lt "$TIMEOUT" ]; do [ -e "$p" ] && return 0; sleep 1; i=$((i + 1)); done; return 1; }
cleanup_machine()
{
	vm=$1; [ -d "$(mach "$vm")" ] || return 0; echo force >"$(mach "$vm")/stopped" 2>>"$LOG"
	i=0; while [ "$i" -lt "$TIMEOUT" ] && [ -d "$(mach "$vm")" ]; do
		rmdir "$(mach "$vm")" >>"$LOG" 2>&1 && return 0; sleep 1; i=$((i + 1))
	done; return 1
}
cleanup()
{
	set +e; cleanup_machine revoke_exit; cleanup_machine revoke_hang
	[ "$MOUNTED" -eq 1 ] && umount "$MNT" >>"$LOG" 2>&1 && MOUNTED=0
	[ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] && kldunload vmm >>"$LOG" 2>&1
	rm -f /var/tmp/vmmld_revoke_exit /var/tmp/vmmld_revoke_hang
}
make_wrapper()
{
	w=/var/tmp/vmmld_revoke_$1; printf '#!/bin/sh\nexec %s %s %s %s\n' \
	    "$LOADER" "$1" "$2" "$DELAY" >"$w" || fail "write $w"
	chmod +x "$w" || fail "chmod $w"; echo "$w"
}
run_case()
{
	mode=$1; vm=revoke_$1; result=/var/tmp/dfvmm-revoke-$1.result; wrapper=$(make_wrapper "$mode" "$result")
	rm -f "$result" "$result.ready"; run mkdir "$(mach "$vm")"
	printf '1\n' >"$(mach "$vm")/vcpu" || fail vcpu; printf '%s\n' "$MEM" >"$(mach "$vm")/mem" || fail mem
	printf '%s\n' "$wrapper" >"$(mach "$vm")/loader" || fail loader; cat "$(mach "$vm")/events" >>"$LOG"
	run rm "$(mach "$vm")/stopped"; wait_path "$result.ready" || fail "$mode child did not inherit fds"
	[ "$mode" = "hang" ] && echo force >"$(mach "$vm")/stopped"; wait_path "$result" || fail "$mode revoke result not observed"
	cat "$result" >>"$LOG"; grep -q '^pass=1$' "$result" || fail "$mode revoke"
	cleanup_machine "$vm" || fail "$mode cleanup"; say "PASS: $mode"
}
: >"$LOG" || exit 1; trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"; [ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
run cc -Wall -Wextra -Werror -std=c11 -O2 "$ROOT/loader_revoke.c" -o "$LOADER"
kldstat -n vmm >/dev/null 2>&1 || { run kldload "$VMM_KO"; LOADED=1; }
[ -x /sbin/mount_vmm ] || run ln -sf /sbin/mount_std /sbin/mount_vmm
run mkdir -p "$MNT"; run mount -t vmm vmm "$MNT"; MOUNTED=1
run_case exit; run_case hang; say "PASS"
