#!/usr/bin/env python3
"""EFI/DragonFly ABI regression using existing fd3 loaders and isolated images."""
import argparse
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import tempfile
import termios
import time

parser = argparse.ArgumentParser()
parser.add_argument("root", type=Path)
parser.add_argument("guest", choices=("efi", "dragonfly"))
parser.add_argument("--named-boot", action="store_true")
parser.add_argument("--trace-backend", action="store_true")
args = parser.parse_args()
project = Path("/home/lileding/projects/dfly-vmm")
machine = args.root / ("compat-" + args.guest)
logs = Path(tempfile.mkdtemp(prefix="vmmfs-compat-", dir="/var/tmp"))
print("LOG_DIRECTORY " + str(logs), flush=True)
guest_log = open(logs / "guest.log", "wb", buffering=0)
backend_log = open(logs / "backend.log", "wb", buffering=0)
serial = None
backend = start = None
created = False
received = bytearray()

def store(path, value):
    fd = os.open(path, os.O_WRONLY)
    try:
        data = value.encode()
        assert os.write(fd, data) == len(data)
    finally:
        os.close(fd)

def expect(pattern, timeout=180):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = re.search(pattern, received)
        if found:
            del received[:found.end()]
            return
        ready, _, _ = select.select([serial], [], [], 0.2)
        if ready:
            try:
                data = os.read(serial, 65536)
            except BlockingIOError:
                continue
            guest_log.write(data)
            os.fsync(guest_log.fileno())
            received.extend(data)
        if b"No bootable option or device was found" in received:
            raise RuntimeError("firmware found no bootable device")
        if backend.poll() is not None:
            raise RuntimeError("backend exited: %d; tail=%r" %
                               (backend.returncode, received[-3000:]))
        if (machine / "stopped").exists():
            raise RuntimeError("guest stopped; tail=%r" % received[-3000:])
    raise RuntimeError("boot timeout; tail=%r" % received[-5000:])

try:
    machine.mkdir()
    created = True
    store(machine / "vcpu", "4")
    store(machine / "mem", "1073741824")
    if args.guest == "efi":
        loader = [str(project / "vmutils/target/debug/vmmld_efi"),
                  str(project / "images/CLOUDHV.fd")]
        disk = "/var/tmp/vmmfs-refactor-alpine.img"
        banner = rb"Linux version"
    else:
        kernel = "/usr/obj/home/lileding/projects/dfly-vmm/DragonFlyBSD/sys/X86_64_VIRTIO_KMOD_TEST/kernel.stripped"
        loader = [str(project / "vmutils/target/debug/vmmld_dragonfly"), kernel]
        modules = ("virtio/virtio.ko", "pci/virtio_pci.ko", "mmio/virtio_mmio.ko",
                   "block/virtio_blk.ko", "net/if_vtnet.ko", "random/virtio_random.ko")
        loader += ["module=" + str(project / "DragonFlyBSD/sys/dev/virtual/virtio" / item)
                   for item in modules]
        # DragonFly virtio_blk exports vbd, not FreeBSD vtbd.
        loader += ["vfs.root.mountfrom=ufs:vbd0s1a"]
        disk = "/var/tmp/vmmfs-compat-dfly.img"
        banner = rb"DragonFly v"
    store(machine / "loader", "exec " + shlex.join(loader))
    serial = os.open(machine / "serial/com1", os.O_CREAT | os.O_RDWR | os.O_NONBLOCK, 0o600)
    attributes = termios.tcgetattr(serial)
    attributes[0] = attributes[1] = attributes[3] = 0
    attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attributes[4] = attributes[5] = termios.B115200
    attributes[6][termios.VMIN] = 1
    attributes[6][termios.VTIME] = 0
    termios.tcsetattr(serial, termios.TCSANOW, attributes)
    specs = [("0000:00:01.0", "blk,file=" + disk),
             ("0000:00:02.0", "net,tap=tap90")]
    if args.guest == "efi":
        specs += [("0000:00:04.0", "blk,file=" +
                   str(project / "images/alpine-virt-3.24.0-x86_64.iso") + ",readonly")]
    command = ["virtiod"]
    for bdf, spec in specs:
        slot = machine / "pci" / bdf
        slot.mkdir()
        command += ["--device", str(slot) + "," + spec]
    if args.trace_backend:
        trace = logs / "backend.ktrace"
        command = ["ktrace", "-i", "-t", "ci", "-f", str(trace)] + command
        print("BACKEND_TRACE " + str(trace), flush=True)
    backend = subprocess.Popen(command, stdout=backend_log, stderr=backend_log)
    deadline = time.monotonic() + 20
    while any((machine / "pci" / bdf / "descriptor").stat().st_size == 0 for bdf, _ in specs):
        if backend.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError("backend did not register descriptors")
        time.sleep(0.05)
    if args.named_boot:
        fd = os.open(machine / "boot", os.O_RDWR)
        try:
            start = subprocess.Popen(["/bin/sh", "-c",
                "exec 3<&%d; exec " % fd + shlex.join(loader)],
                pass_fds=(fd,), stdout=guest_log, stderr=guest_log)
        finally:
            os.close(fd)
    else:
        start = subprocess.Popen(["rm", str(machine / "stopped")],
                                 stdout=guest_log, stderr=guest_log)
    print("STAGE " + args.guest + " loading", flush=True)
    # A successful fd3 write must have consumed stopped before waiting for boot.
    deadline = time.monotonic() + 30
    while (machine / "stopped").exists():
        if start.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError("loader did not submit a running guest")
        time.sleep(0.05)
    expect(banner)
    expect(rb"login:")
    assert start.wait(timeout=10) == 0
    print("PASS " + args.guest + " reached login", flush=True)
    store(machine / "events", "reset")
    expect(banner)
    expect(rb"login:")
    print("PASS " + args.guest + " warm reset reached login", flush=True)
finally:
    print("STAGE cleanup", flush=True)
    if created and machine.exists() and not (machine / "stopped").exists():
        subprocess.run(["touch", str(machine / "stopped")], check=True, timeout=20)
        deadline = time.monotonic() + 30
        while not (machine / "stopped").exists():
            if time.monotonic() > deadline:
                raise RuntimeError("stop incomplete; retained machine for inspection")
            time.sleep(0.1)
    if start is not None and start.poll() is None:
        start.send_signal(2)
        start.wait(timeout=20)
    if backend is not None:
        print("BACKEND_STATUS %r" % backend.poll(), flush=True)
        if backend.poll() is None:
            backend.terminate()
            try:
                backend.wait(timeout=10)
            except subprocess.TimeoutExpired:
                backend.kill()
                backend.wait(timeout=10)
    if serial is not None:
        os.close(serial)
    if created and machine.exists():
        fd = os.open(machine / "events", os.O_RDONLY | os.O_NONBLOCK)
        try:
            while True:
                try:
                    data = os.read(fd, 65536)
                except BlockingIOError:
                    break
                if not data:
                    break
                print(data.decode(errors="replace"), end="", flush=True)
        finally:
            os.close(fd)
        machine.rmdir()
        print("PASS stop/rmdir", flush=True)
    guest_log.close()
    backend_log.close()
