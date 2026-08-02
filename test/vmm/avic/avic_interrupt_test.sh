#!/bin/sh
# pc64 AMD AVIC guest-interrupt delivery harness.  The guest asks root to
# inject vector 0x40 through AVIC, then the guest interrupt handler emits a
# marker through VMMCALL.  Seeing the marker proves hardware delivery reached
# guest code; the root request log alone is not enough.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-avic-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-avic-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_avic_smoke_loader}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-avic-$$-mount_vmm}
WRAPPER=${VMM_LOADER_WRAPPER:-/var/tmp/vmmld_avicirq}
MEM=${VMM_AVIC_MEM:-2M}
TIMEOUT=${VMM_TIMEOUT:-20}
LOADED=0
MOUNTED=0
EVENTS_SEEN=

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
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach avicirq)" ]; then
		append_file avicirq-events "$(mach avicirq)/events"
		append_file avicirq-console "$(mach avicirq)/console"
	fi
}

wait_event()
{
	file=$1
	pattern=$2
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$file" 2>>"$LOG")
		if [ -n "$out" ]; then
			EVENTS_SEEN=$out
			{
				printf -- '--- poll events: %s ---\n' "$file"
				printf '%s\n' "$out"
				printf -- '--- end poll events ---\n'
			} >>"$LOG"
		fi
		printf '%s\n' "$EVENTS_SEEN" | grep -q "$pattern" && return 0
		sleep 1
		i=$((i + 1))
	done
	printf '%s\n' "$EVENTS_SEEN" >>"$LOG"
	return 1
}

cleanup_machine()
{
	dir=$(mach avicirq)
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
	i=0

	set +e
	cleanup_machine
	[ "$MOUNTED" -eq 1 ] && unmount_vmm
	while [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] &&
	    [ "$i" -lt "$TIMEOUT" ]; do
		kldunload vmm >>"$LOG" 2>&1 && LOADED=0 && break
		sleep 1
		i=$((i + 1))
	done
	rm -f "$MOUNT_HELPER" "$WRAPPER" "$LOADER"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
say "AMD AVIC guest interrupt delivery test"
say "repo=$REPO vmm_ko=$VMM_KO mount=$MNT mem=$MEM"
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be an absolute path" ;; esac
case "$MOUNT_HELPER" in *_vmm) ;; *) fail "VMM_MOUNT_HELPER path must end in _vmm" ;; esac
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
check_module_image
run cc -Wall -Wextra -Werror -std=c11 -O2     "$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"
printf '#!/bin/sh\nexec %s avicirq\n' "$LOADER" >"$WRAPPER" || fail "write $WRAPPER"
chmod +x "$WRAPPER" || fail "chmod $WRAPPER"

kldstat -n vmm >/dev/null 2>&1 &&
    fail "vmm already loaded; unload it before running this harness"
run kldload "$VMM_KO"; LOADED=1
run rm -f "$MOUNT_HELPER"; run ln -s /sbin/mount_std "$MOUNT_HELPER"
run mkdir -p "$MNT"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1

run mkdir "$(mach avicirq)"
printf '1\n' >"$(mach avicirq)/vcpu" || fail "write vcpu"
printf '%s\n' "$MEM" >"$(mach avicirq)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(mach avicirq)/loader" || fail "write loader"
cat "$(mach avicirq)/events" >>"$LOG"
run rm "$(mach avicirq)/stopped"
wait_event "$(mach avicirq)/events" 'state running' || fail "machine did not run"
wait_event "$(mach avicirq)/events" 'svm avic enabled' || fail "AVIC not enabled"
wait_event "$(mach avicirq)/events" 'svm avic bound' || fail "AVIC not bound to host CPU"
wait_event "$(mach avicirq)/events" 'smoke avic request vector=0x40' ||
    fail "guest did not request AVIC injection"
wait_event "$(mach avicirq)/events" 'smoke avic marker=0xa51c0040' ||
    fail "guest AVIC handler marker not observed"
touch "$(mach avicirq)/stopped" || fail "stop avicirq"
wait_event "$(mach avicirq)/events" 'state stopped reason=stop' ||
    fail "machine did not stop"
run rmdir "$(mach avicirq)"
unmount_vmm || fail "umount $MNT"
run kldunload vmm; LOADED=0
say "PASS: AMD AVIC guest interrupt delivery"
