#!/bin/sh
# pc64 true-hardware Linux reset harness.
#
# One long-lived console reader spans every reset.  Each reset must stop the
# old vCPU, boot a fresh in-memory Linux, retain the terminal endpoint, and
# return to running without recreating the stopped control file.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-reset-vmm}
VM=${VMM_MACHINE:-linuxreset0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-reset-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-linux-reset.console}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_reset}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-reset-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-40}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-30}
RESET_ROUNDS=${VMM_RESET_ROUNDS:-10}

LOADED=0
MOUNTED=0
CREATED=0
CONSOLE_READER_PID=

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

machine_dir()
{
	printf '%s/%s\n' "$MNT" "$VM"
}

console_path()
{
	printf '%s/console\n' "$(machine_dir)"
}

events_path()
{
	printf '%s/events\n' "$(machine_dir)"
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

	if [ -e "$file" ]; then
		{
			printf -- '--- %s: %s ---\n' "$label" "$file"
			cat "$file" 2>&1
			printf -- '--- end %s ---\n' "$label"
		} >>"$LOG"
	fi
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
	if [ "$MOUNTED" -eq 1 ] && [ "$CREATED" -eq 1 ] &&
	    [ -d "$(machine_dir)" ]; then
		append_file events "$(events_path)"
		append_file console "$CONSOLE_LOG"
	fi
}

start_console_reader()
{
	: >"$CONSOLE_LOG" || fail "truncate $CONSOLE_LOG"
	cat "$(console_path)" >>"$CONSOLE_LOG" 2>>"$LOG" &
	CONSOLE_READER_PID=$!
}

stop_console_reader()
{
	[ -n "$CONSOLE_READER_PID" ] || return 0
	kill "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
	wait "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
	CONSOLE_READER_PID=
}

wait_console_pattern()
{
	pattern=$1
	label=$2
	wait_i=0

	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$CONSOLE_LOG" && return 0
		sleep 1
		wait_i=$((wait_i + 1))
	done
	append_file "$label" "$CONSOLE_LOG"
	return 1
}

event_seq()
{
	cat "$(events_path)" 2>>"$LOG" | awk 'END { print $1 }'
}

wait_reset_sequence()
{
	after=$1
	wait_i=0

	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		if cat "$(events_path)" 2>>"$LOG" |
		    awk -v after="$after" '
			$1 > after {
				if ($0 ~ /reset requested/)
					requested = 1
				if (requested && $0 ~ /state draining reason=stop/)
					draining = 1
				if (draining && $0 ~ /state stopped reason=stop/)
					stopped = 1
				if (stopped && $0 ~ /state starting/)
					starting = 1
				if (starting && $0 ~ /state running/)
					running = 1
			}
			END { exit running ? 0 : 1 }
		'; then
			return 0
		fi
		sleep 1
		wait_i=$((wait_i + 1))
	done
	append_file reset-events "$(events_path)"
	return 1
}

write_console()
{
	line=$1

	printf '%s\n' "$line" >"$(console_path)" ||
	    fail "console write failed"
}

wait_guest_shell()
{
	round=$1
	base="DFVMM_RESET_ROUND_${round}"
	marker="${base}_READY"
	probe_i=0

	while [ "$probe_i" -lt "$TIMEOUT" ]; do
		write_console "a=$base; echo \${a}_READY"
		sleep 1
		grep -q "$marker" "$CONSOLE_LOG" && return 0
		probe_i=$((probe_i + 1))
	done
	fail "shell readiness probe missing in round $round"
}

force_stop()
{
	stop_i=0

	touch "$(machine_dir)/stopped" ||
	    fail "stop request failed"
	while [ "$stop_i" -lt "$STOP_TIMEOUT" ]; do
		if [ -e "$(machine_dir)/stopped" ] &&
		    cat "$(events_path)" 2>>"$LOG" |
		    grep -q 'state stopped reason=stop'; then
			return 0
		fi
		sleep 1
		stop_i=$((stop_i + 1))
	done
	fail "stop did not complete"
}

remove_machine()
{
	remove_i=0

	[ "$CREATED" -eq 1 ] || return 0
	while [ "$remove_i" -lt "$STOP_TIMEOUT" ]; do
		rmdir "$(machine_dir)" >>"$LOG" 2>&1 && {
			CREATED=0
			return 0
		}
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
	if [ "$MOUNTED" -eq 1 ] && [ "$CREATED" -eq 1 ] &&
	    [ -d "$(machine_dir)" ]; then
		touch "$(machine_dir)/stopped"
	fi
	stop_console_reader
	if [ "$MOUNTED" -eq 1 ] && [ "$CREATED" -eq 1 ]; then
		remove_machine || say "machine cleanup did not finish"
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		unmount_vmmfs || say "vmmfs unmount did not finish"
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1 || say "kldunload vmm failed"
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER"
}

preflight()
{
	: >"$LOG" || exit 1
	say "Linux reset true-hardware test"
	say "repo=$REPO vmm_ko=$VMM_KO kernel=$KERNEL initrd=$INITRD"
	say "machine=$VM mem=$MEM reset_rounds=$RESET_ROUNDS"
	[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
	case "$VMM_KO" in
	/*) ;;
	*) fail "VMM_KO must be an absolute path" ;;
	esac
	case "$MOUNT_HELPER" in
	*_vmm) ;;
	*) fail "VMM_MOUNT_HELPER path must end in _vmm for mount_std" ;;
	esac
	[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
	[ -f "$KERNEL" ] || fail "missing $KERNEL"
	[ -f "$INITRD" ] ||
	    fail "missing $INITRD; run linux_initrd_rootfs_build.sh first"
	[ "$RESET_ROUNDS" -gt 0 ] || fail "VMM_RESET_ROUNDS must be positive"
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
	run rm "$(machine_dir)/stopped"
	wait_console_pattern 'DFVMM_LINUX_SERIAL_OK' console ||
	    fail "initial Linux serial marker missing"
	wait_guest_shell 0
}

reset_machine()
{
	round=$1
	before=$(event_seq)

	[ -n "$before" ] || fail "cannot read event sequence before reset $round"
	say "reset round=$round after_event=$before"
	printf '%s\n' 'reset' >"$(events_path)" ||
	    fail "reset request failed in round $round"
	wait_reset_sequence "$before" ||
	    fail "reset event sequence incomplete in round $round"
	[ ! -e "$(machine_dir)/stopped" ] ||
	    fail "stopped control file reappeared after reset $round"
	kill -0 "$CONSOLE_READER_PID" >/dev/null 2>&1 ||
	    fail "long-lived console reader exited in reset $round"
	wait_guest_shell "$round"
}

trap cleanup EXIT INT TERM
preflight
build_loader_wrapper
run kldload "$VMM_KO"
LOADED=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1

run mkdir "$(machine_dir)"
CREATED=1
printf '%s\n' 1 >"$(machine_dir)/vcpu" || fail "vcpu config"
printf '%s\n' "$MEM" >"$(machine_dir)/mem" || fail "mem config"
printf '%s\n' "$WRAPPER" >"$(machine_dir)/loader" || fail "loader config"
start_console_reader
start_machine

round=1
while [ "$round" -le "$RESET_ROUNDS" ]; do
	reset_machine "$round"
	round=$((round + 1))
done

force_stop
stop_console_reader
remove_machine || fail "rmdir $(machine_dir)"
unmount_vmmfs || fail "umount $MNT"
run kldunload vmm
LOADED=0
rm -f "$MOUNT_HELPER" "$WRAPPER"

say "PASS: Linux reset test"
