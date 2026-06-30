#!/bin/sh
set -u

REPO=${REPO:-/home/lileding/src/dfvmm}
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-initrd-vmm}
VM=${VMM_MACHINE:-linuxinitrd0}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_initrd_rootfs}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-initrd-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-initrd-rootfs-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-linux-initrd-rootfs-console.log}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-30}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}

LOADED=0
MOUNTED=0
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

mach()
{
	printf '%s/machines/%s\n' "$MNT" "$VM"
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
			cat "$(mach)/events" 2>&1
			printf '%s\n' '--- console ---'
			cat "$CONSOLE_LOG" 2>&1
		fi
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
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$CONSOLE_LOG" && return 0
		sleep 1
		i=$((i + 1))
	done
	{
		printf '%s\n' "--- final console $label ---"
		cat "$CONSOLE_LOG" 2>&1
		printf '%s\n' "--- end final console $label ---"
	} >>"$LOG"
	return 1
}

cleanup()
{
	set +e
	stop_console_reader
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		say "cleanup: force stop $VM"
		echo force >"$(mach)/stopped" 2>>"$LOG"
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
		i=0
		while [ "$i" -lt "$STOP_TIMEOUT" ]; do
			kldunload vmm >>"$LOG" 2>&1 && break
			sleep 1
			i=$((i + 1))
		done
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
[ -f "$KERNEL" ] || fail "missing $KERNEL"
[ -f "$INITRD" ] || fail "missing $INITRD"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
exec "$LOADER" "$KERNEL" "initramfs=$INITRD" "console=ttyS0,115200" "earlycon=uart,io,0x3f8,115200" "loglevel=7" "rdinit=/init"
EOF_WRAP
chmod +x "$WRAPPER" || fail "chmod $WRAPPER"

run kldload "$VMM_KO"
LOADED=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1

run mkdir "$(mach)"
printf '1\n' >"$(mach)/vcpu" || fail "write vcpu"
printf '%s\n' "$MEM" >"$(mach)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(mach)/loader" || fail "write loader"
start_console_reader
run rm "$(mach)/stopped"

wait_console_pattern 'DFVMM_LINUX_INITRD_ROOTFS_OK' initrd ||
	fail "initrd rootfs marker not observed"
wait_console_pattern 'DFVMM_LINUX_SERIAL_OK' serial ||
	fail "serial marker not observed"

printf 'dfvmm-core-smoke\n' >"$(mach)/console" ||
	fail "write core smoke command"
wait_console_pattern 'DFVMM_CORE_SMOKE_END' core-smoke ||
	fail "core smoke marker not observed"

say "PASS: Linux in-memory initrd rootfs test"
