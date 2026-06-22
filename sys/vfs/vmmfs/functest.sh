#!/bin/sh
# vmmfs functional test (runs inside the vkernel via vkrun2).
# Emits "PASS <name>" / "FAIL <name> ..." per check and a final summary line
# "VMMFS-TESTS: <p> passed <f> failed" that the host runner greps.

pass=0
fail=0
ckok()   { if [ "$2" = "0" ];  then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1 (rc=$2 want 0)"; fail=$((fail+1)); fi; }
ckfail() { if [ "$2" != "0" ]; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1 (rc=0 want nonzero)"; fail=$((fail+1)); fi; }
ckeq()   { if [ "$2" = "$3" ]; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1 (got [$2] want [$3])"; fail=$((fail+1)); fi; }

kldload /xchg/vmmfs.ko; ckok "kldload" $?
ln -sf /sbin/mount_std /sbin/mount_vmmfs
mkdir -p /vmm
mount -t vmmfs vmm /vmm; ckok "mount" $?
ckeq "machines empty" "$(ls /vmm/machines)" ""

# valid config (deterministic loader: 10 bytes, mode 0755)
mkdir -p /tmp/cfg; echo 4 > /tmp/cfg/vcpu; echo 512M > /tmp/cfg/mem
printf '#!/bin/sh\n' > /tmp/cfg/loader; chmod 755 /tmp/cfg/loader

# --- import / presentation (M2) ---
ln -s /tmp/cfg /vmm/machines/vm0; ckok "import vm0" $?
ckeq "ls vm0" "$(ls /vmm/machines/vm0 | sort | tr '\n' ' ')" "loader mem vcpu "
ckeq "cat vcpu" "$(cat /vmm/machines/vm0/vcpu)" "4"
ckeq "cat mem" "$(cat /vmm/machines/vm0/mem)" "536870912"
ckeq "cat loader" "$(cat /vmm/machines/vm0/loader)" "10 0755"
ckeq "machines lists vm0" "$(ls /vmm/machines)" "vm0"
ls /vmm/machines/nope >/dev/null 2>&1; ckfail "lookup nonexistent" $?

# --- import validation (M2) ---
mkdir -p /tmp/b1; echo 0 > /tmp/b1/vcpu; echo 2M > /tmp/b1/mem; cp /tmp/cfg/loader /tmp/b1/loader
ln -s /tmp/b1 /vmm/machines/b1 2>/dev/null; ckfail "reject vcpu=0" $?
mkdir -p /tmp/b2; echo 2 > /tmp/b2/vcpu; echo 512Q > /tmp/b2/mem; cp /tmp/cfg/loader /tmp/b2/loader
ln -s /tmp/b2 /vmm/machines/b2 2>/dev/null; ckfail "reject bad mem" $?
mkdir -p /tmp/b3; echo 2 > /tmp/b3/vcpu; echo 2M > /tmp/b3/mem
ln -s /tmp/b3 /vmm/machines/b3 2>/dev/null; ckfail "reject missing loader" $?
mkdir -p /tmp/b4; echo 2 > /tmp/b4/vcpu; echo 2M > /tmp/b4/mem; echo x > /tmp/b4/loader; chmod 644 /tmp/b4/loader
ln -s /tmp/b4 /vmm/machines/b4 2>/dev/null; ckfail "reject non-exec loader" $?
ln -s /tmp/nope /vmm/machines/bx 2>/dev/null; ckfail "reject missing config-dir" $?
ckeq "rejects left no machines" "$(ls /vmm/machines)" "vm0"

# --- lifecycle / stopped control (M3a) ---
echo apic > /vmm/machines/vm0/stopped; ckok "stop apic" $?
ckeq "vm0 has stopped" "$(ls /vmm/machines/vm0 | sort | tr '\n' ' ')" "loader mem stopped vcpu "
ckeq "cat stopped empty" "$(cat /vmm/machines/vm0/stopped)" ""
echo apic > /vmm/machines/vm0/stopped; ckok "re-stop idempotent" $?
rm /vmm/machines/vm0/stopped; ckok "start (rm stopped)" $?
ckeq "vm0 no stopped" "$(ls /vmm/machines/vm0 | sort | tr '\n' ' ')" "loader mem vcpu "
rm /vmm/machines/vm0/stopped 2>/dev/null; ckfail "rm stopped while running -> ENOENT" $?
echo force > /vmm/machines/vm0/stopped; ckok "stop force" $?

# --- read-only config protection ---
echo 9 > /vmm/machines/vm0/vcpu 2>/dev/null; ckfail "write vcpu -> EPERM" $?
rm /vmm/machines/vm0/vcpu 2>/dev/null; ckfail "rm vcpu -> EPERM" $?

# --- import stopped, then remove ---
mkdir -p /tmp/cfg2; echo 8 > /tmp/cfg2/vcpu; echo 1G > /tmp/cfg2/mem
cp /tmp/cfg/loader /tmp/cfg2/loader; touch /tmp/cfg2/stopped
ln -s /tmp/cfg2 /vmm/machines/vm1; ckok "import stopped vm1" $?
ckeq "vm1 has stopped" "$(ls /vmm/machines/vm1 | sort | tr '\n' ' ')" "loader mem stopped vcpu "
rmdir /vmm/machines/vm0; ckok "rmdir vm0" $?
rmdir /vmm/machines/vm1; ckok "rmdir vm1" $?
ckeq "machines empty again" "$(ls /vmm/machines)" ""

umount /vmm; ckok "umount" $?
kldunload vmmfs; ckok "kldunload" $?

echo "VMMFS-TESTS: $pass passed $fail failed"
