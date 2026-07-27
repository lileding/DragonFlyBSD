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
CONSOLE_CAPTURE=${VMM_SMOKE_CONSOLE_CAPTURE:-/var/tmp/vmm_smoke_console_capture}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-smoke-$$-mount_vmm}
MEM=${VMM_SMOKE_MEM:-2M}
TIMEOUT=${VMM_TIMEOUT:-20}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}
KEEP_ARTIFACTS=${VMM_KEEP_ARTIFACTS:-0}
FORCE_UMOUNT_ON_CLEANUP=${VMM_FORCE_UMOUNT_ON_CLEANUP:-1}
SVM_TRACE=${VMM_SVM_TRACE:-0}
MODES=${VMM_SMOKE_MODES:-"vmmcall cpuid msrpatch msrsyscfg mtrrcap msrhwcr pcicfg pitfallback elcr hpet hpet_oneshot hpet_periodic hpet_masked rtc_periodic rtc_masked rtc_update_alarm rtc_settime pmtimer acpi_s5 serial serialin serialirq time xsetbv apicmsr timerint lapictimer lapictimer_periodic_hlt lapictimer_periodic_busy lapictimer_periodic_masked hireslapic tscdeadline tscscale hiresscale pausefilter lapictimer_masked ud mwaitud mwaitxud pic ioapic ioapicirq x2apic cachetlb pm64 avicread hlt loop"}
SELF_EXIT_MODES=${VMM_SMOKE_SELF_EXIT_MODES:-"vmmcall cpuid msrpatch msrsyscfg mtrrcap msrhwcr pcicfg pitfallback elcr hpet hpet_oneshot hpet_periodic hpet_masked rtc_periodic rtc_masked rtc_update_alarm rtc_settime pmtimer serial serialin serialirq time xsetbv apicmsr timerint lapictimer lapictimer_periodic_hlt lapictimer_periodic_busy lapictimer_periodic_masked hireslapic tscdeadline tscscale hiresscale pausefilter lapictimer_masked ud mwaitud mwaitxud pic ioapic ioapicirq x2apic cachetlb pm64 avicread"}

LOADED=0
MOUNTED=0
CREATED_MACHINES=
CONSOLE_CLIENT_PID=
CONSOLE_LOG=
CONSOLE_READY=
CONSOLE_INPUT=

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
			fi
		done
	fi
	if [ -n "$CONSOLE_LOG" ]; then
		append_file "console-capture" "$CONSOLE_LOG"
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
			    printf '%s\n' "$seen" | awk -v first="$first" \
			    -v second="$second" '
				$0 ~ first { found = 1; next }
				found && $0 ~ second { ordered = 1 }
				END { exit !ordered }
			'; then
				return 0
			fi
		fi
		sleep 1
		i=$((i + 1))
	done
	printf '%s\n' "$seen" >>"$LOG"
	return 1
}

wait_guest_exit()
{
	file=$1
	i=0
	seen=

	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$file" 2>>"$LOG")
		if [ -n "$out" ]; then
			seen="$seen
$out"
			{
				printf '%s\n' "--- poll events: $file ---"
				printf '%s\n' "$out"
				printf '%s\n' '--- end poll events ---'
			} >>"$LOG"
		fi
		if printf '%s\n' "$seen" | awk '
			/vmmcall exit/ { vmmcall = 1; next }
			vmmcall && /vcpu0 thread exit active=0/ { exited = 1 }
			END { exit !exited }
		'; then
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	printf '%s\n' "$seen" >>"$LOG"
	return 1
}

start_console_client()
{
	mode=$1
	i=0

	CONSOLE_LOG=/var/tmp/dfvmm-smoke-$mode-$$.console
	CONSOLE_READY=$CONSOLE_LOG.ready
	CONSOLE_INPUT=$CONSOLE_LOG.input
	rm -f "$CONSOLE_LOG" "$CONSOLE_READY" "$CONSOLE_INPUT" || return 1
	mkfifo -m 600 "$CONSOLE_INPUT" || return 1
	"$CONSOLE_CAPTURE" "$(mach "$mode")/console" "$CONSOLE_LOG" \
	    "$CONSOLE_READY" "$CONSOLE_INPUT" >>"$LOG" 2>&1 &
	CONSOLE_CLIENT_PID=$!
	while [ "$i" -lt "$TIMEOUT" ]; do
		[ -s "$CONSOLE_READY" ] && return 0
		if ! kill -0 "$CONSOLE_CLIENT_PID" 2>/dev/null; then
			wait "$CONSOLE_CLIENT_PID" 2>/dev/null || true
			CONSOLE_CLIENT_PID=
			return 1
		fi
		sleep 1
		i=$((i + 1))
	done
	return 1
}

stop_console_client()
{
	if [ -n "$CONSOLE_CLIENT_PID" ]; then
		kill -TERM "$CONSOLE_CLIENT_PID" >/dev/null 2>&1 || true
		wait "$CONSOLE_CLIENT_PID" >/dev/null 2>&1 || true
		CONSOLE_CLIENT_PID=
	fi
}

send_console_input()
{
	printf '%s' "$1" >"$CONSOLE_INPUT"
}

wait_console()
{
	pattern=$1
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$CONSOLE_LOG" && return 0
		sleep 1
		i=$((i + 1))
	done
	{
		printf '--- final console: %s ---\n' "$CONSOLE_LOG"
		cat "$CONSOLE_LOG" 2>&1
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
	stop_console_client
	if [ "$MOUNTED" -eq 1 ]; then
		cleanup_machines
		unmount_vmmfs || say "vmmfs unmount did not finish"
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		sysctl debug.vmm.svm_trace=0 >>"$LOG" 2>&1 || true
		kldunload vmm >>"$LOG" 2>&1 || say "kldunload vmm failed"
	fi
	for mode in $MODES; do
		rm -f "/var/tmp/vmmld_smoke_$mode"
	done
	rm -f "$MOUNT_HELPER"
	if [ "$KEEP_ARTIFACTS" -eq 0 ]; then
		rm -f "$LOADER" "$CONSOLE_CAPTURE" "$CONSOLE_LOG" "$CONSOLE_READY" \
		    "$CONSOLE_INPUT"
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
	run cc -Wall -Wextra -Werror -std=c11 -O2 \
	    "$ROOT/console_capture.c" -o "$CONSOLE_CAPTURE"
}

prepare_mount_helper()
{
	rm -f "$MOUNT_HELPER" || fail "remove stale $MOUNT_HELPER"
	ln -s /sbin/mount_std "$MOUNT_HELPER" || fail "link $MOUNT_HELPER"
}

load_module()
{
	case "$SVM_TRACE" in
	0|1) ;;
	*) fail "VMM_SVM_TRACE must be 0 or 1" ;;
	esac
	if kldstat -n vmm >/dev/null 2>&1; then
		fail "vmm already loaded; unload it before running this harness"
	else
		run kldload "$VMM_KO"
		LOADED=1
		if [ "$SVM_TRACE" -eq 1 ]; then
			run sysctl debug.vmm.svm_trace=1
		fi
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
		wait_console 'dfvmm-serial-ok' ||
		    fail "$mode console output"
		;;
	serialin)
		wait_console 'dfvmm-serialin-ok' ||
		    fail "$mode console input output"
		;;
	serialirq)
		wait_console 'dfvmm-serialirq-ok' ||
		    fail "$mode console irq output"
		;;
	ud)
		wait_console 'dfvmm-ud-ok' ||
		    fail "$mode console output"
		wait_console 'dfvmm-iret-ok' ||
		    fail "$mode iret output"
		;;
	mwaitud)
		wait_console 'dfvmm-mwait-ud-ok' ||
		    fail "$mode console output"
		;;
	mwaitxud)
		wait_console 'dfvmm-mwaitx-ud-ok' ||
		    fail "$mode console output"
		;;
	pic)
		wait_console 'dfvmm-pic-ok' ||
		    fail "$mode console output"
		;;
	ioapic)
		wait_console 'dfvmm-ioapic-ok' ||
		    fail "$mode console output"
		;;
	ioapicirq)
		wait_console 'dfvmm-ioapicirq-ok' ||
		    fail "$mode console output"
		;;
	x2apic)
		wait_console 'dfvmm-x2apic-ok' ||
		    fail "$mode console output"
		;;
	cachetlb)
		wait_console 'dfvmm-cachetlb-ok' ||
		    fail "$mode console output"
		;;
	pm64)
		wait_console 'dfvmm-pm64-ok' ||
		    fail "$mode console output"
		;;
	lapictimer)
		wait_console 'dfvmm-lapic-timer-ok' ||
		    fail "$mode console output"
		;;
	lapictimer_periodic_hlt)
		wait_console 'dfvmm-lapic-periodic-hlt-ok' ||
		    fail "$mode console output"
		;;
	lapictimer_periodic_busy)
		wait_console 'dfvmm-lapic-periodic-busy-ok' ||
		    fail "$mode console output"
		;;
	lapictimer_periodic_masked)
		wait_console 'dfvmm-lapic-periodic-masked-ok' ||
		    fail "$mode console output"
		;;
	hpet_oneshot)
		wait_console 'dfvmm-hpet-oneshot-ok' ||
		    fail "$mode console output"
		;;
	hpet_periodic)
		wait_console 'dfvmm-hpet-periodic-ok' ||
		    fail "$mode console output"
		;;
	hpet_masked)
		wait_console 'dfvmm-hpet-masked-ok' ||
		    fail "$mode console output"
		;;
	rtc_periodic)
		wait_console 'dfvmm-rtc-periodic-ok' ||
		    fail "$mode console output"
		;;
	rtc_masked)
		wait_console 'dfvmm-rtc-masked-ok' ||
		    fail "$mode console output"
		;;
	rtc_update_alarm)
		wait_console 'dfvmm-rtc-update-alarm-ok' ||
		    fail "$mode console output"
		;;
	rtc_settime)
		wait_console 'dfvmm-rtc-settime-ok' ||
		    fail "$mode console output"
		;;
	lapictimer_masked)
		wait_console 'dfvmm-lapic-masked-ok' ||
		    fail "$mode console output"
		;;
	hireslapic)
		wait_console 'dfvmm-hires-lapic-ok' ||
		    fail "$mode console output"
		;;
	hiresscale)
		wait_console 'dfvmm-hires-scale-ok' ||
		    fail "$mode console output"
		;;
	avicread)
		wait_console 'dfvmm-avicread-ok' ||
		    fail "$mode console output"
		;;
	esac
}

run_case()
{
	mode=$1
	w=$(wrapper "$mode")

	configure_machine "$mode" "$w"
	start_console_client "$mode" || fail "$mode console client"
	run rm "$(mach "$mode")/stopped"
	if [ "$mode" = "acpi_s5" ]; then
		wait_event "$(mach "$mode")/events" \
		    'guest shutdown source=acpi_s5' \
		    'state stopped reason=guest_shutdown' ||
		    fail "$mode guest shutdown"
		[ ! -e "$(mach "$mode")/stopped" ] ||
		    fail "$mode desired changed"
	elif mode_self_exits "$mode"; then
		case "$mode" in
		pausefilter)
			console_input=
			;;
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
			send_console_input "$console_input" ||
			    fail "$mode console input"
		fi
		wait_guest_exit "$(mach "$mode")/events" ||
		    fail "$mode guest self exit"
		if [ "$mode" = "hireslapic" ] || [ "$mode" = "hiresscale" ]; then
			case "$mode" in
			hireslapic)
				host_tsc_hz=$(sysctl -n kern.cputimer.freq 2>>"$LOG") ||
				    fail "$mode read host TSC frequency"
				case "$host_tsc_hz" in
				''|*[!0-9]*)
					fail "$mode invalid host TSC frequency=$host_tsc_hz"
					;;
				esac
				target=$((host_tsc_hz / 5000))
				minimum=$((target / 4))
				maximum=$((target * 4 + 100000))
				;;
			hiresscale)
				target=100000
				minimum=25000
				maximum=500000
				;;
			esac
			marker=$(sed -n \
			    's/.*smoke avic marker=0x\([0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
			    "$LOG" | tail -n 1)
			case "$marker" in
			''|*[!0-9a-fA-F]*) fail "$mode missing timer marker" ;;
			esac
			value=$((0x$marker))
			[ "$value" -ge "$minimum" ] && [ "$value" -le "$maximum" ] ||
			    fail "$mode delta=$value expected=$minimum..$maximum target=$target"
			say "$mode TSC delta=$value target=$target"
		fi
		case "$mode" in
		lapictimer_periodic_hlt|lapictimer_periodic_busy|hpet_periodic)
			periodic_marker=3
			;;
		lapictimer_periodic_masked|hpet_masked|rtc_masked)
			periodic_marker=51
			;;
		hpet_oneshot|rtc_periodic)
			periodic_marker=1
			;;
	rtc_update_alarm)
			periodic_marker=176
			;;
		rtc_settime)
			periodic_marker=4
			;;
		*)
			periodic_marker=
			;;
		esac
		if [ -n "$periodic_marker" ]; then
			marker=$(sed -n \
			    's/.*smoke avic marker=0x\([0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
			    "$LOG" | tail -n 1)
			case "$marker" in
			''|*[!0-9a-fA-F]*) fail "$mode missing periodic marker" ;;
			esac
			value=$((0x$marker))
			[ "$value" -eq "$periodic_marker" ] ||
			    fail "$mode marker=$value expected=$periodic_marker"
			say "$mode marker=$value"
		fi
		if [ "$mode" = "pausefilter" ]; then
			pause_exits=$(sed -n \
			    's/.*smoke pause filter exits=\([0-9][0-9]*\).*/\1/p' \
			    "$LOG" | tail -n 1)
			case "$pause_exits" in
			''|*[!0-9]*) fail "$mode missing exit count" ;;
			esac
			[ "$pause_exits" -gt 0 ] && [ "$pause_exits" -lt 1024 ] ||
			    fail "$mode exit count=$pause_exits"
		fi
		[ ! -e "$(mach "$mode")/stopped" ] ||
		    fail "$mode desired changed"
		echo force >"$(mach "$mode")/stopped" ||
		    fail "$mode request stopped"
	else
		if [ "$mode" = "tscscale" ]; then
			wait_event "$(mach "$mode")/events" \
			    'svm tsc scale .*guest_hz=1000000000' 'state running' ||
			    fail "$mode tsc scaling start"
		else
			wait_event "$(mach "$mode")/events" 'state running' ||
			    fail "$mode started"
		fi
		if [ "$mode" = "avicirq" ]; then
			wait_event "$(mach "$mode")/events" \
			    'smoke avic marker=0xa51c0040' ||
			    fail "$mode guest interrupt handler"
			[ ! -e "$(mach "$mode")/stopped" ] ||
			    fail "$mode desired changed"
		fi
		if [ "$mode" = "tscdeadline" ] || [ "$mode" = "tscscale" ]; then
			wait_event "$(mach "$mode")/events" 'vmmcall exit' ||
			    fail "$mode deadline handler"
		fi
		echo force >"$(mach "$mode")/stopped" ||
		    fail "$mode request stopped"
		wait_event "$(mach "$mode")/events" 'state stopped' ||
		    fail "$mode stopped"
		[ -e "$(mach "$mode")/stopped" ] ||
		    fail "$mode stopped file"
	fi
	check_console "$mode"
	append_file "$mode-console" "$CONSOLE_LOG"
	stop_console_client
	cleanup_machine "$mode" || fail "$mode cleanup"
	rm -f "$CONSOLE_LOG" "$CONSOLE_READY" "$CONSOLE_INPUT"
	CONSOLE_LOG=
	CONSOLE_READY=
	CONSOLE_INPUT=
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
