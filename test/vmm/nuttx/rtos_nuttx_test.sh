#!/bin/sh
#
# pc64 true-hardware NuttX bring-up harness.
#
# This script intentionally keeps the whole VM lifecycle visible.  It is the
# standard entry point for the single-vCPU RTOS bring-up path, so cleanup and
# diagnostics matter more than line count.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-nuttx-vmm}
VM=${VMM_MACHINE:-nuttx0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-nuttx-test.log}
ELF=${NUTTX_ELF:-/var/tmp/nuttx.elf}
LOADER=${NUTTX_LOADER:-/var/tmp/vmmld_nuttx_elf}
WRAPPER=${NUTTX_LOADER_WRAPPER:-/var/tmp/vmmld_nuttx}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-nuttx-$$-mount_vmm}
MEM=${NUTTX_MEM:-64M}
PAT=${NUTTX_BOOT_PATTERN:-NuttShell}
TIMEOUT=${VMM_TIMEOUT:-20}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}
KEEP_ARTIFACTS=${VMM_KEEP_ARTIFACTS:-0}
FORCE_UMOUNT_ON_CLEANUP=${VMM_FORCE_UMOUNT_ON_CLEANUP:-1}

LOADED=0
MOUNTED=0
MACHINE_CREATED=0

say()
{
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"
}

fail()
{
	say "FAIL: $*"
	dump_runtime_state
	exit 1
}

run()
{
	say "+ $*"
	"$@" >>"$LOG" 2>&1 || fail "$*"
}

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

mach()
{
	printf '%s/%s\n' "$MNT" "$VM"
}

append_file()
{
	label=$1
	file=$2

	if [ -e "$file" ]; then
		{
			printf '--- %s: %s ---\n' "$label" "$file"
			cat "$file" 2>&1
			printf '--- end %s ---\n' "$label"
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
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		append_file events "$(mach)/events"
		append_file console "$(mach)/console"
	fi
}

wait_for_file_pattern()
{
	file=$1
	pattern=$2
	timeout=$3
	label=$4
	i=0

	while [ "$i" -lt "$timeout" ]; do
		out=$(cat "$file" 2>>"$LOG")
		if [ -n "$out" ]; then
			{
				printf '--- poll %s: %s ---\n' "$label" "$file"
				printf '%s\n' "$out"
				printf '--- end poll %s ---\n' "$label"
			} >>"$LOG"
		fi
		printf '%s\n' "$out" | grep -q "$pattern" && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

stop_machine()
{
	dir=$(mach)
	i=0

	[ -d "$dir" ] || return 0
	say "requesting stop for $VM"
	touch "$dir/stopped" 2>>"$LOG" || true
	wait_for_file_pattern "$dir/events" 'state stopped' "$STOP_TIMEOUT" stopped \
	    >/dev/null 2>&1 || say "stopped event not observed during cleanup"

	while [ "$i" -lt "$STOP_TIMEOUT" ] && [ -d "$dir" ]; do
		rmdir "$dir" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	[ ! -d "$dir" ]
}

unmount_vmmfs()
{
	i=0

	[ "$MOUNTED" -eq 1 ] || return 0
	while [ "$i" -lt "$STOP_TIMEOUT" ]; do
		umount "$MNT" >>"$LOG" 2>&1 && {
			MOUNTED=0
			return 0
		}
		sleep 1
		i=$((i + 1))
	done
	if [ "$FORCE_UMOUNT_ON_CLEANUP" -eq 1 ]; then
		say "normal umount timed out; trying umount -f"
		umount -f "$MNT" >>"$LOG" 2>&1 && {
			MOUNTED=0
			return 0
		}
	fi
	return 1
}

cleanup()
{
	set +e
	if [ "$MOUNTED" -eq 1 ]; then
		if [ "$MACHINE_CREATED" -eq 1 ]; then
			stop_machine || say "machine cleanup did not finish"
		fi
		unmount_vmmfs || say "vmmfs unmount did not finish"
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1 || say "kldunload vmm failed"
	fi
	rm -f "$WRAPPER" "$MOUNT_HELPER"
	if [ "$KEEP_ARTIFACTS" -eq 0 ]; then
		rm -f "$LOADER"
	fi
}

preflight()
{
	: >"$LOG" || exit 1
	say "NuttX RTOS true-hardware bring-up"
	say "repo=$REPO"
	say "vmm_ko=$VMM_KO"
	say "elf=$ELF"
	say "mem=$MEM mount=$MNT machine=$VM pattern=$PAT"

	[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
	case "$VMM_KO" in
	/*) ;;
	*) fail "VMM_KO must be an absolute path" ;;
	esac
	[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
	[ -f "$ELF" ] || fail "missing NUTTX_ELF=$ELF"
	case "$MOUNT_HELPER" in
	*_vmm) ;;
	*) fail "VMM_MOUNT_HELPER path must end in _vmm for mount_std" ;;
	esac
	check_module_image
}

prepare_loader()
{
	run cc -Wall -Wextra -Werror -std=c11 -O2 \
	    "$ROOT/rtos_nuttx_loader.c" -o "$LOADER"
	printf '#!/bin/sh\nexec %s %s\n' "$LOADER" "$ELF" >"$WRAPPER" ||
	    fail "write $WRAPPER"
	chmod +x "$WRAPPER" || fail "chmod $WRAPPER"
}

prepare_mount_helper()
{
	rm -f "$MOUNT_HELPER" || fail "remove stale $MOUNT_HELPER"
	ln -s /sbin/mount_std "$MOUNT_HELPER" || fail "link $MOUNT_HELPER"
}

load_module()
{
	if kldstat -n vmm >/dev/null 2>&1; then
		fail "vmm already loaded; unload it before running this harness"
	else
		run kldload "$VMM_KO"
		LOADED=1
	fi
}

mount_vmmfs()
{
	run mkdir -p "$MNT"
	run "$MOUNT_HELPER" vmm "$MNT"
	MOUNTED=1
}

configure_machine()
{
	run mkdir "$(mach)"
	MACHINE_CREATED=1
	printf '1\n' >"$(mach)/vcpu" || fail "write vcpu"
	printf '%s\n' "$MEM" >"$(mach)/mem" || fail "write mem"
	printf '%s\n' "$WRAPPER" >"$(mach)/loader" || fail "write loader"
	append_file initial-events "$(mach)/events"
}

run_guest()
{
	run rm "$(mach)/stopped"
	wait_for_file_pattern "$(mach)/events" 'state running' "$TIMEOUT" started ||
	    fail "started event not observed"
	wait_for_file_pattern "$(mach)/console" "$PAT" "$TIMEOUT" console ||
	    fail "console pattern not observed: $PAT"
	say "console pattern observed: $PAT"
	touch "$(mach)/stopped" || fail "request stop"
	wait_for_file_pattern "$(mach)/events" 'state stopped' "$STOP_TIMEOUT" stopped ||
	    fail "stopped event not observed"
	say "PASS"
}

trap cleanup EXIT INT TERM
preflight
prepare_loader
prepare_mount_helper
load_module
mount_vmmfs
configure_machine
run_guest
