#!/bin/sh
# pc64 isolated TAP gate for the modern single-queue virtio-net provider.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-virtiod-net-vmm}
VM=${VMM_MACHINE:-virtiodnet0}
ISO=${ALPINE_ISO:-/var/tmp/alpine-extended-3.24.1-x86_64.iso}
LOADER=${ALPINE_LOADER:-/var/tmp/dfvmm-alpine-boot-extended.sh}
VIRTIOD=${VIRTIOD:-$REPO/sbin/virtiod/virtiod}
LOG=${VMM_LOG:-/var/tmp/dfvmm-virtiod-net-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-virtiod-net-console.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-virtiod-net-$$-mount_vmm}
MEM=${VMM_NET_MEM:-1G}
TIMEOUT=${VMM_TIMEOUT:-60}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}
HOST_ADDR=${VMM_NET_HOST_ADDR:-192.0.2.1/30}
GUEST_ADDR=${VMM_NET_GUEST_ADDR:-192.0.2.2/30}
MAC=${VMM_NET_MAC:-02:11:22:33:44:55}

LOADED=0
MOUNTED=0
TAP_LOADED=0
TAP=
CONSOLE_READER_PID=
BLK_PID=
NET_PID=

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

machine()
{
	printf '%s/machines/%s\n' "$MNT" "$VM"
}

device()
{
	printf '%s/devices/%s\n' "$(machine)" "$1"
}

host_device()
{
	printf '%s/machines/host/devices/%s\n' "$MNT" "$1"
}

wait_pattern()
{
	file=$1
	pattern=$2
	label=$3
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$file" && return 0
		sleep 1
		i=$((i + 1))
	done
	say "missing marker: $label"
	return 1
}

wait_state()
{
	path=$1
	expected=$2
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		state=$(cat "$path/state" 2>>"$LOG") || state=
		[ "$state" = "$expected" ] && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

wait_provider_exit()
{
	pid=$1
	name=$2
	i=0

	while kill -0 "$pid" >/dev/null 2>&1; do
		[ "$i" -lt "$STOP_TIMEOUT" ] || return 1
		sleep 1
		i=$((i + 1))
	done
	wait "$pid" || return 1
	say "$name provider stopped"
	return 0
}

remove_path()
{
	path=$1
	i=0

	while [ "$i" -lt "$STOP_TIMEOUT" ]; do
		rmdir "$path" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

stop_console_reader()
{
	if [ -n "$CONSOLE_READER_PID" ]; then
		kill "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
		wait "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
		CONSOLE_READER_PID=
	fi
}

stop_provider()
{
	pid=$1
	if [ -n "$pid" ] && kill -0 "$pid" >/dev/null 2>&1; then
		kill "$pid" >/dev/null 2>&1 || true
		wait "$pid" >/dev/null 2>&1 || true
	fi
}

dump_state()
{
	{
		printf '%s\n' '--- kldstat ---'
		kldstat -n vmm 2>&1 || true
		printf '%s\n' '--- events ---'
		[ -d "$(machine)" ] && cat "$(machine)/events" 2>&1 || true
		printf '%s\n' '--- provider state ---'
		[ -d "$(device blk0)" ] && cat "$(device blk0)/state" 2>&1 || true
		[ -d "$(device net0)" ] && cat "$(device net0)/state" 2>&1 || true
		printf '%s\n' '--- console ---'
		cat "$CONSOLE_LOG" 2>&1 || true
		printf '%s\n' '--- end state ---'
	} >>"$LOG"
}

cleanup()
{
	set +e
	stop_console_reader
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(machine)" ]; then
		echo force >"$(machine)/stopped" 2>>"$LOG"
	fi
	stop_provider "$NET_PID"
	stop_provider "$BLK_PID"
	[ "$MOUNTED" -eq 1 ] && [ -d "$(device net0)" ] &&
		remove_path "$(device net0)"
	[ "$MOUNTED" -eq 1 ] && [ -d "$(device blk0)" ] &&
		remove_path "$(device blk0)"
	[ "$MOUNTED" -eq 1 ] && [ -d "$(machine)" ] &&
		remove_path "$(machine)"
	[ "$MOUNTED" -eq 1 ] && [ -d "$(host_device net0)" ] &&
		remove_path "$(host_device net0)"
	[ "$MOUNTED" -eq 1 ] && [ -d "$(host_device blk0)" ] &&
		remove_path "$(host_device blk0)"
	if [ "$MOUNTED" -eq 1 ]; then
		i=0
		while [ "$i" -lt "$STOP_TIMEOUT" ]; do
			umount "$MNT" >>"$LOG" 2>&1 && { MOUNTED=0; break; }
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		i=0
		while [ "$i" -lt "$STOP_TIMEOUT" ]; do
			kldunload vmm >>"$LOG" 2>&1 && { LOADED=0; break; }
			sleep 1
			i=$((i + 1))
		done
	fi
	[ -z "$TAP" ] || ifconfig "$TAP" destroy >/dev/null 2>&1
	[ "$TAP_LOADED" -eq 0 ] || kldunload if_tap >/dev/null 2>&1
	rm -f "$MOUNT_HELPER"
}

: >"$LOG" || exit 1
: >"$CONSOLE_LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be absolute" ;; esac
case "$MOUNT_HELPER" in *_vmm) ;; *) fail "VMM_MOUNT_HELPER must end in _vmm" ;; esac
[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
[ -f "$ISO" ] || fail "missing $ISO"
[ -x "$LOADER" ] || fail "missing $LOADER"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
readelf -SW "$VMM_KO" 2>>"$LOG" | grep -qi eh_frame &&
	fail "$VMM_KO contains .eh_frame"
run make -C "$REPO/sbin/virtiod" test
run make -C "$REPO/sbin/virtiod"
[ -x "$VIRTIOD" ] || fail "missing $VIRTIOD"

if ! kldstat -n if_tap >/dev/null 2>&1; then
	run kldload if_tap
	TAP_LOADED=1
fi
TAP=$(ifconfig tap create) || fail "create TAP"
run ifconfig "$TAP" inet "$HOST_ADDR" up

run kldload "$VMM_KO"
LOADED=1
run mkdir -p "$MNT"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1
run mkdir "$(machine)"
printf '1\n' >"$(machine)/vcpu" || fail "write vcpu"
printf '%s\n' "$MEM" >"$(machine)/mem" || fail "write mem"
printf '%s\n' "$LOADER" >"$(machine)/loader" || fail "write loader"
run mkdir "$(host_device blk0)"
run mkdir "$(host_device net0)"
run mv "$(host_device blk0)" "$(device blk0)"
run mv "$(host_device net0)" "$(device net0)"

"$VIRTIOD" blk "$(device blk0)" "$ISO" >>"$LOG" 2>&1 &
BLK_PID=$!
"$VIRTIOD" net "$(device net0)" "$TAP" "$MAC" >>"$LOG" 2>&1 &
NET_PID=$!
wait_state "$(device blk0)" 'provider=pending
consumer=root' || fail "block provider was not pending"
wait_state "$(device net0)" 'provider=pending
consumer=root' || fail "network provider was not pending"

cat "$(machine)/console" >>"$CONSOLE_LOG" 2>>"$LOG" &
CONSOLE_READER_PID=$!
run rm "$(machine)/stopped"
wait_state "$(device blk0)" 'provider=registered
consumer=root' || fail "block provider did not register"
wait_state "$(device net0)" 'provider=registered
consumer=root' || fail "network provider did not register"
wait_pattern "$LOG" "virtiod: ready net=$TAP mac=$MAC" provider ||
	fail "network provider was not ready"
wait_pattern "$CONSOLE_LOG" 'localhost login:' console ||
	fail "Linux login prompt missing"
printf 'root\n' >"$(machine)/console" || fail "console login"
wait_pattern "$CONSOLE_LOG" 'localhost:~#' shell || fail "Linux shell missing"
printf '%s\n' "ip link set eth0 up; ip addr add $GUEST_ADDR dev eth0; ip link show eth0; ping -c 3 -W 2 ${HOST_ADDR%/*}; echo DFVMM_VIRTIOD_NET_DONE" >"$(machine)/console" ||
	fail "network probe write"
wait_pattern "$CONSOLE_LOG" "link/ether $MAC" mac || fail "guest MAC mismatch"
wait_pattern "$CONSOLE_LOG" '3 packets transmitted, 3 packets received' ping ||
	fail "guest TAP ping failed"
wait_pattern "$CONSOLE_LOG" 'DFVMM_VIRTIOD_NET_DONE' shell_done ||
	fail "guest network command did not finish"

stop_console_reader
echo force >"$(machine)/stopped" || fail "force stop"
wait_pattern "$(machine)/events" 'state stopped reason=force' stopped ||
	fail "machine did not stop"
wait_provider_exit "$NET_PID" network || fail "network provider did not stop"
NET_PID=
wait_provider_exit "$BLK_PID" block || fail "block provider did not stop"
BLK_PID=
run rmdir "$(device net0)"
run rmdir "$(device blk0)"
run rmdir "$(machine)"
run umount "$MNT"
MOUNTED=0
run kldunload vmm
LOADED=0
ifconfig "$TAP" destroy || fail "destroy TAP"
TAP=
[ "$TAP_LOADED" -eq 0 ] || kldunload if_tap
TAP_LOADED=0
say 'PASS: Linux virtio-net TAP RX/TX and lifecycle'
