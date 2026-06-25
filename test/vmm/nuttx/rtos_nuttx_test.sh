#!/bin/sh
# pc64 true-hardware NuttX bring-up harness. Keep this wrapper small.
set -u
ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko} MNT=${VMM_MOUNT:-/var/tmp/dfvmm-nuttx-vmm}
VM=${VMM_MACHINE:-nuttx0} LOG=${VMM_LOG:-/var/tmp/dfvmm-nuttx-test.log}
ELF=${NUTTX_ELF:-/var/tmp/nuttx.elf} LOADER=${NUTTX_LOADER:-/var/tmp/vmmld_nuttx_elf}
WRAPPER=${NUTTX_LOADER_WRAPPER:-/var/tmp/vmmld_nuttx}
MEM=${NUTTX_MEM:-64M} PAT=${NUTTX_BOOT_PATTERN:-NuttShell}
TIMEOUT=${VMM_TIMEOUT:-20}
LOADED=0 MOUNTED=0
say() { echo "$@" | tee -a "$LOG"; }; fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }; mach() { echo "$MNT/machines/$VM"; }
cleanup()
{
	set +e
	if [ "$MOUNTED" -eq 1 ]; then
		if [ -d "$(mach)" ]; then
			echo force >"$(mach)/stopped"
			wait_for "$(mach)/events" '^stopped$' >/dev/null 2>&1
			i=0
			while [ "$i" -lt "$TIMEOUT" ] && [ -d "$(mach)" ]; do
				rmdir "$(mach)" >>"$LOG" 2>&1 && break
				sleep 1; i=$((i + 1))
			done
		fi
		i=0
		while [ "$i" -lt "$TIMEOUT" ] && [ "$MOUNTED" -eq 1 ]; do
			umount "$MNT" >>"$LOG" 2>&1 && { MOUNTED=0; break; }
			sleep 1; i=$((i + 1))
		done
	fi
	[ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] && kldunload vmm >>"$LOG" 2>&1
	rm -f "$WRAPPER"
}
wait_for()
{
	file=$1 pattern=$2 i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$file" 2>>"$LOG")
		[ -n "$out" ] && printf "%s\n" "$out" >>"$LOG"
		printf "%s\n" "$out" | grep -q "$pattern" && return 0
		sleep 1; i=$((i + 1))
	done
	return 1
}
: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
[ -f "$ELF" ] || fail "missing NUTTX_ELF=$ELF"
run cc -Wall -Wextra -Werror -std=c11 -O2 "$ROOT/rtos_nuttx_loader.c" -o "$LOADER"
printf '#!/bin/sh\nexec %s %s\n' "$LOADER" "$ELF" >"$WRAPPER" || fail "write $WRAPPER"
chmod +x "$WRAPPER" || fail "chmod $WRAPPER"
kldstat -n vmm >/dev/null 2>&1 || { run kldload "$VMM_KO"; LOADED=1; }
[ -x /sbin/mount_vmm ] || run ln -sf /sbin/mount_std /sbin/mount_vmm
run mkdir -p "$MNT"; run mount -t vmm vmm "$MNT"; MOUNTED=1
run mkdir "$(mach)"
printf '1\n' >"$(mach)/vcpu"; printf '%s\n' "$MEM" >"$(mach)/mem"
printf '%s\n' "$WRAPPER" >"$(mach)/loader"; cat "$(mach)/events" >>"$LOG"
run rm "$(mach)/stopped"
wait_for "$(mach)/events" '^started$' || fail "started event not observed"
wait_for "$(mach)/console" "$PAT" || fail "console pattern not observed: $PAT"
echo force >"$(mach)/stopped"
wait_for "$(mach)/events" '^stopped$' || fail "stopped event not observed"
say "PASS"
