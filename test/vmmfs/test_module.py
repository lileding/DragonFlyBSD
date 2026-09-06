#!/usr/bin/env python3
"""Verify detached PCI auth files retain the VMMFS module, including SCM_RIGHTS."""
import array
import ctypes
import errno
import os
from pathlib import Path
import socket
import subprocess
import sys

root = Path(sys.argv[1])
machine = root / "auth-lifetime"
libc = ctypes.CDLL(None, use_errno=True)
libc.kldfind.argtypes = [ctypes.c_char_p]
libc.kldunload.argtypes = [ctypes.c_int]
auth = None
sender = receiver = None
mounted = True


def unload(expected):
    module = libc.kldfind(b"vmmfs")
    assert module > 0
    result = libc.kldunload(module)
    error = ctypes.get_errno() if result != 0 else 0
    assert error == expected, (result, error, expected)
    print("PASS kldunload errno=%d" % error, flush=True)


def descriptors():
    opened = set()
    for fd in range(256):
        try:
            os.fstat(fd)
            opened.add(fd)
        except OSError as error:
            if error.errno != errno.EBADF:
                raise
    return opened


try:
    machine.mkdir()
    slot = machine / "pci/0000:00:01.0"
    slot.mkdir()
    before = descriptors()
    fd = os.open(slot / "descriptor", os.O_WRONLY)
    try:
        value = (b"version=1\nheader.type=endpoint\nvendor_id=0x1234\n"
                 b"device_id=1\nsubsystem_vendor_id=0x1234\n"
                 b"subsystem_device_id=1\nclass=0xff0000\nrevision=0\nintx.pin=none\n")
        assert os.write(fd, value) == len(value)
    finally:
        os.close(fd)
    injected = descriptors() - before
    assert len(injected) == 1, injected
    auth = injected.pop()
    assert os.fstat(auth).st_ino == slot.stat().st_ino
    sender, receiver = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sender.sendmsg([b"a"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [auth]))])
    os.close(auth)
    auth = None
    machine.rmdir()
    subprocess.run(["umount", str(root)], check=True)
    mounted = False
    unload(errno.EBUSY)
    data, ancillary, flags, address = receiver.recvmsg(1, socket.CMSG_SPACE(4))
    assert data == b"a" and flags == 0
    rights = array.array("i")
    for level, kind, payload in ancillary:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            rights.frombytes(payload)
    assert len(rights) == 1
    auth = rights[0]
    os.fstat(auth)
    unload(errno.EBUSY)
    os.close(auth)
    auth = None
    sender.close()
    receiver.close()
    unload(0)
finally:
    if auth is not None:
        os.close(auth)
    if sender is not None:
        sender.close()
        receiver.close()
    if machine.exists():
        machine.rmdir()
    if mounted:
        subprocess.run(["umount", str(root)], check=True)
