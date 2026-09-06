#!/usr/bin/env python3
"""Race real Linux fd3 submission with SIGINT to synchronous rm stopped."""
import argparse
import errno
import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("root", type=Path)
args = parser.parse_args()
project = Path("/home/lileding/projects/dfly-vmm")
loader = project / "vmutils/target/debug/vmmld_linux"
kernel = project / "images/vmlinuz-rootfs-virt"
initrd = project / "images/initramfs-rootfs-virt"
for path in (loader, kernel, initrd):
    if not path.is_file():
        raise RuntimeError("missing test input: " + str(path))
logs = Path(tempfile.mkdtemp(prefix="vmmfs-signal-", dir="/var/tmp"))
print("LOG_DIRECTORY " + str(logs), flush=True)


def store(path, text):
    fd = os.open(path, os.O_WRONLY)
    try:
        data = text.encode()
        if os.write(fd, data) != len(data):
            raise RuntimeError("short control write")
    finally:
        os.close(fd)


def events(machine):
    fd = os.open(machine / "events", os.O_RDONLY | os.O_NONBLOCK)
    result = bytearray()
    try:
        while True:
            try:
                data = os.read(fd, 65536)
            except BlockingIOError:
                break
            if not data:
                break
            result.extend(data)
    finally:
        os.close(fd)
    return bytes(result)


def stop(machine):
    subprocess.run(["touch", str(machine / "stopped")],
                   check=True, timeout=20)
    deadline = time.monotonic() + 30
    while not (machine / "stopped").exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("stop did not complete; retained " + str(machine))
        time.sleep(0.01)


outcomes = {"cancelled": 0, "submitted": 0}
# First cancel before releasing the loader.  Last allow an unopposed submit.
delays = (-1, 0, 0.001, 0.01, 0.03, 0.1, None)
for iteration, delay in enumerate(delays):
    machine = args.root / ("signal-race-" + str(iteration))
    machine.mkdir()
    process = None
    try:
        store(machine / "vcpu", "4")
        store(machine / "mem", str(1024 * 1024 * 1024))
        command = [str(loader), str(kernel), "initramfs=" + str(initrd),
                   "console=ttyS0"]
        store(machine / "loader",
              "printf 'READY\\n'; read gate; exec " + shlex.join(command))
        process = subprocess.Popen(
            ["/bin/rm", str(machine / "stopped")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 20)
        if not ready or process.stdout.readline() != b"READY\n":
            raise RuntimeError("loader did not reach the submission gate")
        if delay == -1:
            process.send_signal(signal.SIGINT)
            # EOF releases the independently running loader after cancellation.
            process.stdin.close()
            process.stdin = None
        else:
            process.stdin.write(b"go\n")
            process.stdin.flush()
            if delay is not None:
                time.sleep(delay)
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
            process.stdin.close()
            process.stdin = None
        output, error = process.communicate(timeout=30)
        record = events(machine)
        submitted = b"machine boot completed error=0" in record
        stopped = (machine / "stopped").exists()
        if submitted:
            outcomes["submitted"] += 1
        else:
            if not stopped or process.returncode == 0:
                raise RuntimeError("cancel left an active or ambiguous launch")
            expected = ("machine boot failed error=%d" % errno.ECANCELED).encode()
            if expected not in record:
                raise RuntimeError("cancel did not publish BOOT_FAILED")
            outcomes["cancelled"] += 1
        if delay == -1 and submitted:
            raise RuntimeError("cancel before gate release committed the guest")
        if delay is None and (not submitted or process.returncode != 0):
            raise RuntimeError("unopposed Linux submission failed")
        if not stopped:
            stop(machine)
        # The preceding request must not poison the next boot admission.
        fd = os.open(machine / "boot", os.O_RDWR)
        os.close(fd)
        if not (machine / "stopped").exists():
            raise RuntimeError("next launch close did not return to STOPPED")
        record += events(machine)
        (logs / (str(iteration) + ".log")).write_bytes(
            ("delay=%r rm_status=%d submitted=%s\n" %
             (delay, process.returncode, submitted)).encode() +
            output + error + record)
        machine.rmdir()
        print("PASS delay=%r submitted=%s retry/rmdir" %
              (delay, submitted), flush=True)
    finally:
        if process is not None:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                process.communicate(timeout=30)
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream is not None:
                    stream.close()
        if machine.exists():
            # Preserve the test outcome above, but never leave a running guest.
            stop(machine)
            machine.rmdir()
if not all(outcomes.values()):
    raise RuntimeError("both terminal outcomes were not exercised: %r" % outcomes)
print("PASS terminal outcomes %r" % outcomes, flush=True)
