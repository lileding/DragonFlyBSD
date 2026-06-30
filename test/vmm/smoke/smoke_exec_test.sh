#!/bin/sh
#
# pc64 true-hardware smoke harness for the SVM execution backend.
#
# Keep this script explicit.  It is the first gate before RTOS bring-up, so it
# must leave enough log evidence to distinguish guest behavior from lifecycle
# cleanup failures.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-smoke-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-smoke-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_smoke_loader}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-smoke-$$-mount_vmm}
MEM=${VMM_SMOKE_MEM:-2M}
TIMEOUT=${VMM_TIMEOUT:-20}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}
KEEP_ARTIFACTS=${VMM_KEEP_ARTIFACTS:-0}
FORCE_UMOUNT_ON_CLEANUP=${VMM_FORCE_UMOUNT_ON_CLEANUP:-1}

MODES=${VMM_SMOKE_MODES:-"vmmcall cpuid msrpatch msrsyscfg mtrrcap msrhwcr pcicfg pitfallback elcr hpet pmtimer serial serialin serialirq time xsetbv apicmsr timerint lapictimer ud pic ioapic ioapicirq x2apic cachetlb pm64 hlt loop"}
SELF_EXIT_MODES=${VMM_SMOKE_SELF_EXIT_MODES:-"vmmcall cpuid msrpatch msrsyscfg mtrrcap msrhwcr pcicfg pitfallback elcr hpet pmtimer serial serialin serialirq time xsetbv apicmsr timerint lapictimer ud pic ioapic ioapicirq x2apic cachetlb pm64"}

LOADED=0
MOUNTED=0
CREATED_MACHINES=

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
	printf '%s/machines/%s\n' "$MNT" "$1"
}

mode_self_exits()
{
	mode=$1

	for self_mode in $SELF_EXIT_MODES; do
		[ "$mode" = "$self_mode" ] && return 0
	done
	return 1
}

mark_machine_created()
{
	CREATED_MACHINES="$CREATED_MACHINES $1"
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
	if [ "$MOUNTED" -eq 1 ]; then
		for vm in $CREATED_MACHINES; do
			if [ -d "$(mach "$vm")" ]; then
				append_file "$vm-events" "$(mach "$vm")/events"
				append_file "$vm-console" "$(mach "$vm")/console"
			fi
		done
	fi
}

wait_event()
{
	file=$1
	first=$2
	second=${3:-}
	i=0
	seen=

	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$file" 2>>"$LOG")
		if [ -n "$out" ]; then
			seen="$seen
$out"
			{
				printf '--- poll events: %s ---\n' "$file"
				printf '%s\n' "$out"
				printf '--- end poll events ---\n'
			} >>"$LOG"
		fi
		if printf '%s\n' "$seen" | grep -q "$first"; then
			if [ -z "$second" ] ||
			    printf '%s\n' "$seen" | grep -q "$second"; then
				return 0
			fi
		fi
		sleep 1
		i=$((i + 1))
	done
	printf '%s\n' "$seen" >>"$LOG"
	return 1
}

wait_console()
{
	file=$1
	pattern=$2
	i=0
	out=

	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$file" 2>>"$LOG")
		printf '%s\n' "$out" | grep -q "$pattern" && return 0
		sleep 1
		i=$((i + 1))
	done
	{
		printf '--- final console: %s ---\n' "$file"
		printf '%s\n' "$out"
		printf '--- end final console ---\n'
	} >>"$LOG"
	return 1
}

cleanup_machine()
{
	vm=$1
	dir=$(mach "$vm")
	i=0

	[ -d "$dir" ] || return 0
	say "requesting force stop for $vm"
	echo force >"$dir/stopped" 2>>"$LOG" || true
	wait_event "$dir/events" 'state stopped' >/dev/null 2>&1 ||
	    say "$vm stopped event not observed during cleanup"
	while [ "$i" -lt "$STOP_TIMEOUT" ] && [ -d "$dir" ]; do
		rmdir "$dir" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	[ ! -d "$dir" ]
}

cleanup_machines()
{
	for vm in $CREATED_MACHINES; do
		cleanup_machine "$vm" || say "$vm cleanup did not finish"
	done
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
		cleanup_machines
		unmount_vmmfs || say "vmmfs unmount did not finish"
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1 || say "kldunload vmm failed"
	fi
	for mode in $MODES; do
		rm -f "/var/tmp/vmmld_smoke_$mode"
	done
	rm -f "$MOUNT_HELPER"
	if [ "$KEEP_ARTIFACTS" -eq 0 ]; then
		rm -f "$LOADER"
	fi
}

preflight()
{
	: >"$LOG" || exit 1
	say "SVM smoke true-hardware bring-up"
	say "repo=$REPO"
	say "vmm_ko=$VMM_KO"
	say "mem=$MEM mount=$MNT"
	[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
	case "$VMM_KO" in
	/*) ;;
	*) fail "VMM_KO must be an absolute path" ;;
	esac
	[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
	case "$MOUNT_HELPER" in
	*_vmm) ;;
	*) fail "VMM_MOUNT_HELPER path must end in _vmm for mount_std" ;;
	esac
	check_module_image
}

prepare_loader()
{
	run cc -Wall -Wextra -Werror -std=c11 -O2 \
	    "$ROOT/smoke_loader.c" -o "$LOADER"
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

wrapper()
{
	mode=$1
	w=/var/tmp/vmmld_smoke_$mode

	printf '#!/bin/sh\nexec %s %s\n' "$LOADER" "$mode" >"$w" ||
	    fail "write $w"
	chmod +x "$w" || fail "chmod $w"
	printf '%s\n' "$w"
}

configure_machine()
{
	mode=$1
	w=$2

	run mkdir "$(mach "$mode")"
	mark_machine_created "$mode"
	printf '1\n' >"$(mach "$mode")/vcpu" || fail "$mode write vcpu"
	printf '%s\n' "$MEM" >"$(mach "$mode")/mem" || fail "$mode write mem"
	printf '%s\n' "$w" >"$(mach "$mode")/loader" ||
	    fail "$mode write loader"
	append_file "$mode-initial-events" "$(mach "$mode")/events"
}

check_console()
{
	mode=$1

	case "$mode" in
	serial)
		wait_console "$(mach "$mode")/console" 'dfvmm-serial-ok' ||
		    fail "$mode console output"
		;;
	serialin)
		wait_console "$(mach "$mode")/console" 'dfvmm-serialin-ok' ||
		    fail "$mode console input output"
		;;
	serialirq)
		wait_console "$(mach "$mode")/console" 'dfvmm-serialirq-ok' ||
		    fail "$mode console irq output"
		;;
	ud)
		wait_console "$(mach "$mode")/console" 'dfvmm-ud-ok' ||
		    fail "$mode console output"
		wait_console "$(mach "$mode")/console" 'dfvmm-iret-ok' ||
		    fail "$mode iret output"
		;;
	pic)
		wait_console "$(mach "$mode")/console" 'dfvmm-pic-ok' ||
		    fail "$mode console output"
		;;
	ioapic)
		wait_console "$(mach "$mode")/console" 'dfvmm-ioapic-ok' ||
		    fail "$mode console output"
		;;
	ioapicirq)
		wait_console "$(mach "$mode")/console" 'dfvmm-ioapicirq-ok' ||
		    fail "$mode console output"
		;;
	x2apic)
		wait_console "$(mach "$mode")/console" 'dfvmm-x2apic-ok' ||
		    fail "$mode console output"
		;;
	cachetlb)
		wait_console "$(mach "$mode")/console" 'dfvmm-cachetlb-ok' ||
		    fail "$mode console output"
		;;
	pm64)
		wait_console "$(mach "$mode")/console" 'dfvmm-pm64-ok' ||
		    fail "$mode console output"
		;;
	lapictimer)
		wait_console "$(mach "$mode")/console" 'dfvmm-lapic-timer-ok' ||
		    fail "$mode console output"
		;;
	esac
}

run_case()
{
	mode=$1
	w=$(wrapper "$mode")

	configure_machine "$mode" "$w"
	run rm "$(mach "$mode")/stopped"
	if mode_self_exits "$mode"; then
		case "$mode" in
		serialin)
			console_input=Z
			;;
		serialirq)
			console_input=Q
			;;
		*)
			console_input=
			;;
		esac
		if [ -n "$console_input" ]; then
			wait_event "$(mach "$mode")/events" 'state running' ||
			    fail "$mode started"
			printf '%s' "$console_input" >"$(mach "$mode")/console" ||
			    fail "$mode console input"
			wait_event "$(mach "$mode")/events" 'state stopped' ||
			    fail "$mode self exit"
		else
			wait_event "$(mach "$mode")/events" 'state running' 'state stopped' ||
			    fail "$mode self exit"
		fi
		[ ! -e "$(mach "$mode")/stopped" ] ||
		    fail "$mode desired changed"
		echo force >"$(mach "$mode")/stopped" ||
		    fail "$mode request stopped"
	else
		wait_event "$(mach "$mode")/events" 'state running' ||
		    fail "$mode started"
		echo force >"$(mach "$mode")/stopped" ||
		    fail "$mode request stopped"
		wait_event "$(mach "$mode")/events" 'state stopped' ||
		    fail "$mode stopped"
		[ -e "$(mach "$mode")/stopped" ] ||
		    fail "$mode stopped file"
	fi
	check_console "$mode"
	cleanup_machine "$mode" || fail "$mode cleanup"
	say "PASS: $mode"
}

trap cleanup EXIT INT TERM
preflight
prepare_loader
prepare_mount_helper
load_module
mount_vmmfs
for mode in $MODES; do
	run_case "$mode"
done
say "PASS"
