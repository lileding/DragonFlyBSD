#!/bin/sh
# pc64 lifecycle/pager soak for loader capability and module teardown.
#
# Each command stage enters a looping one-vCPU guest, stops it, removes
# the machine, unmounts vmmfs, and unloads vmm.ko.  Each revoke stage uses a
# real fd3/fd4 mmap loader to cover both loader-exit and VM-stop revoke.
# Periodically, an inherited mapping must veto kldunload until its holder exits.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
WORK=${VMM_SOAK_WORK:-/var/tmp/dfvmm-lifecycle-capability-soak-$$}
ROUNDS=${VMM_SOAK_ROUNDS:-60}
HOLD_EVERY=${VMM_SOAK_HOLD_EVERY:-10}
TIMEOUT=${VMM_TIMEOUT:-20}

say()
{
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"
}

fail()
{
	say "FAIL: $*"
	exit 1
}

check_unloaded()
{
	kldstat -n vmm >/dev/null 2>&1 && fail "vmm.ko remains loaded after $1"
}

check_number()
{
	case "$2" in
	''|*[!0-9]*) fail "$1 must be a positive integer" ;;
	esac
	[ "$2" -gt 0 ] || fail "$1 must be a positive integer"
}

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO") || fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
		fail "$VMM_KO contains .eh_frame"
}

run_stage()
{
	name=$1
	shift
	log="$WORK/$name.log"

	say "BEGIN: $name"
	"$@" >"$log" 2>&1 || {
		cat "$log"
		fail "$name"
	}
	check_unloaded "$name"
	say "PASS: $name"
}

run_command_stage()
{
	round=$1

	run_stage "round-$round-command" env \
		VMM_KO="$VMM_KO" \
		VMM_MOUNT="$WORK/round-$round-command-vmm" \
		VMM_LOG="$WORK/round-$round-command-detail.log" \
		VMM_SMOKE_LOADER="$WORK/round-$round-command-loader" \
		VMM_MOUNT_HELPER="$WORK/round-$round-command-mount_vmmfs" \
		VMM_TIMEOUT="$TIMEOUT" \
		"$ROOT/command_atomicity_test.sh"
}

run_revoke_stage()
{
	round=$1

	run_stage "round-$round-revoke" env \
		VMM_KO="$VMM_KO" \
		VMM_MOUNT="$WORK/round-$round-revoke-vmm" \
		VMM_LOG="$WORK/round-$round-revoke-detail.log" \
		VMM_REVOKE_LOADER="$WORK/round-$round-revoke-loader" \
		VMM_MOUNT_HELPER="$WORK/round-$round-revoke-mount_vmmfs" \
		VMM_REVOKE_DELAY=1 \
		VMM_TIMEOUT="$TIMEOUT" \
		"$REPO/test/vmm/revoke/loader_revoke_test.sh"
}

run_holder_stage()
{
	round=$1

	run_stage "round-$round-holder" env \
		VMM_KO="$VMM_KO" \
		VMM_MOUNT="$WORK/round-$round-holder-vmm" \
		VMM_LOG="$WORK/round-$round-holder-detail.log" \
		VMM_REVOKE_LOADER="$WORK/round-$round-holder-loader" \
		VMM_MOUNT_HELPER="$WORK/round-$round-holder-mount_vmmfs" \
		VMM_REVOKE_DELAY=1 \
		VMM_TIMEOUT="$TIMEOUT" \
		"$REPO/test/vmm/revoke/loader_unload_busy_test.sh"
}

[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
[ "$(uname -m)" = x86_64 ] || fail "run on a pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be an absolute path" ;; esac
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
check_number VMM_SOAK_ROUNDS "$ROUNDS"
check_number VMM_SOAK_HOLD_EVERY "$HOLD_EVERY"
check_number VMM_TIMEOUT "$TIMEOUT"
check_unloaded entry
check_module_image
mkdir -p "$WORK" || fail "mkdir $WORK"

round=1
while [ "$round" -le "$ROUNDS" ]; do
	run_command_stage "$round"
	run_revoke_stage "$round"
	if [ $((round % HOLD_EVERY)) -eq 0 ]; then
		run_holder_stage "$round"
	fi
	round=$((round + 1))
done

say "PASS: $ROUNDS lifecycle capability rounds; holder interval $HOLD_EVERY"
