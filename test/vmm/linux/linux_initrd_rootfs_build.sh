#!/bin/sh
set -eu

ROOT=$(dirname "$0")
BASE=${LINUX_BASE_INITRAMFS:-/var/tmp/alpine-initramfs-virt}
WORK=${LINUX_INITRD_WORK:-/var/tmp/dfvmm-linux-initrd-rootfs}
OUT=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
PACKER=${LINUX_INITRD_PACKER:-$ROOT/linux_initrd_pack_newc.py}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

[ -f "$BASE" ] || fail "missing base initramfs: $BASE"
[ -f "$PACKER" ] || fail "missing packer: $PACKER"

rm -rf "$WORK"
mkdir -p "$WORK"

(
	cd "$WORK"
	gzip -dc "$BASE" | cpio -idmu >/dev/null
	if [ -f init ]; then
		mv init init.alpine
	fi

	mkdir -p dev proc sys run tmp root usr/local/bin etc
	: >dev/console
	: >dev/ttyS0
	: >dev/null
	: >dev/kmsg
	: >dev/zero
	: >dev/random
	: >dev/urandom
	if [ -x bin/busybox ]; then
		for applet in cat chmod dmesg echo false grep ls mkdir mknod mount ps sed sh sleep sync true uname; do
			ln -sf busybox "bin/$applet"
		done
	fi

	cat >etc/profile <<'EOF'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
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
tty=/dev/ttyS0

$bb mount -t proc proc /proc 2>/dev/null || true
$bb mount -t sysfs sysfs /sys 2>/dev/null || true
$bb mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
$bb mkdir -p /run /tmp /root
$bb mount -t tmpfs tmpfs /run 2>/dev/null || true
$bb mount -t tmpfs tmpfs /tmp 2>/dev/null || true

[ -c /dev/null ] || $bb mknod /dev/null c 1 3 2>/dev/null || true
[ -c /dev/kmsg ] || $bb mknod /dev/kmsg c 1 11 2>/dev/null || true
[ -c /dev/console ] || $bb mknod /dev/console c 5 1 2>/dev/null || true
[ -c "$tty" ] || $bb mknod "$tty" c 4 64 2>/dev/null || true
$bb chmod 666 /dev/null /dev/kmsg /dev/console "$tty" 2>/dev/null || true

if [ -x /sbin/mdev ]; then
	echo /sbin/mdev >/proc/sys/kernel/hotplug 2>/dev/null || true
	/sbin/mdev -s 2>/dev/null || true
fi

echo DFVMM_LINUX_INITRD_ROOTFS_OK >/dev/kmsg
echo DFVMM_LINUX_INITRD_ROOTFS_OK >/dev/console
echo DFVMM_LINUX_SERIAL_OK >"$tty"
echo "dfvmm in-memory Linux initrd rootfs is ready." >"$tty"
echo "Run dfvmm-core-smoke for the baseline vmm core check." >"$tty"

while :; do
	$bb sh -l -i <"$tty" >"$tty" 2>&1
	echo DFVMM_LINUX_SHELL_EXITED >"$tty"
	$bb sleep 1
done
EOF
	chmod +x init
)

"$PACKER" "$WORK" "$OUT"
ls -lh "$OUT"
