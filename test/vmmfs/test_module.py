#!/usr/bin/env python3
"""Verify detached PCI auth files retain the VMMFS module, including SCM_RIGHTS."""
from pci_descriptor import descriptor
import argparse
import array
from concurrent.futures import ThreadPoolExecutor
import ctypes
import errno
import os
from pathlib import Path
import socket
import subprocess
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument("root", type=Path)
parser.add_argument("--race", action="store_true")
args = parser.parse_args()
root = args.root
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



def unload_until_closed(module, ready, closing):
    attempts = 0
    deadline = time.monotonic() + 10
    try:
        while time.monotonic() < deadline:
            result = libc.kldunload(module)
            error = ctypes.get_errno() if result != 0 else 0
            if error == 0:
                assert closing.is_set(), "unloaded while the auth file was held"
                return attempts
            if error != errno.EBUSY:
                raise OSError(error, "concurrent kldunload")
            attempts += 1
            ready.set()
        raise TimeoutError("module still busy after the close/unload race")
    finally:
        ready.set()


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
        value = (descriptor())
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
    if args.race:
        module = libc.kldfind(b"vmmfs")
        assert module > 0
        ready, closing = threading.Event(), threading.Event()
        with ThreadPoolExecutor(max_workers=1) as executor:
            result = executor.submit(unload_until_closed, module, ready, closing)
            try:
                if not ready.wait(5):
                    raise TimeoutError("unload worker did not start")
                if result.done():
                    result.result()
                    raise AssertionError("unload completed before close")
                closing.set()
            finally:
                fd, auth = auth, None
                os.close(fd)
            attempts = result.result(timeout=15)
        assert libc.kldfind(b"vmmfs") == -1
        print("PASS concurrent close/unload busy_attempts=%d" % attempts, flush=True)
    else:
        os.close(auth)
        auth = None
        unload(0)
    sender.close()
    receiver.close()
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
