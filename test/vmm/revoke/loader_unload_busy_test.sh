#!/bin/sh
set -u
ROOT=$(dirname "$0"); REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}; MNT=${VMM_MOUNT:-/var/tmp/dfvmm-unload-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-unload-busy-test.log}; LOADER=${VMM_REVOKE_LOADER:-/var/tmp/vmm_revoke_loader}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-unload-$$-mount_vmm}
MEM=${VMM_REVOKE_MEM:-2M}; TIMEOUT=${VMM_TIMEOUT:-20}; DELAY=${VMM_REVOKE_DELAY:-5}; LOADED=0; MOUNTED=0; HOLD_PID=
say() { echo "$@" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }
mach() { echo "$MNT/$1"; }
check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}
wait_path() { p=$1; i=0; while [ "$i" -lt "$TIMEOUT" ]; do [ -e "$p" ] && return 0; sleep 1; i=$((i + 1)); done; return 1; }
cleanup_machine() { vm=$1; [ -d "$(mach "$vm")" ] || return 0; touch "$(mach "$vm")/stopped" 2>>"$LOG"; rmdir "$(mach "$vm")" >>"$LOG" 2>&1 || true; }
cleanup()
{
	set +e; [ -n "$HOLD_PID" ] && kill "$HOLD_PID" >>"$LOG" 2>&1
	cleanup_machine revoke_hold
	if [ "$MOUNTED" -eq 1 ]; then
		i=0
		while [ "$i" -lt "$TIMEOUT" ]; do
			umount "$MNT" >>"$LOG" 2>&1 && { MOUNTED=0; break; }
			sleep 1
			i=$((i + 1))
		done
	fi
	[ "$LOADED" -eq 1 ] && kldunload vmm >>"$LOG" 2>&1
	rm -f /var/tmp/vmmld_revoke_hold "$MOUNT_HELPER"
}
make_wrapper()
{
	w=/var/tmp/vmmld_revoke_hold; printf '#!/bin/sh\nexec %s hold %s %s\n' "$LOADER" "$RESULT" "$DELAY" >"$w" || fail "write $w"
	chmod +x "$w" || fail "chmod $w"; echo "$w"
}
: >"$LOG" || exit 1; trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be an absolute path" ;; esac
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
check_module_image
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded; unload_busy needs script-owned module"
run cc -Wall -Wextra -Werror -std=c11 -O2 "$ROOT/loader_revoke.c" -o "$LOADER"
run kldload "$VMM_KO"; LOADED=1
case "$MOUNT_HELPER" in *_vmm) ;; *) fail "VMM_MOUNT_HELPER path must end in _vmm for mount_std" ;; esac
run rm -f "$MOUNT_HELPER"; run ln -s /sbin/mount_std "$MOUNT_HELPER"
run mkdir -p "$MNT"; run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1
RESULT=/var/tmp/dfvmm-revoke-hold.result; rm -f "$RESULT" "$RESULT.ready"; WRAPPER=$(make_wrapper)
run mkdir "$(mach revoke_hold)"
printf '1\n' >"$(mach revoke_hold)/vcpu" || fail vcpu; printf '%s\n' "$MEM" >"$(mach revoke_hold)/mem" || fail mem
printf '%s\n' "$WRAPPER" >"$(mach revoke_hold)/loader" || fail loader; cat "$(mach revoke_hold)/events" >>"$LOG"
run rm "$(mach revoke_hold)/stopped"; wait_path "$RESULT.ready" || fail "hold child did not inherit fds"
wait_path "$RESULT" || fail "hold revoke result not observed"; cat "$RESULT" >>"$LOG"; grep -q '^pass=1$' "$RESULT" || fail "hold revoke"
HOLD_PID=$(awk -F= '/^pid=/{print $2}' "$RESULT"); cleanup_machine revoke_hold
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
	if umount "$MNT" >>"$LOG" 2>&1; then
		MOUNTED=0
		break
	fi
	sleep 1
	i=$((i + 1))
done
[ "$MOUNTED" -eq 0 ] || fail "umount $MNT"
kldunload vmm >>"$LOG" 2>&1 && fail "kldunload succeeded while loader mapping was held"
kill "$HOLD_PID" >>"$LOG" 2>&1 || true; HOLD_PID=
i=0; while [ "$i" -lt "$TIMEOUT" ]; do kldunload vmm >>"$LOG" 2>&1 && { LOADED=0; say "PASS"; exit 0; }; sleep 1; i=$((i + 1)); done
fail "kldunload did not complete after holder exit"
