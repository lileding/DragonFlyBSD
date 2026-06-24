#!/bin/sh
# vmmfs functional test (runs inside the vkernel via vkrun2).
#
# Declarative model: a machine is `mkdir`-ed (always stopped, empty config);
# vcpu/mem/loader are PCIe-like registers (write into a buffer, commit on
# close, read back to confirm); `rm stopped` only declares desired running and
# queues a kernel worker.
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
wait_file() { i=0; while [ $i -lt 10 ]; do [ -f "$1" ] && return 0; i=$((i+1)); sleep 1; done; return 1; }
wait_event() { i=0; while [ $i -lt 10 ]; do cat "$1/events" | grep -q "^$2$" && return 0; i=$((i+1)); sleep 1; done; return 1; }

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

# loader fixtures: a real fd3/fd4 dummy loader, a non-exec file
cat > /tmp/vmmld_dummy.c <<'EOF_DUMMY'
#include <sys/mman.h>
#include <sys/stat.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define MAGIC "VMMLD0\0\0"
#define REC_X64 1
#define REC_GPA 2
#define REC_NOTE 3
#define MAND 1
#define PGSZ 4096ULL
#define MEM_MIN (2ULL * 1024 * 1024)
#define PML4 0x1000ULL
#define PDPT 0x2000ULL
#define PD 0x3000ULL
#define GDT 0x5000ULL
#define TSS 0x6000ULL
#define ENTRY 0x100000ULL
#define STACK 0x180000ULL
#define CR0_PE 1ULL
#define CR0_NE 0x20ULL
#define CR0_PG 0x80000000ULL
#define CR4_PAE 0x20ULL
#define EFER_LME 0x100ULL
#define EFER_LMA 0x400ULL
#define XCR0_X87 1ULL
#define SEG_S 0x10
#define SEG_P 0x80
#define SEG_L 0x200
#define SEG_DB 0x400
#define SEG_G 0x800
#define SEG_UNUSABLE 0x1000

struct hdr { char magic[8]; uint16_t abi, arch; uint32_t hsz, total, nrec; uint64_t memsz; uint32_t flags, reserved; } __attribute__((packed));
struct rec { uint16_t type, flags; uint32_t size; } __attribute__((packed));
struct seg { uint16_t sel, attr; uint32_t limit; uint64_t base; } __attribute__((packed));
struct vcpu { uint32_t id, flags; uint64_t runnable, gpr[18], cr[6], msr[11]; struct seg seg[10]; uint64_t intr; } __attribute__((packed));
struct range { uint64_t start, size; uint32_t type, flags; } __attribute__((packed));

static size_t a8(size_t v) { return (v + 7) & ~(size_t)7; }
static void w64(void *m, uint64_t o, uint64_t v) { memcpy((uint8_t *)m + o, &v, 8); }
static void seg(struct seg *s, uint16_t sel, uint16_t attr, uint32_t limit, uint64_t base) { s->sel = sel; s->attr = attr; s->limit = limit; s->base = base; }
static void probe(void *mem, uint8_t *man) {
	int fd = open("/tmp/vmmld_dummy.probe", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) err(1, "probe open");
	if (write(fd, (uint8_t *)mem + ENTRY, 4) != 4) err(1, "probe mem");
	if (write(fd, man, 8) != 8) err(1, "probe manifest");
	if (close(fd) != 0) err(1, "probe close");
}
static uint8_t *add(uint8_t *p, uint16_t type, uint16_t flags, const void *payload, uint32_t size) {
	struct rec r = { type, flags, size };
	size_t total = a8(sizeof(r) + size);
	memcpy(p, &r, sizeof(r));
	memcpy(p + sizeof(r), payload, size);
	memset(p + sizeof(r) + size, 0, total - sizeof(r) - size);
	return p + total;
}

int main(void) {
	struct stat ms, xs;
	void *mem;
	uint8_t *man, *p;
	struct hdr h;
	struct vcpu v;
	struct range r[4];
	const char note[] = "dummy nop loader";
	if (fstat(3, &ms) || fstat(4, &xs)) err(1, "fstat");
	if (ms.st_size < (off_t)MEM_MIN || xs.st_size < 4096) return 2;
	mem = mmap(NULL, (size_t)ms.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0);
	man = mmap(NULL, (size_t)xs.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
	if (mem == MAP_FAILED || man == MAP_FAILED) err(1, "mmap");
	memset((uint8_t *)mem + PML4, 0, PGSZ * 3);
	w64(mem, PML4, PDPT | 3); w64(mem, PDPT, PD | 3); w64(mem, PD, 0x83);
	memset((uint8_t *)mem + GDT, 0, PGSZ);
	w64(mem, GDT + 8, 0x00209a0000000000ULL); w64(mem, GDT + 16, 0x0000920000000000ULL);
	w64(mem, GDT + 24, 0x0000890060000067ULL); w64(mem, GDT + 32, 0);
	memset((uint8_t *)mem + TSS, 0, 0x68);
	memcpy((uint8_t *)mem + ENTRY, "\x90\x90\x90\xf4", 4);
	memset(&v, 0, sizeof(v));
	v.runnable = 1; v.gpr[4] = STACK; v.gpr[16] = ENTRY; v.gpr[17] = 2;
	v.cr[0] = CR0_PE | CR0_NE | CR0_PG; v.cr[3] = PML4; v.cr[4] = CR4_PAE; v.cr[5] = XCR0_X87;
	v.msr[0] = EFER_LME | EFER_LMA; v.msr[9] = 0x0007040600070406ULL;
	seg(&v.seg[0], 0, SEG_UNUSABLE, 0, 0);
	seg(&v.seg[1], 0x08, 0xb | SEG_S | SEG_P | SEG_L | SEG_G, 0xffffffffU, 0);
	seg(&v.seg[2], 0x10, 0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	seg(&v.seg[3], 0x10, 0x3 | SEG_S | SEG_P | SEG_DB | SEG_G, 0xffffffffU, 0);
	seg(&v.seg[4], 0, SEG_UNUSABLE, 0, 0); seg(&v.seg[5], 0, SEG_UNUSABLE, 0, 0);
	seg(&v.seg[6], 0, 0, 39, GDT); seg(&v.seg[7], 0, 0, 0, 0);
	seg(&v.seg[8], 0, SEG_UNUSABLE, 0, 0); seg(&v.seg[9], 0x18, 0x9 | SEG_P, 0x67, TSS);
	r[0] = (struct range){ ENTRY, 4, 1, 0 };
	r[1] = (struct range){ PML4, PGSZ * 3, 5, 0 };
	r[2] = (struct range){ GDT, PGSZ, 6, 0 };
	r[3] = (struct range){ STACK - PGSZ, PGSZ, 7, 0 };
	memset(man, 0, (size_t)xs.st_size);
	p = man + sizeof(h);
	p = add(p, REC_X64, MAND, &v, sizeof(v));
	p = add(p, REC_GPA, MAND, r, sizeof(r));
	p = add(p, REC_NOTE, 0, note, sizeof(note));
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, MAGIC, 8); h.arch = 1; h.hsz = sizeof(h); h.total = (uint32_t)(p - man); h.nrec = 3; h.memsz = (uint64_t)ms.st_size;
	memcpy(man, &h, sizeof(h));
	probe(mem, man);
	return 0;
}
EOF_DUMMY
cc -Wall -Wextra -Werror -std=c11 -O2 /tmp/vmmld_dummy.c -o /tmp/vmmld_dummy
printf 'x' > /tmp/noexec; chmod 644 /tmp/noexec
printf '#!/bin/sh\nexit 0\n' > /tmp/no_manifest; chmod 755 /tmp/no_manifest
printf '#!/bin/sh\necho start > /tmp/vmmld_sleep.probe\nsleep 30\necho done >> /tmp/vmmld_sleep.probe\n' > /tmp/vmmld_sleep; chmod 755 /tmp/vmmld_sleep
printf '#!/bin/sh\necho start > /tmp/vmmld_hang.probe\nwhile :; do sleep 1; done\n' > /tmp/vmmld_hang; chmod 755 /tmp/vmmld_hang
printf '#!/bin/sh\necho start >> /tmp/vmmld_flap.probe\nwhile :; do sleep 1; done\n' > /tmp/vmmld_flap; chmod 755 /tmp/vmmld_flap
rm -f /tmp/vmmld_dummy.probe /tmp/vmmld_sleep.probe /tmp/vmmld_hang.probe /tmp/vmmld_flap.probe

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
echo /tmp/vmmld_dummy > $M/vm0/loader; ckeq "loader readback" "$(cat $M/vm0/loader)" "/tmp/vmmld_dummy"

# --- an invalid write succeeds but does not update; read-back is the truth ---
echo 0 > $M/vm0/vcpu;       ckeq "invalid vcpu kept old" "$(cat $M/vm0/vcpu)" "4"
echo 3M > $M/vm0/mem;       ckeq "unaligned mem kept old" "$(cat $M/vm0/mem)" "536870912"
echo 8 > $M/vm0/vcpu;       ckeq "rewrite vcpu" "$(cat $M/vm0/vcpu)" "8"

# --- start request: rm stopped is declarative and returns before loader runs ---
mkdir $M/inc; echo 2 > $M/inc/vcpu
rm $M/inc/stopped 2>/dev/null; ckfail "rm stopped incomplete -> fail" $?
ckeq "inc still stopped" "$(ls $M/inc | grep -c '^stopped$')" "1"

mkdir $M/nx; echo 1 > $M/nx/vcpu; echo 2M > $M/nx/mem; echo /tmp/noexec > $M/nx/loader
cat $M/nx/events >/dev/null
rm $M/nx/stopped; ckok "rm stopped non-exec is declarative" $?
ckeq "nx desired running" "$(ls $M/nx | grep -c '^stopped$')" "0"
sleep 1
ckeq "nx no started event" "$(cat $M/nx/events | grep -c '^started$')" "0"
echo force > $M/nx/stopped

mkdir $M/ml; echo 1 > $M/ml/vcpu; echo 2M > $M/ml/mem; echo /tmp/nope > $M/ml/loader
cat $M/ml/events >/dev/null
rm $M/ml/stopped; ckok "rm stopped missing loader is declarative" $?
sleep 1
ckeq "ml no started event" "$(cat $M/ml/events | grep -c '^started$')" "0"
echo force > $M/ml/stopped

mkdir $M/badld; echo 1 > $M/badld/vcpu; echo 2M > $M/badld/mem; echo /tmp/no_manifest > $M/badld/loader
cat $M/badld/events >/dev/null
rm $M/badld/stopped; ckok "rm stopped no manifest is declarative" $?
sleep 1
ckeq "badld no started event" "$(cat $M/badld/events | grep -c '^started$')" "0"
echo force > $M/badld/stopped

cat $M/vm0/events >/dev/null
rm $M/vm0/stopped; ckok "start vm0 (valid)" $?
ckeq "rm returns with desired running" "$(ls $M/vm0 | grep -c '^stopped$')" "0"
wait_file /tmp/vmmld_dummy.probe; ckok "vm0 worker produced probe" $?
ckeq "vm0 dummy loader probe" "$(hexdump -v -e '1/1 "%02x"' /tmp/vmmld_dummy.probe)" "909090f4564d4d4c44300000"
wait_event $M/vm0 started; ckok "vm0 started event after worker" $?
ckeq "vm0 running no stopped" "$(ls $M/vm0 | sort | tr '\n' ' ')" "console devices events lease loader mem status.tar.gz vcpu "
rm $M/vm0/stopped 2>/dev/null; ckfail "rm stopped while running -> ENOENT" $?
echo 2 > $M/vm0/vcpu
ckeq "running vcpu write kept old" "$(cat $M/vm0/vcpu)" "8"
echo 2M > $M/vm0/mem
ckeq "running mem write kept old" "$(cat $M/vm0/mem)" "536870912"
echo /tmp/no_manifest > $M/vm0/loader
ckeq "running loader write kept old" "$(cat $M/vm0/loader)" "/tmp/vmmld_dummy"

# --- a stop request can cancel a loader that is still executing ---
mkdir $M/cancel; echo 1 > $M/cancel/vcpu; echo 2M > $M/cancel/mem; echo /tmp/vmmld_sleep > $M/cancel/loader
cat $M/cancel/events >/dev/null
rm $M/cancel/stopped; ckok "cancel start request" $?
wait_file /tmp/vmmld_sleep.probe; ckok "sleep loader entered" $?
echo force > $M/cancel/stopped; ckok "stop cancels worker" $?
sleep 2
ckeq "sleep loader killed before done" "$(grep -c '^done$' /tmp/vmmld_sleep.probe 2>/dev/null)" "0"
ckeq "cancel desired stopped" "$(ls $M/cancel | grep -c '^stopped$')" "1"
rmdir $M/cancel; ckok "rmdir cancel" $?

# --- a loader that never exits must be externally cancellable ---
mkdir $M/hang; echo 1 > $M/hang/vcpu; echo 2M > $M/hang/mem; echo /tmp/vmmld_hang > $M/hang/loader
cat $M/hang/events >/dev/null
rm $M/hang/stopped; ckok "hang start request" $?
wait_file /tmp/vmmld_hang.probe; ckok "hang loader entered" $?
sleep 1
ckeq "hang no started while stuck" "$(cat $M/hang/events | grep -c '^started$')" "0"
ckeq "hang desired running while stuck" "$(ls $M/hang | grep -c '^stopped$')" "0"
echo force > $M/hang/stopped; ckok "hang stop cancels loader" $?
sleep 2
ckeq "hang no started after cancel" "$(cat $M/hang/events | grep -c '^started$')" "0"
ckeq "hang desired stopped after cancel" "$(ls $M/hang | grep -c '^stopped$')" "1"
rmdir $M/hang; ckok "rmdir hang" $?

# --- frequent rm/touch ending stopped: last desired state wins ---
mkdir $M/flapstop; echo 1 > $M/flapstop/vcpu; echo 2M > $M/flapstop/mem; echo /tmp/vmmld_flap > $M/flapstop/loader
cat $M/flapstop/events >/dev/null
i=0
while [ $i -lt 12 ]; do
	rm $M/flapstop/stopped 2>/dev/null
	echo force > $M/flapstop/stopped 2>/dev/null
	i=$((i+1))
done
sleep 2
ckeq "flap stopped desired stopped" "$(ls $M/flapstop | grep -c '^stopped$')" "1"
ckeq "flap stopped never started" "$(cat $M/flapstop/events | grep -c '^started$')" "0"
rmdir $M/flapstop; ckok "rmdir flapstop" $?

# --- frequent rm/touch ending running: last desired state wins ---
rm -f /tmp/vmmld_dummy.probe
mkdir $M/flaprun; echo 1 > $M/flaprun/vcpu; echo 2M > $M/flaprun/mem; echo /tmp/vmmld_dummy > $M/flaprun/loader
cat $M/flaprun/events >/dev/null
i=0
while [ $i -lt 8 ]; do
	rm $M/flaprun/stopped 2>/dev/null
	echo force > $M/flaprun/stopped 2>/dev/null
	i=$((i+1))
done
cat $M/flaprun/events >/dev/null
rm $M/flaprun/stopped; ckok "flap running final start request" $?
wait_file /tmp/vmmld_dummy.probe; ckok "flap running worker produced probe" $?
wait_event $M/flaprun started; ckok "flap running started event" $?
ckeq "flap running no stopped" "$(ls $M/flaprun | grep -c '^stopped$')" "0"
echo force > $M/flaprun/stopped; ckok "flap running stop" $?
wait_event $M/flaprun stopped; ckok "flap running stopped event" $?
rmdir $M/flaprun; ckok "rmdir flaprun" $?

# --- stop (echo apic|force > stopped), idempotent ---
echo apic > $M/vm0/stopped; ckok "stop apic" $?
ckeq "vm0 has stopped" "$(ls $M/vm0 | grep -c '^stopped$')" "1"
wait_event $M/vm0 stopped; ckok "vm0 stopped event" $?
echo force > $M/vm0/stopped; ckok "re-stop force idempotent" $?

# --- read-only files reject writes / removal ---
echo x > $M/vm0/events 2>/dev/null;        ckfail "write events -> fail" $?
echo x > $M/vm0/status.tar.gz 2>/dev/null; ckfail "write status -> fail" $?
rm $M/vm0/vcpu 2>/dev/null;                ckfail "rm vcpu -> EPERM" $?

# --- events: created+stopped on mkdir, one-shot, lifecycle ---
mkdir $M/ev
ckeq "events on create" "$(cat $M/ev/events | tr '\n' ',')" "created,stopped,"
ckeq "events one-shot drained" "$(cat $M/ev/events)" ""
echo 1 > $M/ev/vcpu; echo 2M > $M/ev/mem; echo /tmp/vmmld_dummy > $M/ev/loader
rm $M/ev/stopped; wait_event $M/ev started; ckok "event on start" $?
echo apic > $M/ev/stopped; wait_event $M/ev stopped; ckok "event on stop" $?
echo apic > $M/ev/stopped; ckeq "idempotent stop -> no event" "$(cat $M/ev/events)" ""
rmdir $M/ev; ckok "rmdir ev" $?

# --- rmdir requires stopped (M3b) ---
mkdir $M/rr; echo 1 > $M/rr/vcpu; echo 2M > $M/rr/mem; echo /tmp/vmmld_dummy > $M/rr/loader
rm $M/rr/stopped; ckok "start rr" $?
wait_event $M/rr started; ckok "rr started event" $?
rmdir $M/rr 2>/dev/null; ckfail "rmdir running -> EBUSY" $?
echo apic > $M/rr/stopped
wait_event $M/rr stopped; ckok "rr stopped event" $?
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

mkdir $M/dv; echo 1 > $M/dv/vcpu; echo 2M > $M/dv/mem; echo /tmp/vmmld_dummy > $M/dv/loader
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
rmdir $M/inc 2>/dev/null; rmdir $M/nx 2>/dev/null; rmdir $M/ml 2>/dev/null; rmdir $M/badld 2>/dev/null
ckeq "machines back to just host" "$(ls $M)" "host"
umount /vmm; ckok "umount" $?
kldunload vmm; ckok "kldunload" $?

echo "VMMFS-TESTS: $pass passed $fail failed"
