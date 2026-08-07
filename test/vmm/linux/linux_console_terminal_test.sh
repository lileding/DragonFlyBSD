#!/bin/sh
# pc64 true-hardware serial terminal harness.
#
# The console file is exercised like a terminal: one long-lived reader keeps the
# file open while independent writes feed the guest serial shell.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-console-vmm}
VM=${VMM_MACHINE:-linuxterm0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-console-terminal-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-linux-console-terminal.log}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_console_terminal}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-console-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-45}
READY_TIMEOUT=${VMM_READY_TIMEOUT:-$TIMEOUT}
PROMPT_TIMEOUT=${VMM_PROMPT_TIMEOUT:-3}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}
SVM_TRACE=${VMM_SVM_TRACE:-0}

LOADED=0
MOUNTED=0
CONSOLE_READER_PID=
CONSOLE_WRITER_OPEN=0
CONSOLE_CPR_REPLIED=0

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

mach()
{
	printf '%s/%s\n' "$MNT" "$VM"
}

console_path()
{
	printf '%s/console\n' "$(mach)"
}

dump_state()
{
	{
		printf '%s\n' '--- kldstat ---'
		kldstat -n vmm 2>&1 || true
		printf '%s\n' '--- mount ---'
		mount 2>&1 || true
		if [ -d "$(mach)" ]; then
			printf '%s\n' '--- events ---'
			cat "$(mach)/events" 2>&1 || true
		fi
		printf '%s\n' '--- console log ---'
		cat "$CONSOLE_LOG" 2>&1 || true
		printf '%s\n' '--- end state ---'
	} >>"$LOG"
}

start_console_reader()
{
	: >"$CONSOLE_LOG" || fail "create console log"
	cat "$(mach)/console" >>"$CONSOLE_LOG" 2>>"$LOG" &
	CONSOLE_READER_PID=$!
}

stop_console_reader()
{
	if [ -n "$CONSOLE_READER_PID" ]; then
		kill "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
		wait "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
		CONSOLE_READER_PID=
	fi
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
	{
		printf '%s\n' "--- final console $label ---"
		cat "$CONSOLE_LOG" 2>&1
		printf '%s\n' "--- end final console $label ---"
	} >>"$LOG"
	return 1
}

check_console_tty()
{
	console=$(console_path)

	[ -c "$console" ] || fail "$console is not a character device"
	run stty -f "$console" -a
}

configure_console_tty()
{
	console=$(console_path)

	run stty -f "$console" raw -echo cs8 -parenb -cstopb 115200
	run stty -f "$console" -a
}

write_console()
{
	answer_console_cpr
	say "console write: $1"
	printf '%s\n' "$1" >&3 ||
	    fail "console write failed"
}

answer_console_cpr()
{
	cpr_count=$(awk -v pat="$(printf '\033[6n')" '
		{
			line = $0
			while ((pos = index(line, pat)) != 0) {
				count++
				line = substr(line, pos + length(pat))
			}
		}
		END { print count + 0 }
	' "$CONSOLE_LOG" 2>>"$LOG")

	while [ "$CONSOLE_CPR_REPLIED" -lt "$cpr_count" ]; do
		say "console write: CPR response"
		printf '\033[1;1R' >&3 ||
		    fail "console CPR response failed"
		CONSOLE_CPR_REPLIED=$((CONSOLE_CPR_REPLIED + 1))
	done
}

cleanup()
{
	set +e
	if [ "$CONSOLE_WRITER_OPEN" -eq 1 ]; then
		exec 3>&-
		CONSOLE_WRITER_OPEN=0
	fi
	stop_console_reader
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		touch "$(mach)/stopped" 2>>"$LOG"
		i=0
		while [ "$i" -lt "$STOP_TIMEOUT" ] && [ -d "$(mach)" ]; do
			rmdir "$(mach)" >>"$LOG" 2>&1 && break
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		i=0
		while [ "$i" -lt "$STOP_TIMEOUT" ]; do
			umount "$MNT" >>"$LOG" 2>&1 && {
				MOUNTED=0
				break
			}
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1 || true
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER"
}

ensure_module_image()
{
	if [ -f "$VMM_KO" ]; then
		return
	fi
	if [ "$VMM_KO" != "$REPO/sys/dev/vmm/vmm.ko" ]; then
		fail "missing $VMM_KO"
	fi
	say "+ make -C $REPO/sys/dev/vmm MACHINE_PLATFORM=pc64"
	( cd "$REPO/sys/dev/vmm" && make MACHINE_PLATFORM=pc64 ) >>"$LOG" 2>&1 ||
	    fail "build $VMM_KO"
	[ -f "$VMM_KO" ] || fail "missing $VMM_KO after build"
}

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
ensure_module_image
[ -f "$KERNEL" ] || fail "missing $KERNEL"
[ -f "$INITRD" ] || fail "missing $INITRD"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
check_module_image

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
exec "$LOADER" "$KERNEL" "initramfs=$INITRD" "console=ttyS0,115200" "loglevel=3" "rdinit=/init"
EOF_WRAP
chmod +x "$WRAPPER" || fail "chmod $WRAPPER"

run kldload "$VMM_KO"
LOADED=1
if [ "$SVM_TRACE" -eq 1 ]; then
	run sysctl debug.vmm.svm_trace=1
fi
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1

run mkdir "$(mach)"
printf '1\n' >"$(mach)/vcpu" || fail "write vcpu"
printf '%s\n' "$MEM" >"$(mach)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(mach)/loader" || fail "write loader"
check_console_tty
start_console_reader
run rm "$(mach)/stopped"

wait_console_pattern 'DFVMM_LINUX_SERIAL_OK' boot ||
	fail "serial shell marker not observed"

exec 3>"$(mach)/console" || fail "open console writer"
CONSOLE_WRITER_OPEN=1
configure_console_tty
saved_timeout=$TIMEOUT
TIMEOUT=$PROMPT_TIMEOUT
wait_console_pattern "$(printf '\033\\[6n')" cpr || true
TIMEOUT=$saved_timeout
answer_console_cpr

write_console 'echo DFVMM_TERM_READY'
saved_timeout=$TIMEOUT
TIMEOUT=$READY_TIMEOUT
wait_console_pattern 'DFVMM_TERM_READY' ready ||
	fail "terminal ready marker missing"
TIMEOUT=$saved_timeout

i=0
while [ "$i" -lt 40 ]; do
	write_console "echo DFVMM_TERM_LINE_$i"
	wait_console_pattern "DFVMM_TERM_LINE_$i" "line-$i" ||
	    fail "terminal line $i missing"
	i=$((i + 1))
done

{
	printf 'i=0\n'
	printf 'while [ "$i" -lt 140 ]; do\n'
	printf '  echo DFVMM_TERM_BURST_$i\n'
	printf '  i=$((i + 1))\n'
	printf 'done\n'
	printf 'echo DFVMM_TERM_BURST_END\n'
} >&3 || fail "burst console write failed"
wait_console_pattern 'DFVMM_TERM_BURST_END' burst ||
	fail "terminal burst marker missing"

if [ "$SVM_TRACE" -eq 0 ]; then
	events=$(cat "$(mach)/events" 2>>"$LOG" || true)
	printf '%s\n' "$events" >>"$LOG"
	printf '%s\n' "$events" | grep -E 'com1 rx irq|ioapic raise source=com1' &&
		fail "serial hot path leaked into default events"
fi

touch "$(mach)/stopped" 2>>"$LOG" ||
	fail "stop request failed"
i=0
while [ "$i" -lt "$STOP_TIMEOUT" ]; do
	grep -q 'state stopped reason=stop' "$(mach)/events" 2>>"$LOG" &&
	    break
	sleep 1
	i=$((i + 1))
done
[ "$i" -lt "$STOP_TIMEOUT" ] || fail "stop event missing"
stop_console_reader
exec 3>&-
CONSOLE_WRITER_OPEN=0
run rmdir "$(mach)"
say "+ umount $MNT"
umount_i=0
while [ "$umount_i" -lt "$STOP_TIMEOUT" ]; do
	umount "$MNT" >>"$LOG" 2>&1 && {
		MOUNTED=0
		break
	}
	sleep 1
	umount_i=$((umount_i + 1))
done
[ "$MOUNTED" -eq 0 ] || fail "umount $MNT"
run kldunload vmm
LOADED=0
rm -f "$MOUNT_HELPER" "$WRAPPER"

say "PASS: Linux serial terminal test"
