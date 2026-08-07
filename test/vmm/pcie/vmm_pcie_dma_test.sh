#!/bin/sh
#
# pc64 P7 revocable per-provider DMA capability harness.
#
# This uses the boot kernel's existing OBJT_MGTDEVICE page-removal path.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BASE_SYS="$REPO/sys"
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-pcie-dma-vmm}
VM=${VMM_MACHINE:-pciedma0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-pcie-dma-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_pcie_dma_loader}
WRAPPER=${VMM_LOADER_WRAPPER:-/var/tmp/vmm_pcie_dma_loader_wrapper}
PROVIDER=${VMM_PCIE_DMA_PROVIDER:-/var/tmp/vmm_pcie_dma_provider}
PROVIDER_LOG=${VMM_PCIE_DMA_PROVIDER_LOG:-/var/tmp/dfvmm-pcie-dma-provider.log}
UNLOAD_LOG=${VMM_PCIE_DMA_UNLOAD_LOG:-/var/tmp/dfvmm-pcie-dma-unload.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-pcie-dma-$$-mount_vmm}
TIMEOUT=${VMM_TIMEOUT:-20}

LOADED=0
MOUNTED=0
PROVIDER_PID=

say()
{
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"
}

fail()
{
	say "FAIL: $*"
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(machine)" ]; then
		say "machine events:"
		cat "$(machine)/events" >>"$LOG" 2>&1
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(device)" ]; then
		say "device state:"
		cat "$(device)/state" >>"$LOG" 2>&1
	fi
	[ -f "$PROVIDER_LOG" ] && cat "$PROVIDER_LOG" >>"$LOG" 2>&1
	exit 1
}

run()
{
	say "+ $*"
	"$@" >>"$LOG" 2>&1 || fail "$*"
}

machine()
{
	printf '%s/%s\n' "$MNT" "$VM"
}

device()
{
	printf '%s/devices/dma0\n' "$(machine)"
}

wait_pattern()
{
	file=$1
	pattern=$2
	label=$3
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$file" 2>/dev/null && return 0
		sleep 1
		i=$((i + 1))
	done
	fail "missing $label"
}

remove_path()
{
	path=$1
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		rmdir "$path" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(machine)" ]; then
		touch "$(machine)/stopped" 2>>"$LOG"
	fi
	if [ -n "$PROVIDER_PID" ]; then
		kill "$PROVIDER_PID" >/dev/null 2>&1
		wait "$PROVIDER_PID" >/dev/null 2>&1
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(device)" ]; then
		remove_path "$(device)"
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(machine)" ]; then
		remove_path "$(machine)"
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		i=0
		while [ "$i" -lt "$TIMEOUT" ]; do
			umount "$MNT" >>"$LOG" 2>&1 && MOUNTED=0 && break
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1
	fi
	rm -f "$MOUNT_HELPER" "$LOADER" "$WRAPPER" "$PROVIDER" "$UNLOAD_LOG"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be absolute" ;; esac
[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
readelf -SW "$VMM_KO" 2>>"$LOG" | grep -qi eh_frame &&
	fail "$VMM_KO contains .eh_frame"

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"
run cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys/vmm" -I "$BASE_SYS" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$REPO/test/vmm/pcie/vmm_pcie_dma_provider.c" -o "$PROVIDER"
cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
exec "$LOADER" loop
EOF_WRAP
chmod 755 "$WRAPPER" || fail "chmod $WRAPPER"

run kldload "$VMM_KO"; LOADED=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"; MOUNTED=1
run mkdir "$(machine)"
printf '1\n' >"$(machine)/vcpu" || fail "write vcpu"
printf '2M\n' >"$(machine)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(machine)/loader" || fail "write loader"
run mkdir "$(device)"
: >"$PROVIDER_LOG" || fail "create provider log"
"$PROVIDER" "$(device)" >>"$PROVIDER_LOG" 2>>"$LOG" &
PROVIDER_PID=$!
wait_pattern "$(device)/state" 'provider=pending' provider_pending

run rm "$(machine)/stopped"
wait_pattern "$PROVIDER_LOG" 'DFVMM_PCIE_DMA_PROVIDER_READY' dma_start
touch "$(machine)/stopped" || fail "stop"
wait_pattern "$PROVIDER_LOG" 'DFVMM_PCIE_DMA_PROVIDER_REVOKED' dma_revoke
remove_path "$(device)" || fail "remove device"
remove_path "$(machine)" || fail "remove machine"
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
	umount "$MNT" >>"$LOG" 2>&1 && MOUNTED=0 && break
	sleep 1
	i=$((i + 1))
done
[ "$MOUNTED" -eq 0 ] || fail "umount $MNT"
: >"$UNLOAD_LOG" || fail "create unload log"
say '+ kldunload vmm (expect EBUSY while revoked mappings remain)'
if kldunload vmm >"$UNLOAD_LOG" 2>&1; then
	cat "$UNLOAD_LOG" >>"$LOG"
	fail "kldunload succeeded with retained capability mappings"
fi
cat "$UNLOAD_LOG" >>"$LOG"
grep -qi 'device busy' "$UNLOAD_LOG" ||
	fail "kldunload did not report EBUSY for retained mappings"
kldstat -n vmm >/dev/null 2>&1 ||
	fail "vmm unloaded despite retained capability mappings"
kill "$PROVIDER_PID" || fail "terminate retained provider"
wait "$PROVIDER_PID" >/dev/null 2>&1
PROVIDER_PID=
run kldunload vmm; LOADED=0
say 'PASS: vPCIe BAR and DMA revoke retains module unload busy'
