#!/bin/sh
# pc64 SVM root interrupt VMEXIT harness.  It intentionally avoids serial
# console checks; serial output/input is a later escape-hatch feature.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-root-intr-vmm}
LOG=${VMM_LOG:-/var/tmp/dfvmm-root-intr-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_root_intr_smoke_loader}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-root-intr-$$-mount_vmm}
STRESS_C=${VMM_STRESS_C:-/var/tmp/dfvmm-root-intr-stress-$$.c}
STRESS_BIN=${VMM_STRESS_BIN:-/var/tmp/dfvmm-root-intr-stress-$$}
MEM=${VMM_ROOT_INTR_MEM:-2M}
TIMEOUT=${VMM_TIMEOUT:-20}
LOADED=0
MOUNTED=0

say() { echo "$@" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }
mach() { echo "$MNT/$1"; }

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

wait_event()
{
	file=$1
	pattern=$2
	i=0
	seen=
	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$file" 2>>"$LOG")
		[ -n "$out" ] && seen="$seen
$out"
		printf '%s\n' "$seen" | grep -q "$pattern" && return 0
		sleep 1
		i=$((i + 1))
	done
	printf '%s\n' "$seen" >>"$LOG"
	return 1
}

wrapper()
{
	vm=$1
	mode=$2
	w=/var/tmp/vmmld_root_intr_$vm
	printf '#!/bin/sh\nexec %s %s\n' "$LOADER" "$mode" >"$w" ||
	    fail "write $w"
	chmod +x "$w" || fail "chmod $w"
	echo "$w"
}

cleanup_machine()
{
	vm=$1
	[ "$MOUNTED" -eq 1 ] || return 0
	[ -d "$(mach "$vm")" ] || return 0
	if [ -e "$(mach "$vm")/stopped" ]; then
		:
	else
		touch "$(mach "$vm")/stopped" 2>>"$LOG"
	fi
	i=0
	while [ "$i" -lt "$TIMEOUT" ] && [ -d "$(mach "$vm")" ]; do
		rmdir "$(mach "$vm")" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

unmount_vmm()
{
	i=0
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
	set +e
	cleanup_machine cliloop_cpu0
	cleanup_machine loop
	cleanup_machine hlt
	cleanup_machine loopstress
	[ "$MOUNTED" -eq 1 ] && unmount_vmm
	[ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ] &&
	    kldunload vmm >>"$LOG" 2>&1
	rm -f /var/tmp/vmmld_root_intr_cliloop_cpu0 \
	    /var/tmp/vmmld_root_intr_loop /var/tmp/vmmld_root_intr_hlt \
	    /var/tmp/vmmld_root_intr_loopstress "$MOUNT_HELPER" \
	    "$STRESS_C" "$STRESS_BIN"
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

start_machine()
{
	vm=$1
	mode=$2
	w=$(wrapper "$vm" "$mode")

	run mkdir "$(mach "$vm")"
	printf '1\n' >"$(mach "$vm")/vcpu" || fail "$vm write vcpu"
	printf '%s\n' "$MEM" >"$(mach "$vm")/mem" || fail "$vm write mem"
	printf '%s\n' "$w" >"$(mach "$vm")/loader" ||
	    fail "$vm write loader"
	cat "$(mach "$vm")/events" >>"$LOG"
	run rm "$(mach "$vm")/stopped"
	wait_event "$(mach "$vm")/events" 'state running' ||
	    fail "$vm did not reach running"
}

stop_machine()
{
	vm=$1

	touch "$(mach "$vm")/stopped" || fail "$vm stop"
	wait_event "$(mach "$vm")/events" 'state stopped reason=stop' ||
	    fail "$vm did not stop"
	cat "$(mach "$vm")/events" >>"$LOG"
	[ -e "$(mach "$vm")/stopped" ] || fail "$vm stopped file missing"
	run rmdir "$(mach "$vm")"
}

run_stop_case()
{
	vm=$1
	mode=$2

	start_machine "$vm" "$mode"
	stop_machine "$vm"
	say "PASS: $vm"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be an absolute path" ;; esac
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
case "$MOUNT_HELPER" in *_vmm) ;; *)
	fail "VMM_MOUNT_HELPER path must end in _vmm for mount_std"
esac

check_module_image
run cc -Wall -Wextra -Werror -std=c11 -O2 \
    "$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"
build_stress
kldstat -n vmm >/dev/null 2>&1 &&
    fail "vmm already loaded; unload it before running this harness"
run kldload "$VMM_KO"; LOADED=1
run rm -f "$MOUNT_HELPER"; run ln -s /sbin/mount_std "$MOUNT_HELPER"
run mkdir -p "$MNT"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1

start_machine cliloop_cpu0 cliloop
wait_event "$(mach cliloop_cpu0)/events" 'vcpu0 create backend=svm cpu=0' ||
    fail "cliloop_cpu0 was not placed on cpu0"
stop_machine cliloop_cpu0
say "PASS: cliloop_cpu0"

run_stop_case loop loop
run_stop_case hlt hlt

start_machine loopstress loop
"$STRESS_BIN" >>"$LOG" 2>&1 || fail "host pmap stress failed"
stop_machine loopstress
say "PASS: loopstress"

unmount_vmm || fail "umount $MNT"
run kldunload vmm; LOADED=0
say "PASS: root interrupt VMEXIT"
