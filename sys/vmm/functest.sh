#!/bin/sh
# vmmfs functional test (runs inside the vkernel via vkrun2).
#
# New model: a machine is `mkdir`-ed (always stopped, empty config); vcpu/mem/
# loader are PCIe-like registers (write into a buffer, commit on close, read
# back to confirm); `rm stopped` starts it after validating the config.
#
# NOTE: the lease file is a reference handle, not data — opening it (cat/exec)
# acquires a handle and closing it releases one, deleting the machine on the
# last release.  Lease behavior is tested on dedicated throwaway machines.
M=/vmm/machines
pass=0
fail=0
ckok()   { if [ "$2" = "0" ];  then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1 (rc=$2 want 0)"; fail=$((fail+1)); fi; }
ckfail() { if [ "$2" != "0" ]; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1 (rc=0 want nonzero)"; fail=$((fail+1)); fi; }
ckeq()   { if [ "$2" = "$3" ]; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }

kldload /xchg/vmm.ko; ckok "kldload" $?
ln -sf /sbin/mount_std /sbin/mount_vmm
mkdir -p /vmm
mount -t vmm vmm /vmm; ckok "mount" $?
ckeq "machines has host" "$(ls $M)" "host"

# --- host machine + stub device pool ---
ckeq "ls host" "$(ls $M/host)" "devices"
ckeq "ls host/devices" "$(ls $M/host/devices | sort | tr '\n' ' ')" "0000:00:02.0 0000:00:03.0 0000:00:04.0 "
ckeq "cat a device" "$(cat $M/host/devices/0000:00:02.0)" "0000:00:02.0"
mkdir $M/host 2>/dev/null; ckfail "mkdir host -> reserved" $?
rmdir $M/host 2>/dev/null; ckfail "rmdir host -> EPERM" $?

# loader fixtures: an executable script, a non-exec file
printf '#!/bin/sh\necho hi\n' > /tmp/ld.sh; chmod 755 /tmp/ld.sh
printf 'x' > /tmp/noexec; chmod 644 /tmp/noexec

# --- create via mkdir: always stopped, default files, empty config ---
mkdir $M/vm0; ckok "mkdir vm0" $?
ckeq "ls vm0 default" "$(ls $M/vm0 | sort | tr '\n' ' ')" "console devices events lease loader mem status.tar.gz stopped vcpu "
ckeq "vm0 devices empty" "$(ls $M/vm0/devices)" ""
ckeq "vcpu unset empty" "$(cat $M/vm0/vcpu)" ""
ckeq "mem unset empty" "$(cat $M/vm0/mem)" ""
ckeq "loader unset empty" "$(cat $M/vm0/loader)" ""

# --- config registers: write, then read back (PCIe semantics) ---
echo 4 > $M/vm0/vcpu;       ckeq "vcpu readback" "$(cat $M/vm0/vcpu)" "4"
echo 512M > $M/vm0/mem;     ckeq "mem readback" "$(cat $M/vm0/mem)" "536870912"
echo /tmp/ld.sh > $M/vm0/loader; ckeq "loader readback" "$(cat $M/vm0/loader)" "/tmp/ld.sh"

# --- an invalid write succeeds but does not update; read-back is the truth ---
echo 0 > $M/vm0/vcpu;       ckeq "invalid vcpu kept old" "$(cat $M/vm0/vcpu)" "4"
echo 3M > $M/vm0/mem;       ckeq "unaligned mem kept old" "$(cat $M/vm0/mem)" "536870912"
echo 8 > $M/vm0/vcpu;       ckeq "rewrite vcpu" "$(cat $M/vm0/vcpu)" "8"

# --- start (rm stopped) requires a complete, valid config ---
mkdir $M/inc; echo 2 > $M/inc/vcpu
rm $M/inc/stopped 2>/dev/null; ckfail "rm stopped incomplete -> fail" $?
ckeq "inc still stopped" "$(ls $M/inc | grep -c '^stopped$')" "1"

mkdir $M/nx; echo 1 > $M/nx/vcpu; echo 2M > $M/nx/mem; echo /tmp/noexec > $M/nx/loader
rm $M/nx/stopped 2>/dev/null; ckfail "rm stopped non-exec loader -> fail" $?

mkdir $M/ml; echo 1 > $M/ml/vcpu; echo 2M > $M/ml/mem; echo /tmp/nope > $M/ml/loader
rm $M/ml/stopped 2>/dev/null; ckfail "rm stopped missing loader -> fail" $?

rm $M/vm0/stopped; ckok "start vm0 (valid)" $?
ckeq "vm0 running no stopped" "$(ls $M/vm0 | sort | tr '\n' ' ')" "console devices events lease loader mem status.tar.gz vcpu "
rm $M/vm0/stopped 2>/dev/null; ckfail "rm stopped while running -> ENOENT" $?

# --- stop (echo apic|force > stopped), idempotent ---
echo apic > $M/vm0/stopped; ckok "stop apic" $?
ckeq "vm0 has stopped" "$(ls $M/vm0 | grep -c '^stopped$')" "1"
echo force > $M/vm0/stopped; ckok "re-stop force idempotent" $?

# --- read-only files reject writes / removal ---
echo x > $M/vm0/events 2>/dev/null;        ckfail "write events -> fail" $?
echo x > $M/vm0/status.tar.gz 2>/dev/null; ckfail "write status -> fail" $?
rm $M/vm0/vcpu 2>/dev/null;                ckfail "rm vcpu -> EPERM" $?

# --- events: created+stopped on mkdir, one-shot, lifecycle ---
mkdir $M/ev
ckeq "events on create" "$(cat $M/ev/events | tr '\n' ',')" "created,stopped,"
ckeq "events one-shot drained" "$(cat $M/ev/events)" ""
echo 1 > $M/ev/vcpu; echo 2M > $M/ev/mem; echo /tmp/ld.sh > $M/ev/loader
rm $M/ev/stopped;          ckeq "event on start" "$(cat $M/ev/events | tr '\n' ',')" "started,"
echo apic > $M/ev/stopped; ckeq "event on stop" "$(cat $M/ev/events | tr '\n' ',')" "stopped,"
echo apic > $M/ev/stopped; ckeq "idempotent stop -> no event" "$(cat $M/ev/events)" ""
rmdir $M/ev; ckok "rmdir ev" $?

# --- rmdir requires stopped (M3b) ---
mkdir $M/rr; echo 1 > $M/rr/vcpu; echo 2M > $M/rr/mem; echo /tmp/ld.sh > $M/rr/loader
rm $M/rr/stopped; ckok "start rr" $?
rmdir $M/rr 2>/dev/null; ckfail "rmdir running -> EBUSY" $?
echo apic > $M/rr/stopped
rmdir $M/rr; ckok "rmdir after stop" $?

# --- lease: open/close reference counting (M3b) ---
mkdir $M/lv
exec 7< $M/lv/lease
ckeq "leased machine present" "$(ls $M | grep -c '^lv$')" "1"
exec 7<&-
ckeq "lease release deleted machine" "$(ls $M | grep -c '^lv$')" "0"

mkdir $M/lc
exec 7< $M/lc/lease
exec 8< $M/lc/lease
exec 7<&-
ckeq "present after 1 of 2 releases" "$(ls $M | grep -c '^lc$')" "1"
exec 8<&-
ckeq "deleted after last release" "$(ls $M | grep -c '^lc$')" "0"

# --- lease: rmdir(stopped) deletes despite an open lease (source 1) ---
mkdir $M/rl
exec 7< $M/rl/lease
rmdir $M/rl; ckok "rmdir leased+stopped" $?
ckeq "rl gone after rmdir" "$(ls $M | grep -c '^rl$')" "0"
exec 7<&-

# --- lease: an open lease blocks unmount (source 2) ---
mkdir $M/u1
exec 7< $M/u1/lease
umount /vmm 2>/dev/null; ckfail "umount with open lease -> EBUSY" $?
exec 7<&-
ckeq "u1 gone after lease release" "$(ls $M | grep -c '^u1$')" "0"

# --- device index + binding (D2-D4) ---
ckeq "ls /vmm/devices index" "$(ls /vmm/devices | sort | tr '\n' ' ')" "0000:00:02.0 0000:00:03.0 0000:00:04.0 "
ckeq "symlink -> host owner" "$(readlink /vmm/devices/0000:00:02.0)" "../machines/host/devices/0000:00:02.0"

mkdir $M/dv; echo 1 > $M/dv/vcpu; echo 2M > $M/dv/mem; echo /tmp/ld.sh > $M/dv/loader
mv $M/host/devices/0000:00:02.0 $M/dv/devices/; ckok "mv bind device to dv" $?
ckeq "dv has device" "$(ls $M/dv/devices)" "0000:00:02.0"
ckeq "host pool lost it" "$(ls $M/host/devices | sort | tr '\n' ' ')" "0000:00:03.0 0000:00:04.0 "
ckeq "symlink repointed to dv" "$(readlink /vmm/devices/0000:00:02.0)" "../machines/dv/devices/0000:00:02.0"

cp $M/host/devices/0000:00:03.0 $M/dv/devices/ 2>/dev/null; ckfail "cp device rejected" $?

rm $M/dv/devices/0000:00:02.0; ckok "rm unbind device" $?
ckeq "device back in host" "$(ls $M/host/devices | sort | tr '\n' ' ')" "0000:00:02.0 0000:00:03.0 0000:00:04.0 "
ckeq "dv devices empty again" "$(ls $M/dv/devices)" ""
ckeq "symlink back to host" "$(readlink /vmm/devices/0000:00:02.0)" "../machines/host/devices/0000:00:02.0"

rm $M/host/devices/0000:00:03.0 2>/dev/null; ckfail "rm host pool device -> EPERM" $?

mv $M/host/devices/0000:00:04.0 $M/dv/devices/; ckok "rebind 04 to dv" $?
rmdir $M/dv; ckok "rmdir dv with bound device" $?
ckeq "deleted machine returned device" "$(ls $M/host/devices | sort | tr '\n' ' ')" "0000:00:02.0 0000:00:03.0 0000:00:04.0 "

# --- cleanup ---
rmdir $M/vm0; ckok "rmdir vm0" $?
rmdir $M/inc 2>/dev/null; rmdir $M/nx 2>/dev/null; rmdir $M/ml 2>/dev/null
ckeq "machines back to just host" "$(ls $M)" "host"
umount /vmm; ckok "umount" $?
kldunload vmm; ckok "kldunload" $?

echo "VMMFS-TESTS: $pass passed $fail failed"
