#!/bin/sh
# pc64 SVM staged Linux SMP bring-up and concurrent pmap pressure regression.
set -u

REPO=${REPO:-/home/lileding/projects/dfly-vmm/DragonFlyBSD}
VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-smp-bringup-vmm}
VM=${VMM_MACHINE:-linuxsmpbringup0}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_smp_bringup}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-smp-bringup-$$-mount_vmmfs}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-smp-bringup-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-linux-smp-bringup-console.log}
STRESS_C=${VMM_STRESS_C:-/var/tmp/dfvmm-linux-smp-bringup-stress-$$.c}
STRESS_BIN=${VMM_STRESS_BIN:-/var/tmp/dfvmm-linux-smp-bringup-stress-$$}
MEM=${LINUX_MEM:-256M}
VCPU_COUNT=${VMM_VCPU_COUNT:-2}
TIMEOUT=${VMM_TIMEOUT:-30}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}
CORE_TRACE=${VMM_CORE_TRACE:-0}

LOADED=0
MOUNTED=0
CONSOLE_READER_PID=
HOST_STRESS_PID=
OLD_CORE_TRACE=
DMESG_LINES=

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
		if [ -n "$DMESG_LINES" ]; then
			printf '%s\n' '--- new dmesg ---'
			dmesg | tail -n "+$((DMESG_LINES + 1))" 2>&1
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

wait_smp_online()
{
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		printf '%s\n' 'echo DFVMM_SMP_BRINGUP $(cat /sys/devices/system/cpu/online)' \
		    >"$(mach)/console" || fail "write SMP probe"
		sleep 1
		grep -q "DFVMM_SMP_BRINGUP 0-$((VCPU_COUNT - 1))" \
		    "$CONSOLE_LOG" && return 0
		i=$((i + 1))
	done
	return 1
}

force_stop()
{
	i=0
	run touch "$(mach)/stopped"
	while [ "$i" -lt "$STOP_TIMEOUT" ]; do
		if [ -e "$(mach)/stopped" ] && cat "$(mach)/events" 2>>"$LOG" |
		    grep -q 'state stopped reason=stop'; then
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	return 1
}

build_stress()
{
	cat >"$STRESS_C" <<'EOF'
#include <sys/mman.h>
#include <err.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int
main(void)
{
	const size_t len = 32 * 1024 * 1024;
	const size_t page = (size_t)getpagesize();
	volatile uint8_t sink = 0;
	int i;

	for (i = 0; i < 24; i++) {
		uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		    MAP_ANON | MAP_PRIVATE, -1, 0);
		size_t off;

		if (p == MAP_FAILED)
			err(1, "mmap");
		for (off = 0; off < len; off += page)
			p[off] = (uint8_t)(i + off);
		if (mprotect(p, len, PROT_READ) != 0)
			err(1, "mprotect read");
		for (off = 0; off < len; off += page)
			sink ^= p[off];
		if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0)
			err(1, "mprotect write");
		memset(p, sink, len);
		if (munmap(p, len) != 0)
			err(1, "munmap");
	}
	return sink == 0xff;
}
EOF
	run cc -Wall -Wextra -Werror -O2 "$STRESS_C" -o "$STRESS_BIN"
}

cleanup()
{
	set +e
	if [ -n "$HOST_STRESS_PID" ]; then
		kill "$HOST_STRESS_PID" >/dev/null 2>&1 || true
		wait "$HOST_STRESS_PID" >/dev/null 2>&1 || true
		HOST_STRESS_PID=
	fi
	stop_console_reader
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		touch "$(mach)/stopped" >>"$LOG" 2>&1
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
	if [ "$LOADED" -eq 1 ] && [ -n "$OLD_CORE_TRACE" ]; then
		sysctl debug.vmm.trace="$OLD_CORE_TRACE" >>"$LOG" 2>&1
		OLD_CORE_TRACE=
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER" "$STRESS_C" "$STRESS_BIN"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
[ -f "$KERNEL" ] || fail "missing $KERNEL"
[ -f "$INITRD" ] || fail "missing $INITRD"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
case "$CORE_TRACE" in
0|1) ;;
*) fail "VMM_CORE_TRACE must be 0 or 1" ;;
esac
case "$VCPU_COUNT" in
2|4|8) ;;
*) fail "VMM_VCPU_COUNT must be 2, 4, or 8" ;;
esac

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
build_stress
cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
exec "$LOADER" "$KERNEL" "initramfs=$INITRD" "vcpu=$VCPU_COUNT" "console=ttyS0,115200" "earlycon=uart,io,0x3f8,115200" "loglevel=7" "rdinit=/init"
EOF_WRAP
run chmod +x "$WRAPPER"
run kldload "$VMM_KO"; LOADED=1
if [ "$CORE_TRACE" -eq 1 ]; then
	OLD_CORE_TRACE=$(sysctl -n debug.vmm.trace 2>>"$LOG") ||
		fail "read debug.vmm.trace"
	run sysctl debug.vmm.trace=1
fi
DMESG_LINES=$(dmesg | wc -l | tr -d ' ')
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1
run mkdir "$(mach)"
printf '%s\n' "$VCPU_COUNT" >"$(mach)/vcpu" || fail "write vcpu"
printf '%s\n' "$MEM" >"$(mach)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(mach)/loader" || fail "write loader"
start_console_reader
run rm "$(mach)/stopped"

wait_console_pattern 'DFVMM_LINUX_INITRD_ROOTFS_OK' initrd ||
	fail "initrd rootfs marker not observed"
wait_console_pattern 'DFVMM_LINUX_SERIAL_OK' serial ||
	fail "serial marker not observed"
wait_smp_online || fail "Linux did not bring CPU1 online"

"$STRESS_BIN" >>"$LOG" 2>&1 &
HOST_STRESS_PID=$!
printf '%s\n' '(i=0; while [ $i -lt 12 ]; do dd if=/dev/zero of=/tmp/dfvmm-smp-a bs=1M count=24 2>/dev/null; rm -f /tmp/dfvmm-smp-a; i=$((i+1)); done) & (i=0; while [ $i -lt 12 ]; do dd if=/dev/zero of=/tmp/dfvmm-smp-b bs=1M count=24 2>/dev/null; rm -f /tmp/dfvmm-smp-b; i=$((i+1)); done) & wait; echo DFVMM_SMP_MEM_STRESS_OK' \
	>"$(mach)/console" || fail "write guest memory stress"
wait_console_pattern 'DFVMM_SMP_MEM_STRESS_OK' guest-memory ||
	fail "guest memory stress did not complete"
wait "$HOST_STRESS_PID" || fail "host pmap stress failed"
HOST_STRESS_PID=
dmesg | tail -n "+$((DMESG_LINES + 1))" >>"$LOG"
if dmesg | tail -n "+$((DMESG_LINES + 1))" |
	grep -Eq 'smp_inval|taking too long|panic|double fault'; then
	fail "host reported a shootdown or trap failure"
fi

force_stop || fail "external force stop did not complete"
cat "$(mach)/events" >>"$LOG" 2>&1
say "PASS: Linux ${VCPU_COUNT}-vCPU bring-up and concurrent memory/pmap pressure"
