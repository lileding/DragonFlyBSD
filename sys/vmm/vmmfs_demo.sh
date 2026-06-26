#!/bin/sh
# Legacy vkernel-only vmmfs demo.
#
# This predates the real pc64 SVM execution path.  It is not a current
# validation path and is disabled by default so it cannot mask true-hardware
# failures.
if [ "${VMM_ALLOW_LEGACY_VKERNEL_DEMO:-}" != "1" ]; then
	echo "vmmfs_demo.sh is legacy vkernel-only; use test/vmm true-hardware harnesses" >&2
	exit 1
fi

MOUNT_HELPER=${VMM_MOUNT_HELPER:-/tmp/vmmfs-demo-$$-mount_vmm}
sep() { echo; echo "==================== $1 ===================="; }
run() { echo "\$ $1"; eval "$1"; }

trap 'rm -f "$MOUNT_HELPER"' EXIT INT TERM

kldload /xchg/vmm.ko
ln -sf /sbin/mount_std "$MOUNT_HELPER"
mkdir -p /vmm
"$MOUNT_HELPER" vmm /vmm

sep "1. 挂载后的命名空间"
run "ls /vmm"
echo "-> machines/(机器) + devices/(全局设备符号链接索引)"
run "ls /vmm/machines"
echo "-> host 是物理机，永远在"
run "ls /vmm/machines/host/devices"
echo "-> host 的设备池（桩：几个假 BDF）"

sep "2. 创建机器 = mkdir（永远先 stopped）"
run "mkdir /vmm/machines/vm0"
run "ls /vmm/machines/vm0 | sort | tr '\n' ' '; echo"
echo "-> vcpu/mem/loader 配置寄存器 + stopped + lease/events/console/status.tar.gz + devices/"

sep "3. 配置寄存器：写入缓冲，close 提交，读回为准（PCIe 风格）"
run "cat /vmm/machines/vm0/vcpu"
echo "-> 还没设 = 空"
run "echo 4 > /vmm/machines/vm0/vcpu"
run "cat /vmm/machines/vm0/vcpu"
run "echo 512M > /vmm/machines/vm0/mem; cat /vmm/machines/vm0/mem"
echo "-> 规范化成字节"
run "echo 0 > /vmm/machines/vm0/vcpu; cat /vmm/machines/vm0/vcpu"
echo "-> 非法值(0)被丢弃，读回仍是 4：写成功 != 更新成功"

sep "4. 启动请求 = rm stopped（声明 desired=running，后台 worker 执行 loader）"
printf '#!/bin/sh\necho hi\n' > /tmp/loader; chmod 755 /tmp/loader
run "echo /tmp/loader > /vmm/machines/vm0/loader"
run "rm /vmm/machines/vm0/stopped"
run "ls /vmm/machines/vm0 | sort | tr '\n' ' '; echo"
echo "-> stopped 文件消失 = 期望 running；rm 返回不表示 loader 已完成"

sep "5. 事件流：读 events 拿生命周期（一次性）"
run "cat /vmm/machines/vm0/events"
echo "-> created/started；再读一次就空了"
run "cat /vmm/machines/vm0/events; echo '(空)'"

sep "6. 停机 = echo apic > stopped"
run "echo apic > /vmm/machines/vm0/stopped"
run "ls /vmm/machines/vm0 | grep '^stopped$'"
run "cat /vmm/machines/vm0/events"
echo "-> stopped 文件回来了 + 一条 stopped 事件"

sep "7. 设备直通 = mv（绑定），rm（退回 host）"
run "mv /vmm/machines/host/devices/0000:00:02.0 /vmm/machines/vm0/devices/"
run "ls /vmm/machines/vm0/devices"
run "ls /vmm/machines/host/devices"
echo "-> 设备从 host 池移到 vm0 = 绑定"
run "readlink /vmm/devices/0000:00:02.0"
echo "-> /vmm/devices/ 全局索引的符号链接跟着指向新 owner"
run "rm /vmm/machines/vm0/devices/0000:00:02.0"
run "ls /vmm/machines/host/devices"
echo "-> rm 把 host 设备退回池子 = 解绑"

sep "8. 删除 = rmdir（须先 stopped）"
run "rmdir /vmm/machines/vm0"
run "ls /vmm/machines"

umount /vmm
kldunload vmm
echo; echo "==================== 导览结束 ===================="
