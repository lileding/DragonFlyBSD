#!/bin/sh
# pc64 SVM two-vCPU terminal-fault drain regression.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-smp-terminal-vmm}
VM=${VMM_MACHINE:-smpterminal0}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_smp_terminal_loader}
WRAPPER=${VMM_WRAPPER:-/var/tmp/vmmld_smp_terminal}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-smp-terminal-$$-mount_vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-smp-terminal-test.log}
TIMEOUT=${VMM_TIMEOUT:-30}
LOADED=0
MOUNTED=0

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }
mach() { printf '%s/%s\n' "$MNT" "$VM"; }

wait_fault()
{
	minimum=$1
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		cat "$(mach)/events" >>"$LOG" 2>&1
		if [ "$(grep -c 'guest fault source=svm_shutdown' "$LOG" || true)" -ge "$minimum" ] &&
		    grep -q 'state running' "$LOG" &&
		    grep -q 'state draining reason=guest_fault' "$LOG" &&
		    grep -q 'state stopped reason=guest_fault' "$LOG"; then
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
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		touch "$(mach)/stopped" >>"$LOG" 2>&1
		i=0
		while [ "$i" -lt "$TIMEOUT" ]; do
			rmdir "$(mach)" >>"$LOG" 2>&1 && break
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		i=0
		while [ "$i" -lt "$TIMEOUT" ]; do
			umount "$MNT" >>"$LOG" 2>&1 && {
				MOUNTED=0
				break
			}
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1
	fi
	rm -f "$LOADER" "$WRAPPER" "$MOUNT_HELPER"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"
printf '#!/bin/sh\nexec "%s" smptriplefault\n' "$LOADER" >"$WRAPPER" ||
	fail "write $WRAPPER"
run chmod +x "$WRAPPER"
run kldload "$VMM_KO"; LOADED=1
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run mkdir -p "$MNT"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1
run mkdir "$(mach)"
printf '2\n' >"$(mach)/vcpu" || fail "write vcpu"
printf '2M\n' >"$(mach)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(mach)/loader" || fail "write loader"
run rm "$(mach)/stopped"
wait_fault 1 || fail "first two-vCPU terminal fault did not drain"
[ ! -e "$(mach)/stopped" ] || fail "guest fault recreated stopped"

run sh -c "echo reset > '$(mach)/events'"
wait_fault 2 || fail "reset did not reproduce the two-vCPU terminal fault"
[ ! -e "$(mach)/stopped" ] || fail "guest fault after reset recreated stopped"

run touch "$(mach)/stopped"
run rmdir "$(mach)"
run umount "$MNT"; MOUNTED=0
run kldunload vmm; LOADED=0
say "PASS: two-vCPU terminal-fault drain and reset"
