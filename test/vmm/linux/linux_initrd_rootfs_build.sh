#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
BASE=${LINUX_BASE_INITRAMFS:-/var/tmp/alpine-initramfs-virt}
WORK=${LINUX_INITRD_WORK:-/var/tmp/dfvmm-linux-initrd-rootfs}
OUT=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
PACKER=${LINUX_INITRD_PACKER:-$ROOT/linux_initrd_pack_newc.py}
PROBE_SRC=${LINUX_TSC_PM_PROBE_SRC:-$ROOT/linux_tsc_pm_probe.c}
PROBE_CC=${LINUX_TSC_PM_PROBE_CC:-/usr/local/bin/clang19}
PROBE_LD=${LINUX_TSC_PM_PROBE_LD:-/usr/local/bin/ld.lld19}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

[ -f "$BASE" ] || fail "missing base initramfs: $BASE"
[ -f "$PACKER" ] || fail "missing packer: $PACKER"
[ -f "$PROBE_SRC" ] || fail "missing TSC/PM probe source: $PROBE_SRC"
[ -x "$PROBE_CC" ] || fail "missing TSC/PM probe compiler: $PROBE_CC"
[ -x "$PROBE_LD" ] || fail "missing TSC/PM probe linker: $PROBE_LD"

rm -rf "$WORK"
mkdir -p "$WORK"

(
	cd "$WORK"
	gzip -dc "$BASE" | cpio -idmu >/dev/null
	if [ -f init ]; then
		mv init init.alpine
	fi

	mkdir -p dev proc sys run tmp root usr/local/bin etc
	"$PROBE_CC" --target=x86_64-linux-gnu -std=c11 -O2 -Wall -Wextra \
		-Werror -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib -static \
		--ld-path="$PROBE_LD" -Wl,-e,_start -Wl,-z,noexecstack \
		-Wl,--build-id=none "$PROBE_SRC" \
		-o usr/local/bin/dfvmm-tsc-pm-probe
	readelf -SW usr/local/bin/dfvmm-tsc-pm-probe | grep -qi eh_frame &&
		fail "TSC/PM probe contains unwind sections"
	: >dev/console
	: >dev/ttyS0
	: >dev/null
	: >dev/kmsg
	: >dev/zero
	: >dev/random
	: >dev/urandom
	if [ -x bin/busybox ]; then
		for applet in cat chmod dmesg echo false grep ls mkdir mknod mount poweroff ps reboot sed sh sleep sync true uname; do
			ln -sf busybox "bin/$applet"
		done
	fi

	cat >etc/profile <<'EOF'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export TERM=dumb
export PS1='dfvmm-linux:\w# '
EOF

	cat >usr/local/bin/dfvmm-core-smoke <<'EOF'
#!/bin/sh
set -eu

bb=/bin/busybox
echo DFVMM_CORE_SMOKE_BEGIN
$bb uname -a
$bb cat /proc/uptime
$bb cat /proc/meminfo | $bb sed -n '1,8p'
$bb cat /proc/interrupts
$bb cat /proc/tty/driver/serial 2>/dev/null || true
for i in 1 2 3; do
	echo "DFVMM_CORE_TICK_$i $($bb cat /proc/uptime)"
	$bb sleep 1
done
echo DFVMM_CORE_SMOKE_END
EOF
	chmod +x usr/local/bin/dfvmm-core-smoke

	cat >init <<'EOF'
#!/bin/sh

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
bb=/bin/busybox
console=/dev/console
serial=/dev/ttyS0

$bb mount -t proc proc /proc 2>/dev/null || true
$bb mount -t sysfs sysfs /sys 2>/dev/null || true
$bb mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
$bb mkdir -p /run /tmp /root
$bb mount -t tmpfs tmpfs /run 2>/dev/null || true
$bb mount -t tmpfs tmpfs /tmp 2>/dev/null || true

[ -c /dev/null ] || $bb mknod /dev/null c 1 3 2>/dev/null || true
[ -c /dev/kmsg ] || $bb mknod /dev/kmsg c 1 11 2>/dev/null || true
[ -c "$console" ] || $bb mknod "$console" c 5 1 2>/dev/null || true
[ -c "$serial" ] || $bb mknod "$serial" c 4 64 2>/dev/null || true
$bb chmod 666 /dev/null /dev/kmsg "$console" "$serial" 2>/dev/null || true

if [ -x /sbin/mdev ]; then
	echo /sbin/mdev >/proc/sys/kernel/hotplug 2>/dev/null || true
	/sbin/mdev -s 2>/dev/null || true
fi

echo DFVMM_LINUX_INITRD_ROOTFS_OK >/dev/kmsg
echo DFVMM_LINUX_INITRD_ROOTFS_OK >"$console"
exec <>"$console"
exec >&0 2>&1
echo DFVMM_LINUX_SERIAL_OK
echo "dfvmm in-memory Linux initrd rootfs is ready."
echo "Run dfvmm-core-smoke for the baseline vmm core check."
echo DFVMM_LINUX_CONSOLE_READY
cd /root 2>/dev/null || cd /
while :; do
	printf 'dfvmm-linux:%s# ' "$PWD"
	if ! IFS= read -r line; then
		echo DFVMM_LINUX_CONSOLE_READ_FAILED
		$bb sleep 1
		continue
	fi
	case "$line" in
	"")
		;;
	exit|logout)
		echo DFVMM_LINUX_CONSOLE_EXIT_IGNORED
		;;
	cd)
		cd /root || cd / || true
		;;
	cd\ *)
		dir=${line#cd }
		if ! cd "$dir"; then
			echo "cd: $dir: failed"
		fi
		;;
	*)
		$bb sh -c "$line"
		;;
	esac
done
EOF
	chmod +x init
)

"$PACKER" "$WORK" "$OUT"
ls -lh "$OUT"
