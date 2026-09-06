#!/usr/bin/env python3
"""Foreground Alpine loader/serial/backend regression on an isolated mount."""
import argparse
import errno
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import termios
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("root", type=Path)
parser.add_argument("--cpus", type=int, default=1)
parser.add_argument("--reset", action="store_true")
parser.add_argument("--boot", action="store_true")
parser.add_argument("--network", action="store_true")
parser.add_argument("--console", action="store_true")
parser.add_argument("--rng", action="store_true")
parser.add_argument("--bme", action="store_true")
parser.add_argument("--stall-config", action="store_true")
parser.add_argument("--io-reset", action="store_true")
parser.add_argument("--trace-reset", action="store_true")
parser.add_argument("--image", default="/var/tmp/vmmfs-refactor-alpine.img")
args = parser.parse_args()
if args.bme and not args.network:
    parser.error("--bme requires --network")
if args.trace_reset and not args.reset:
    parser.error("--trace-reset requires --reset")
if args.io_reset and not args.reset:
    parser.error("--io-reset requires --reset")
machine = args.root / ("linux-" + str(args.cpus))
project = Path("/home/lileding/projects/dfly-vmm")
log_directory = Path(tempfile.mkdtemp(prefix="vmmfs-linux-", dir="/var/tmp"))
print("LOG_DIRECTORY " + str(log_directory), flush=True)
log = open(log_directory / "guest.log", "wb", buffering=0)
backend_log = open(log_directory / "backend.log", "wb", buffering=0)
# Persist names before entering the kernel paths under test.
os.fsync(log.fileno())
os.fsync(backend_log.fileno())
for directory in (log_directory, log_directory.parent):
    directory_fd = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)
serial = events = console_fd = None
backend = start = None
created = False
tracing = False
received = bytearray()

def store(path, text):
    fd = os.open(path, os.O_WRONLY)
    try:
        data = text.encode()
        assert os.write(fd, data) == len(data)
    finally:
        os.close(fd)

def pump(seconds=0.2):
    ready, _, _ = select.select([serial], [], [], seconds)
    if ready:
        try:
            data = os.read(serial, 65536)
        except BlockingIOError:
            return
        received.extend(data)
        log.write(data)
        os.fsync(log.fileno())

def expect(pattern, seconds=90):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if backend is not None and backend.poll() is not None:
            raise RuntimeError("backend exited with status %d; logs: %s" %
                               (backend.returncode, log_directory))
        match = re.search(pattern, bytes(received))
        if match:
            del received[:match.end()]
            return match
        pump()
    raise RuntimeError("serial timeout waiting for %r; tail=%r" %
                       (pattern, received[-2000:]))

def login():
    expect(rb"login:")
    os.write(serial, b"root\n")
    expect(rb"Password:")
    os.write(serial, b"root@alpine\n")
    expect(rb"alpine:.*#")

def command(text, marker):
    os.write(serial, text.encode() + b"\n")
    return expect(marker, 30)

def check_console(label):
    marker = ("_VMMFS_CONSOLE_" + label + "_" + log_directory.name).encode()
    os.write(serial, b"printf '%s\\n' '" + marker + b"' >/dev/hvc0\n")
    output = bytearray()
    deadline = time.monotonic() + 15
    while marker not in output:
        if time.monotonic() >= deadline:
            raise RuntimeError("virtio-console did not deliver " + label)
        ready, _, _ = select.select([console_fd], [], [], 0.2)
        if ready:
            try:
                data = os.read(console_fd, 65536)
            except BlockingIOError:
                continue
            if not data:
                raise RuntimeError("virtio-console endpoint closed")
            output.extend(data)
    print("PASS virtio-console PTY " + label, flush=True)

def wait_stopped():
    deadline = time.monotonic() + 30
    while not (machine / "stopped").exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("machine did not become stopped")
        time.sleep(0.1)

try:
    print("STAGE create " + str(machine), flush=True)
    machine.mkdir()
    created = True
    store(machine / "mem", "1073741824")
    store(machine / "vcpu", str(args.cpus))
    store(machine / "loader",
          "exec /usr/local/bin/vmmld_linux " +
          str(project / "images/vmlinuz-rootfs-virt") + " initramfs=" +
          str(project / "images/initramfs-rootfs-virt") +
          " root=/dev/vda3 rootfstype=ext4" +
          (" console=hvc0" if args.console else "") + " console=ttyS0")
    fd = os.open(machine / "serial/com1", os.O_CREAT | os.O_RDWR | os.O_NONBLOCK, 0o600)
    serial = fd
    attr = termios.tcgetattr(serial)
    attr[0] = 0
    attr[1] = 0
    attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attr[3] = 0
    attr[4] = attr[5] = termios.B115200
    attr[6][termios.VMIN] = 1
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(serial, termios.TCSANOW, attr)
    slot = machine / "pci/0000:00:01.0"
    slot.mkdir()
    devices = ["virtiod", "--device", str(slot) + ",blk,file=" + args.image]
    if args.network:
        network = machine / "pci/0000:00:02.0"
        network.mkdir()
        devices += ["--device", str(network) + ",net,tap=tap90"]
    if args.console:
        console = machine / "pci/0000:00:03.0"
        console.mkdir()
        devices += ["--device", str(console) + ",console"]
    if args.rng:
        rng = machine / "pci/0000:00:04.0"
        rng.mkdir()
        devices += ["--device", str(rng) + ",rng"]
    backend = subprocess.Popen(devices, stdout=backend_log, stderr=backend_log)
    deadline = time.monotonic() + 20
    while any((entry / "descriptor").stat().st_size == 0
              for entry in (machine / "pci").iterdir()):
        if backend.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError("backend descriptor not registered")
        time.sleep(0.05)
    if args.console:
        deadline = time.monotonic() + 20
        while True:
            endpoint = re.search(rb"virtio-console PTY (/dev/pts/[0-9]+)",
                                 Path(backend_log.name).read_bytes())
            if endpoint:
                break
            if backend.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("virtio-console PTY not published")
            time.sleep(0.05)
        console_fd = os.open(endpoint[1].decode(),
                             os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
        termios.tcsetattr(console_fd, termios.TCSANOW, attr)
    print("STAGE backend descriptors ready", flush=True)
    if args.stall_config:
        while Path(backend_log.name).read_bytes().count(b"config responder ready") < devices.count("--device"):
            if backend.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("config responders not ready")
            time.sleep(0.05)
        backend.send_signal(signal.SIGSTOP)
    if args.boot:
        print("STAGE opening boot", flush=True)
        fd = os.open(machine / "boot", os.O_RDWR)
        print("STAGE boot opened fd=%d" % fd, flush=True)
        try:
            start = subprocess.Popen(
                ["/bin/sh", "-c", "exec 3<&%d; exec " % fd +
                 (machine / "loader").read_text().removeprefix("exec ")],
                pass_fds=(fd,), stdout=log, stderr=log)
        finally:
            os.close(fd)
    else:
        start = subprocess.Popen(["/bin/rm", str(machine / "stopped")],
                                 stdout=log, stderr=log)
    print("STAGE loader started pid=%d" % start.pid, flush=True)
    if args.stall_config:
        machine_id = str(int((machine / "id").read_text().strip()))
        deadline = time.monotonic() + 30
        while True:
            workers = subprocess.check_output(["ps", "ax", "-o", "pid,wchan,comm"], text=True)
            waiting = [line for line in workers.splitlines()
                       if "vmm" + machine_id + "-vcpu" in line and "vmmfspci" in line]
            if waiting:
                print("CONFIRMED config wait: " + waiting[0], flush=True)
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("no vCPU entered config wait")
            pump()
        eventfd = os.open(machine / "events", os.O_RDONLY | os.O_NONBLOCK)
        try:
            store(machine / "events", "reset")
            evidence = b""
            deadline = time.monotonic() + 20
            while b"machine reset completed" not in evidence:
                try:
                    evidence += os.read(eventfd, 65536)
                except BlockingIOError:
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError("reset could not cancel config wait")
                pump()
            print("PASS reset cancelled stalled config", flush=True)
        finally:
            os.close(eventfd)
            backend.send_signal(signal.SIGCONT)
    login()
    print("PASS loader/login cpus=%d" % args.cpus, flush=True)
    assert start.wait(timeout=10) == 0
    command("printf '\\137CPU='; nproc", rb"_CPU=" + str(args.cpus).encode() + rb"\r?\n")
    command("dd if=/dev/zero of=/var/tmp/refactor-test bs=4096 count=16 2>/dev/null; "
            "sync; test $(wc -c </var/tmp/refactor-test) -eq 65536 && "
            "printf '\\137DISK_OK\\n'", rb"_DISK_OK\r?\n")
    print("PASS cpus/block", flush=True)
    if args.console:
        check_console("boot")
    if args.network:
        command("ip link set eth0 up; ip addr add 192.0.2.2/24 dev eth0; "
                "ping -c 3 -W 2 192.0.2.1 >/dev/null && printf '\\137NET_OK\\n'",
                rb"_NET_OK\r?\n")
        print("PASS network", flush=True)
    if args.rng:
        command("test -c /dev/hwrng && dd if=/dev/hwrng of=/var/tmp/rng-test "
                "bs=32 count=1 2>/dev/null && test $(wc -c </var/tmp/rng-test) -eq 32 "
                "&& printf '\\137RNG_OK\\n'", rb"_RNG_OK\r?\n")
        print("PASS rng", flush=True)
    if args.bme:
        config = "/sys/bus/pci/devices/0000:00:02.0/config"
        check = "od -An -tu2 -j4 -N2 " + config + " | tr -d ' \\n'"
        match = command("printf '\\137BME_COMMAND='; " + check + "; echo",
                        rb"_BME_COMMAND=([0-9]+)\r?\n")
        original = int(match[1])
        if not original & 4:
            raise RuntimeError("network bus mastering was not enabled")
        command("dd if=" + config + " of=/dev/null bs=1 count=6 2>/dev/null && "
                "printf '\\137BME_READY\\n'", rb"_BME_READY\r?\n")
        backend.send_signal(signal.SIGSTOP)
        try:
            for value, label in ((original & ~4, "OFF"), (original, "ON")):
                octets = "\\%03o\\%03o" % (value & 255, value >> 8)
                command("printf '" + octets + "' | dd of=" + config +
                        " bs=1 seek=4 conv=notrunc 2>/dev/null && "
                        "test \"$(" + check + ")\" = " + str(value) + " && "
                        "printf '\\137BME_" + label + "\\n'",
                        ("_BME_" + label + "\r?\n").encode())
        finally:
            if backend.poll() is None:
                backend.send_signal(signal.SIGCONT)
        command("ping -c 3 -W 2 192.0.2.1 >/dev/null && "
                "printf '\\137BME_NET_OK\\n'", rb"_BME_NET_OK\r?\n")
        print("PASS BME disable/enable and network after remap", flush=True)
    if args.io_reset:
        command("command -v fio >/dev/null && printf '\\137FIO_OK\\n'",
                rb"_FIO_OK\r?\n")
        command("read before rest </sys/block/vda/stat; "
                "fio --name=reset-read --filename=/dev/vda --readonly "
                "--rw=randread --bs=4k --direct=1 --ioengine=sync "
                "--numjobs=4 --time_based --runtime=60 --group_reporting "
                ">/var/tmp/reset-fio.log 2>&1 & worker=$!; "
                "sleep 2; read after rest </sys/block/vda/stat; "
                "kill -0 $worker && test $after -gt $before && "
                "printf '\\137IO_ACTIVE=%s\\n' $((after-before))",
                rb"_IO_ACTIVE=[1-9][0-9]*\r?\n")
        print("PASS direct-read workload active before reset", flush=True)
    if args.reset:
        print("STAGE request reset", flush=True)
        if args.trace_reset:
            subprocess.run(["ktrace", "-t", "cnis", "-f",
                            str(log_directory / "backend.ktrace"),
                            "-p", str(backend.pid)], check=True)
            tracing = True
            snapshot = subprocess.run(["fstat", "-p", str(backend.pid)],
                                      capture_output=True, text=True, check=True)
            print(snapshot.stdout, flush=True)
        store(machine / "events", "reset")
        login()
        command("printf '\\137RESET_OK\\n'", rb"_RESET_OK\r?\n")
        command("test $(getconf _NPROCESSORS_ONLN) -eq " + str(args.cpus) +
                " && dd if=/dev/vda of=/dev/null bs=4096 count=256 2>/dev/null "
                "&& printf '\\137RESET_DISK_OK\\n'", rb"_RESET_DISK_OK\r?\n")
        if args.network:
            command("ip link set eth0 up; ip addr add 192.0.2.2/24 dev eth0; "
                    "ping -c 3 -W 2 192.0.2.1 >/dev/null && "
                    "printf '\\137RESET_NET_OK\\n'", rb"_RESET_NET_OK\r?\n")
        if args.rng:
            command("test -c /dev/hwrng && dd if=/dev/hwrng of=/var/tmp/reset-rng-test "
                    "bs=32 count=1 2>/dev/null && "
                    "test $(wc -c </var/tmp/reset-rng-test) -eq 32 && "
                    "printf '\\137RESET_RNG_OK\\n'", rb"_RESET_RNG_OK\r?\n")
            print("PASS rng after warm reset", flush=True)
        if args.console:
            check_console("reset")
        print("PASS warm reset, CPUs and device I/O", flush=True)
    print("STAGE guest poweroff", flush=True)
    os.write(serial, b"poweroff\n")
    deadline = time.monotonic() + 45
    while not (machine / "stopped").exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("guest poweroff did not stop machine")
        pump()
    print("PASS guest poweroff", flush=True)
finally:
    print("STAGE cleanup", flush=True)
    if backend is not None:
        print("BACKEND_STATUS %r" % backend.poll(), flush=True)
    if tracing and backend is not None and backend.poll() is None:
        cleared = subprocess.run(["ktrace", "-c", "-p", str(backend.pid)],
                                 capture_output=True, text=True)
        if cleared.returncode != 0:
            print("TRACE_CLEANUP " + cleared.stderr.strip(), flush=True)
    if args.stall_config and backend is not None and backend.poll() is None:
        backend.send_signal(signal.SIGCONT)
    if start is not None and start.poll() is None:
        start.send_signal(2)
        try:
            start.wait(timeout=20)
        except subprocess.TimeoutExpired:
            print("rm still executing; preserved for inspection", flush=True)
    if created and machine.exists() and not (machine / "stopped").exists():
        subprocess.run(["touch", str(machine / "stopped")], timeout=20, check=False)
        wait_stopped()
    if backend is not None and backend.poll() is None:
        backend.terminate()
        try:
            backend.wait(timeout=10)
        except subprocess.TimeoutExpired:
            backend.kill()
            backend.wait()
    if console_fd is not None:
        os.close(console_fd)
    if serial is not None:
        os.close(serial)
    if created and machine.exists():
        eventfd = os.open(machine / "events", os.O_RDONLY | os.O_NONBLOCK)
        try:
            while True:
                try:
                    data = os.read(eventfd, 65536)
                except BlockingIOError:
                    break
                if not data:
                    break
                print(data.decode(errors="replace"), end="", flush=True)
        finally:
            os.close(eventfd)
        machine.rmdir()
        print("PASS rmdir", flush=True)
    log.close()
    backend_log.close()
