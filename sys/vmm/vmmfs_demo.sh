#!/bin/sh
# vmmfs 体验导览（在 vkernel 内由 vkrun2 跑）。把每个特性走一遍，打印命令 + 真实输出。
sep() { echo; echo "==================== $1 ===================="; }
run() { echo "\$ $1"; eval "$1"; }

kldload /xchg/vmm.ko
ln -sf /sbin/mount_std /sbin/mount_vmm
mkdir -p /vmm
mount -t vmm vmm /vmm

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

sep "4. 启动 = rm stopped（先校验配置完整 + loader 可执行）"
printf '#!/bin/sh\necho hi\n' > /tmp/loader; chmod 755 /tmp/loader
run "echo /tmp/loader > /vmm/machines/vm0/loader"
run "rm /vmm/machines/vm0/stopped"
run "ls /vmm/machines/vm0 | sort | tr '\n' ' '; echo"
echo "-> stopped 文件消失 = 期望 running"

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
