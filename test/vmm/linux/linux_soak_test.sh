#!/bin/sh
# pc64 true-hardware two-Linux lifecycle soak.
#
# Two independent one-vCPU Linux guests remain interactive while alternating
# reset cycles rebuild one guest at a time.  The harness keeps SVM trace
# disabled so events remain useful for lifecycle assertions.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-soak-vmm}
VM_PREFIX=${VMM_MACHINE_PREFIX:-linuxsoak}
VM_COUNT=${VMM_LINUX_INSTANCES:-2}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-soak-test.log}
CONSOLE_LOG_DIR=${VMM_CONSOLE_LOG_DIR:-/var/tmp/dfvmm-linux-soak-console}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_soak}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-soak-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-40}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-30}
SOAK_ROUNDS=${VMM_SOAK_ROUNDS:-60}
SOAK_INTERVAL=${VMM_SOAK_INTERVAL:-5}
TSC_SETTLE=${VMM_TSC_SETTLE:-4}
TSC_CALIBRATION_MAX_PPM=${VMM_TSC_CALIBRATION_MAX_PPM:-5000}
SVM_TRACE=${VMM_SVM_TRACE:-0}
SVM_TIMING_TRACE=${VMM_SVM_TIMING_TRACE:-0}
TSC_PM_PROBE=${VMM_TSC_PM_PROBE:-0}

LOADED=0
MOUNTED=0
CREATED_MACHINES=
CONSOLE_READER_PIDS=
OLD_SVM_TRACE=
OLD_SVM_TIMING_TRACE=

say()
{
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"
}

fail()
{
	say "FAIL: $*"
	dump_state
	exit 1
}

run()
{
	say "+ $*"
	"$@" >>"$LOG" 2>&1 || fail "$*"
}

machine_name()
{
	printf '%s%s\n' "$VM_PREFIX" "$1"
}

machine_dir()
{
	printf '%s/machines/%s\n' "$MNT" "$1"
}

console_path()
{
	printf '%s/console\n' "$(machine_dir "$1")"
}

console_log()
{
	printf '%s/%s.console\n' "$CONSOLE_LOG_DIR" "$1"
}

events_path()
{
	printf '%s/events\n' "$(machine_dir "$1")"
}

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

	[ -e "$file" ] || return 0
	{
		printf -- '--- %s: %s ---\n' "$label" "$file"
		cat "$file" 2>&1
		printf -- '--- end %s ---\n' "$label"
	} >>"$LOG"
}

dump_state()
{
	{
		printf '%s\n' '--- kldstat ---'
		kldstat -n vmm 2>&1 || true
		printf '%s\n' '--- mount ---'
		mount 2>&1 || true
		printf '%s\n' '--- machines ---'
		[ -d "$MNT/machines" ] && ls -la "$MNT/machines" 2>&1 || true
		printf '%s\n' '--- end state ---'
	} >>"$LOG"
	for vm in $CREATED_MACHINES; do
		[ -d "$(machine_dir "$vm")" ] || continue
		append_file "$vm events" "$(events_path "$vm")"
		append_file "$vm console" "$(console_log "$vm")"
	done
}

start_console_reader()
{
	vm=$1
	log=$(console_log "$vm")

	: >"$log" || fail "truncate $log"
	cat "$(console_path "$vm")" >>"$log" 2>>"$LOG" &
	CONSOLE_READER_PIDS="$CONSOLE_READER_PIDS $!"
}

stop_console_readers()
{
	for pid in $CONSOLE_READER_PIDS; do
		kill "$pid" >/dev/null 2>&1 || true
	done
	for pid in $CONSOLE_READER_PIDS; do
		wait "$pid" >/dev/null 2>&1 || true
	done
	CONSOLE_READER_PIDS=
}

check_console_readers()
{
	for pid in $CONSOLE_READER_PIDS; do
		kill -0 "$pid" >/dev/null 2>&1 ||
		    fail "long-lived console reader exited"
	done
}

wait_console_pattern()
{
	vm=$1
	pattern=$2
	label=$3
	wait_i=0
	log=$(console_log "$vm")

	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$log" && return 0
		sleep 1
		wait_i=$((wait_i + 1))
	done
	append_file "$label" "$log"
	return 1
}

console_pattern_count()
{
	vm=$1
	pattern=$2

	grep -c "$pattern" "$(console_log "$vm")" 2>/dev/null || true
}

console_size()
{
	wc -c <"$(console_log "$1")" 2>/dev/null || printf '%s\n' 0
}

record_vcpu_cpu()
{
	vm=$1
	after=$2
	wait_i=0
	cpu=
	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		candidate=$(cat "$(events_path "$vm")" 2>>"$LOG" | awk -v after="$after" '
			$1 > after && /vcpu0 thread enter cpu=/ {
				match($0, /cpu=[0-9]+/)
				cpu = substr($0, RSTART + 4, RLENGTH - 4)
			}
			END { print cpu }
		')
		[ -n "$candidate" ] && cpu=$candidate
		if [ -n "$cpu" ] && cat "$(events_path "$vm")" 2>>"$LOG" |
		    awk -v after="$after" -v cpu="$cpu" '
			$1 > after && $0 ~ ("svm cpu" cpu " tsc ratio") {
				seen = 1
				if ($0 ~ /match=1/)
					matched = 1
			}
			END { exit seen && matched ? 0 : 1 }
		'; then
			say "$vm vcpu0 cpu=$cpu after_event=$after"
			say "$vm tsc ratio readback matches on cpu=$cpu"
			return 0
		fi
		sleep 1
		wait_i=$((wait_i + 1))
	done
	if [ -z "$cpu" ]; then
		append_file "$vm vcpu events" "$(events_path "$vm")"
		fail "$vm did not report a vcpu0 CPU"
	fi
	append_file "$vm tsc ratio events" "$(events_path "$vm")"
	fail "$vm tsc ratio readback missing or mismatched on cpu=$cpu"
}

check_tsc_clocksource()
{
	vm=$1
	offset=$2
	log=$(console_log "$vm")

	sleep "$TSC_SETTLE"
	if tail -c "+$((offset + 1))" "$log" 2>>"$LOG" |
	    awk '
		/TSC found unstable|Marking clocksource.*as unstable/ {
			failed = 1
		}
		/clocksource: Switched to clocksource tsc$/ {
			switched = 1
		}
		switched && /clocksource: Switched to clocksource acpi_pm/ {
			failed = 1
		}
		END { exit failed ? 0 : 1 }
	'; then
		append_file "$vm tsc failure" "$log"
		fail "$vm switched away from TSC after boot"
	fi
	if ! tail -c "+$((offset + 1))" "$log" 2>>"$LOG" |
	    grep -q "clocksource: Switched to clocksource tsc"; then
		append_file "$vm tsc failure" "$log"
		fail "$vm did not switch to TSC after boot"
	fi
}

check_tsc_calibration()
{
	vm=$1
	offset=$2
	log=$(console_log "$vm")
	# PIO VMEXIT jitter can make Linux reject quick PIT samples.  Require a
	# documented PIT, PM timer, or HPET calibration path instead.
	detected=$(tail -c "+$((offset + 1))" "$log" 2>>"$LOG" |
	    awk '/tsc: Detected/ {
		for (i = 1; i < NF; ++i) {
			if ($i == "Detected")
				detected = $(i + 1)
		}
	    } END { print detected }')
	refined=$(tail -c "+$((offset + 1))" "$log" 2>>"$LOG" |
	    awk '/tsc: Refined TSC clocksource calibration:/ {
		for (i = 1; i < NF; ++i) {
			if ($i == "calibration:")
				refined = $(i + 1)
		}
	    } END { print refined }')

	[ -n "$refined" ] || fail "$vm did not report refined TSC calibration"
	path=$(tail -c "+$((offset + 1))" "$log" 2>>"$LOG" |
	    awk '
		/tsc: Fast TSC calibration using PIT/ { path = "pit" }
		/tsc: using PMTIMER reference calibration/ { path = "pmtimer" }
		/tsc: using HPET reference calibration/ { path = "hpet" }
		END { print path }
	    ')
	case "$path" in
	pit|pmtimer|hpet)
		;;
	*)
		append_file "$vm TSC boot calibration failure" "$log"
		fail "$vm did not report a supported TSC boot calibration path"
		;;
	esac
	if ! awk -v refined="$refined" -v host_hz="$TSC_HOST_HZ" \
	    -v max_ppm="$TSC_CALIBRATION_MAX_PPM" 'BEGIN {
		host_mhz = host_hz / 1000000
		delta = refined - host_mhz
		if (delta < 0)
			delta = -delta
		exit delta * 1000000 <= host_mhz * max_ppm ? 0 : 1
	}'; then
		append_file "$vm tsc calibration failure" "$log"
		fail "$vm refined TSC calibration ${refined}MHz exceeds ${TSC_CALIBRATION_MAX_PPM}ppm"
	fi
	say "$vm TSC calibration path=$path detected=${detected:-missing}MHz refined=${refined}MHz host_hz=$TSC_HOST_HZ"
}

run_tsc_pm_probe()
{
	vm=$1
	before=$(console_pattern_count "$vm" 'DFVMM_TSC_PM_PROBE')
	log=$(console_log "$vm")
	probe=

	write_console "$vm" 'dfvmm-tsc-pm-probe'
	wait_console_next "$vm" 'DFVMM_TSC_PM_PROBE' "$before" \
	    "$vm TSC/PM probe" || fail "$vm TSC/PM probe marker missing"
	if tail -n 80 "$log" 2>>"$LOG" | grep -q 'DFVMM_TSC_PM_PROBE_ERROR'; then
		append_file "$vm TSC/PM probe" "$log"
		fail "$vm TSC/PM probe could not acquire I/O privilege"
	fi
	probe=$(tail -n 80 "$log" 2>>"$LOG" | awk '
		/DFVMM_TSC_PM_PROBE/ {
			for (i = 1; i <= NF; ++i) {
				if ($i ~ /^delta_tsc=/)
					delta_tsc = substr($i, 11)
				if ($i ~ /^delta_pm=/)
					delta_pm = substr($i, 10)
				if ($i ~ /^tsc_hz_from_pm=/)
					hz = substr($i, 16)
			}
		}
		END {
			if (delta_tsc != "" && delta_pm != "" && hz != "")
				print delta_tsc, delta_pm, hz
		}
	')
	set -- $probe
	[ "$#" -eq 3 ] || {
		append_file "$vm TSC/PM probe" "$log"
		fail "$vm TSC/PM probe output is incomplete"
	}
	if ! awk -v probe_hz="$3" -v host_hz="$TSC_HOST_HZ" \
	    -v max_ppm="$TSC_CALIBRATION_MAX_PPM" 'BEGIN {
		delta = probe_hz - host_hz
		if (delta < 0)
			delta = -delta
		exit delta * 1000000 <= host_hz * max_ppm ? 0 : 1
	}'; then
		append_file "$vm TSC/PM probe" "$log"
		fail "$vm raw TSC/PM frequency $3 exceeds ${TSC_CALIBRATION_MAX_PPM}ppm"
	fi
	say "$vm raw TSC/PM delta_tsc=$1 delta_pm=$2 hz=$3 host_hz=$TSC_HOST_HZ"
}

wait_console_next()
{
	vm=$1
	pattern=$2
	before=$3
	label=$4
	wait_i=0

	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		count=$(console_pattern_count "$vm" "$pattern")
		[ "$count" -gt "$before" ] && return 0
		sleep 1
		wait_i=$((wait_i + 1))
	done
	append_file "$label" "$(console_log "$vm")"
	return 1
}

write_console()
{
	vm=$1
	line=$2

	printf '%s\n' "$line" >"$(console_path "$vm")" ||
	    fail "console write failed for $vm"
}

probe_machine()
{
	vm=$1
	marker=$2

	write_console "$vm" \
	    "a=$marker; read u _ < /proc/uptime; echo \${a}_UP=\$u"
	wait_console_pattern "$vm" "$marker" "$vm console" ||
	    fail "$vm shell marker missing: $marker"
}

event_seq()
{
	cat "$(events_path "$1")" 2>>"$LOG" | awk 'END { print $1 }'
}

wait_reset_sequence()
{
	vm=$1
	after=$2
	wait_i=0

	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		if cat "$(events_path "$vm")" 2>>"$LOG" |
		    awk -v after="$after" '
			$1 > after {
				if ($0 ~ /reset begin/)
					begin = 1
				if (begin && $0 ~ /state stopped reason=stop/)
					stopped = 1
				if (stopped && $0 ~ /state running/)
					running = 1
				if (running && $0 ~ /reset done/)
					done = 1
			}
			END { exit done ? 0 : 1 }
		'; then
			return 0
		fi
		sleep 1
		wait_i=$((wait_i + 1))
	done
	append_file "$vm reset events" "$(events_path "$vm")"
	return 1
}

force_stop()
{
	vm=$1
	stop_i=0
	before=$(event_seq "$vm")

	[ -n "$before" ] || fail "cannot read event sequence for $vm stop"
	touch "$(machine_dir "$vm")/stopped" 2>>"$LOG" ||
	    fail "stop request failed for $vm"
	while [ "$stop_i" -lt "$STOP_TIMEOUT" ]; do
		if [ -e "$(machine_dir "$vm")/stopped" ] &&
		    cat "$(events_path "$vm")" 2>>"$LOG" |
		    awk -v after="$before" '
			$1 > after && /state stopped reason=stop/ { found = 1 }
			END { exit found ? 0 : 1 }
		'; then
			return 0
		fi
		sleep 1
		stop_i=$((stop_i + 1))
	done
	fail "stop did not complete for $vm"
}

reset_machine()
{
	vm=$1
	marker=$2
	before=$(event_seq "$vm")
	ready_before=$(console_pattern_count "$vm" 'DFVMM_LINUX_SERIAL_OK')
	console_before=$(console_size "$vm")

	[ -n "$before" ] || fail "cannot read event sequence for $vm"
	[ -n "$ready_before" ] || fail "cannot read serial marker count for $vm"
	say "reset machine=$vm after_event=$before"
	printf '%s\n' 'reset' >"$(events_path "$vm")" ||
	    fail "reset request failed for $vm"
	if [ "$SVM_TRACE" -eq 0 ]; then
		wait_reset_sequence "$vm" "$before" ||
		    fail "reset event sequence incomplete for $vm"
	fi
	[ ! -e "$(machine_dir "$vm")/stopped" ] ||
	    fail "stopped control file reappeared after reset: $vm"
	if [ "$SVM_TRACE" -eq 0 ]; then
		record_vcpu_cpu "$vm" "$before"
	fi
	wait_console_next "$vm" 'DFVMM_LINUX_SERIAL_OK' "$ready_before" \
	    "$vm reset console" || fail "$vm serial marker missing after reset"
	if [ "$TSC_PM_PROBE" -eq 1 ]; then
		run_tsc_pm_probe "$vm"
	fi
	check_tsc_clocksource "$vm" "$console_before"
	check_tsc_calibration "$vm" "$console_before"
	check_console_readers
	probe_machine "$vm" "$marker"
}

remove_machine()
{
	vm=$1
	remove_i=0

	[ -d "$(machine_dir "$vm")" ] || return 0
	while [ "$remove_i" -lt "$STOP_TIMEOUT" ]; do
		rmdir "$(machine_dir "$vm")" >>"$LOG" 2>&1 && return 0
		sleep 1
		remove_i=$((remove_i + 1))
	done
	return 1
}

unmount_vmmfs()
{
	unmount_i=0

	[ "$MOUNTED" -eq 1 ] || return 0
	while [ "$unmount_i" -lt "$STOP_TIMEOUT" ]; do
		umount "$MNT" >>"$LOG" 2>&1 && {
			MOUNTED=0
			return 0
		}
		sleep 1
		unmount_i=$((unmount_i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	if [ "$MOUNTED" -eq 1 ]; then
		for vm in $CREATED_MACHINES; do
			[ -d "$(machine_dir "$vm")" ] || continue
			if [ ! -e "$(machine_dir "$vm")/stopped" ]; then
				touch "$(machine_dir "$vm")/stopped" 2>>"$LOG"
			fi
		done
	fi
	stop_console_readers
	if [ "$MOUNTED" -eq 1 ]; then
		for vm in $CREATED_MACHINES; do
			remove_machine "$vm" || say "machine cleanup did not finish: $vm"
		done
		unmount_vmmfs || say "vmmfs unmount did not finish"
	fi
	if [ "$LOADED" -eq 1 ] && [ -n "$OLD_SVM_TRACE" ]; then
		sysctl debug.vmm.svm_trace="$OLD_SVM_TRACE" >>"$LOG" 2>&1
		OLD_SVM_TRACE=
	fi
	if [ "$LOADED" -eq 1 ] && [ -n "$OLD_SVM_TIMING_TRACE" ]; then
		sysctl debug.vmm.svm_timing_trace="$OLD_SVM_TIMING_TRACE" \
		    >>"$LOG" 2>&1
		OLD_SVM_TIMING_TRACE=
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1 || say "kldunload vmm failed"
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER"
}

preflight()
{
	: >"$LOG" || exit 1
	say "Linux lifecycle soak true-hardware test"
	say "repo=$REPO vmm_ko=$VMM_KO kernel=$KERNEL initrd=$INITRD"
	say "vm_count=$VM_COUNT rounds=$SOAK_ROUNDS interval=$SOAK_INTERVAL tsc_settle=$TSC_SETTLE svm_trace=$SVM_TRACE svm_timing_trace=$SVM_TIMING_TRACE tsc_pm_probe=$TSC_PM_PROBE mem=$MEM"
	[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
	case "$VMM_KO" in
	/*) ;;
	*) fail "VMM_KO must be an absolute path" ;;
	esac
	case "$MOUNT_HELPER" in
	*_vmm) ;;
	*) fail "VMM_MOUNT_HELPER path must end in _vmm for mount_std" ;;
	esac
	case "$VM_COUNT" in
	2) ;;
	*) fail "VMM_LINUX_INSTANCES must be 2" ;;
	esac
	case "$SOAK_ROUNDS" in
	''|*[!0-9]*) fail "VMM_SOAK_ROUNDS must be a positive integer" ;;
	esac
	case "$SOAK_INTERVAL" in
	''|*[!0-9]*) fail "VMM_SOAK_INTERVAL must be a positive integer" ;;
	esac
	case "$TSC_SETTLE" in
	''|*[!0-9]*) fail "VMM_TSC_SETTLE must be a positive integer" ;;
	esac
	case "$TSC_CALIBRATION_MAX_PPM" in
	''|*[!0-9]*) fail "VMM_TSC_CALIBRATION_MAX_PPM must be a positive integer" ;;
	esac
	case "$SVM_TRACE" in
	0|1) ;;
	*) fail "VMM_SVM_TRACE must be 0 or 1" ;;
	esac
	case "$SVM_TIMING_TRACE" in
	0|1) ;;
	*) fail "VMM_SVM_TIMING_TRACE must be 0 or 1" ;;
	esac
	case "$TSC_PM_PROBE" in
	0|1) ;;
	*) fail "VMM_TSC_PM_PROBE must be 0 or 1" ;;
	esac
	[ "$SOAK_ROUNDS" -gt 0 ] || fail "VMM_SOAK_ROUNDS must be positive"
	[ "$SOAK_INTERVAL" -gt 0 ] || fail "VMM_SOAK_INTERVAL must be positive"
	[ "$TSC_SETTLE" -gt 0 ] || fail "VMM_TSC_SETTLE must be positive"
	[ "$TSC_CALIBRATION_MAX_PPM" -gt 0 ] ||
	    fail "VMM_TSC_CALIBRATION_MAX_PPM must be positive"
	TSC_HOST_HZ=$(sysctl -n hw.tsc_frequency 2>>"$LOG") ||
	    fail "read hw.tsc_frequency"
	case "$TSC_HOST_HZ" in
	''|*[!0-9]*) fail "hw.tsc_frequency is invalid: $TSC_HOST_HZ" ;;
	esac
	[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
	[ -f "$KERNEL" ] || fail "missing $KERNEL"
	[ -f "$INITRD" ] ||
	    fail "missing $INITRD; run linux_initrd_rootfs_build.sh first"
	check_module_image
	kldstat -n vmm >/dev/null 2>&1 &&
	    fail "vmm already loaded; unload it before running this harness"
}

build_loader_wrapper()
{
	run cc -Wall -Wextra -Werror -std=c11 -O2 \
		"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
	{
		printf '%s\n' '#!/bin/sh'
		printf 'exec "%s" "%s" "initramfs=%s" ' \
		    "$LOADER" "$KERNEL" "$INITRD"
		printf '%s\n' '"tsc_hz=host" "console=ttyS0,115200" "earlycon=uart,io,0x3f8,115200" "loglevel=7" "rdinit=/init"'
	} >"$WRAPPER" || fail "write $WRAPPER"
	chmod +x "$WRAPPER" || fail "chmod $WRAPPER"
}

start_machine()
{
	vm=$1
	marker=$2

	run mkdir "$(machine_dir "$vm")"
	CREATED_MACHINES="$CREATED_MACHINES $vm"
	before=$(event_seq "$vm")
	[ -n "$before" ] || fail "cannot read event sequence for $vm start"
	printf '%s\n' 1 >"$(machine_dir "$vm")/vcpu" || fail "vcpu config $vm"
	printf '%s\n' "$MEM" >"$(machine_dir "$vm")/mem" || fail "mem config $vm"
	printf '%s\n' "$WRAPPER" >"$(machine_dir "$vm")/loader" ||
	    fail "loader config $vm"
	start_console_reader "$vm"
	console_before=$(console_size "$vm")
	run rm "$(machine_dir "$vm")/stopped"
	if [ "$SVM_TRACE" -eq 0 ]; then
		record_vcpu_cpu "$vm" "$before"
	fi
	wait_console_pattern "$vm" 'DFVMM_LINUX_SERIAL_OK' "$vm console" ||
		fail "$vm serial marker missing"
	if [ "$TSC_PM_PROBE" -eq 1 ]; then
		run_tsc_pm_probe "$vm"
	fi
	check_tsc_clocksource "$vm" "$console_before"
	check_tsc_calibration "$vm" "$console_before"
	probe_machine "$vm" "$marker"
}

trap cleanup EXIT INT TERM
preflight
build_loader_wrapper
run kldload "$VMM_KO"
LOADED=1
OLD_SVM_TRACE=$(sysctl -n debug.vmm.svm_trace 2>>"$LOG") ||
	fail "read debug.vmm.svm_trace"
OLD_SVM_TIMING_TRACE=$(sysctl -n debug.vmm.svm_timing_trace 2>>"$LOG") ||
	fail "read debug.vmm.svm_timing_trace"
run sysctl debug.vmm.svm_trace="$SVM_TRACE"
run sysctl debug.vmm.svm_timing_trace="$SVM_TIMING_TRACE"
run mkdir -p "$MNT" "$CONSOLE_LOG_DIR"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1

index=0
while [ "$index" -lt "$VM_COUNT" ]; do
	vm=$(machine_name "$index")
	start_machine "$vm" "DFVMM_SOAK_INIT_${index}"
	index=$((index + 1))
done

round=1
while [ "$round" -le "$SOAK_ROUNDS" ]; do
	index=0
	while [ "$index" -lt "$VM_COUNT" ]; do
		vm=$(machine_name "$index")
		probe_machine "$vm" "DFVMM_SOAK_${round}_${index}_PRE"
		index=$((index + 1))
	done
	index=$(( (round - 1) % VM_COUNT ))
	vm=$(machine_name "$index")
	reset_machine "$vm" "DFVMM_SOAK_${round}_${index}_POST"
	check_console_readers
	say "round=$round reset_machine=$vm complete"
	sleep "$SOAK_INTERVAL"
	round=$((round + 1))
done

for vm in $CREATED_MACHINES; do
	force_stop "$vm"
done
stop_console_readers
for vm in $CREATED_MACHINES; do
	remove_machine "$vm" || fail "rmdir $vm failed"
done
unmount_vmmfs || fail "umount $MNT"
run sysctl debug.vmm.svm_trace="$OLD_SVM_TRACE"
OLD_SVM_TRACE=
run sysctl debug.vmm.svm_timing_trace="$OLD_SVM_TIMING_TRACE"
OLD_SVM_TIMING_TRACE=
run kldunload vmm
LOADED=0
rm -f "$MOUNT_HELPER" "$WRAPPER"

say "PASS: Linux lifecycle soak"
