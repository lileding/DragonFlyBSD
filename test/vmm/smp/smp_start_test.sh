#!/bin/sh
# pc64 SVM two-vCPU INIT/SIPI and root pmap shootdown regression.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-smp-start-vmm}
VM=${VMM_MACHINE:-smpstart0}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_smp_start_loader}
WRAPPER=${VMM_WRAPPER:-/var/tmp/vmmld_smp_start}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-smp-start-$$-mount_vmm}
STRESS_C=${VMM_STRESS_C:-/var/tmp/dfvmm-smp-start-stress-$$.c}
STRESS_BIN=${VMM_STRESS_BIN:-/var/tmp/dfvmm-smp-start-stress-$$}
LOG=${VMM_LOG:-/var/tmp/dfvmm-smp-start-test.log}
TIMEOUT=${VMM_TIMEOUT:-20}
LOADED=0
MOUNTED=0

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; exit 1; }
run() { say "+ $*"; "$@" >>"$LOG" 2>&1 || fail "$*"; }
mach() { printf '%s/%s\n' "$MNT" "$VM"; }

wait_event()
{
	pattern=$1
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		cat "$(mach)/events" >>"$LOG" 2>&1
		grep -q "$pattern" "$LOG" && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		touch "$(mach)/stopped" >>"$LOG" 2>&1
		i=0
		while [ "$i" -lt "$TIMEOUT" ]; do
			rmdir "$(mach)" >>"$LOG" 2>&1 && break
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		i=0
		while [ "$i" -lt "$TIMEOUT" ]; do
			umount "$MNT" >>"$LOG" 2>&1 && {
				MOUNTED=0
				break
			}
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1
	fi
	rm -f "$WRAPPER" "$MOUNT_HELPER" "$STRESS_C" "$STRESS_BIN"
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

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
[ -f "$VMM_KO" ] || fail "missing VMM_KO=$VMM_KO"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"
build_stress
printf '#!/bin/sh\nexec "%s" smpboot\n' "$LOADER" >"$WRAPPER" ||
	fail "write $WRAPPER"
run chmod +x "$WRAPPER"
run kldload "$VMM_KO"; LOADED=1
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run mkdir -p "$MNT"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1
run mkdir "$(mach)"
printf '2\n' >"$(mach)/vcpu" || fail "write vcpu"
printf '2M\n' >"$(mach)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(mach)/loader" || fail "write loader"
run rm "$(mach)/stopped"
wait_event 'state running' || fail "machine did not reach running"
"$STRESS_BIN" >>"$LOG" 2>&1 || fail "host pmap stress failed"
run touch "$(mach)/stopped"
wait_event 'state stopped reason=stop' || fail "machine did not stop"
run rmdir "$(mach)"
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
	umount "$MNT" >>"$LOG" 2>&1 && {
		MOUNTED=0
		break
	}
	sleep 1
	i=$((i + 1))
done
[ "$MOUNTED" -eq 0 ] || fail "umount $MNT"
run kldunload vmm; LOADED=0
say "PASS: two-vCPU INIT/SIPI plus root pmap shootdown"
