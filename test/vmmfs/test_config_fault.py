#!/usr/bin/env python3
"""Exercise config copyout failure and readiness using a tiny real guest."""
import argparse
import ctypes
from contextlib import closing
import errno
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("root", type=Path)
args = parser.parse_args()
source = Path(__file__).resolve().parents[2]
machine = args.root / "config-copyout"
libc = ctypes.CDLL(None, use_errno=True)
libc.read.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
libc.read.restype = ctypes.c_ssize_t
config = events = auth = None
started = False


def store(path, data):
    fd = os.open(path, os.O_WRONLY)
    try:
        if os.write(fd, data) != len(data):
            raise AssertionError("short declaration write")
    finally:
        os.close(fd)


def descriptors():
    result = set()
    for fd in range(256):
        try:
            os.fstat(fd)
            result.add(fd)
        except OSError as error:
            if error.errno != errno.EBADF:
                raise
    return result


helper = r"""
#define main vmmfs_halt_loader_main
#include "LOADER_SOURCE"
#undef main
#include <fcntl.h>

int main(int argc, char **argv)
{
    static const uint8_t code[] = {
        0xba, 0xf8, 0x0c, 0, 0,       /* mov edx, 0xcf8 */
        0xb8, 0x10, 0x08, 0, 0x80,   /* BDF 00:01.0 BAR0 */
        0xef,                        /* out dx, eax */
        0xba, 0xfc, 0x0c, 0, 0,
        0xb8, 0x01, 0xc0, 0, 0,      /* I/O BAR at 0xc000 */
        0xef,
        0xba, 0xf8, 0x0c, 0, 0,
        0xb8, 0x04, 0x08, 0, 0x80,   /* PCI command */
        0xef,
        0xba, 0xfc, 0x0c, 0, 0,
        0x66, 0xb8, 0x01, 0,         /* I/O decode enable */
        0x66, 0xef,
        0xba, 0, 0xc0, 0, 0,
        0xb0, 0x5a, 0xee,            /* synchronous write */
        0xec,                        /* synchronous read */
        0x3c, 0xa5, 0x75, 0x03,      /* require backend value */
        0xf4, 0xeb, 0xfd,            /* success: halt */
        0x0f, 0x0b                   /* failure: #UD */
    };
    struct vmm_cpustate state;
    struct stat status;
    uint8_t *memory;
    int fd;

    if (argc != 2)
        errx(1, "boot path required");
    fd = open(argv[1], O_RDWR);
    if (fd < 0 || fstat(fd, &status) != 0)
        err(1, "open boot");
    memory = mmap(NULL, status.st_size, PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED)
        err(1, "map boot");
    build_guest(memory, status.st_size, 0, 0, 0, 0);
    memcpy(memory + ENTRY_GPA, code, sizeof(code));
    build_cpu_state(&state);
    if (munmap(memory, status.st_size) != 0)
        err(1, "unmap boot");
    if (write(fd, &state, sizeof(state)) != sizeof(state))
        err(1, "submit boot");
    if (close(fd) != 0)
        err(1, "close boot");
    return 0;
}
""".replace("LOADER_SOURCE", str(source / "test/vmm/vmmfs/vmmfs_halt_loader.c"))

try:
    with tempfile.TemporaryDirectory(prefix="vmmfs-config-copyout-") as directory:
        directory = Path(directory)
        code = directory / "loader.c"
        binary = directory / "loader"
        code.write_text(helper)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(source / "sys"), str(code), "-o", str(binary)],
                       check=True)
        machine.mkdir()
        store(machine / "mem", b"67108864")
        store(machine / "vcpu", b"1")
        slot = machine / "pci/0000:00:01.0"
        slot.mkdir()
        before = descriptors()
        store(slot / "descriptor",
              b"version=1\nheader.type=endpoint\nvendor_id=0x1234\n"
              b"device_id=1\nsubsystem_vendor_id=0x1234\nsubsystem_device_id=1\n"
              b"class=0xff0000\nrevision=0\nintx.pin=none\n"
              b"bar0.type=io\nbar0.size=4\nbar0.prefetchable=0\n"
              b"config0.bar=0\nconfig0.offset=0\nconfig0.width=1\nconfig0.space=pio\n")
        added = descriptors() - before
        assert len(added) == 1, added
        auth = added.pop()
        config = os.open(slot / "config", os.O_RDWR | os.O_NONBLOCK)
        events = os.open(machine / "events", os.O_RDONLY | os.O_NONBLOCK)
        with closing(select.kqueue()) as queue:
            change = select.kevent(config, filter=select.KQ_FILTER_READ,
                                   flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR)
            queue.control([change], 0)
            started = True
            subprocess.run([str(binary), str(machine / "boot")], check=True, timeout=20)
            previous = 0
            for operation in (2, 1):
                ready = queue.control(None, 1, 10)
                assert ready and ready[0].ident == config, "initial request not readable"
                ctypes.set_errno(0)
                assert libc.read(config, None, 40) == -1
                assert ctypes.get_errno() == errno.EFAULT
                ready = queue.control(None, 1, 10)
                assert ready and ready[0].ident == config, "EFAULT lost read readiness"
                record = os.read(config, 40)
                generation, sequence, offset, value, bar, space, width, actual, reserved = (
                    struct.unpack("=QQQQHBBB3s", record))
                assert generation and sequence > previous
                assert (offset, bar, space, width, actual, reserved) == (
                    0, 0, 2, 1, operation, b"\0\0\0")
                if operation == 2:
                    assert value & 255 == 0x5a
                response = struct.pack("=QQQII", generation, sequence, 0xa5, 0, 0)
                assert os.write(config, response) == len(response)
                previous = sequence
                print("PASS EFAULT retry, kqueue and response operation=%d" % operation,
                      flush=True)
            evidence = bytearray()
            deadline = time.monotonic() + 10
            while b"machine vcpu halted index=0" not in evidence:
                try:
                    evidence.extend(os.read(events, 65536))
                except BlockingIOError:
                    time.sleep(0.01)
                if time.monotonic() >= deadline:
                    raise RuntimeError("guest did not consume both responses: %r" % evidence)
            print("PASS guest consumed backend read value and halted", flush=True)
finally:
    if machine.exists() and started and not (machine / "stopped").exists():
        fd = os.open(machine / "stopped", os.O_WRONLY | os.O_CREAT, 0o600)
        os.close(fd)
        deadline = time.monotonic() + 10
        while not (machine / "stopped").exists():
            if time.monotonic() >= deadline:
                raise RuntimeError("guest failed to stop; machine preserved")
            time.sleep(0.01)
    for fd in (config, events, auth):
        if fd is not None:
            os.close(fd)
    if machine.exists():
        machine.rmdir()
        print("PASS stop and rmdir", flush=True)
